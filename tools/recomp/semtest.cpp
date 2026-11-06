// Writes the instruction semantics test: for a broad set of register-only instructions, the C++ the
// code generator emits next to the same instruction encoded for x86-64, which tools/semtest runs
// natively on the host CPU to compare results.
#include "semtest.h"

#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <vector>

#include "codegen_internal.h"

namespace recomp {

namespace {

ZydisEncoderOperand Reg(ZydisRegister r) {
    ZydisEncoderOperand o{};
    o.type = ZYDIS_OPERAND_TYPE_REGISTER;
    o.reg.value = r;
    return o;
}

ZydisEncoderOperand Imm(int64_t v) {
    ZydisEncoderOperand o{};
    o.type = ZYDIS_OPERAND_TYPE_IMMEDIATE;
    o.imm.s = v;
    return o;
}

ZydisEncoderOperand Mem(ZydisRegister base, ZydisRegister index, uint8_t scale, int64_t disp, uint16_t size) {
    ZydisEncoderOperand o{};
    o.type = ZYDIS_OPERAND_TYPE_MEMORY;
    o.mem.base = base;
    o.mem.index = index;
    o.mem.scale = scale;
    o.mem.displacement = disp;
    o.mem.size = size;
    return o;
}

struct Request {
    ZydisMnemonic mnemonic;
    std::vector<ZydisEncoderOperand> ops;
    ZydisInstructionAttributes prefixes = 0;
};

bool Encode(const Request& r, ZydisMachineMode mode, std::vector<uint8_t>* out) {
    ZydisEncoderRequest req{};
    req.mnemonic = r.mnemonic;
    req.machine_mode = mode;
    req.prefixes = r.prefixes;
    req.operand_count = uint8_t(r.ops.size());
    for (size_t i = 0; i < r.ops.size(); i++) req.operands[i] = r.ops[i];
    uint8_t buf[ZYDIS_MAX_INSTRUCTION_LENGTH];
    ZyanUSize len = sizeof buf;
    if (!ZYAN_SUCCESS(ZydisEncoderEncodeInstruction(&req, buf, &len))) return false;
    out->assign(buf, buf + len);
    return true;
}

std::vector<Request> Cases() {
    using M = ZydisMnemonic;
    std::vector<Request> c;
    auto add = [&](M m, std::initializer_list<ZydisEncoderOperand> ops) { c.push_back({m, ops}); };
    const ZydisRegister r32[] = {ZYDIS_REGISTER_EAX, ZYDIS_REGISTER_ECX, ZYDIS_REGISTER_EDX, ZYDIS_REGISTER_EBX,
                                 ZYDIS_REGISTER_ESI, ZYDIS_REGISTER_EDI, ZYDIS_REGISTER_EBP};

    for (M m : {ZYDIS_MNEMONIC_ADD, ZYDIS_MNEMONIC_ADC, ZYDIS_MNEMONIC_SUB, ZYDIS_MNEMONIC_SBB, ZYDIS_MNEMONIC_CMP,
                ZYDIS_MNEMONIC_AND, ZYDIS_MNEMONIC_OR, ZYDIS_MNEMONIC_XOR, ZYDIS_MNEMONIC_TEST}) {
        add(m, {Reg(ZYDIS_REGISTER_EAX), Reg(ZYDIS_REGISTER_ECX)});
        add(m, {Reg(ZYDIS_REGISTER_ESI), Reg(ZYDIS_REGISTER_ESI)});
        add(m, {Reg(ZYDIS_REGISTER_DX), Reg(ZYDIS_REGISTER_BX)});
        add(m, {Reg(ZYDIS_REGISTER_AL), Reg(ZYDIS_REGISTER_BH)});
        add(m, {Reg(ZYDIS_REGISTER_CH), Reg(ZYDIS_REGISTER_DL)});
        add(m, {Reg(ZYDIS_REGISTER_EDX), Imm(-3)});
        add(m, {Reg(ZYDIS_REGISTER_EDI), Imm(0x12345678)});
        add(m, {Reg(ZYDIS_REGISTER_BL), Imm(0x80)});
        add(m, {Reg(ZYDIS_REGISTER_SI), Imm(0x7FFF)});
    }
    for (M m : {ZYDIS_MNEMONIC_INC, ZYDIS_MNEMONIC_DEC, ZYDIS_MNEMONIC_NEG, ZYDIS_MNEMONIC_NOT}) {
        add(m, {Reg(ZYDIS_REGISTER_EBX)});
        add(m, {Reg(ZYDIS_REGISTER_CX)});
        add(m, {Reg(ZYDIS_REGISTER_AH)});
        add(m, {Reg(ZYDIS_REGISTER_DL)});
    }
    for (M m : {ZYDIS_MNEMONIC_SHL, ZYDIS_MNEMONIC_SHR, ZYDIS_MNEMONIC_SAR, ZYDIS_MNEMONIC_ROL, ZYDIS_MNEMONIC_ROR,
                ZYDIS_MNEMONIC_RCL, ZYDIS_MNEMONIC_RCR}) {
        add(m, {Reg(ZYDIS_REGISTER_EDX), Reg(ZYDIS_REGISTER_CL)});
        add(m, {Reg(ZYDIS_REGISTER_EAX), Imm(1)});
        add(m, {Reg(ZYDIS_REGISTER_ESI), Imm(7)});
        add(m, {Reg(ZYDIS_REGISTER_EDI), Imm(31)});
        add(m, {Reg(ZYDIS_REGISTER_BX), Reg(ZYDIS_REGISTER_CL)});
        add(m, {Reg(ZYDIS_REGISTER_AH), Reg(ZYDIS_REGISTER_CL)});
        add(m, {Reg(ZYDIS_REGISTER_DL), Imm(3)});
    }
    for (M m : {ZYDIS_MNEMONIC_SHLD, ZYDIS_MNEMONIC_SHRD}) {
        add(m, {Reg(ZYDIS_REGISTER_EAX), Reg(ZYDIS_REGISTER_EDX), Reg(ZYDIS_REGISTER_CL)});
        add(m, {Reg(ZYDIS_REGISTER_ESI), Reg(ZYDIS_REGISTER_EDI), Imm(5)});
    }
    add(ZYDIS_MNEMONIC_IMUL, {Reg(ZYDIS_REGISTER_EAX), Reg(ZYDIS_REGISTER_EBX)});
    add(ZYDIS_MNEMONIC_IMUL, {Reg(ZYDIS_REGISTER_ECX), Reg(ZYDIS_REGISTER_ESI), Imm(-7)});
    add(ZYDIS_MNEMONIC_IMUL, {Reg(ZYDIS_REGISTER_EDX), Reg(ZYDIS_REGISTER_EDX), Imm(0x10001)});
    add(ZYDIS_MNEMONIC_IMUL, {Reg(ZYDIS_REGISTER_SI), Reg(ZYDIS_REGISTER_DI)});
    add(ZYDIS_MNEMONIC_IMUL, {Reg(ZYDIS_REGISTER_ECX)});
    add(ZYDIS_MNEMONIC_IMUL, {Reg(ZYDIS_REGISTER_BX)});
    add(ZYDIS_MNEMONIC_IMUL, {Reg(ZYDIS_REGISTER_BL)});
    add(ZYDIS_MNEMONIC_MUL, {Reg(ZYDIS_REGISTER_ESI)});
    add(ZYDIS_MNEMONIC_DIV, {Reg(ZYDIS_REGISTER_ECX)});
    add(ZYDIS_MNEMONIC_IDIV, {Reg(ZYDIS_REGISTER_EBX)});
    add(ZYDIS_MNEMONIC_BT, {Reg(ZYDIS_REGISTER_EAX), Reg(ZYDIS_REGISTER_ECX)});
    add(ZYDIS_MNEMONIC_BT, {Reg(ZYDIS_REGISTER_EDX), Imm(17)});
    add(ZYDIS_MNEMONIC_BTS, {Reg(ZYDIS_REGISTER_ESI), Reg(ZYDIS_REGISTER_EDX)});
    add(ZYDIS_MNEMONIC_BSF, {Reg(ZYDIS_REGISTER_EAX), Reg(ZYDIS_REGISTER_EBX)});
    add(ZYDIS_MNEMONIC_MOVZX, {Reg(ZYDIS_REGISTER_EAX), Reg(ZYDIS_REGISTER_BH)});
    add(ZYDIS_MNEMONIC_MOVZX, {Reg(ZYDIS_REGISTER_ECX), Reg(ZYDIS_REGISTER_DX)});
    add(ZYDIS_MNEMONIC_MOVZX, {Reg(ZYDIS_REGISTER_SI), Reg(ZYDIS_REGISTER_AL)});
    add(ZYDIS_MNEMONIC_MOVSX, {Reg(ZYDIS_REGISTER_EDI), Reg(ZYDIS_REGISTER_CL)});
    add(ZYDIS_MNEMONIC_MOVSX, {Reg(ZYDIS_REGISTER_EBX), Reg(ZYDIS_REGISTER_SI)});
    add(ZYDIS_MNEMONIC_MOVSX, {Reg(ZYDIS_REGISTER_DX), Reg(ZYDIS_REGISTER_AH)});
    add(ZYDIS_MNEMONIC_MOV, {Reg(ZYDIS_REGISTER_AH), Reg(ZYDIS_REGISTER_DL)});
    add(ZYDIS_MNEMONIC_MOV, {Reg(ZYDIS_REGISTER_CX), Imm(0xBEEF)});
    add(ZYDIS_MNEMONIC_XCHG, {Reg(ZYDIS_REGISTER_ESI), Reg(ZYDIS_REGISTER_EDI)});
    add(ZYDIS_MNEMONIC_XCHG, {Reg(ZYDIS_REGISTER_AL), Reg(ZYDIS_REGISTER_CH)});
    add(ZYDIS_MNEMONIC_XADD, {Reg(ZYDIS_REGISTER_EBX), Reg(ZYDIS_REGISTER_ECX)});
    add(ZYDIS_MNEMONIC_BSWAP, {Reg(ZYDIS_REGISTER_EDX)});
    add(ZYDIS_MNEMONIC_CDQ, {});
    add(ZYDIS_MNEMONIC_CWDE, {});
    add(ZYDIS_MNEMONIC_LAHF, {});
    add(ZYDIS_MNEMONIC_SAHF, {});
    add(ZYDIS_MNEMONIC_CMC, {});
    add(ZYDIS_MNEMONIC_CLC, {});
    add(ZYDIS_MNEMONIC_STC, {});
    add(ZYDIS_MNEMONIC_LEA, {Reg(ZYDIS_REGISTER_EAX), Mem(ZYDIS_REGISTER_ECX, ZYDIS_REGISTER_EDX, 4, -8, 4)});
    add(ZYDIS_MNEMONIC_LEA, {Reg(ZYDIS_REGISTER_ESI), Mem(ZYDIS_REGISTER_ESI, ZYDIS_REGISTER_ESI, 2, 0x100, 4)});
    for (int cc = 0; cc < 16; cc++) {
        static const M kSet[] = {ZYDIS_MNEMONIC_SETO, ZYDIS_MNEMONIC_SETNO, ZYDIS_MNEMONIC_SETB, ZYDIS_MNEMONIC_SETNB,
                                 ZYDIS_MNEMONIC_SETZ, ZYDIS_MNEMONIC_SETNZ, ZYDIS_MNEMONIC_SETBE, ZYDIS_MNEMONIC_SETNBE,
                                 ZYDIS_MNEMONIC_SETS, ZYDIS_MNEMONIC_SETNS, ZYDIS_MNEMONIC_SETP, ZYDIS_MNEMONIC_SETNP,
                                 ZYDIS_MNEMONIC_SETL, ZYDIS_MNEMONIC_SETNL, ZYDIS_MNEMONIC_SETLE, ZYDIS_MNEMONIC_SETNLE};
        static const M kCmov[] = {ZYDIS_MNEMONIC_CMOVO, ZYDIS_MNEMONIC_CMOVNO, ZYDIS_MNEMONIC_CMOVB, ZYDIS_MNEMONIC_CMOVNB,
                                  ZYDIS_MNEMONIC_CMOVZ, ZYDIS_MNEMONIC_CMOVNZ, ZYDIS_MNEMONIC_CMOVBE, ZYDIS_MNEMONIC_CMOVNBE,
                                  ZYDIS_MNEMONIC_CMOVS, ZYDIS_MNEMONIC_CMOVNS, ZYDIS_MNEMONIC_CMOVP, ZYDIS_MNEMONIC_CMOVNP,
                                  ZYDIS_MNEMONIC_CMOVL, ZYDIS_MNEMONIC_CMOVNL, ZYDIS_MNEMONIC_CMOVLE, ZYDIS_MNEMONIC_CMOVNLE};
        add(kSet[cc], {Reg(cc & 1 ? ZYDIS_REGISTER_DH : ZYDIS_REGISTER_BL)});
        add(kCmov[cc], {Reg(r32[cc % 7]), Reg(r32[(cc + 3) % 7])});
    }

    // sse
    auto X = [](int i) { return Reg(ZydisRegister(ZYDIS_REGISTER_XMM0 + i)); };
    for (M m : {ZYDIS_MNEMONIC_ADDSS, ZYDIS_MNEMONIC_SUBSS, ZYDIS_MNEMONIC_MULSS, ZYDIS_MNEMONIC_DIVSS,
                ZYDIS_MNEMONIC_MINSS, ZYDIS_MNEMONIC_MAXSS, ZYDIS_MNEMONIC_SQRTSS, ZYDIS_MNEMONIC_ADDPS,
                ZYDIS_MNEMONIC_SUBPS, ZYDIS_MNEMONIC_MULPS, ZYDIS_MNEMONIC_DIVPS, ZYDIS_MNEMONIC_MINPS,
                ZYDIS_MNEMONIC_MAXPS, ZYDIS_MNEMONIC_SQRTPS, ZYDIS_MNEMONIC_ANDPS, ZYDIS_MNEMONIC_ANDNPS,
                ZYDIS_MNEMONIC_ORPS, ZYDIS_MNEMONIC_XORPS, ZYDIS_MNEMONIC_UNPCKLPS, ZYDIS_MNEMONIC_UNPCKHPS,
                ZYDIS_MNEMONIC_MOVLHPS, ZYDIS_MNEMONIC_MOVHLPS, ZYDIS_MNEMONIC_MOVSS, ZYDIS_MNEMONIC_MOVAPS,
                ZYDIS_MNEMONIC_COMISS, ZYDIS_MNEMONIC_UCOMISS}) {
        add(m, {X(1), X(2)});
        add(m, {X(3), X(3)});
    }
    add(ZYDIS_MNEMONIC_SHUFPS, {X(0), X(5), Imm(0x1B)});
    add(ZYDIS_MNEMONIC_SHUFPS, {X(4), X(4), Imm(0xE4)});
    add(ZYDIS_MNEMONIC_CVTSI2SS, {X(2), Reg(ZYDIS_REGISTER_EDX)});
    add(ZYDIS_MNEMONIC_CVTTSS2SI, {Reg(ZYDIS_REGISTER_EAX), X(6)});
    add(ZYDIS_MNEMONIC_CVTSS2SI, {Reg(ZYDIS_REGISTER_ECX), X(7)});

    // mmx
    auto MM = [](int i) { return Reg(ZydisRegister(ZYDIS_REGISTER_MM0 + i)); };
    for (M m : {ZYDIS_MNEMONIC_PADDW, ZYDIS_MNEMONIC_PADDD, ZYDIS_MNEMONIC_PSUBW, ZYDIS_MNEMONIC_PSUBD,
                ZYDIS_MNEMONIC_PADDSW, ZYDIS_MNEMONIC_PSUBSW, ZYDIS_MNEMONIC_PADDUSW, ZYDIS_MNEMONIC_PSUBUSW,
                ZYDIS_MNEMONIC_PMULLW, ZYDIS_MNEMONIC_PMULHW, ZYDIS_MNEMONIC_PMADDWD, ZYDIS_MNEMONIC_PAND,
                ZYDIS_MNEMONIC_POR, ZYDIS_MNEMONIC_PXOR, ZYDIS_MNEMONIC_PANDN, ZYDIS_MNEMONIC_PCMPEQD,
                ZYDIS_MNEMONIC_PCMPGTW, ZYDIS_MNEMONIC_PUNPCKLBW, ZYDIS_MNEMONIC_PUNPCKHBW, ZYDIS_MNEMONIC_PUNPCKLWD,
                ZYDIS_MNEMONIC_PUNPCKHWD, ZYDIS_MNEMONIC_PUNPCKLDQ, ZYDIS_MNEMONIC_PUNPCKHDQ, ZYDIS_MNEMONIC_PACKSSDW,
                ZYDIS_MNEMONIC_PACKSSWB, ZYDIS_MNEMONIC_PSLLW, ZYDIS_MNEMONIC_PSLLD, ZYDIS_MNEMONIC_PSLLQ,
                ZYDIS_MNEMONIC_PSRLW, ZYDIS_MNEMONIC_PSRLD, ZYDIS_MNEMONIC_PSRLQ, ZYDIS_MNEMONIC_PSRAW,
                ZYDIS_MNEMONIC_PSRAD, ZYDIS_MNEMONIC_MOVQ})
        add(m, {MM(1), MM(2)});
    for (M m : {ZYDIS_MNEMONIC_PSLLW, ZYDIS_MNEMONIC_PSLLD, ZYDIS_MNEMONIC_PSLLQ, ZYDIS_MNEMONIC_PSRLW,
                ZYDIS_MNEMONIC_PSRLD, ZYDIS_MNEMONIC_PSRLQ, ZYDIS_MNEMONIC_PSRAW, ZYDIS_MNEMONIC_PSRAD}) {
        add(m, {MM(3), Imm(5)});
        add(m, {MM(4), Imm(17)});
    }
    add(ZYDIS_MNEMONIC_PSHUFW, {MM(0), MM(6), Imm(0x93)});
    add(ZYDIS_MNEMONIC_PMOVMSKB, {Reg(ZYDIS_REGISTER_EDX), MM(5)});
    add(ZYDIS_MNEMONIC_MOVD, {MM(7), Reg(ZYDIS_REGISTER_EBX)});
    add(ZYDIS_MNEMONIC_MOVD, {Reg(ZYDIS_REGISTER_ESI), MM(2)});
    return c;
}

}  // namespace

int WriteSemanticsTest(Codegen& cg, const std::string& path) {
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32);
    ZydisFormatter fmt;
    ZydisFormatterInit(&fmt, ZYDIS_FORMATTER_STYLE_INTEL);
    Function dummy;
    FunctionWriter writer(cg, dummy);

    std::string funcs, table;
    int n = 0, skipped = 0;
    for (const auto& req : Cases()) {
        std::vector<uint8_t> b32, b64;
        if (!Encode(req, ZYDIS_MACHINE_MODE_LEGACY_32, &b32) || !Encode(req, ZYDIS_MACHINE_MODE_LONG_64, &b64)) {
            std::printf("semtest: cannot encode %s\n", ZydisMnemonicGetString(req.mnemonic));
            skipped++;
            continue;
        }
        Decoded d;
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, b32.data(), b32.size(), &d.ins, d.ops))) {
            skipped++;
            continue;
        }
        std::string body = writer.EmitOne(d);
        char text[96];
        ZydisFormatterFormatInstruction(&fmt, &d.ins, d.ops, d.ins.operand_count_visible, text, sizeof text, 0, nullptr);
        if (body.empty()) {
            std::printf("semtest: no translation for %s\n", text);
            skipped++;
            continue;
        }
        uint32_t tested = 0, undefined = 0;
        if (d.ins.cpu_flags) {
            tested = (d.ins.cpu_flags->modified | d.ins.cpu_flags->set_0 | d.ins.cpu_flags->set_1) & kStatusFlags;
            undefined = d.ins.cpu_flags->undefined & kStatusFlags;
        }
        // as in a recompiled function: registers in locals, written back at the end (also after a return)
        funcs += F("static void Emu%d(Cpu& c, Flags& f) { FCI_TEST_IN [&] { %s}(); FCI_SAVE }\n", n, body.c_str());
        std::string bytes;
        for (uint8_t v : b64) bytes += F("0x%02X,", v);
        table += F("    {\"%s\", Emu%d, {%s}, %zu, 0x%Xu, 0x%Xu, %d},\n", text, n, bytes.c_str(), b64.size(), tested,
                   undefined, int(d.ins.mnemonic));
        n++;
    }

    std::ofstream out(path);
    out << "// generated by fci-recomp --semtest\n#include \"semtest/harness.h\"\n\nusing namespace rt;\n\n";
    std::string in = "#define FCI_TEST_IN", save = "#define FCI_SAVE", load = "#define FCI_LOAD";
    for (const char* r : {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"}) {
        in += F(" uint32_t %s = c.%s;", r, r);
        save += F(" c.%s = %s;", r, r);
        load += F(" %s = c.%s;", r, r);
    }
    for (int x = 0; x < 8; x++) {
        in += F(" Xmm x%d = c.xmm[%d];", x, x);
        save += F(" c.xmm[%d] = x%d;", x, x);
        load += F(" x%d = c.xmm[%d];", x, x);
    }
    out << in << "\n" << save << "\n" << load << "\n\n";
    out << funcs << "\nconst TestCase kCases[] = {\n" << table << "};\nconst size_t kCaseCount = " << n << ";\n";
    std::printf("semtest: %d cases written to %s, %d skipped\n", n, path.c_str(), skipped);
    return n;
}

}  // namespace recomp
