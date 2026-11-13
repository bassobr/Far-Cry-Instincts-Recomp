// Instruction semantics: one C++ statement block per x86 instruction.
#include <cstring>

#include "codegen_internal.h"

namespace recomp {

namespace {

const char* Type(int bits) {
    switch (bits) {
    case 8: return "uint8_t";
    case 16: return "uint16_t";
    case 64: return "uint64_t";
    default: return "uint32_t";
    }
}

const char* SType(int bits) {
    switch (bits) {
    case 8: return "int8_t";
    case 16: return "int16_t";
    case 64: return "int64_t";
    default: return "int32_t";
    }
}

struct Gpr {
    const char* name;
    int bits;
    int shift;
};

bool GetGpr(ZydisRegister r, Gpr* g) {
    static const char* const kNames[] = {"c.eax", "c.ecx", "c.edx", "c.ebx", "c.esp", "c.ebp", "c.esi", "c.edi"};
    if (r >= ZYDIS_REGISTER_EAX && r <= ZYDIS_REGISTER_EDI) *g = {kNames[r - ZYDIS_REGISTER_EAX], 32, 0};
    else if (r >= ZYDIS_REGISTER_AX && r <= ZYDIS_REGISTER_DI) *g = {kNames[r - ZYDIS_REGISTER_AX], 16, 0};
    else if (r >= ZYDIS_REGISTER_AL && r <= ZYDIS_REGISTER_BL) *g = {kNames[r - ZYDIS_REGISTER_AL], 8, 0};
    else if (r >= ZYDIS_REGISTER_AH && r <= ZYDIS_REGISTER_BH) *g = {kNames[r - ZYDIS_REGISTER_AH], 8, 8};
    else return false;
    return true;
}

bool IsSeg(const ZydisDecodedOperand& op) {
    return op.type == ZYDIS_OPERAND_TYPE_REGISTER && op.reg.value >= ZYDIS_REGISTER_ES && op.reg.value <= ZYDIS_REGISTER_GS;
}

bool IsReg(const ZydisDecodedOperand& op, ZydisRegister first, ZydisRegister last) {
    return op.type == ZYDIS_OPERAND_TYPE_REGISTER && op.reg.value >= first && op.reg.value <= last;
}
bool IsXmm(const ZydisDecodedOperand& op) { return IsReg(op, ZYDIS_REGISTER_XMM0, ZYDIS_REGISTER_XMM7); }
bool IsMm(const ZydisDecodedOperand& op) { return IsReg(op, ZYDIS_REGISTER_MM0, ZYDIS_REGISTER_MM7); }
bool IsSt(const ZydisDecodedOperand& op) { return IsReg(op, ZYDIS_REGISTER_ST0, ZYDIS_REGISTER_ST7); }
bool IsMem(const ZydisDecodedOperand& op) { return op.type == ZYDIS_OPERAND_TYPE_MEMORY; }

}  // namespace

const char* Cond(ZydisMnemonic m) {
    const char* name = ZydisMnemonicGetString(m);
    const char* suffix = nullptr;
    for (const char* prefix : {"fcmov", "cmov", "set", "j"}) {
        size_t n = std::strlen(prefix);
        if (std::strncmp(name, prefix, n) == 0) {
            suffix = name + n;
            break;
        }
    }
    if (!suffix) return nullptr;
    struct Entry {
        const char* suffix;
        const char* cond;
    };
    static const Entry kConds[] = {
        {"o", "f.of"},   {"no", "!f.of"},  {"b", "f.cf"},   {"nb", "!f.cf"},  {"z", "f.zf"},
        {"nz", "!f.zf"}, {"e", "f.zf"},    {"ne", "!f.zf"}, {"be", "(f.cf || f.zf)"},
        {"nbe", "(!f.cf && !f.zf)"},       {"s", "f.sf"},   {"ns", "!f.sf"},  {"p", "f.pf"},
        {"np", "!f.pf"}, {"u", "f.pf"},    {"nu", "!f.pf"}, {"l", "(f.sf != f.of)"},
        {"nl", "(f.sf == f.of)"},          {"le", "(f.zf || f.sf != f.of)"},
        {"nle", "(!f.zf && f.sf == f.of)"},
    };
    for (const auto& e : kConds)
        if (std::strcmp(e.suffix, suffix) == 0) return e.cond;
    return nullptr;
}

// ---------------------------------------------------------------------------------------------
// operand access

std::string FunctionWriter::Ea(const ZydisDecodedOperand& op) const {
    std::string e;
    auto add = [&](const std::string& part) { e += e.empty() ? part : " + " + part; };
    if (op.mem.segment == ZYDIS_REGISTER_FS) add("c.fs_base");
    Gpr g;
    if (op.mem.base != ZYDIS_REGISTER_NONE && GetGpr(op.mem.base, &g)) add(g.name);
    if (op.mem.index != ZYDIS_REGISTER_NONE && GetGpr(op.mem.index, &g))
        add(op.mem.scale > 1 ? F("%s * %u", g.name, op.mem.scale) : std::string(g.name));
    if (op.mem.disp.value || e.empty()) add(F("0x%Xu", uint32_t(op.mem.disp.value)));
    return "uint32_t(" + e + ")";
}

std::string FunctionWriter::Rd(int i, int bits) const {
    const auto& op = cur_->ops[i];
    switch (op.type) {
    case ZYDIS_OPERAND_TYPE_REGISTER: {
        Gpr g;
        if (!GetGpr(op.reg.value, &g)) return "0u";
        if (g.bits == 32) return g.name;
        if (g.bits == 16) return F("uint16_t(%s)", g.name);
        return g.shift ? F("uint8_t(%s >> 8)", g.name) : F("uint8_t(%s)", g.name);
    }
    case ZYDIS_OPERAND_TYPE_MEMORY:
        return F("R%d(ea)", op.size);
    case ZYDIS_OPERAND_TYPE_IMMEDIATE: {
        int b = bits ? bits : cur_->ops[0].size;
        if (b != 8 && b != 16 && b != 32) b = 32;
        uint64_t mask = b == 32 ? 0xFFFFFFFFull : (1ull << b) - 1;
        return F("%s(0x%llXu)", Type(b), (unsigned long long)(uint64_t(op.imm.value.s) & mask));
    }
    default:
        return "0u";
    }
}

std::string FunctionWriter::Wr(int i, const std::string& v) const {
    const auto& op = cur_->ops[i];
    if (op.type == ZYDIS_OPERAND_TYPE_MEMORY) return F("W%d(ea, %s(%s));", op.size, Type(op.size), v.c_str());
    Gpr g;
    if (!GetGpr(op.reg.value, &g)) return "";
    if (g.bits == 32) return F("%s = uint32_t(%s);", g.name, v.c_str());
    if (g.bits == 16) return F("%s = (%s & 0xFFFF0000u) | uint16_t(%s);", g.name, g.name, v.c_str());
    if (g.shift) return F("%s = (%s & 0xFFFF00FFu) | (uint32_t(uint8_t(%s)) << 8);", g.name, g.name, v.c_str());
    return F("%s = (%s & 0xFFFFFF00u) | uint8_t(%s);", g.name, g.name, v.c_str());
}

std::string FunctionWriter::Xf(int i) const {
    return IsXmm(cur_->ops[i]) ? F("c.xmm[%d].f[0]", Xmm(i)) : std::string("RF32(ea)");
}
std::string FunctionWriter::Xv(int i) const {
    return IsXmm(cur_->ops[i]) ? F("c.xmm[%d]", Xmm(i)) : std::string("LoadX(ea)");
}
std::string FunctionWriter::Mv(int i) const {
    const auto& op = cur_->ops[i];
    if (IsMm(op)) return F("c.mm[%d]", Mm(i));
    if (IsMem(op)) return op.size == 64 ? "R64(ea)" : "uint64_t(R32(ea))";
    if (op.type == ZYDIS_OPERAND_TYPE_IMMEDIATE) return F("uint64_t(%lluu)", (unsigned long long)op.imm.value.u);
    return F("uint64_t(%s)", Rd(i).c_str());
}
std::string FunctionWriter::Fv(int i) const {
    const auto& op = cur_->ops[i];
    if (IsSt(op)) return F("ST(c, %d)", St(i));
    if (op.size == 32) return "double(RF32(ea))";
    if (op.size == 64) return "RF64(ea)";
    return "LoadF80(ea)";
}
std::string FunctionWriter::Iv(int i) const {
    const auto& op = cur_->ops[i];
    if (op.size == 16) return "double(int16_t(R16(ea)))";
    if (op.size == 64) return "double(int64_t(R64(ea)))";
    return "double(int32_t(R32(ea)))";
}

// ---------------------------------------------------------------------------------------------
// control flow

bool FunctionWriter::EmitBranch(const Decoded& d, bool* handled) {
    const auto& ins = d.ins;
    const auto& op0 = d.ops[0];
    uint32_t next = d.va + ins.length;
    bool rel = op0.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && op0.imm.is_relative;
    uint32_t target = rel ? uint32_t(next + int32_t(op0.imm.value.s)) : 0;
    *handled = true;

    switch (ins.mnemonic) {
    case ZYDIS_MNEMONIC_CALL: {
        if (rel) {
            S(CallTo(target, next));
        } else if (int ord = KernelOrdinal(op0); ord >= 0) {
            S(F("c.esp -= 4; W32(c.esp, 0x%08Xu); FCI_SAVE KernelCall(c, %d); FCI_LOAD", next, ord));
        } else if (op0.size != 32) {
            return false;
        } else {
            S(F("const uint32_t t = %s; c.esp -= 4; W32(c.esp, 0x%08Xu); FCI_SAVE CallIndirect(c, t); FCI_LOAD", Rd(0).c_str(), next));
        }
        return true;
    }
    case ZYDIS_MNEMONIC_RET: {
        uint32_t pop = ins.operand_count_visible ? uint32_t(op0.imm.value.u) : 0;
        // a caller reads the flags this function returns with (AnalyzeFlags)
        S(F("RETURN(0x%08Xu); c.esp += %uu; FCI_SAVE%s return;", fn_.start, 4 + pop,
            cg_.ReturnsFlags(fn_.start) ? " c.flags = f;" : ""));
        return true;
    }
    case ZYDIS_MNEMONIC_JMP: {
        if (rel) {
            S(Branch(d.va, target));
        } else if (int ord = KernelOrdinal(op0); ord >= 0) {
            S(F("FCI_SAVE KernelCall(c, %d); return;", ord));
        } else if (op0.size != 32 || (op0.type != ZYDIS_OPERAND_TYPE_MEMORY && op0.type != ZYDIS_OPERAND_TYPE_REGISTER)) {
            return false;
        } else if (op0.type == ZYDIS_OPERAND_TYPE_MEMORY && op0.mem.base == ZYDIS_REGISTER_NONE &&
                   op0.mem.index != ZYDIS_REGISTER_NONE && op0.mem.scale == 4) {
            S("const uint32_t t = R32(ea); switch (t) {");
            std::set<uint32_t> done;
            auto it = cg_.analysis().jump_tables.find(uint32_t(op0.mem.disp.value));
            if (it != cg_.analysis().jump_tables.end())
                for (uint32_t t : it->second)
                    if (done.insert(t).second) S(F("case 0x%08Xu: %s", t, Jump(t).c_str()));
            S("default: FCI_SAVE c.flags = f; Call(c, t); return; }");
        } else {
            S(F("const uint32_t t = %s; FCI_SAVE c.flags = f; CallIndirect(c, t); return;", Rd(0).c_str()));
        }
        return true;
    }
    case ZYDIS_MNEMONIC_LOOP:
        S(F("c.ecx--; if (c.ecx != 0) %s", Branch(d.va, target).c_str()));
        return true;
    case ZYDIS_MNEMONIC_LOOPE:
        S(F("c.ecx--; if (c.ecx != 0 && f.zf) %s", Branch(d.va, target).c_str()));
        return true;
    case ZYDIS_MNEMONIC_LOOPNE:
        S(F("c.ecx--; if (c.ecx != 0 && !f.zf) %s", Branch(d.va, target).c_str()));
        return true;
    case ZYDIS_MNEMONIC_JECXZ:
        S(F("if (c.ecx == 0) %s", Branch(d.va, target).c_str()));
        return true;
    default:
        break;
    }
    if (ins.meta.category == ZYDIS_CATEGORY_COND_BR) {
        const char* cond = Cond(ins.mnemonic);
        if (!cond || !rel) return false;
        S(F("if (%s) %s", cond, Branch(d.va, target).c_str()));
        return true;
    }
    *handled = false;
    return false;
}

// ---------------------------------------------------------------------------------------------
// integer

bool FunctionWriter::EmitInsn(const Decoded& d) {
    cur_ = &d;
    const auto& ins = d.ins;
    const auto* o = d.ops;
    uint32_t defined = ins.cpu_flags ? (ins.cpu_flags->modified | ins.cpu_flags->set_0 | ins.cpu_flags->set_1 |
                                        ins.cpu_flags->undefined) & kStatusFlags
                                     : 0;
    need_ = d.live_out & defined;

    // the explicit memory operand's address is taken once, before any register changes
    ea_ram_ = ea_base_ = false;
    for (int i = 0; i < ins.operand_count_visible; i++) {
        if (IsMem(o[i]) && o[i].mem.type == ZYDIS_MEMOP_TYPE_MEM && ins.mnemonic != ZYDIS_MNEMONIC_POP) {
            S("const uint32_t ea = " + Ea(o[i]) + ";");
            // RAM for sure: the stack (esp based, small displacement) and constant addresses below the hardware
            const auto& m = o[i].mem;
            const int64_t disp = m.disp.value;
            if (m.segment != ZYDIS_REGISTER_FS) {
                if (m.base == ZYDIS_REGISTER_ESP && disp >= -0x10000 && disp < 0x100000) ea_ram_ = true;
                // the same through ebp where it is a frame pointer (FramePointer): the locals of code with frames
                if (m.base == ZYDIS_REGISTER_EBP && m.index == ZYDIS_REGISTER_NONE && d.ebp_frame && disp >= -0x10000 &&
                    disp < 0x100000)
                    ea_ram_ = true;
                if (m.base == ZYDIS_REGISTER_NONE && m.index == ZYDIS_REGISTER_NONE && disp >= 0 &&
                    disp + 16 <= int64_t(0xFD000000))
                    ea_ram_ = true;
                // [base + small positive displacement]: the check looks at the base register (BaseR32), the same
                // for every access through it, so the compiler keeps one compare for a run of them
                Gpr g;
                if (BaseCheckOn() && !ea_ram_ && m.base != ZYDIS_REGISTER_NONE && m.index == ZYDIS_REGISTER_NONE && disp >= 0 &&
                    disp < int64_t(kBaseDispLimit) && GetGpr(m.base, &g) && g.bits == 32) {
                    ea_base_ = true;
                    S(std::string("const uint32_t eb = ") + g.name + ";");
                }
            }
            break;
        }
    }

    bool handled;
    bool ok = EmitBranch(d, &handled);
    if (handled) return ok;
    ok = EmitSse(d, &handled);
    if (handled) return ok;
    ok = EmitMmx(d, &handled);
    if (handled) return ok;
    ok = EmitX87(d, &handled);
    if (handled) return ok;

    const int n = ins.operand_count_visible;
    const int bits = n ? o[0].size : 0;
    const char* T = Type(bits);
    const std::string m = F("0x%Xu", need_);
    const bool lock = ins.attributes & ZYDIS_ATTRIB_HAS_LOCK;
    const bool mem0 = n && IsMem(o[0]);
    const auto mn = ins.mnemonic;

    auto write_result = [&](const std::string& expr) {
        S(F("const %s r = %s;", T, expr.c_str()));
        S(Wr(0, "r"));
    };

    switch (mn) {
    case ZYDIS_MNEMONIC_MOV:
        if (IsSeg(o[0])) return true;  // segment registers are flat
        S(Wr(0, IsSeg(o[1]) ? "0u" : Rd(1, bits)));
        return true;
    case ZYDIS_MNEMONIC_MOVZX:
        S(Wr(0, Rd(1)));
        return true;
    case ZYDIS_MNEMONIC_MOVSX:
        S(Wr(0, F("%s(%s(%s))", T, SType(o[1].size), Rd(1).c_str())));
        return true;
    case ZYDIS_MNEMONIC_LEA:
        S(Wr(0, Ea(o[1])));
        return true;
    case ZYDIS_MNEMONIC_XCHG:
        if (IsMem(o[0]) || IsMem(o[1])) {
            int r = IsMem(o[0]) ? 1 : 0;
            S(F("const %s v = __atomic_exchange_n(reinterpret_cast<%s*>(Host(ea)), %s, __ATOMIC_SEQ_CST);", T, T,
                Rd(r).c_str()));
            S(Wr(r, "v"));
        } else {
            S(F("const %s a = %s, b = %s;", T, Rd(0).c_str(), Rd(1).c_str()));
            S(Wr(0, "b"));
            S(Wr(1, "a"));
        }
        return true;
    case ZYDIS_MNEMONIC_BSWAP:
        S(Wr(0, F("__builtin_bswap32(%s)", Rd(0).c_str())));
        return true;
    case ZYDIS_MNEMONIC_PUSH: {
        std::string v = IsSeg(o[0]) ? "0u" : o[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE ? Rd(0, 32) : "uint32_t(" + Rd(0) + ")";
        S("const uint32_t v = " + v + "; c.esp -= 4; W32(c.esp, v);");
        return true;
    }
    case ZYDIS_MNEMONIC_POP:
        if (IsSeg(o[0])) {
            S("c.esp += 4;");
            return true;
        }
        S("const uint32_t v = R32(c.esp); c.esp += 4;");
        if (mem0) S("const uint32_t ea = " + Ea(o[0]) + ";");  // pop [esp+x] addresses with the new esp
        S(Wr(0, "v"));
        return true;
    case ZYDIS_MNEMONIC_PUSHAD:
        S("const uint32_t t = c.esp; c.esp -= 32; W32(c.esp + 28, c.eax); W32(c.esp + 24, c.ecx); "
          "W32(c.esp + 20, c.edx); W32(c.esp + 16, c.ebx); W32(c.esp + 12, t); W32(c.esp + 8, c.ebp); "
          "W32(c.esp + 4, c.esi); W32(c.esp, c.edi);");
        return true;
    case ZYDIS_MNEMONIC_POPAD:
        S("c.edi = R32(c.esp); c.esi = R32(c.esp + 4); c.ebp = R32(c.esp + 8); c.ebx = R32(c.esp + 16); "
          "c.edx = R32(c.esp + 20); c.ecx = R32(c.esp + 24); c.eax = R32(c.esp + 28); c.esp += 32;");
        return true;
    case ZYDIS_MNEMONIC_PUSHFD:
        S("c.esp -= 4; W32(c.esp, PackFlags(c, f));");
        return true;
    case ZYDIS_MNEMONIC_POPFD:
        S("UnpackFlags(c, f, R32(c.esp), false); c.esp += 4;");
        return true;
    case ZYDIS_MNEMONIC_LAHF:
        S("c.eax = (c.eax & 0xFFFF00FFu) | ((PackFlags(c, f) & 0xFFu) << 8);");
        return true;
    case ZYDIS_MNEMONIC_SAHF:
        S("UnpackFlags(c, f, (c.eax >> 8) & 0xFFu, true);");
        return true;
    case ZYDIS_MNEMONIC_CDQ:
        S("c.edx = uint32_t(int32_t(c.eax) >> 31);");
        return true;
    case ZYDIS_MNEMONIC_CWDE:
        S("c.eax = uint32_t(int32_t(int16_t(c.eax)));");
        return true;
    case ZYDIS_MNEMONIC_LEAVE:
        S("c.esp = c.ebp; c.ebp = R32(c.esp); c.esp += 4;");
        return true;
    case ZYDIS_MNEMONIC_ENTER:
        if (o[1].imm.value.u != 0) return false;
        S(F("c.esp -= 4; W32(c.esp, c.ebp); c.ebp = c.esp; c.esp -= %uu;", uint32_t(o[0].imm.value.u)));
        return true;

    case ZYDIS_MNEMONIC_ADD:
    case ZYDIS_MNEMONIC_ADC:
    case ZYDIS_MNEMONIC_SUB:
    case ZYDIS_MNEMONIC_SBB:
    case ZYDIS_MNEMONIC_CMP:
    case ZYDIS_MNEMONIC_AND:
    case ZYDIS_MNEMONIC_OR:
    case ZYDIS_MNEMONIC_XOR:
    case ZYDIS_MNEMONIC_TEST: {
        std::string a = Rd(0), b = Rd(1, bits);
        if (lock && mem0 && mn != ZYDIS_MNEMONIC_ADC && mn != ZYDIS_MNEMONIC_SBB && mn != ZYDIS_MNEMONIC_CMP &&
            mn != ZYDIS_MNEMONIC_TEST) {
            const char* fetch = mn == ZYDIS_MNEMONIC_ADD ? "add" : mn == ZYDIS_MNEMONIC_SUB ? "sub"
                              : mn == ZYDIS_MNEMONIC_AND ? "and" : mn == ZYDIS_MNEMONIC_OR ? "or" : "xor";
            const char* op = mn == ZYDIS_MNEMONIC_AND ? "&" : mn == ZYDIS_MNEMONIC_OR ? "|" : "^";
            S(F("const %s b = %s; const %s old = __atomic_fetch_%s(reinterpret_cast<%s*>(Host(ea)), b, __ATOMIC_SEQ_CST);",
                T, b.c_str(), T, fetch, T));
            if (mn == ZYDIS_MNEMONIC_ADD) S(F("Add<%s>(f, %s, old, b);", T, m.c_str()));
            else if (mn == ZYDIS_MNEMONIC_SUB) S(F("Sub<%s>(f, %s, old, b);", T, m.c_str()));
            else S(F("Logic<%s>(f, %s, %s(old %s b));", T, m.c_str(), T, op));
            return true;
        }
        switch (mn) {
        case ZYDIS_MNEMONIC_ADD: write_result(F("Add<%s>(f, %s, %s, %s)", T, m.c_str(), a.c_str(), b.c_str())); break;
        case ZYDIS_MNEMONIC_ADC: write_result(F("Add<%s>(f, %s, %s, %s, f.cf)", T, m.c_str(), a.c_str(), b.c_str())); break;
        case ZYDIS_MNEMONIC_SUB: write_result(F("Sub<%s>(f, %s, %s, %s)", T, m.c_str(), a.c_str(), b.c_str())); break;
        case ZYDIS_MNEMONIC_SBB: write_result(F("Sub<%s>(f, %s, %s, %s, f.cf)", T, m.c_str(), a.c_str(), b.c_str())); break;
        case ZYDIS_MNEMONIC_CMP: S(F("Sub<%s>(f, %s, %s, %s);", T, m.c_str(), a.c_str(), b.c_str())); break;
        case ZYDIS_MNEMONIC_AND: write_result(F("Logic<%s>(f, %s, %s(%s & %s))", T, m.c_str(), T, a.c_str(), b.c_str())); break;
        case ZYDIS_MNEMONIC_OR: write_result(F("Logic<%s>(f, %s, %s(%s | %s))", T, m.c_str(), T, a.c_str(), b.c_str())); break;
        case ZYDIS_MNEMONIC_XOR: write_result(F("Logic<%s>(f, %s, %s(%s ^ %s))", T, m.c_str(), T, a.c_str(), b.c_str())); break;
        default: S(F("Logic<%s>(f, %s, %s(%s & %s));", T, m.c_str(), T, a.c_str(), b.c_str())); break;
        }
        return true;
    }
    case ZYDIS_MNEMONIC_INC:
    case ZYDIS_MNEMONIC_DEC: {
        const bool inc = mn == ZYDIS_MNEMONIC_INC;
        const std::string mm = F("0x%Xu", need_ & ~uint32_t(ZYDIS_CPUFLAG_CF));
        if (lock && mem0) {
            S(F("const %s old = __atomic_fetch_%s(reinterpret_cast<%s*>(Host(ea)), %s(1), __ATOMIC_SEQ_CST);", T,
                inc ? "add" : "sub", T, T));
            S(F("%s<%s>(f, %s, old, %s(1));", inc ? "Add" : "Sub", T, mm.c_str(), T));
            return true;
        }
        write_result(F("%s<%s>(f, %s, %s, %s(1))", inc ? "Add" : "Sub", T, mm.c_str(), Rd(0).c_str(), T));
        return true;
    }
    case ZYDIS_MNEMONIC_NEG:
        write_result(F("Sub<%s>(f, %s, %s(0), %s)", T, m.c_str(), T, Rd(0).c_str()));
        return true;
    case ZYDIS_MNEMONIC_NOT:
        S(Wr(0, F("%s(~%s)", T, Rd(0).c_str())));
        return true;

    case ZYDIS_MNEMONIC_SHL:
    case ZYDIS_MNEMONIC_SHR:
    case ZYDIS_MNEMONIC_SAR:
    case ZYDIS_MNEMONIC_ROL:
    case ZYDIS_MNEMONIC_ROR:
    case ZYDIS_MNEMONIC_RCL:
    case ZYDIS_MNEMONIC_RCR: {
        const char* fn = mn == ZYDIS_MNEMONIC_SHL ? "Shl" : mn == ZYDIS_MNEMONIC_SHR ? "Shr" : mn == ZYDIS_MNEMONIC_SAR ? "Sar"
                       : mn == ZYDIS_MNEMONIC_ROL ? "Rol" : mn == ZYDIS_MNEMONIC_ROR ? "Ror"
                       : mn == ZYDIS_MNEMONIC_RCL ? "Rcl" : "Rcr";
        std::string count = n > 1 ? Rd(1, 8) : "uint8_t(1)";
        write_result(F("%s<%s>(f, %s, %s, %s)", fn, T, m.c_str(), Rd(0).c_str(), count.c_str()));
        return true;
    }
    case ZYDIS_MNEMONIC_SHLD:
    case ZYDIS_MNEMONIC_SHRD:
        if (bits != 32) return false;
        write_result(F("%s(f, %s, %s, %s, %s)", mn == ZYDIS_MNEMONIC_SHLD ? "Shld" : "Shrd", m.c_str(), Rd(0).c_str(),
                       Rd(1).c_str(), Rd(2, 8).c_str()));
        return true;

    case ZYDIS_MNEMONIC_IMUL:
        if (n == 1) {
            S(F("Imul%d(FCI_GPRS, f, %s, %s);", bits, m.c_str(), Rd(0).c_str()));
        } else if (n == 2) {
            write_result(F("Imul<%s>(f, %s, %s, %s)", T, m.c_str(), Rd(0).c_str(), Rd(1).c_str()));
        } else {
            write_result(F("Imul<%s>(f, %s, %s, %s)", T, m.c_str(), Rd(1).c_str(), Rd(2, bits).c_str()));
        }
        return true;
    case ZYDIS_MNEMONIC_MUL:
        if (bits != 32) return false;
        S(F("Mul32(FCI_GPRS, f, %s, %s);", m.c_str(), Rd(0).c_str()));
        return true;
    case ZYDIS_MNEMONIC_DIV:
    case ZYDIS_MNEMONIC_IDIV:
        if (bits != 32) return false;
        S(F("%s(c, FCI_GPRS, %s);", mn == ZYDIS_MNEMONIC_DIV ? "Div32" : "Idiv32", Rd(0).c_str()));
        return true;

    case ZYDIS_MNEMONIC_BT:
    case ZYDIS_MNEMONIC_BTS: {
        if (bits != 32) return false;
        const bool set = mn == ZYDIS_MNEMONIC_BTS;
        const bool imm = o[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE;
        if (mem0) {
            if (imm) S(F("const uint32_t a = ea; const uint32_t bit = %uu;", uint32_t(o[1].imm.value.u & 31)));
            else S(F("const int32_t off = int32_t(%s); const uint32_t a = ea + uint32_t(off >> 5) * 4; const uint32_t bit = uint32_t(off) & 31;",
                     Rd(1).c_str()));
            S("const uint32_t v = R32(a); f.cf = (v >> bit) & 1;");
            if (set) S("W32(a, v | (1u << bit));");
        } else {
            S(F("const uint32_t bit = uint32_t(%s) & 31; const uint32_t v = %s; f.cf = (v >> bit) & 1;", Rd(1, 8).c_str(),
                Rd(0).c_str()));
            if (set) S(Wr(0, "v | (1u << bit)"));
        }
        return true;
    }
    case ZYDIS_MNEMONIC_BSF:
        S(F("const uint32_t s = %s; f.zf = s == 0; if (s) { %s }", Rd(1).c_str(),
            Wr(0, "uint32_t(__builtin_ctz(s))").c_str()));
        return true;
    case ZYDIS_MNEMONIC_XADD:
        if (mem0) {
            S(F("const %s b = %s; const %s old = __atomic_fetch_add(reinterpret_cast<%s*>(Host(ea)), b, __ATOMIC_SEQ_CST);",
                T, Rd(1).c_str(), T, T));
            S(F("Add<%s>(f, %s, old, b);", T, m.c_str()));
            S(Wr(1, "old"));
        } else {
            S(F("const %s a = %s, b = %s;", T, Rd(0).c_str(), Rd(1).c_str()));
            S(Wr(1, "a"));
            S(Wr(0, F("Add<%s>(f, %s, a, b)", T, m.c_str())));
        }
        return true;
    case ZYDIS_MNEMONIC_CMPXCHG:
        if (bits != 32 || !mem0) return false;
        S(F("uint32_t cur = c.eax; const bool ok = __atomic_compare_exchange_n(reinterpret_cast<uint32_t*>(Host(ea)), "
            "&cur, %s, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); Sub<uint32_t>(f, %s, c.eax, cur); if (!ok) c.eax = cur;",
            Rd(1).c_str(), m.c_str()));
        return true;

    case ZYDIS_MNEMONIC_MOVSB:
    case ZYDIS_MNEMONIC_MOVSW:
    case ZYDIS_MNEMONIC_MOVSD:
    case ZYDIS_MNEMONIC_STOSB:
    case ZYDIS_MNEMONIC_STOSW:
    case ZYDIS_MNEMONIC_STOSD:
    case ZYDIS_MNEMONIC_LODSB:
    case ZYDIS_MNEMONIC_LODSW:
    case ZYDIS_MNEMONIC_LODSD:
    case ZYDIS_MNEMONIC_CMPSB:
    case ZYDIS_MNEMONIC_CMPSW:
    case ZYDIS_MNEMONIC_CMPSD:
    case ZYDIS_MNEMONIC_SCASB:
    case ZYDIS_MNEMONIC_SCASW:
    case ZYDIS_MNEMONIC_SCASD: {
        if (ins.meta.category != ZYDIS_CATEGORY_STRINGOP) return false;  // sse2 movsd/cmpsd
        const char* name = ZydisMnemonicGetString(mn);
        size_t len = std::strlen(name);
        const char* elem = name[len - 1] == 'b' ? "uint8_t" : name[len - 1] == 'w' ? "uint16_t" : "uint32_t";
        bool rep = ins.attributes & (ZYDIS_ATTRIB_HAS_REP | ZYDIS_ATTRIB_HAS_REPE | ZYDIS_ATTRIB_HAS_REPNE);
        int cmp_rep = (ins.attributes & ZYDIS_ATTRIB_HAS_REPNE) ? 2 : rep ? 1 : 0;
        std::string base(name, len - 1);
        if (base == "movs") S(F("Movs<%s>(FCI_GPRS, %s);", elem, rep ? "true" : "false"));
        else if (base == "stos") S(F("Stos<%s>(FCI_GPRS, %s);", elem, rep ? "true" : "false"));
        else if (base == "lods") S(F("Lods<%s>(FCI_GPRS);", elem));
        else if (base == "cmps") S(F("Cmps<%s>(FCI_GPRS, f, %d);", elem, cmp_rep));
        else S(F("Scas<%s>(FCI_GPRS, f, %d);", elem, cmp_rep));
        return true;
    }

    case ZYDIS_MNEMONIC_CLC: S("f.cf = false;"); return true;
    case ZYDIS_MNEMONIC_STC: S("f.cf = true;"); return true;
    case ZYDIS_MNEMONIC_CMC: S("f.cf = !f.cf;"); return true;
    case ZYDIS_MNEMONIC_CLD: S("c.df = 0;"); return true;
    case ZYDIS_MNEMONIC_STD: S("c.df = 1;"); return true;
    case ZYDIS_MNEMONIC_NOP:
    case ZYDIS_MNEMONIC_PREFETCHNTA:
    case ZYDIS_MNEMONIC_PREFETCHT0:
    case ZYDIS_MNEMONIC_PREFETCHT1:
    case ZYDIS_MNEMONIC_PREFETCHT2:
    case ZYDIS_MNEMONIC_SFENCE:
    case ZYDIS_MNEMONIC_WBINVD:
    case ZYDIS_MNEMONIC_CLI:
    case ZYDIS_MNEMONIC_STI:
        return true;
    case ZYDIS_MNEMONIC_CPUID:
        S("FCI_SAVE Cpuid(c); FCI_LOAD");
        return true;
    case ZYDIS_MNEMONIC_RDTSC:
        S("const uint64_t t = ReadTsc(); c.eax = uint32_t(t); c.edx = uint32_t(t >> 32);");
        return true;
    case ZYDIS_MNEMONIC_IN: {
        std::string port = o[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE ? Rd(1, 16) : "uint16_t(c.edx)";
        S(Wr(0, F("PortIn(c, %s, %d)", port.c_str(), bits)));
        return true;
    }
    case ZYDIS_MNEMONIC_OUT: {
        std::string port = o[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE ? Rd(0, 16) : "uint16_t(c.edx)";
        S(F("PortOut(c, %s, uint32_t(%s), %d);", port.c_str(), Rd(1).c_str(), o[1].size));
        return true;
    }
    case ZYDIS_MNEMONIC_INT:
        if (o[0].imm.value.u != 0x2D) return false;
        S("FCI_SAVE DebugService(c); FCI_LOAD");
        return true;
    case ZYDIS_MNEMONIC_XLAT:
        S("c.eax = (c.eax & 0xFFFFFF00u) | R8(c.ebx + (c.eax & 0xFFu));");
        return true;
    default:
        break;
    }

    if (const char* cond = Cond(mn)) {
        if (std::strncmp(ZydisMnemonicGetString(mn), "set", 3) == 0) {
            S(Wr(0, F("uint8_t(%s)", cond)));
            return true;
        }
        if (std::strncmp(ZydisMnemonicGetString(mn), "cmov", 4) == 0) {
            S(F("if (%s) { %s }", cond, Wr(0, Rd(1)).c_str()));
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// sse (Pentium III: single precision only)

bool FunctionWriter::EmitSse(const Decoded& d, bool* handled) {
    const auto* o = d.ops;
    const auto mn = d.ins.mnemonic;
    *handled = true;
    const int x = IsXmm(o[0]) ? Xmm(0) : -1;

    auto scalar = [&](const char* op) {
        if (x < 0) return false;
        S(F("c.xmm[%d].f[0] = c.xmm[%d].f[0] %s %s;", x, x, op, Xf(1).c_str()));
        return true;
    };
    auto scalar_fn = [&](const char* expr) {  // expr uses `a` (dst) and `b` (src)
        if (x < 0) return false;
        S(F("const float a = c.xmm[%d].f[0], b = %s; c.xmm[%d].f[0] = %s;", x, Xf(1).c_str(), x, expr));
        return true;
    };
    auto packed = [&](const char* expr) {  // per lane, `a` and `b` are the lane values
        if (x < 0) return false;
        S(F("const Xmm s = %s; for (int i = 0; i < 4; i++) { const float a = c.xmm[%d].f[i], b = s.f[i]; c.xmm[%d].f[i] = %s; }",
            Xv(1).c_str(), x, x, expr));
        return true;
    };
    auto bitwise = [&](const char* expr) {
        if (x < 0) return false;
        S(F("const Xmm s = %s; for (int i = 0; i < 4; i++) { const uint32_t a = c.xmm[%d].u[i], b = s.u[i]; c.xmm[%d].u[i] = %s; }",
            Xv(1).c_str(), x, x, expr));
        return true;
    };

    switch (mn) {
    case ZYDIS_MNEMONIC_MOVSS:
        if (x >= 0 && IsXmm(o[1])) S(F("c.xmm[%d].u[0] = c.xmm[%d].u[0];", x, Xmm(1)));
        else if (x >= 0) S(F("c.xmm[%d] = Xmm{.u = {R32(ea), 0, 0, 0}};", x));
        else S(F("W32(ea, c.xmm[%d].u[0]);", Xmm(1)));
        return true;
    case ZYDIS_MNEMONIC_MOVAPS:
    case ZYDIS_MNEMONIC_MOVUPS:
    case ZYDIS_MNEMONIC_MOVNTPS:
        if (x >= 0) S(F("c.xmm[%d] = %s;", x, Xv(1).c_str()));
        else S(F("StoreX(ea, c.xmm[%d]);", Xmm(1)));
        return true;
    case ZYDIS_MNEMONIC_MOVLPS:
    case ZYDIS_MNEMONIC_MOVHPS: {
        int q = mn == ZYDIS_MNEMONIC_MOVHPS;
        if (x >= 0) S(F("c.xmm[%d].q[%d] = R64(ea);", x, q));
        else S(F("W64(ea, c.xmm[%d].q[%d]);", Xmm(1), q));
        return true;
    }
    case ZYDIS_MNEMONIC_MOVLHPS:
        S(F("c.xmm[%d].q[1] = c.xmm[%d].q[0];", x, Xmm(1)));
        return true;
    case ZYDIS_MNEMONIC_MOVHLPS:
        S(F("c.xmm[%d].q[0] = c.xmm[%d].q[1];", x, Xmm(1)));
        return true;
    case ZYDIS_MNEMONIC_ADDSS: return scalar("+");
    case ZYDIS_MNEMONIC_SUBSS: return scalar("-");
    case ZYDIS_MNEMONIC_MULSS: return scalar("*");
    case ZYDIS_MNEMONIC_DIVSS: return scalar("/");
    case ZYDIS_MNEMONIC_MAXSS: return scalar_fn("MaxF(a, b)");
    case ZYDIS_MNEMONIC_MINSS: return scalar_fn("MinF(a, b)");
    case ZYDIS_MNEMONIC_SQRTSS: return scalar_fn("std::sqrt(b)");
    case ZYDIS_MNEMONIC_RSQRTSS: return scalar_fn("1.0f / std::sqrt(b)");
    case ZYDIS_MNEMONIC_RCPSS: return scalar_fn("1.0f / b");
    case ZYDIS_MNEMONIC_ADDPS: return packed("a + b");
    case ZYDIS_MNEMONIC_SUBPS: return packed("a - b");
    case ZYDIS_MNEMONIC_MULPS: return packed("a * b");
    case ZYDIS_MNEMONIC_DIVPS: return packed("a / b");
    case ZYDIS_MNEMONIC_MAXPS: return packed("MaxF(a, b)");
    case ZYDIS_MNEMONIC_MINPS: return packed("MinF(a, b)");
    case ZYDIS_MNEMONIC_SQRTPS: return packed("std::sqrt(b)");
    case ZYDIS_MNEMONIC_RSQRTPS: return packed("1.0f / std::sqrt(b)");
    case ZYDIS_MNEMONIC_RCPPS: return packed("1.0f / b");
    case ZYDIS_MNEMONIC_ANDPS: return bitwise("a & b");
    case ZYDIS_MNEMONIC_ANDNPS: return bitwise("~a & b");
    case ZYDIS_MNEMONIC_ORPS: return bitwise("a | b");
    case ZYDIS_MNEMONIC_XORPS:
        if (XmmLiveOn() && x >= 0 && IsXmm(o[1]) && Xmm(1) == x) {  // the zero idiom: no read (xmm liveness relies on it)
            S(F("c.xmm[%d] = Xmm{};", x));
            return true;
        }
        return bitwise("a ^ b");
    case ZYDIS_MNEMONIC_SHUFPS:
        S(F("c.xmm[%d] = Shufps(c.xmm[%d], %s, 0x%Xu);", x, x, Xv(1).c_str(), uint32_t(o[2].imm.value.u & 0xFF)));
        return true;
    case ZYDIS_MNEMONIC_UNPCKLPS:
    case ZYDIS_MNEMONIC_UNPCKHPS: {
        int h = mn == ZYDIS_MNEMONIC_UNPCKHPS ? 2 : 0;
        S(F("const Xmm a = c.xmm[%d], b = %s; c.xmm[%d] = Xmm{.f = {a.f[%d], b.f[%d], a.f[%d], b.f[%d]}};", x,
            Xv(1).c_str(), x, h, h, h + 1, h + 1));
        return true;
    }
    case ZYDIS_MNEMONIC_COMISS:
    case ZYDIS_MNEMONIC_UCOMISS:
        S(F("CompareToFlags(f, c.xmm[%d].f[0], %s);", x, Xf(1).c_str()));
        return true;
    case ZYDIS_MNEMONIC_CVTSI2SS:
        S(F("c.xmm[%d].f[0] = float(int32_t(%s));", x, Rd(1).c_str()));
        return true;
    case ZYDIS_MNEMONIC_CVTTSS2SI:
        S(Wr(0, F("uint32_t(CvttToInt(%s))", Xf(1).c_str())));
        return true;
    case ZYDIS_MNEMONIC_CVTSS2SI:
        S(Wr(0, F("uint32_t(CvtToInt(c, %s))", Xf(1).c_str())));
        return true;
    case ZYDIS_MNEMONIC_CVTPI2PS:
        S(F("const uint64_t v = %s; c.xmm[%d].f[0] = float(int32_t(uint32_t(v))); c.xmm[%d].f[1] = float(int32_t(uint32_t(v >> 32)));",
            Mv(1).c_str(), x, x));
        return true;
    case ZYDIS_MNEMONIC_LDMXCSR:
        S("SetMxcsr(c, R32(ea));");
        return true;
    case ZYDIS_MNEMONIC_STMXCSR:
        S("W32(ea, c.mxcsr);");
        return true;
    default:
        break;
    }
    *handled = false;
    return false;
}

// ---------------------------------------------------------------------------------------------
// mmx

bool FunctionWriter::EmitMmx(const Decoded& d, bool* handled) {
    const auto* o = d.ops;
    const auto mn = d.ins.mnemonic;
    *handled = true;
    const int x = IsMm(o[0]) ? Mm(0) : -1;

    auto set = [&](const std::string& expr) {
        if (x < 0) return false;
        S(F("const uint64_t a = c.mm[%d], b = %s; c.mm[%d] = %s;", x, Mv(1).c_str(), x, expr.c_str()));
        return true;
    };
    auto lanes = [&](const char* lane, const char* expr) {
        return set(F("Lanes<%s>(a, b, [](%s x, %s y) { return %s; })", lane, lane, lane, expr));
    };

    switch (mn) {
    case ZYDIS_MNEMONIC_MOVD:
        if (x >= 0) S(F("c.mm[%d] = uint64_t(uint32_t(%s));", x, Rd(1).c_str()));
        else if (IsMm(o[1])) S(Wr(0, F("uint32_t(c.mm[%d])", Mm(1))));
        else return false;
        return true;
    case ZYDIS_MNEMONIC_MOVQ:
    case ZYDIS_MNEMONIC_MOVNTQ:
        if (x >= 0) S(F("c.mm[%d] = %s;", x, Mv(1).c_str()));
        else if (IsMm(o[1])) S(F("W64(ea, c.mm[%d]);", Mm(1)));
        else return false;
        return true;
    case ZYDIS_MNEMONIC_EMMS: return true;
    case ZYDIS_MNEMONIC_PADDW: return lanes("uint16_t", "uint16_t(x + y)");
    case ZYDIS_MNEMONIC_PADDD: return lanes("uint32_t", "uint32_t(x + y)");
    case ZYDIS_MNEMONIC_PSUBW: return lanes("uint16_t", "uint16_t(x - y)");
    case ZYDIS_MNEMONIC_PSUBD: return lanes("uint32_t", "uint32_t(x - y)");
    case ZYDIS_MNEMONIC_PADDSW: return lanes("int16_t", "Saturate<int16_t>(int64_t(x) + y)");
    case ZYDIS_MNEMONIC_PSUBSW: return lanes("int16_t", "Saturate<int16_t>(int64_t(x) - y)");
    case ZYDIS_MNEMONIC_PADDUSW: return lanes("uint16_t", "Saturate<uint16_t>(int64_t(x) + y)");
    case ZYDIS_MNEMONIC_PSUBUSW: return lanes("uint16_t", "Saturate<uint16_t>(int64_t(x) - y)");
    case ZYDIS_MNEMONIC_PMULLW: return lanes("int16_t", "int16_t(int32_t(x) * y)");
    case ZYDIS_MNEMONIC_PMULHW: return lanes("int16_t", "int16_t((int32_t(x) * y) >> 16)");
    case ZYDIS_MNEMONIC_PCMPEQD: return lanes("uint32_t", "x == y ? 0xFFFFFFFFu : 0u");
    case ZYDIS_MNEMONIC_PCMPGTW: return lanes("int16_t", "int16_t(x > y ? -1 : 0)");
    case ZYDIS_MNEMONIC_PMADDWD: return set("Pmaddwd(a, b)");
    case ZYDIS_MNEMONIC_PAND: return set("a & b");
    case ZYDIS_MNEMONIC_POR: return set("a | b");
    case ZYDIS_MNEMONIC_PXOR: return set("a ^ b");
    case ZYDIS_MNEMONIC_PANDN: return set("~a & b");
    case ZYDIS_MNEMONIC_PSLLW: return set("ShiftLeft<uint16_t>(a, b)");
    case ZYDIS_MNEMONIC_PSLLD: return set("ShiftLeft<uint32_t>(a, b)");
    case ZYDIS_MNEMONIC_PSLLQ: return set("b > 63 ? 0 : a << b");
    case ZYDIS_MNEMONIC_PSRLW: return set("ShiftRight<uint16_t>(a, b)");
    case ZYDIS_MNEMONIC_PSRLD: return set("ShiftRight<uint32_t>(a, b)");
    case ZYDIS_MNEMONIC_PSRLQ: return set("b > 63 ? 0 : a >> b");
    case ZYDIS_MNEMONIC_PSRAW: return set("ShiftRight<int16_t>(a, b)");
    case ZYDIS_MNEMONIC_PSRAD: return set("ShiftRight<int32_t>(a, b)");
    case ZYDIS_MNEMONIC_PUNPCKLBW: return set("Unpack<uint16_t, uint8_t>(a, b, false)");
    case ZYDIS_MNEMONIC_PUNPCKHBW: return set("Unpack<uint16_t, uint8_t>(a, b, true)");
    case ZYDIS_MNEMONIC_PUNPCKLWD: return set("Unpack<uint32_t, uint16_t>(a, b, false)");
    case ZYDIS_MNEMONIC_PUNPCKHWD: return set("Unpack<uint32_t, uint16_t>(a, b, true)");
    case ZYDIS_MNEMONIC_PUNPCKLDQ: return set("Unpack<uint64_t, uint32_t>(a, b, false)");
    case ZYDIS_MNEMONIC_PUNPCKHDQ: return set("Unpack<uint64_t, uint32_t>(a, b, true)");
    case ZYDIS_MNEMONIC_PACKSSDW: return set("Pack<int32_t, int16_t>(a, b)");
    case ZYDIS_MNEMONIC_PACKSSWB: return set("Pack<int16_t, int8_t>(a, b)");
    case ZYDIS_MNEMONIC_PMOVMSKB:
        S(Wr(0, F("Pmovmskb(c.mm[%d])", Mm(1))));
        return true;
    case ZYDIS_MNEMONIC_PSHUFW:
        S(F("c.mm[%d] = Pshufw(%s, 0x%Xu);", x, Mv(1).c_str(), uint32_t(o[2].imm.value.u & 0xFF)));
        return true;
    default:
        break;
    }
    *handled = false;
    return false;
}

// ---------------------------------------------------------------------------------------------
// x87

bool FunctionWriter::EmitX87(const Decoded& d, bool* handled) {
    const auto* o = d.ops;
    const auto mn = d.ins.mnemonic;
    const int n = d.ins.operand_count_visible;
    *handled = true;

    // two operand arithmetic: dst = dst op src, or src op dst for the reversed forms
    auto arith = [&](const char* op, bool reverse, bool pop) {
        if (n == 1 && IsMem(o[0])) {
            S(reverse ? F("ST(c, 0) = %s %s ST(c, 0);", Fv(0).c_str(), op) : F("ST(c, 0) = ST(c, 0) %s %s;", op, Fv(0).c_str()));
        } else if (n == 2) {
            int dst = St(0), src = St(1);
            S(reverse ? F("ST(c, %d) = ST(c, %d) %s ST(c, %d);", dst, src, op, dst)
                      : F("ST(c, %d) = ST(c, %d) %s ST(c, %d);", dst, dst, op, src));
        } else if (n == 0) {
            S(reverse ? F("ST(c, 1) = ST(c, 0) %s ST(c, 1);", op) : F("ST(c, 1) = ST(c, 1) %s ST(c, 0);", op));
        } else {
            return false;
        }
        if (pop) S("FPop(c);");
        return true;
    };
    auto unary = [&](const char* expr) {  // expr uses `v` = st0
        S(F("const double v = ST(c, 0); ST(c, 0) = %s;", expr));
        return true;
    };
    auto store_float = [&](bool pop) {
        const auto& op = o[0];
        if (IsSt(op)) S(F("ST(c, %d) = ST(c, 0);", St(0)));
        else if (op.size == 32) S("WF32(ea, float(ST(c, 0)));");
        else if (op.size == 64) S("WF64(ea, ST(c, 0));");
        else if (op.size == 80) S("StoreF80(ea, ST(c, 0));");
        else return false;
        if (pop) S("FPop(c);");
        return true;
    };
    auto store_int = [&](bool pop, bool truncate) {
        int b = o[0].size;
        if (b != 16 && b != 32 && b != 64) return false;
        S(F("W%d(ea, %s(FToInt<%s>(%s)));", b, Type(b), SType(b), truncate ? "std::trunc(ST(c, 0))" : "FRound(c, ST(c, 0))"));
        if (pop) S("FPop(c);");
        return true;
    };
    auto compare = [&](int pops) {
        std::string v = n >= 1 ? (IsSt(o[n - 1]) && n == 2 ? Fv(1) : Fv(0)) : "ST(c, 1)";
        S(F("FCompare(c, ST(c, 0), %s);", v.c_str()));
        for (int i = 0; i < pops; i++) S("FPop(c);");
        return true;
    };
    auto compare_flags = [&](bool pop) {
        S(F("CompareToFlags(f, ST(c, 0), %s);", n == 2 ? Fv(1).c_str() : "ST(c, 1)"));
        if (pop) S("FPop(c);");
        return true;
    };

    switch (mn) {
    case ZYDIS_MNEMONIC_FLD:
        S(F("const double v = %s; FPush(c, v);", Fv(0).c_str()));
        return true;
    case ZYDIS_MNEMONIC_FILD:
        S(F("FPush(c, %s);", Iv(0).c_str()));
        return true;
    case ZYDIS_MNEMONIC_FLD1: S("FPush(c, 1.0);"); return true;
    case ZYDIS_MNEMONIC_FLDZ: S("FPush(c, 0.0);"); return true;
    case ZYDIS_MNEMONIC_FLDPI: S("FPush(c, 3.14159265358979323846);"); return true;
    case ZYDIS_MNEMONIC_FLDL2E: S("FPush(c, 1.44269504088896340736);"); return true;
    case ZYDIS_MNEMONIC_FLDL2T: S("FPush(c, 3.32192809488736234787);"); return true;
    case ZYDIS_MNEMONIC_FLDLN2: S("FPush(c, 0.69314718055994530942);"); return true;
    case ZYDIS_MNEMONIC_FLDLG2: S("FPush(c, 0.30102999566398119521);"); return true;
    case ZYDIS_MNEMONIC_FST: return store_float(false);
    case ZYDIS_MNEMONIC_FSTP: return store_float(true);
    case ZYDIS_MNEMONIC_FIST: return store_int(false, false);
    case ZYDIS_MNEMONIC_FISTP: return store_int(true, false);
    case ZYDIS_MNEMONIC_FISTTP: return store_int(true, true);
    case ZYDIS_MNEMONIC_FADD: return arith("+", false, false);
    case ZYDIS_MNEMONIC_FADDP: return arith("+", false, true);
    case ZYDIS_MNEMONIC_FSUB: return arith("-", false, false);
    case ZYDIS_MNEMONIC_FSUBP: return arith("-", false, true);
    case ZYDIS_MNEMONIC_FSUBR: return arith("-", true, false);
    case ZYDIS_MNEMONIC_FSUBRP: return arith("-", true, true);
    case ZYDIS_MNEMONIC_FMUL: return arith("*", false, false);
    case ZYDIS_MNEMONIC_FMULP: return arith("*", false, true);
    case ZYDIS_MNEMONIC_FDIV: return arith("/", false, false);
    case ZYDIS_MNEMONIC_FDIVP: return arith("/", false, true);
    case ZYDIS_MNEMONIC_FDIVR: return arith("/", true, false);
    case ZYDIS_MNEMONIC_FDIVRP: return arith("/", true, true);
    case ZYDIS_MNEMONIC_FIADD: S(F("ST(c, 0) = ST(c, 0) + %s;", Iv(0).c_str())); return true;
    case ZYDIS_MNEMONIC_FISUB: S(F("ST(c, 0) = ST(c, 0) - %s;", Iv(0).c_str())); return true;
    case ZYDIS_MNEMONIC_FISUBR: S(F("ST(c, 0) = %s - ST(c, 0);", Iv(0).c_str())); return true;
    case ZYDIS_MNEMONIC_FIMUL: S(F("ST(c, 0) = ST(c, 0) * %s;", Iv(0).c_str())); return true;
    case ZYDIS_MNEMONIC_FIDIV: S(F("ST(c, 0) = ST(c, 0) / %s;", Iv(0).c_str())); return true;
    case ZYDIS_MNEMONIC_FIDIVR: S(F("ST(c, 0) = %s / ST(c, 0);", Iv(0).c_str())); return true;
    case ZYDIS_MNEMONIC_FCHS: return unary("-v");
    case ZYDIS_MNEMONIC_FABS: return unary("std::fabs(v)");
    case ZYDIS_MNEMONIC_FSQRT: return unary("std::sqrt(v)");
    case ZYDIS_MNEMONIC_FSIN: S("c.fsw &= ~kC2;"); return unary("std::sin(v)");
    case ZYDIS_MNEMONIC_FCOS: S("c.fsw &= ~kC2;"); return unary("std::cos(v)");
    case ZYDIS_MNEMONIC_F2XM1: return unary("std::exp2(v) - 1.0");
    case ZYDIS_MNEMONIC_FRNDINT: return unary("FRound(c, v)");
    case ZYDIS_MNEMONIC_FSINCOS:
        S("c.fsw &= ~kC2; const double v = ST(c, 0); ST(c, 0) = std::sin(v); FPush(c, std::cos(v));");
        return true;
    case ZYDIS_MNEMONIC_FPTAN:
        S("c.fsw &= ~kC2; ST(c, 0) = std::tan(ST(c, 0)); FPush(c, 1.0);");
        return true;
    case ZYDIS_MNEMONIC_FPATAN:
        S("ST(c, 1) = std::atan2(ST(c, 1), ST(c, 0)); FPop(c);");
        return true;
    case ZYDIS_MNEMONIC_FYL2X:
        S("ST(c, 1) = ST(c, 1) * std::log2(ST(c, 0)); FPop(c);");
        return true;
    case ZYDIS_MNEMONIC_FSCALE:
        S("ST(c, 0) = std::ldexp(ST(c, 0), int(std::trunc(ST(c, 1))));");
        return true;
    // partial remainder, always complete (C2 clear); C0, C3, C1 take the low quotient bits
    case ZYDIS_MNEMONIC_FPREM:
    case ZYDIS_MNEMONIC_FPREM1: {
        bool ieee = d.ins.mnemonic == ZYDIS_MNEMONIC_FPREM1;
        S(std::string("const double a = ST(c, 0), b = ST(c, 1); const double q = ") +
          (ieee ? "std::nearbyint(a / b)" : "std::trunc(a / b)") +
          "; const uint64_t n = uint64_t(std::fabs(q)); ST(c, 0) = " + (ieee ? "std::remainder(a, b)" : "std::fmod(a, b)") +
          "; c.fsw = uint16_t((c.fsw & ~0x4700u) | ((n & 4) ? 0x0100u : 0) | ((n & 2) ? 0x4000u : 0) | "
          "((n & 1) ? 0x0200u : 0));");
        return true;
    }
    case ZYDIS_MNEMONIC_FXTRACT:
        S("const double v = ST(c, 0); int e = 0; const double m = std::frexp(v, &e); ST(c, 0) = double(e - 1); FPush(c, m * 2.0);");
        return true;
    case ZYDIS_MNEMONIC_FXCH: {
        int k = 1;
        for (int i = 0; i < n; i++)
            if (IsSt(o[i]) && St(i) != 0) k = St(i);
        S(F("const double t = ST(c, 0); ST(c, 0) = ST(c, %d); ST(c, %d) = t;", k, k));
        return true;
    }
    case ZYDIS_MNEMONIC_FCOM:
    case ZYDIS_MNEMONIC_FUCOM: return compare(0);
    case ZYDIS_MNEMONIC_FCOMP:
    case ZYDIS_MNEMONIC_FUCOMP: return compare(1);
    case ZYDIS_MNEMONIC_FCOMPP:
    case ZYDIS_MNEMONIC_FUCOMPP:
        S("FCompare(c, ST(c, 0), ST(c, 1)); FPop(c); FPop(c);");
        return true;
    case ZYDIS_MNEMONIC_FICOM:
    case ZYDIS_MNEMONIC_FICOMP:
        S(F("FCompare(c, ST(c, 0), %s);", Iv(0).c_str()));
        if (mn == ZYDIS_MNEMONIC_FICOMP) S("FPop(c);");
        return true;
    case ZYDIS_MNEMONIC_FTST:
        S("FCompare(c, ST(c, 0), 0.0);");
        return true;
    case ZYDIS_MNEMONIC_FCOMI:
    case ZYDIS_MNEMONIC_FUCOMI: return compare_flags(false);
    case ZYDIS_MNEMONIC_FCOMIP:
    case ZYDIS_MNEMONIC_FUCOMIP: return compare_flags(true);
    case ZYDIS_MNEMONIC_FXAM:
        S("FExamine(c);");
        return true;
    case ZYDIS_MNEMONIC_FNSTSW:
        S(Wr(0, "FStatus(c)"));
        return true;
    case ZYDIS_MNEMONIC_FNSTCW:
        S("W16(ea, c.fcw);");
        return true;
    case ZYDIS_MNEMONIC_FLDCW:
        S("c.fcw = R16(ea);");
        return true;
    case ZYDIS_MNEMONIC_FNSAVE:
        S("FSave(c, ea);");
        return true;
    case ZYDIS_MNEMONIC_FRSTOR:
        S("FRestore(c, ea);");
        return true;
    case ZYDIS_MNEMONIC_FWAIT:
    case ZYDIS_MNEMONIC_FNCLEX:
    case ZYDIS_MNEMONIC_FFREE:
    case ZYDIS_MNEMONIC_FNOP:
        return true;
    default:
        break;
    }
    if (std::strncmp(ZydisMnemonicGetString(mn), "fcmov", 5) == 0) {
        const char* cond = Cond(mn);
        if (!cond || n != 2) return false;
        S(F("if (%s) ST(c, 0) = ST(c, %d);", cond, St(1)));
        return true;
    }
    *handled = false;
    return false;
}

}  // namespace recomp
