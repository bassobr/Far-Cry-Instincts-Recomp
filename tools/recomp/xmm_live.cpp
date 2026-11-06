// Liveness of the xmm registers across calls and returns, over the whole program.
//
// A recompiled function holds the guest's xmm registers in locals and, without this, writes every one it changed back
// to the Cpu before each call and return and reads every one again after each call: a register's value could be an
// argument of the callee, a result for the caller, or simply kept across the call. MSVC's x86 code mostly treats
// xmm0-7 as scratch, but not always (__m128 arguments and results travel in xmm0-2; an experiment that dropped the
// write-backs broke the game), so this proves per call site and return what is really needed:
//   reads(F) - the parts of xmm registers F may read before writing them (directly or in callees), and
//   exit(F)  - the parts a caller may read after F returns before writing them.
// A part is lane 0 (scalar SSE reads and writes only that) or lanes 1-3. Indirect calls use the union over all
// functions, unknown control flow counts as everything live. Host code (kernel, HLE hooks) never touches guest xmm;
// a hooked function counts as an indirect call (the host may call guest functions on).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "codegen_internal.h"
#include "hooks.h"

namespace recomp {

namespace {

constexpr uint16_t kAll = 0xFFFF;
uint16_t Low(int r) { return uint16_t(1u << r); }
uint16_t High(int r) { return uint16_t(1u << (8 + r)); }
uint16_t Full(int r) { return uint16_t(Low(r) | High(r)); }

int XmmOf(const ZydisDecodedOperand& op) {
    if (op.type != ZYDIS_OPERAND_TYPE_REGISTER || op.reg.value < ZYDIS_REGISTER_XMM0 || op.reg.value > ZYDIS_REGISTER_XMM7)
        return -1;
    return op.reg.value - ZYDIS_REGISTER_XMM0;
}

// what an instruction reads (use) and writes completely (def) of the xmm registers; conservative where unsure (more
// use, less def). Matches the C++ codegen_insn.cpp emits (e.g. sqrtss reads its destination's lane 0 there).
void UseDef(const Decoded& d, uint16_t* use, uint16_t* def) {
    *use = *def = 0;
    const auto* o = d.ops;
    const int dst = XmmOf(o[0]), src = d.ins.operand_count_visible > 1 ? XmmOf(o[1]) : -1;
    switch (d.ins.mnemonic) {
    case ZYDIS_MNEMONIC_ADDSS: case ZYDIS_MNEMONIC_SUBSS: case ZYDIS_MNEMONIC_MULSS: case ZYDIS_MNEMONIC_DIVSS:
    case ZYDIS_MNEMONIC_MAXSS: case ZYDIS_MNEMONIC_MINSS: case ZYDIS_MNEMONIC_SQRTSS: case ZYDIS_MNEMONIC_RSQRTSS:
    case ZYDIS_MNEMONIC_RCPSS:
        if (dst < 0) break;
        *use = Low(dst) | (src >= 0 ? Low(src) : 0);
        *def = Low(dst);
        return;
    case ZYDIS_MNEMONIC_MOVSS:
        if (dst >= 0 && src >= 0) *use = Low(src), *def = Low(dst);
        else if (dst >= 0) *def = Full(dst);           // load: the upper lanes become 0
        else if (src >= 0) *use = Low(src);            // store
        return;
    case ZYDIS_MNEMONIC_COMISS: case ZYDIS_MNEMONIC_UCOMISS:
        *use = (dst >= 0 ? Low(dst) : 0) | (src >= 0 ? Low(src) : 0);
        return;
    case ZYDIS_MNEMONIC_CVTSI2SS:
        if (dst >= 0) *def = Low(dst);
        return;
    case ZYDIS_MNEMONIC_CVTTSS2SI: case ZYDIS_MNEMONIC_CVTSS2SI:
        if (src >= 0) *use = Low(src);
        return;
    case ZYDIS_MNEMONIC_XORPS:
        if (dst >= 0 && dst == src) {  // the zero idiom, emitted as a plain zero
            *def = Full(dst);
            return;
        }
        break;
    default:
        break;
    }
    // everything else: the whole register for every xmm operand read, written over completely only when not read
    for (int k = 0; k < d.ins.operand_count; k++) {
        const int r = XmmOf(o[k]);
        if (r < 0) continue;
        const bool reads = o[k].actions & ZYDIS_OPERAND_ACTION_MASK_READ;
        const bool writes = o[k].actions & ZYDIS_OPERAND_ACTION_MASK_WRITE;
        if (reads) *use |= Full(r);
        if (writes && !reads) *def |= Full(r);
    }
    // partial writes keep the other lanes: they count as read
    switch (d.ins.mnemonic) {
    case ZYDIS_MNEMONIC_MOVLPS: case ZYDIS_MNEMONIC_MOVHPS: case ZYDIS_MNEMONIC_MOVLHPS: case ZYDIS_MNEMONIC_MOVHLPS:
    case ZYDIS_MNEMONIC_CVTPI2PS:
        if (dst >= 0) *use |= Full(dst), *def &= uint16_t(~Full(dst));
        break;
    default:
        break;
    }
}

}  // namespace

// the summary of one instruction (analysis order = FunctionWriter::insns_ order)
void FunctionWriter::XmmSummarize(XmmFunction* out) {
    Prepare();
    out->entry = index_.count(fn_.start) ? uint32_t(index_[fn_.start]) : 0;
    out->nodes.resize(insns_.size());
    out->vas.resize(insns_.size());
    for (size_t i = 0; i < insns_.size(); i++) out->vas[i] = insns_[i].va;
    const auto& functions = cg_.analysis().functions;
    auto exit_to = [&](uint32_t target) {
        XmmExit e;
        if (functions.count(target) && !HookName(target)) e.kind = XmmExit::kTail, e.target = target;
        else if (functions.count(target)) e.kind = XmmExit::kIndirect;
        else e.kind = XmmExit::kUnknown;
        return e;
    };
    for (size_t i = 0; i < insns_.size(); i++) {
        const Decoded& d = insns_[i];
        XmmNode& n = out->nodes[i];
        UseDef(d, &n.use, &n.def);
        for (uint32_t s : d.succ) n.succ.push_back(uint32_t(index_[s]));
        const auto& op0 = d.ops[0];
        const uint32_t next = cg_.analysis().NextAddress(d.va, d.ins, d.ops);
        const bool rel = op0.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && op0.imm.is_relative;
        const uint32_t target = rel ? uint32_t(d.va + d.ins.length + int32_t(op0.imm.value.s)) : 0;
        switch (d.ins.meta.category) {
        case ZYDIS_CATEGORY_CALL:
            if (rel) {
                if (target == kFtol2 && cg_.IsFtol2()) n.call = XmmNode::kNone;  // emitted inline, no xmm
                else if (target == kSetjmp && functions.count(target)) n.call = XmmNode::kSetjmp, n.target = target;
                else if (functions.count(target) && !HookName(target)) n.call = XmmNode::kDirect, n.target = target;
                else if (functions.count(target)) n.call = XmmNode::kIndirect;
                else n.call = XmmNode::kUnknown;
            } else {
                n.call = KernelOrdinal(op0) >= 0 ? XmmNode::kKernel : XmmNode::kIndirect;
            }
            break;
        case ZYDIS_CATEGORY_RET:
            n.exits.push_back({XmmExit::kRet, 0});
            break;
        case ZYDIS_CATEGORY_COND_BR:
            if (rel && !index_.count(target)) n.exits.push_back(exit_to(target));
            break;
        case ZYDIS_CATEGORY_UNCOND_BR:
            if (rel) {
                if (!index_.count(target)) n.exits.push_back(exit_to(target));
            } else if (KernelOrdinal(op0) >= 0) {
                n.exits.push_back({XmmExit::kRet, 0});  // KernelCall, then return to our caller
            } else {
                n.exits.push_back({XmmExit::kIndirect, 0});  // indirect jump, or the switch default
            }
            break;
        default:
            break;
        }
        if (d.falls_through && !index_.count(next)) {
            n.fall = exit_to(next);
            n.has_fall = true;
        }
    }
}

uint16_t Codegen::XmmExitLive(const XmmExit& e, uint32_t self, bool with_exit) const {
    const uint16_t exit = with_exit ? xmm_[self].exit : 0;
    switch (e.kind) {
    case XmmExit::kRet: return exit;
    case XmmExit::kTail: return uint16_t(xmm_[e.target].reads | exit);
    case XmmExit::kIndirect: return uint16_t(xmm_indirect_reads_ | exit);
    default: return kAll;
    }
}

uint16_t Codegen::XmmCallReads(const XmmNode& n) const {
    switch (n.call) {
    case XmmNode::kDirect: case XmmNode::kSetjmp: return xmm_[n.target].reads;
    case XmmNode::kIndirect: return xmm_indirect_reads_;
    case XmmNode::kUnknown: return kAll;
    default: return 0;
    }
}

// the local dataflow of one function with the current summaries: live before each instruction; out = live after it
void Codegen::XmmFlow(uint32_t start, std::vector<uint16_t>* in, std::vector<uint16_t>* out, bool with_exit) const {
    const XmmFunction& f = xmm_functions_.at(start);
    const size_t n = f.nodes.size();
    in->assign(n, 0);
    out->assign(n, 0);
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t i = n; i-- > 0;) {
            const XmmNode& node = f.nodes[i];
            uint16_t o = 0;
            for (uint32_t s : node.succ) o |= (*in)[s];
            for (const XmmExit& e : node.exits) o |= XmmExitLive(e, start, with_exit);
            if (node.has_fall) o |= XmmExitLive(node.fall, start, with_exit);
            uint16_t v;
            if (node.call == XmmNode::kSetjmp) o = kAll;  // a longjmp can land after it
            if (node.call != XmmNode::kNone && node.call != XmmNode::kKernel) v = uint16_t(o | XmmCallReads(node));
            else v = uint16_t(node.use | (o & ~node.def));
            if (v != (*in)[i] || o != (*out)[i]) {
                (*in)[i] = v;
                (*out)[i] = o;
                changed = true;
            }
        }
    }
}

void Codegen::AnalyzeXmm() {
    const auto& functions = analysis_.functions;
    for (const auto& [va, fn] : functions) {
        FunctionWriter writer(*this, fn);
        writer.XmmSummarize(&xmm_functions_[va]);
        xmm_[va] = {};
        xmm_address_taken_[va] = false;
    }
    // Indirect calls can only reach functions whose address the program has as a value: an aligned 32-bit word in the
    // image equal to a function start (vtables, callback tables; not the switch tables, whose entries are jump
    // targets inside functions), or an absolute immediate/displacement in an instruction (push offset, mov reg, offset),
    // plus the functions the host calls by address (hooks). Code the analysis marks as data decoded as code never runs.
    // Functions whose start is an instruction inside another function's code are fragments the pointer heuristics of
    // the analysis made into functions (their code runs as part of the enclosing function; their reads before writes
    // are that function's values); small integers in data often look like their addresses, so they are no targets.
    std::unordered_map<uint32_t, bool> inside;
    for (const auto& [va, f] : xmm_functions_)
        for (uint32_t a : f.vas)
            if (a != va && functions.count(a)) inside[a] = true;
    size_t taken = 0;
    auto take = [&](uint32_t v) {
        auto it = xmm_address_taken_.find(v);
        if (it == xmm_address_taken_.end() || it->second || inside.count(v)) return;
        auto fn = functions.find(v);
        if (fn != functions.end() && fn->second.suspicious) return;
        it->second = true;
        taken++;
    };
    std::vector<std::pair<uint32_t, uint32_t>> tables;  // [begin, end) of the switch tables
    for (const auto& [table, targets] : analysis_.jump_tables) tables.push_back({table, table + 4 * uint32_t(targets.size())});
    std::sort(tables.begin(), tables.end());
    auto in_table = [&](uint32_t va) {
        auto it = std::upper_bound(tables.begin(), tables.end(), std::make_pair(va, 0xFFFFFFFFu));
        return it != tables.begin() && va < std::prev(it)->second;
    };
    for (const auto& s : image_.sections) {
        if (s.rsize < 4) continue;
        const uint8_t* p = image_.At(s.va);
        for (uint32_t off = (4 - (s.va & 3)) & 3; off + 4 <= s.rsize; off += 4) {
            uint32_t v;
            std::memcpy(&v, p + off, 4);
            if (xmm_address_taken_.count(v) && !in_table(s.va + off)) take(v);
        }
    }
    for (const auto& [va, insn] : analysis_.insns) {
        ZydisDecodedInstruction ins;
        ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
        if (!analysis_.Decode(va, &ins, ops)) continue;
        for (int k = 0; k < ins.operand_count_visible; k++) {
            if (ops[k].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && !ops[k].imm.is_relative) take(uint32_t(ops[k].imm.value.u));
            if (ops[k].type == ZYDIS_OPERAND_TYPE_MEMORY) take(uint32_t(ops[k].mem.disp.value));
        }
    }
    for (const auto& [address, name] : Hooks())
        if (xmm_address_taken_.count(address) && !xmm_address_taken_[address]) xmm_address_taken_[address] = true, taken++;
    std::printf("xmm liveness: %zu of %zu functions may be called through a pointer\n", taken, functions.size());
    std::vector<uint16_t> in, out;
    int rounds = 0;
    for (bool changed = true; changed && rounds < 100; rounds++) {
        changed = false;
        std::unordered_map<uint32_t, uint16_t> exit_contrib;
        uint16_t indirect_exit = xmm_indirect_exit_;
        for (const auto& [va, f] : xmm_functions_) {
            XmmFlow(va, &in, &out, false);
            const uint16_t reads = f.nodes.empty() ? 0 : in[f.entry];
            if (reads != xmm_[va].reads) xmm_[va].reads = reads, changed = true;
            XmmFlow(va, &in, &out, true);
            for (size_t i = 0; i < f.nodes.size(); i++) {
                const XmmNode& node = f.nodes[i];
                if (node.call == XmmNode::kDirect || node.call == XmmNode::kSetjmp) exit_contrib[node.target] |= out[i];
                if (node.call == XmmNode::kIndirect) indirect_exit |= out[i];
                for (const XmmExit& e : node.exits)
                    if (e.kind == XmmExit::kTail) exit_contrib[e.target] |= xmm_[va].exit;
                if (node.has_fall && node.fall.kind == XmmExit::kTail) exit_contrib[node.fall.target] |= xmm_[va].exit;
            }
        }
        uint16_t indirect_reads = 0;
        for (const auto& [va, s] : xmm_)
            if (xmm_address_taken_[va]) indirect_reads |= s.reads;
        if (indirect_reads != xmm_indirect_reads_ || indirect_exit != xmm_indirect_exit_) changed = true;
        xmm_indirect_reads_ = indirect_reads;
        xmm_indirect_exit_ = indirect_exit;
        // any function may be called indirectly, from host code (hooks) or by a longjmp's landing: those callers'
        // needs count everywhere; hooked originals (o_) return through host code to unknown callers
        for (auto& [va, s] : xmm_) {
            uint16_t e = uint16_t(s.exit | exit_contrib[va] | (xmm_address_taken_[va] ? xmm_indirect_exit_ : 0));
            if (HookName(va)) e = kAll;
            if (e != s.exit) s.exit = e, changed = true;
        }
    }
    size_t reading = 0, exiting = 0;
    for (const auto& [va, s] : xmm_) reading += s.reads != 0, exiting += s.exit != 0;
    if (std::getenv("FCI_RECOMP_XMM_DEBUG")) {
        // the roots: functions whose own instructions read xmm before writing it (calls, tail jumps read nothing here)
        size_t roots = 0;
        uint16_t all_roots = 0;
        for (const auto& [va, f] : xmm_functions_) {
            const size_t n = f.nodes.size();
            std::vector<uint16_t> lin(n, 0);
            for (bool ch = true; ch;) {
                ch = false;
                for (size_t i = n; i-- > 0;) {
                    const XmmNode& node = f.nodes[i];
                    uint16_t o = 0;
                    for (uint32_t s : node.succ) o |= lin[s];
                    for (const XmmExit& e : node.exits) o |= e.kind == XmmExit::kUnknown ? kAll : 0;
                    if (node.has_fall && node.fall.kind == XmmExit::kUnknown) o |= kAll;
                    uint16_t v = node.call == XmmNode::kUnknown ? kAll
                                 : node.call != XmmNode::kNone && node.call != XmmNode::kKernel ? o
                                                                                                   : uint16_t(node.use | (o & ~node.def));
                    if (v != lin[i]) lin[i] = v, ch = true;
                }
            }
            if (n && lin[f.entry] && xmm_address_taken_.at(va)) {
                roots++;
                all_roots |= lin[f.entry];
                if (roots <= 40) {
                    size_t first = 0;  // an instruction that uses what is live at entry
                    for (size_t i = 0; i < n; i++)
                        if (f.nodes[i].use & lin[f.entry]) { first = i; break; }
                    std::printf("  root %08X reads %04X (%zu insns, first use at node %zu, unknown call %d)\n", va,
                                lin[f.entry], n, first,
                                int(std::count_if(f.nodes.begin(), f.nodes.end(), [](const XmmNode& x) { return x.call == XmmNode::kUnknown; })));
                }
            }
        }
        std::printf("xmm liveness: %zu root functions read xmm themselves, together %04X\n", roots, all_roots);
        // which functions read xmm on entry and why: the smallest ones, with their unknown control flow
        std::vector<std::pair<size_t, uint32_t>> small;
        for (const auto& [va, s] : xmm_)
            if (s.reads && xmm_address_taken_[va]) small.push_back({xmm_functions_[va].nodes.size(), va});
        std::sort(small.begin(), small.end());
        for (size_t k = 0; k < small.size() && k < 25; k++) {
            const XmmFunction& f = xmm_functions_[small[k].second];
            int unknown = 0, indirect = 0, calls = 0;
            for (const XmmNode& n : f.nodes) {
                unknown += n.call == XmmNode::kUnknown;
                indirect += n.call == XmmNode::kIndirect;
                calls += n.call == XmmNode::kDirect;
                for (const XmmExit& e : n.exits) unknown += e.kind == XmmExit::kUnknown, indirect += e.kind == XmmExit::kIndirect;
            }
            std::printf("  %08X: %zu insns, reads %04X exit %04X, direct calls %d, indirect %d, unknown %d\n",
                        small[k].second, f.nodes.size(), xmm_[small[k].second].reads, xmm_[small[k].second].exit, calls,
                        indirect, unknown);
        }
    }
    std::printf("xmm liveness: %d rounds, %zu of %zu functions read xmm on entry, %zu have xmm live after returning; "
                "indirect calls: reads %04X, live after %04X\n",
                rounds, reading, xmm_.size(), exiting, xmm_indirect_reads_, xmm_indirect_exit_);
}

}  // namespace recomp
