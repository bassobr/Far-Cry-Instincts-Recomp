#include "codegen.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "codegen_internal.h"
#include "hooks.h"

namespace recomp {

std::string F(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return buf;
}

namespace {

uint32_t Defined(const ZydisDecodedInstruction& ins) {
    if (!ins.cpu_flags) return 0;
    const auto& fl = *ins.cpu_flags;
    return (fl.modified | fl.set_0 | fl.set_1 | fl.undefined) & kStatusFlags;
}

uint32_t Tested(const ZydisDecodedInstruction& ins) { return ins.cpu_flags ? ins.cpu_flags->tested & kStatusFlags : 0; }

bool IsTableJump(const ZydisDecodedOperand& op) {
    return op.type == ZYDIS_OPERAND_TYPE_MEMORY && op.mem.base == ZYDIS_REGISTER_NONE &&
           op.mem.index != ZYDIS_REGISTER_NONE && op.mem.scale == 4;
}

}  // namespace

void FunctionWriter::Prepare() {
    const auto& analysis = cg_.analysis();
    // The analysis records instructions globally; collect this function's by walking it again. The walk
    // takes in everything reachable without a call, also past other function starts (functions found
    // through data pointers can begin inside this one); only unconditional jumps to a function start
    // stay tail calls.
    std::vector<uint32_t> work(fn_.blocks.begin(), fn_.blocks.end());
    std::set<uint32_t> seen;
    auto reach = [&](uint32_t t) {
        if (analysis.insns.count(t)) work.push_back(t);
    };
    while (!work.empty()) {
        uint32_t va = work.back();
        work.pop_back();
        while (analysis.insns.count(va) && seen.insert(va).second) {
            Decoded d;
            d.va = va;
            analysis.Decode(va, &d.ins, d.ops);
            uint32_t next = analysis.NextAddress(va, d.ins, d.ops);
            const auto& op0 = d.ops[0];
            bool rel = op0.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && op0.imm.is_relative;
            uint32_t target = rel ? uint32_t(next + int32_t(op0.imm.value.s)) : 0;
            bool stop = false;
            switch (d.ins.meta.category) {
            case ZYDIS_CATEGORY_RET:
                stop = true;
                break;
            case ZYDIS_CATEGORY_COND_BR:
                if (rel) reach(target);
                break;
            case ZYDIS_CATEGORY_UNCOND_BR:
                stop = true;
                if (rel && (target == fn_.start || !analysis.functions.count(target))) reach(target);
                if (IsTableJump(op0)) {
                    auto it = analysis.jump_tables.find(uint32_t(op0.mem.disp.value));
                    if (it != analysis.jump_tables.end())
                        for (uint32_t t : it->second) reach(t);
                }
                break;
            default:
                if (d.ins.mnemonic == ZYDIS_MNEMONIC_INT3 || d.ins.mnemonic == ZYDIS_MNEMONIC_HLT ||
                    d.ins.mnemonic == ZYDIS_MNEMONIC_UD2)
                    stop = true;
                break;
            }
            insns_.push_back(d);
            if (stop) break;
            va = next;
        }
    }
    std::sort(insns_.begin(), insns_.end(), [](const Decoded& a, const Decoded& b) { return a.va < b.va; });
    for (size_t i = 0; i < insns_.size(); i++) index_[insns_[i].va] = i;

    for (size_t i = 0; i < insns_.size(); i++) {
        auto& d = insns_[i];
        uint32_t next = analysis.NextAddress(d.va, d.ins, d.ops);
        const auto& op0 = d.ops[0];
        bool rel = op0.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && op0.imm.is_relative;
        uint32_t target = rel ? uint32_t(d.va + d.ins.length + int32_t(op0.imm.value.s)) : 0;
        auto add_target = [&](uint32_t t) {
            if (!index_.count(t)) {
                d.exits = true;
                return;
            }
            d.succ.push_back(t);
            labels_.insert(t);
        };
        switch (d.ins.meta.category) {
        case ZYDIS_CATEGORY_RET:
            d.falls_through = false;
            break;
        case ZYDIS_CATEGORY_COND_BR:
            if (rel) add_target(target);
            break;
        case ZYDIS_CATEGORY_UNCOND_BR:
            d.falls_through = false;
            if (rel) add_target(target);
            else if (IsTableJump(op0)) {
                d.exits = true;  // the switch default
                auto it = analysis.jump_tables.find(uint32_t(op0.mem.disp.value));
                if (it != analysis.jump_tables.end())
                    for (uint32_t t : it->second) add_target(t);
            } else if (KernelOrdinal(op0) < 0) {
                d.exits = true;  // indirect jump
            }
            break;
        default:
            if (d.ins.mnemonic == ZYDIS_MNEMONIC_INT3 || d.ins.mnemonic == ZYDIS_MNEMONIC_HLT ||
                d.ins.mnemonic == ZYDIS_MNEMONIC_UD2 || d.ins.mnemonic == ZYDIS_MNEMONIC_IRETD)
                d.falls_through = false;
            break;
        }
        if (d.falls_through) {
            if (index_.count(next)) {
                d.succ.push_back(next);
                if (i + 1 >= insns_.size() || insns_[i + 1].va != next) labels_.insert(next);
            } else {
                d.exits = true;
            }
        }
    }
}

// the target of a direct call (0 for others)
static uint32_t DirectCallTarget(const Decoded& d) {
    if (d.ins.meta.category != ZYDIS_CATEGORY_CALL) return 0;
    const auto& op0 = d.ops[0];
    if (op0.type != ZYDIS_OPERAND_TYPE_IMMEDIATE || !op0.imm.is_relative) return 0;
    return uint32_t(d.va + d.ins.length + int32_t(op0.imm.value.s));
}

// the flags live where this instruction leaves the function without a call: what the function jumps (or falls) into
// reads on entry, all of them where that is not known (indirect jumps, the default of a jump table)
uint32_t FunctionWriter::ExitFlags(const Decoded& d) const {
    if (!d.exits) return 0;
    const auto& functions = cg_.analysis().functions;
    const auto& op0 = d.ops[0];
    const uint32_t next = d.va + d.ins.length;
    const bool rel = op0.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && op0.imm.is_relative;
    const uint32_t target = rel ? uint32_t(next + int32_t(op0.imm.value.s)) : 0;
    uint32_t live = 0;
    auto to = [&](uint32_t t) { live |= functions.count(t) ? cg_.FlagsAtEntry(t) : kStatusFlags; };
    switch (d.ins.meta.category) {
    case ZYDIS_CATEGORY_UNCOND_BR:
        if (!rel) return kStatusFlags;
        to(target);
        return live;
    case ZYDIS_CATEGORY_COND_BR:
        if (!rel) return kStatusFlags;
        if (!index_.count(target)) to(target);
        if (d.falls_through && !index_.count(next)) to(next);
        return live;
    default:
        if (d.falls_through && !index_.count(next)) to(next);
        else live = kStatusFlags;
        return live;
    }
}

// backward dataflow over the function's instructions; flags are live where the function leaves without a call as far
// as the code there reads them on entry, at returns as far as a caller reads them (both AnalyzeFlags), and across a
// call as far as the callee reads them on entry
void FunctionWriter::Liveness() {
    std::vector<uint32_t> live_in(insns_.size(), 0);
    const uint32_t at_ret = cg_.ReturnsFlags(fn_.start);
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t i = insns_.size(); i-- > 0;) {
            auto& d = insns_[i];
            uint32_t out = ExitFlags(d);
            if (d.ins.meta.category == ZYDIS_CATEGORY_RET) out |= at_ret;
            for (uint32_t s : d.succ) out |= live_in[index_[s]];
            uint32_t in = d.ins.meta.category == ZYDIS_CATEGORY_CALL ? cg_.FlagsAtEntry(DirectCallTarget(d))
                                                                        : Tested(d.ins) | (out & ~Defined(d.ins));
            if (out != d.live_out || in != live_in[i]) {
                d.live_out = out;
                live_in[i] = in;
                changed = true;
            }
        }
    }
    entry_flags_ = index_.count(fn_.start) ? live_in[index_[fn_.start]] : 0;
}

uint32_t FunctionWriter::FlagSummary(std::vector<std::pair<uint32_t, uint32_t>>* returns,
                                     std::vector<uint32_t>* targets, std::vector<uint32_t>* tails) {
    Prepare();
    Liveness();
    const auto& functions = cg_.analysis().functions;
    for (const auto& d : insns_) {
        if (uint32_t t = DirectCallTarget(d); t && functions.count(t)) {
            targets->push_back(t);
            if (d.live_out & kStatusFlags) returns->push_back({t, d.live_out & kStatusFlags});
        }
        // jumps (and falls) into other functions: their entries feed this one's liveness
        const auto& op0 = d.ops[0];
        if (d.exits && (d.ins.meta.category == ZYDIS_CATEGORY_UNCOND_BR || d.ins.meta.category == ZYDIS_CATEGORY_COND_BR) &&
            op0.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && op0.imm.is_relative) {
            const uint32_t t = uint32_t(d.va + d.ins.length + int32_t(op0.imm.value.s));
            if (!index_.count(t)) targets->push_back(t), tails->push_back(t);
        }
        if (d.exits && d.falls_through && !index_.count(d.va + d.ins.length)) {
            targets->push_back(d.va + d.ins.length);
            tails->push_back(d.va + d.ins.length);
        }
    }
    return entry_flags_;
}

// to a fixpoint: what a function reads on entry depends on what it calls and jumps into, what it must return with on
// its callers; most functions neither read nor return flags, so the code of nearly all stays as it was
void Codegen::AnalyzeFlags() {
    const auto& functions = analysis_.functions;
    std::unordered_map<uint32_t, std::vector<uint32_t>> users;  // the functions that call or jump into one
    std::vector<uint32_t> work;
    for (const auto& [va, fn] : functions) work.push_back(va);
    bool first = true;
    size_t rounds = 0;
    while (!work.empty()) {
        rounds++;
        std::sort(work.begin(), work.end());
        work.erase(std::unique(work.begin(), work.end()), work.end());
        std::vector<uint32_t> next;
        for (uint32_t va : work) {
            FunctionWriter writer(*this, functions.at(va));
            std::vector<std::pair<uint32_t, uint32_t>> returns;
            std::vector<uint32_t> targets, tails;
            const uint32_t in = writer.FlagSummary(&returns, &targets, &tails);
            // what it returns with, the code it jumps into returns with for it
            if (const uint32_t mine = ReturnsFlags(va))
                for (uint32_t t : tails)
                    if (functions.count(t)) returns.push_back({t, mine});
            if (first)
                for (uint32_t t : targets) users[t].push_back(va);
            if (in != FlagsAtEntry(va)) {
                flags_in_[va] = in;
                for (uint32_t u : users[va]) next.push_back(u);  // their calls and jumps to it read more now
            }
            for (const auto& [t, mask] : returns) {
                uint32_t& have = flags_out_[t];
                if ((have | mask) != have) {
                    have |= mask;
                    next.push_back(t);  // its returns hand more flags back now
                }
            }
        }
        first = false;
        work.swap(next);
    }
    size_t entry = 0, out = 0;
    for (const auto& [va, in] : flags_in_) entry += in != 0;
    for (const auto& [va, mask] : flags_out_) out += mask != 0;
    std::printf("flags across calls: %zu functions read them on entry, %zu return them (%zu rounds)\n", entry, out,
                rounds);
}

// leaving the function without a call hands the flags over through Cpu
std::string FunctionWriter::Jump(uint32_t target) const {
    if (index_.count(target)) return F("goto L_%08X;", target);
    if (cg_.analysis().functions.count(target)) return F("{ FCI_SAVE c.flags = f; f_%08X(c); return; }", target);
    return F("{ FCI_SAVE c.flags = f; Call(c, 0x%08Xu); return; }", target);
}

// a backward branch closes a loop: the place to give the single guest CPU to a waiting thread
std::string FunctionWriter::Branch(uint32_t from, uint32_t target) const {
    if (index_.count(target) && target <= from)
        return F("{ if (g_preempt) [[unlikely]] { FCI_SAVE Preempt(c); FCI_LOAD } goto L_%08X; }", target);
    return Jump(target);
}

std::string FunctionWriter::CallTo(uint32_t target, uint32_t ret) const {
    if (target == kFtol2 && cg_.IsFtol2())
        return "const uint64_t r = uint64_t(FToInt<int64_t>(std::trunc(ST(c, 0)))); FPop(c); c.eax = uint32_t(r); "
               "c.edx = uint32_t(r >> 32);";
    if (target == kSetjmp && cg_.analysis().functions.count(target))
        return F("c.esp -= 4; W32(c.esp, 0x%08Xu); FCI_SAVE f_%08X(c); "
                 "if (setjmp(*Landing(c, 0x%08Xu))) { SetjmpBarrier(c); f = c.flags; } else { SetjmpBarrier(c); } FCI_LOAD",
                 ret, target, ret);
    if (cg_.analysis().functions.count(target)) {
        // flags across the call (AnalyzeFlags): handed over when the callee reads them on entry, taken back when this
        // function reads the ones the callee returns with
        const char* in = cg_.FlagsAtEntry(target) ? " c.flags = f;" : "";
        const char* back = cur_ && (cur_->live_out & cg_.ReturnsFlags(target)) ? " f = c.flags;" : "";
        return F("c.esp -= 4; W32(c.esp, 0x%08Xu); FCI_SAVE%s f_%08X(c); FCI_LOAD%s", ret, in, target, back);
    }
    return F("c.esp -= 4; W32(c.esp, 0x%08Xu); FCI_SAVE Call(c, 0x%08Xu); FCI_LOAD", ret, target);
}

int FunctionWriter::KernelOrdinal(const ZydisDecodedOperand& op) const {
    const auto& img = cg_.image();
    if (op.type != ZYDIS_OPERAND_TYPE_MEMORY || op.mem.base != ZYDIS_REGISTER_NONE || op.mem.index != ZYDIS_REGISTER_NONE)
        return -1;
    uint32_t slot = uint32_t(op.mem.disp.value);
    if (slot < img.kernel_thunk || slot >= img.kernel_thunk + 4 * img.kernel_imports.size() || (slot & 3)) return -1;
    return int(img.kernel_imports[(slot - img.kernel_thunk) / 4]);
}

namespace {

const char* const kGprNames[] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};

bool IdentChar(char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_'; }

// whether the register name that ends before s[at] is assigned: lanes (.f[..], .u[..], .q[..]), then =, op= or ++/--
bool Assigned(const std::string& s, size_t at) {
    while (at + 2 < s.size() && s[at] == '.' && (s[at + 1] == 'f' || s[at + 1] == 'u' || s[at + 1] == 'q') && s[at + 2] == '[') {
        int depth = 0;
        size_t j = at + 2;
        for (; j < s.size(); j++) {
            if (s[j] == '[') depth++;
            else if (s[j] == ']' && --depth == 0) break;
        }
        at = j + 1;
    }
    while (at < s.size() && s[at] == ' ') at++;
    if (at >= s.size()) return false;
    const char a = s[at], b = at + 1 < s.size() ? s[at + 1] : 0, c = at + 2 < s.size() ? s[at + 2] : 0;
    if (a == '=') return b != '=';
    if ((a == '+' || a == '-') && (b == a || b == '=')) return true;
    if ((a == '*' || a == '/' || a == '%' || a == '&' || a == '|' || a == '^') && b == '=') return true;
    return (a == '<' || a == '>') && b == a && c == '=';
}

}  // namespace

// The emitters name guest registers as members of the Cpu (c.eax, c.xmm[3]); a function holds them in locals (eax, x3),
// so they are renamed here. used collects them (bits 0-7 the general registers, 8-15 xmm0-7), written those assigned.
std::string FunctionWriter::Localize(const std::string& in, uint32_t* used, uint32_t* written) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); i++) {
        if (in[i] == 'c' && i + 1 < in.size() && in[i + 1] == '.' && (i == 0 || (!IdentChar(in[i - 1]) && in[i - 1] != '.'))) {
            const char* rest = in.c_str() + i + 2;
            int reg = -1;
            size_t length = 0;
            for (int r = 0; r < 8 && reg < 0; r++)
                if (std::strncmp(rest, kGprNames[r], 3) == 0 && !IdentChar(rest[3])) reg = r, length = 5;  // c.eax
            if (reg < 0 && std::strncmp(rest, "xmm[", 4) == 0 && rest[4] >= '0' && rest[4] <= '7' && rest[5] == ']')
                reg = 8 + rest[4] - '0', length = 8;  // c.xmm[N]
            if (reg >= 0) {
                const bool pre = out.size() >= 2 && (out.compare(out.size() - 2, 2, "++") == 0 || out.compare(out.size() - 2, 2, "--") == 0);
                if (pre || Assigned(in, i + length)) *written |= 1u << reg;
                out += reg < 8 ? std::string(kGprNames[reg]) : F("x%d", reg - 8);
                *used |= 1u << reg;
                i += length - 1;  // the loop steps over the last character
                continue;
            }
        }
        out += in[i];
    }
    if (out.find("FCI_GPRS") != std::string::npos) *used |= 0xFFu, *written |= 0xFFu;
    return out;
}

std::string FunctionWriter::RamAccesses(const std::string& in) const {
    static const char* const kEa[] = {"R8(ea)", "R16(ea)", "R32(ea)", "R64(ea)", "RF32(ea)", "RF64(ea)",
                                      "W8(ea,", "W16(ea,", "W32(ea,", "W64(ea,", "WF32(ea,", "WF64(ea,"};
    static const char* const kStack[] = {"R32(c.esp", "W32(c.esp"};
    std::string out;
    out.reserve(in.size() + 32);
    for (size_t i = 0; i < in.size(); i++) {
        if ((in[i] == 'R' || in[i] == 'W') && (i == 0 || !IdentChar(in[i - 1]))) {
            bool ram = false;
            for (const char* p : kStack) ram = ram || in.compare(i, std::strlen(p), p) == 0;
            if (ea_ram_)
                for (const char* p : kEa) ram = ram || in.compare(i, std::strlen(p), p) == 0;
            if (ram) out += "Ram";
            if (!ram && ea_base_ && BaseCheckOn()) {
                bool hit = false;
                for (const char* p : kEa) {
                    const size_t n = std::strlen(p);
                    if (in.compare(i, n, p) != 0) continue;
                    // R32(ea) -> BaseR32(eb, ea), W32(ea, v) -> BaseW32(eb, ea, v)
                    const size_t paren = in.find('(', i);
                    out += "Base" + in.substr(i, paren + 1 - i) + "eb, ";
                    i = paren;
                    hit = true;
                    break;
                }
                if (hit) continue;
            }
        }
        out += in[i];
    }
    return out;
}

std::string FunctionWriter::EmitOne(Decoded d) {
    d.live_out = kStatusFlags;
    body_.clear();
    if (!EmitInsn(d)) return {};
    uint32_t used = 0, written = 0;
    return Localize(RamAccesses(body_), &used, &written);
}

std::string FunctionWriter::Write() {
    Prepare();
    Liveness();

    ZydisFormatter fmt;
    ZydisFormatterInit(&fmt, ZYDIS_FORMATTER_STYLE_INTEL);

    const char* hook = HookName(fn_.start);
    uint32_t used = 0, written = 0;
    out_.clear();

    // xmm: which registers the function has at all and changes, and what each call/exit must write back or read again
    uint32_t xmm_used = 0, xmm_written = 0;
    for (const auto& d : insns_)
        for (int k = 0; k < d.ins.operand_count; k++) {
            const auto& op = d.ops[k];
            if (op.type != ZYDIS_OPERAND_TYPE_REGISTER || op.reg.value < ZYDIS_REGISTER_XMM0 || op.reg.value > ZYDIS_REGISTER_XMM7)
                continue;
            xmm_used |= 1u << (op.reg.value - ZYDIS_REGISTER_XMM0);
            if (op.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) xmm_written |= 1u << (op.reg.value - ZYDIS_REGISTER_XMM0);
        }
    const XmmFunction* xf = cg_.XmmOf(fn_.start);
    std::vector<uint16_t> xin, xout;
    if (xf) cg_.XmmFlow(fn_.start, &xin, &xout);
    const bool xmm_exact = xf && xf->nodes.size() == insns_.size();
    auto xmm_text = [&](uint16_t mask, bool save) {
        std::string s;
        for (int r = 0; r < 8; r++) {
            if (!(xmm_used & (1u << r)) || !(mask & (0x101u << r))) continue;
            if (save && !(xmm_written & (1u << r))) continue;
            s += save ? F(" c.xmm[%d] = x%d;", r, r) : F(" x%d = c.xmm[%d];", r, r);
        }
        return s;
    };
    auto with_xmm = [&](std::string text, uint16_t save, uint16_t load) {
        for (const auto& [token, add] : {std::pair<std::string, std::string>{"FCI_SAVE", xmm_text(save, true)},
                                         std::pair<std::string, std::string>{"FCI_LOAD", xmm_text(load, false)}}) {
            if (add.empty()) continue;
            for (size_t at = text.find(token); at != std::string::npos; at = text.find(token, at + token.size() + add.size()))
                text.insert(at + token.size(), add);
        }
        return text;
    };
    // blocks can sit below the entry (shared error paths), and the body is emitted in address order
    if (!insns_.empty() && insns_.front().va != fn_.start) {
        labels_.insert(fn_.start);
        out_ += F("    goto L_%08X;\n", fn_.start);
    }
    for (size_t i = 0; i < insns_.size(); i++) {
        const auto& d = insns_[i];
        char text[128];
        ZydisFormatterFormatInstruction(&fmt, &d.ins, d.ops, d.ins.operand_count_visible, text, sizeof text, d.va, nullptr);
        if (labels_.count(d.va)) out_ += F("L_%08X:\n", d.va);
        out_ += F("    // %08X %s\n", d.va, text);

        // the xmm this instruction's calls and exits need (everything without the analysis)
        uint16_t xsave = 0xFFFF, xload = 0xFFFF, xfall = 0xFFFF;
        if (xmm_exact) {
            const XmmNode& node = xf->nodes[i];
            xsave = xload = xfall = 0;
            for (const XmmExit& e : node.exits) xsave |= cg_.XmmExitLive(e, fn_.start);
            if (node.call != XmmNode::kNone && node.call != XmmNode::kKernel) {
                xsave |= uint16_t(cg_.XmmCallReads(node) | xout[i]);
                xload = xout[i];
            }
            if (node.has_fall) xfall = cg_.XmmExitLive(node.fall, fn_.start);
        }
        body_.clear();
        if (EmitInsn(d)) {
            out_ += F("    { PC(0x%08Xu); ", d.va) + with_xmm(Localize(RamAccesses(body_), &used, &written), xsave, xload) +
                    "}\n";
            // and what the instruction itself says it writes (hidden operands too), in case the text hides it
            for (int k = 0; k < d.ins.operand_count; k++) {
                const auto& op = d.ops[k];
                if (op.type != ZYDIS_OPERAND_TYPE_REGISTER || !(op.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE)) continue;
                const ZydisRegister r = op.reg.value;
                if (r >= ZYDIS_REGISTER_XMM0 && r <= ZYDIS_REGISTER_XMM7) written |= 1u << (8 + r - ZYDIS_REGISTER_XMM0);
                const ZydisRegister full = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LEGACY_32, r);
                if (full >= ZYDIS_REGISTER_EAX && full <= ZYDIS_REGISTER_EDI) written |= 1u << (full - ZYDIS_REGISTER_EAX);
            }
        } else {
            cg_.unimplemented[ZydisMnemonicGetString(d.ins.mnemonic)]++;
            out_ += F("    { FCI_SAVE Unimplemented(c, 0x%08Xu, \"%s\"); return; }\n", d.va, text);
            continue;
        }
        if (d.falls_through) {
            uint32_t next = cg_.analysis().NextAddress(d.va, d.ins, d.ops);
            if (i + 1 >= insns_.size() || insns_[i + 1].va != next) out_ += "    " + with_xmm(Jump(next), xfall, 0) + "\n";
        }
    }
    out_ += "}\n#undef FCI_SAVE\n#undef FCI_LOAD\n\n";

    // the registers it uses become locals; FCI_SAVE writes the general registers it changes back before control leaves
    // (calls, returns, tail jumps), FCI_LOAD reads them all again after a call; the xmm registers only where the
    // liveness analysis says a callee or caller needs them (xmm_live.cpp), added at each site above
    const uint16_t xentry = xmm_exact ? cg_.XmmReads(fn_.start) : 0xFFFF;
    std::string head = F("void FCI_GUEST_CC %c_%08X(Cpu& FCI_NOESCAPE FCI_RESTRICT c) {\n    Flags f = c.flags;\n", hook ? 'o' : 'f', fn_.start);
    std::string save, load;
    for (int r = 0; r < 16; r++) {
        if (!(used & (1u << r))) continue;
        std::string local = r < 8 ? kGprNames[r] : F("x%d", r - 8);
        std::string member = r < 8 ? F("c.%s", kGprNames[r]) : F("c.xmm[%d]", r - 8);
        if (r < 8 || (xentry & (0x101u << (r - 8))))
            head += F("    FCI_REG(%s) %s = %s;\n", r < 8 ? "uint32_t" : "Xmm", local.c_str(), member.c_str());
        else
            head += F("    FCI_REG_UNREAD(Xmm, %s, %s);\n", local.c_str(), member.c_str());  // not read before written
        if (r >= 8) continue;
        if (written & (1u << r)) save += F(" %s = %s;", member.c_str(), local.c_str());
        load += F(" %s = %s;", local.c_str(), member.c_str());
    }
    head += F("#define FCI_SAVE%s\n#define FCI_LOAD%s\n    ENTER(0x%08Xu);\n", save.c_str(), load.c_str(), fn_.start);
    out_ = head + out_;
    if (hook) out_ += F("void FCI_GUEST_CC f_%08X(Cpu& FCI_NOESCAPE FCI_RESTRICT c) { hle::%s(c); }\n\n", fn_.start, hook);
    return out_;
}

bool Codegen::IsFtol2() {
    if (ftol2_ < 0) {
        static const char* const kStart[] = {"push ebp", "mov ebp, esp", "sub esp, 0x20", "and esp, 0xFFFFFFF0",
                                             "fld st0", "fst [esp+0x18]", "fistp [esp+0x10]", "fild [esp+0x10]"};
        ZydisFormatter fmt;
        ZydisFormatterInit(&fmt, ZYDIS_FORMATTER_STYLE_INTEL);
        uint32_t va = kFtol2;
        ftol2_ = analysis_.functions.count(va) ? 1 : 0;
        for (const char* want : kStart) {
            ZydisDecodedInstruction ins;
            ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
            char text[128];
            if (!analysis_.Decode(va, &ins, ops) ||
                !ZYAN_SUCCESS(ZydisFormatterFormatInstruction(&fmt, &ins, ops, ins.operand_count_visible, text, sizeof text,
                                                              va, nullptr)) ||
                std::strcmp(text, want) != 0) {
                ftol2_ = 0;
                break;
            }
            va += ins.length;
        }
        std::printf("_ftol2 at %08X: %s\n", kFtol2, ftol2_ ? "calls emitted inline" : "not found, calls stay calls");
    }
    return ftol2_ == 1;
}

void WriteHostHeaders(const std::filesystem::path& dir) {
    std::filesystem::create_directories(dir);
    {
        // SYM_<name>: the address as eight hex digits, so SYM(name) can paste 0x in front
        std::ofstream s(dir / "image_symbols.h");
        s << "// generated by fci-recomp from config/symbols*.txt\n#pragma once\n\n";
        for (const auto& [name, sym] : Symbols()) s << F("#define SYM_%s %08X\n", name.c_str(), sym.address);
    }
    {
        // the host replacements the executable exports to the game code DLL (its f_<address> calls them)
        std::ofstream h(dir / "hle_hooks.h");
        h << "// generated by fci-recomp from config/hle.txt and tools/recomp/hooks.h\n#pragma once\n"
             "#include \"runtime/guest.h\"\n\nnamespace hle {\n";
        for (const auto& [address, name] : Hooks()) h << F("FCI_RT_API void %s(rt::Cpu& c);  // %08X\n", name.c_str(), address);
        h << "}  // namespace hle\n";
    }
    {
        // refers to each of them, so a replacement without a definition fails the executable's link, not the DLL's load
        std::ofstream c(dir / "hle_hooks.cpp");
        c << "// generated by fci-recomp from config/hle.txt and tools/recomp/hooks.h\n#include \"hle_hooks.h\"\n\n"
             "extern void (*const kHleExports[])(rt::Cpu&);\nvoid (*const kHleExports[])(rt::Cpu&) = {\n";
        for (const auto& [address, name] : Hooks()) c << F("    hle::%s,\n", name.c_str());
        c << "    nullptr,\n};\n";
    }
}

void Codegen::Write(const std::filesystem::path& dir, int partitions, uint64_t xbe_hash) {
    std::filesystem::create_directories(dir);
    const auto& functions = analysis_.functions;
    // off unless FCI_RECOMP_XMM_LIVE=1 when fci-recomp runs (codegen_internal.h)
    if (XmmLiveOn()) AnalyzeXmm();
    AnalyzeFlags();

    {
        std::ofstream h(dir / "funcs.h");
        h << "// generated by fci-recomp\n#pragma once\n#include \"runtime/guest.h\"\n\n";
        for (const auto& [va, fn] : functions) h << F("void FCI_GUEST_CC f_%08X(rt::Cpu& FCI_NOESCAPE FCI_RESTRICT c);\n", va);
        h << "\n// host replacements (src/hle, exported by the executable) and the originals they replace\nnamespace hle {\n";
        for (const auto& [address, name] : Hooks()) h << F("FCI_RT_API void %s(rt::Cpu& c);\n", name.c_str());
        h << "}  // namespace hle\n";
        for (const auto& [address, name] : Hooks())
            if (functions.count(address)) h << F("void FCI_GUEST_CC o_%08X(rt::Cpu& FCI_NOESCAPE FCI_RESTRICT c);\n", address);
    }
    {
        // the DLL's export (rt::GameCode, runtime/guest.h) and the data the generated code reads directly
        std::ofstream t(dir / "table.cpp");
        t << "// generated by fci-recomp\n#include \"funcs.h\"\n\n"
             "#ifndef FCI_ABI_HASH\n#error FCI_ABI_HASH: compile with the flags of CMakeLists.txt / src/app/game_code.cpp\n#endif\n"
             "#ifndef FCI_BUILD_KEY\n#define FCI_BUILD_KEY 0\n#endif\n\n"
             "namespace rt {\nvolatile int g_preempt = 0;\nDispatchEntry const* g_dispatch = nullptr;\n"
             "uint32_t g_dispatch_base = 0, g_dispatch_size = 0;\n}  // namespace rt\n\nnamespace {\n\n";
        t << "const rt::FunctionEntry kFunctions[] = {\n";
        for (const auto& [va, fn] : functions) t << F("    {0x%08Xu, f_%08X},\n", va, va);
        t << "};\n\nconst rt::FunctionEntry kOriginals[] = {\n";
        size_t originals = 0;
        for (const auto& [address, name] : Hooks())
            if (functions.count(address)) {
                t << F("    {0x%08Xu, o_%08X},  // %s\n", address, address, name.c_str());
                originals++;
            }
        if (!originals) t << "    {0, nullptr},\n";
        t << "};\n\n}  // namespace\n\n"
             "#ifdef FCI_PGO_GENERATE\nextern \"C\" int __llvm_profile_write_file(void);\n"
             "#define FCI_WRITE_PROFILE __llvm_profile_write_file\n#else\n#define FCI_WRITE_PROFILE nullptr\n"
             "// the one C runtime symbol clang expects in any object with floating point\n"
             "extern \"C\" int _fltused = 0;\n#endif\n\n"
             "extern \"C\" __declspec(dllexport) const rt::GameCode fci_game_code = {\n"
             "    rt::kGameCodeVersion,\n    sizeof(rt::GameCode),\n    FCI_ABI_HASH,\n";
        t << F("    0x%016llXull,\n", (unsigned long long)xbe_hash);
        t << F("    FCI_BUILD_KEY,\n    kFunctions,\n    %zuu,\n    %zuu,\n    kOriginals,\n", functions.size(), originals);
        t << "    &rt::g_preempt,\n    &rt::g_dispatch,\n    &rt::g_dispatch_base,\n    &rt::g_dispatch_size,\n"
             "    FCI_WRITE_PROFILE,\n};\n";
    }

    // partitions of roughly equal instruction count, so the compile load spreads evenly
    size_t total = 0;
    for (const auto& [va, fn] : functions) total += fn.insn_count;
    size_t per_part = total / partitions + 1;

    int part = 0;
    size_t in_part = 0;
    std::ofstream out;
    auto open = [&] {
        out.close();
        out.open(dir / F("code_%03d.cpp", part));
        out << "// generated by fci-recomp\n#include \"funcs.h\"\n\nusing namespace rt;\n\n";
    };
    open();
    for (const auto& [va, fn] : functions) {
        if (in_part >= per_part && part + 1 < partitions) {
            part++;
            in_part = 0;
            open();
        }
        FunctionWriter writer(*this, fn);
        out << writer.Write();
        in_part += fn.insn_count;
    }
    // keep a fixed set of files so the build system's file list stays stable
    for (part++; part < partitions; part++) open();
}

}  // namespace recomp
