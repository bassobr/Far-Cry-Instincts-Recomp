#include "analysis.h"

#include <algorithm>
#include <cstdio>
#include <fstream>

namespace recomp {

namespace {

constexpr size_t kMaxJumpTable = 1024;

bool IsPadding(uint8_t b) { return b == 0xCC || b == 0x90; }

// instructions an Xbox title never executes in its own code; seeing one means data was decoded
// (plain in/out stay plausible: the XDK's drivers talk to the MCPX through ports)
bool IsImplausible(const ZydisDecodedInstruction& ins) {
    switch (ins.mnemonic) {
    case ZYDIS_MNEMONIC_INSB:
    case ZYDIS_MNEMONIC_INSW:
    case ZYDIS_MNEMONIC_INSD:
    case ZYDIS_MNEMONIC_OUTSB:
    case ZYDIS_MNEMONIC_OUTSW:
    case ZYDIS_MNEMONIC_OUTSD:
    case ZYDIS_MNEMONIC_ARPL:
    case ZYDIS_MNEMONIC_BOUND:
    case ZYDIS_MNEMONIC_DAA:
    case ZYDIS_MNEMONIC_DAS:
    case ZYDIS_MNEMONIC_AAA:
    case ZYDIS_MNEMONIC_AAS:
    case ZYDIS_MNEMONIC_AAM:
    case ZYDIS_MNEMONIC_AAD:
    case ZYDIS_MNEMONIC_SALC:
    case ZYDIS_MNEMONIC_INTO:
    case ZYDIS_MNEMONIC_INT:
    case ZYDIS_MNEMONIC_INT1:
    case ZYDIS_MNEMONIC_IRETD:
    case ZYDIS_MNEMONIC_HLT:
    case ZYDIS_MNEMONIC_SLDT:
    case ZYDIS_MNEMONIC_SGDT:
    case ZYDIS_MNEMONIC_SIDT:
    case ZYDIS_MNEMONIC_STR:
    case ZYDIS_MNEMONIC_LDS:
    case ZYDIS_MNEMONIC_LES:
    case ZYDIS_MNEMONIC_LSS:
    case ZYDIS_MNEMONIC_LFS:
    case ZYDIS_MNEMONIC_LGS:
    case ZYDIS_MNEMONIC_FBSTP:
    case ZYDIS_MNEMONIC_FISTTP:  // SSE3, not on the Pentium III
        return true;
    default:
        return false;
    }
}

}  // namespace

Analysis::Analysis(const xbe::Image& image, std::vector<std::string> code_sections) : image_(image) {
    for (const auto& name : code_sections)
        if (const auto* s = image_.FindSection(name)) code_.push_back(s);
    ZydisDecoderInit(&decoder_, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32);
}

bool Analysis::IsCode(uint32_t va) const {
    for (const auto* s : code_)
        if (va >= s->va && va < s->va + s->rsize) return true;
    return false;
}

bool Analysis::Decode(uint32_t va, ZydisDecodedInstruction* ins, ZydisDecodedOperand* ops) const {
    if (!IsCode(va)) return false;
    const auto* s = image_.SectionAt(va);
    size_t avail = s->va + s->rsize - va;
    return ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder_, image_.At(va), avail, ins, ops));
}

// the debug service interrupt (DbgPrint, DbgBreakPoint) is followed by an int3 the kernel steps over
uint32_t Analysis::NextAddress(uint32_t va, const ZydisDecodedInstruction& ins, const ZydisDecodedOperand* ops) const {
    uint32_t next = va + ins.length;
    if (ins.mnemonic == ZYDIS_MNEMONIC_INT && ops[0].imm.value.u == 0x2D && image_.Mapped(next) && image_.At(next)[0] == 0xCC)
        next++;
    return next;
}

// MSVC pads between functions with int3 or nop and aligns most of them; a pointer into code is
// only taken as a function when it sits on such a boundary and the first instructions decode.
bool Analysis::LooksLikeFunctionStart(uint32_t va) const {
    if (!IsCode(va)) return false;
    if (functions.count(va)) return true;
    if (image_.At(va)[0] == 0) return false;  // zero fill, decodes as add [eax], al
    uint8_t prev = image_.At(va - 1)[0];
    bool boundary = (va & 15) == 0 || IsPadding(prev) || prev == 0xC3 ||
                    (image_.Mapped(va - 3, 3) && image_.At(va - 3)[0] == 0xC2);
    if (!boundary) return false;
    ZydisDecodedInstruction ins;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    uint32_t at = va;
    for (int i = 0; i < 4; i++) {
        if (!Decode(at, &ins, ops)) return false;
        if (ins.mnemonic == ZYDIS_MNEMONIC_INT3 && i == 0) return false;
        if (ins.meta.category == ZYDIS_CATEGORY_RET || ins.meta.category == ZYDIS_CATEGORY_UNCOND_BR) break;
        at += ins.length;
    }
    return true;
}

void Analysis::Queue(uint32_t va, bool from_pointer) {
    if (!IsCode(va) || functions.count(va)) return;
    queue_.push_back({va, from_pointer});
}

// Reads the entries around `table`. Hand-written CRT code (memcpy) indexes its tables with negative
// indices or leaves entry 0 unused, so entries are taken in both directions as long as they point at
// code close to the jump.
std::vector<uint32_t> Analysis::ReadJumpTable(uint32_t table, uint32_t jump) const {
    constexpr uint32_t kNear = 0x4000;
    auto valid = [&](uint32_t at) {
        if (!image_.Mapped(at, 4)) return false;
        uint32_t t = image_.Read32(at);
        return IsCode(t) && (t > jump ? t - jump : jump - t) < kNear;
    };
    std::vector<uint32_t> targets;
    for (uint32_t at = table - 4; targets.size() < 32 && valid(at); at -= 4) targets.push_back(image_.Read32(at));
    uint32_t at = table;
    if (!valid(at) && valid(at + 4)) at += 4;  // unused entry 0
    for (; targets.size() < kMaxJumpTable && valid(at); at += 4) {
        // a table ends where the next function or table begins
        if (at != table && (functions.count(at) || jump_tables.count(at))) break;
        targets.push_back(image_.Read32(at));
    }
    return targets;
}

void Analysis::Explore(Function& fn) {
    ZydisDecodedInstruction ins;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    std::vector<uint32_t> work{fn.start};
    std::set<uint32_t> seen;
    fn.blocks.insert(fn.start);

    while (!work.empty()) {
        uint32_t va = work.back();
        work.pop_back();
        for (;;) {
            if (!seen.insert(va).second) break;
            if (!Decode(va, &ins, ops)) {
                bad_decodes.insert(va);
                fn.suspicious = true;
                break;
            }
            uint32_t next = NextAddress(va, ins, ops);
            if (IsImplausible(ins) && next == va + ins.length) fn.suspicious = true;
            insns[va] = {ins.length, ins.mnemonic};
            fn.insn_count++;
            fn.end = std::max(fn.end, va + ins.length);

            // code addresses used as values: callbacks, thread routines, vtables built in code
            for (uint8_t i = 0; i < ins.operand_count_visible; i++) {
                const auto& op = ops[i];
                if (op.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && !op.imm.is_relative &&
                    ins.meta.category != ZYDIS_CATEGORY_COND_BR) {
                    uint32_t v = uint32_t(op.imm.value.u);
                    if (LooksLikeFunctionStart(v)) {
                        Queue(v, true);
                        fn.pointers.insert(v);
                    }
                }
            }

            const auto& op0 = ops[0];
            bool rel = op0.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && op0.imm.is_relative;
            uint32_t target = rel ? uint32_t(next + int32_t(op0.imm.value.s)) : 0;
            bool absolute_mem = op0.type == ZYDIS_OPERAND_TYPE_MEMORY && op0.mem.base == ZYDIS_REGISTER_NONE &&
                                op0.mem.segment == ZYDIS_REGISTER_DS;

            switch (ins.meta.category) {
            case ZYDIS_CATEGORY_RET:
                goto block_end;
            case ZYDIS_CATEGORY_CALL:
                if (rel) {
                    Queue(target, false);
                    fn.calls.insert(target);
                } else if (absolute_mem && op0.mem.index == ZYDIS_REGISTER_NONE) {
                    uint32_t slot = uint32_t(op0.mem.disp.value);
                    if (slot >= image_.kernel_thunk && slot < image_.kernel_thunk + 4 * image_.kernel_imports.size())
                        kernel_calls.insert(va);
                }
                break;
            case ZYDIS_CATEGORY_COND_BR:
                if (rel) {
                    fn.blocks.insert(target);
                    work.push_back(target);
                }
                break;
            case ZYDIS_CATEGORY_UNCOND_BR:
                if (rel) {
                    if (functions.count(target) && target != fn.start) {  // tail call
                        fn.calls.insert(target);
                        goto block_end;
                    }
                    fn.blocks.insert(target);
                    work.push_back(target);
                } else if (absolute_mem && op0.mem.index != ZYDIS_REGISTER_NONE && op0.mem.scale == 4) {
                    uint32_t table = uint32_t(op0.mem.disp.value);
                    auto targets = ReadJumpTable(table, va);
                    for (uint32_t t : targets) {
                        fn.blocks.insert(t);
                        work.push_back(t);
                    }
                    jump_tables[table] = std::move(targets);
                }
                goto block_end;
            default:
                if (ins.mnemonic == ZYDIS_MNEMONIC_INT3 || ins.mnemonic == ZYDIS_MNEMONIC_HLT ||
                    ins.mnemonic == ZYDIS_MNEMONIC_UD2)
                    goto block_end;
                break;
            }
            va = next;
            if (functions.count(va)) {  // fell into the next function
                fn.calls.insert(va);
                break;
            }
        }
    block_end:;
    }
}

void Analysis::ScanDataPointers() {
    for (const auto& s : image_.sections) {
        bool is_code = std::find(code_.begin(), code_.end(), &s) != code_.end();
        if (is_code || s.name.rfind("$$", 0) == 0) continue;
        for (uint32_t at = s.va; at + 4 <= s.va + s.rsize; at += 4) {
            uint32_t v = image_.Read32(at);
            if (LooksLikeFunctionStart(v)) {
                Queue(v, true);
                data_pointer_roots_.insert(v);
            }
        }
    }
}

// Keeps what is reachable from the entry point: direct calls are always followed, code pointers
// (in data or immediates) only into functions that decode as plausible code.
void Analysis::DropDataAsCode() {
    std::set<uint32_t> keep;
    std::vector<uint32_t> work{image_.entry};
    // named XDK functions are code whoever reaches them (also through pointers found before the data scan)
    for (uint32_t r : extra_entries_) {
        auto it = functions.find(r);
        if (it != functions.end() && !it->second.suspicious) work.push_back(r);
    }
    for (uint32_t r : data_pointer_roots_) {
        auto it = functions.find(r);
        if (it != functions.end() && !it->second.suspicious) work.push_back(r);
    }
    while (!work.empty()) {
        uint32_t va = work.back();
        work.pop_back();
        auto it = functions.find(va);
        if (it == functions.end() || !keep.insert(va).second) continue;
        for (uint32_t t : it->second.calls) work.push_back(t);
        for (uint32_t t : it->second.pointers) {
            auto p = functions.find(t);
            if (p != functions.end() && !p->second.suspicious) work.push_back(t);
        }
    }
    dropped = 0;
    for (auto it = functions.begin(); it != functions.end();) {
        if (keep.count(it->first)) {
            ++it;
        } else {
            it = functions.erase(it);
            dropped++;
        }
    }
    // only instructions of kept functions count as code
    std::map<uint32_t, Insn> kept;
    ZydisDecodedInstruction ins;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    for (auto& [start, fn] : functions) {
        std::vector<uint32_t> blocks(fn.blocks.begin(), fn.blocks.end());
        for (uint32_t va : blocks) {
            while (insns.count(va) && !kept.count(va)) {
                kept[va] = insns[va];
                Decode(va, &ins, ops);
                auto cat = ins.meta.category;
                if (cat == ZYDIS_CATEGORY_RET || cat == ZYDIS_CATEGORY_UNCOND_BR) break;
                va = NextAddress(va, ins, ops);
            }
        }
    }
    insns.swap(kept);
}

bool Analysis::InsideInstruction(uint32_t va) const {
    auto it = insns.upper_bound(va);
    if (it == insns.begin()) return false;
    --it;
    return va < it->first + it->second.length;
}

// Code pointers in data that lead into bytes nothing reached yet: functions without the alignment or
// padding the first scan asks for, such as static initializers in the CRT's _initterm tables. The XDK's
// writable code sections (XPP, D3D, DSOUND) keep their driver tables between the code, so code sections
// are scanned too, wherever no decoded instruction covers the bytes.
size_t Analysis::ScanUnreachedPointers() {
    size_t added = 0;
    ZydisDecodedInstruction ins;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    for (const auto& s : image_.sections) {
        bool is_code = std::find(code_.begin(), code_.end(), &s) != code_.end();
        if (s.name.rfind("$$", 0) == 0) continue;
        for (uint32_t at = s.va; at + 4 <= s.va + s.rsize; at += 4) {
            if (is_code && (InsideInstruction(at) || InsideInstruction(at + 3))) continue;
            uint32_t v = image_.Read32(at);
            // a pointer to an instruction start that no function begins at still counts (SEH handlers,
            // or real functions that a misdecoded neighbour swallowed); one into the middle of one does not
            if (!IsCode(v) || functions.count(v) || image_.At(v)[0] == 0) continue;
            if (InsideInstruction(v) && !insns.count(v)) continue;
            bool decodes = true;
            uint32_t p = v;
            for (int i = 0; i < 3 && decodes; i++) {
                decodes = Decode(p, &ins, ops) && ins.mnemonic != ZYDIS_MNEMONIC_INT3;
                p += ins.length;
            }
            if (!decodes) continue;
            Queue(v, true);
            data_pointer_roots_.insert(v);
            added++;
        }
    }

    // code addresses in immediates (push offset ctor for the vector constructor iterator, callbacks)
    std::vector<uint32_t> candidates;
    for (const auto& [va, in] : insns) {
        if (!Decode(va, &ins, ops)) continue;
        for (uint8_t i = 0; i < ins.operand_count_visible; i++) {
            if (ops[i].type != ZYDIS_OPERAND_TYPE_IMMEDIATE || ops[i].imm.is_relative) continue;
            uint32_t v = uint32_t(ops[i].imm.value.u);
            if (IsCode(v) && !functions.count(v) && image_.At(v)[0] != 0 && (!InsideInstruction(v) || insns.count(v)))
                candidates.push_back(v);
        }
    }
    for (uint32_t v : candidates) {
        if (functions.count(v)) continue;
        bool decodes = true;
        uint32_t p = v;
        for (int i = 0; i < 3 && decodes; i++) {
            decodes = Decode(p, &ins, ops) && ins.mnemonic != ZYDIS_MNEMONIC_INT3;
            if (decodes && (ins.meta.category == ZYDIS_CATEGORY_RET || ins.meta.category == ZYDIS_CATEGORY_UNCOND_BR)) break;
            p += ins.length;
        }
        if (!decodes) continue;
        Queue(v, true);
        data_pointer_roots_.insert(v);
        added++;
    }
    return added;
}

void Analysis::Drain() {
    while (!queue_.empty()) {
        auto [va, from_pointer] = queue_.back();
        queue_.pop_back();
        if (functions.count(va)) continue;
        auto& fn = functions[va];
        fn.start = va;
        fn.from_pointer = from_pointer;
        Explore(fn);
    }
}

void Analysis::Run() {
    Queue(image_.entry, false);
    ScanDataPointers();
    Drain();
    while (ScanUnreachedPointers()) Drain();
    // named XDK functions only add to what the analysis finds on its own
    for (uint32_t va : extra_entries_) Queue(va, false);
    Drain();
    while (ScanUnreachedPointers()) Drain();
    DropDataAsCode();
}

void Analysis::WriteShapes(const std::string& path) const {
    ZydisFormatter fmt;
    ZydisFormatterInit(&fmt, ZYDIS_FORMATTER_STYLE_INTEL);
    std::map<std::string, std::pair<size_t, std::string>> shapes;
    ZydisDecodedInstruction ins;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    for (const auto& [va, in] : insns) {
        Decode(va, &ins, ops);
        std::string key = ZydisMnemonicGetString(ins.mnemonic);
        for (uint8_t i = 0; i < ins.operand_count; i++) {
            const auto& op = ops[i];
            key += i < ins.operand_count_visible ? " " : " (";
            switch (op.type) {
            case ZYDIS_OPERAND_TYPE_REGISTER: key += ZydisRegisterGetString(op.reg.value); break;
            case ZYDIS_OPERAND_TYPE_MEMORY: key += "m" + std::to_string(op.size); break;
            case ZYDIS_OPERAND_TYPE_IMMEDIATE: key += "i" + std::to_string(op.size); break;
            default: key += "?"; break;
            }
            if (i >= ins.operand_count_visible) key += ")";
        }
        auto& s = shapes[key];
        if (s.first++ == 0) {
            char text[96];
            ZydisFormatterFormatInstruction(&fmt, &ins, ops, ins.operand_count_visible, text, sizeof text, va, nullptr);
            char buf[128];
            std::snprintf(buf, sizeof buf, "%08X %s", va, text);
            s.second = buf;
        }
    }
    std::ofstream out(path);
    for (const auto& [key, s] : shapes) out << key << "  [" << s.first << "]  " << s.second << "\n";
}

void Analysis::WriteReport(const std::string& path) const {
    std::ofstream out(path);
    char line[256];

    uint64_t code_bytes = 0, covered = 0;
    for (const auto* s : code_) code_bytes += s->rsize;
    for (const auto& [va, in] : insns) covered += in.length;

    // uncovered spans that are not padding are code the analysis has not reached yet
    struct Gap { uint32_t va, size; };
    std::vector<Gap> gaps;
    for (const auto* s : code_) {
        uint32_t at = s->va, end = s->va + s->rsize;
        while (at < end) {
            auto it = insns.find(at);
            if (it != insns.end()) {
                at += it->second.length;
                continue;
            }
            if (IsPadding(image_.At(at)[0]) || image_.At(at)[0] == 0) {
                at++;
                continue;
            }
            uint32_t start = at;
            while (at < end && !insns.count(at)) at++;
            gaps.push_back({start, at - start});
        }
    }
    uint64_t gap_bytes = 0;
    for (const auto& g : gaps) gap_bytes += g.size;

    std::snprintf(line, sizeof line, "functions        %zu (%zu only through pointers)\n", functions.size(),
                  size_t(std::count_if(functions.begin(), functions.end(), [](auto& f) { return f.second.from_pointer; })));
    out << line;
    std::snprintf(line, sizeof line, "dropped          %zu (data decoded as code)\n", dropped);
    out << line;
    std::snprintf(line, sizeof line, "instructions     %zu\n", insns.size());
    out << line;
    std::snprintf(line, sizeof line, "code coverage    %.2f%% of %llu bytes\n", 100.0 * covered / code_bytes,
                  (unsigned long long)code_bytes);
    out << line;
    std::snprintf(line, sizeof line, "unreached spans  %zu, %llu bytes\n", gaps.size(), (unsigned long long)gap_bytes);
    out << line;
    std::snprintf(line, sizeof line, "jump tables      %zu\n", jump_tables.size());
    out << line;
    std::snprintf(line, sizeof line, "kernel calls     %zu\n", kernel_calls.size());
    out << line;
    std::snprintf(line, sizeof line, "bad decodes      %zu\n\n", bad_decodes.size());
    out << line;

    std::unordered_map<ZydisMnemonic, size_t> hist;
    for (const auto& [va, in] : insns) hist[in.mnemonic]++;
    std::vector<std::pair<ZydisMnemonic, size_t>> sorted(hist.begin(), hist.end());
    std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.second > b.second; });
    out << "mnemonics (" << sorted.size() << ")\n";
    for (const auto& [m, n] : sorted) {
        std::snprintf(line, sizeof line, "  %-14s %zu\n", ZydisMnemonicGetString(m), n);
        out << line;
    }

    out << "\nbad decodes\n";
    for (uint32_t va : bad_decodes) {
        std::snprintf(line, sizeof line, "  %08X\n", va);
        out << line;
    }

    std::sort(gaps.begin(), gaps.end(), [](auto& a, auto& b) { return a.size > b.size; });
    out << "\nlargest unreached spans\n";
    for (size_t i = 0; i < std::min<size_t>(gaps.size(), 200); i++) {
        std::snprintf(line, sizeof line, "  %08X %6u %s\n", gaps[i].va, gaps[i].size, image_.SectionAt(gaps[i].va)->name.c_str());
        out << line;
    }

    out << "\nfunctions\n";
    for (const auto& [va, fn] : functions) {
        std::snprintf(line, sizeof line, "  %08X %6u bytes %5u insns %4zu blocks%s\n", va, fn.end - fn.start, fn.insn_count,
                      fn.blocks.size(), fn.from_pointer ? " ptr" : "");
        out << line;
    }
}

}  // namespace recomp
