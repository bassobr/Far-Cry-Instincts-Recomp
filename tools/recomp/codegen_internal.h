// Shared between the function level (codegen.cpp) and instruction level (codegen_insn.cpp) emitters.
#pragma once

#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "codegen.h"

namespace recomp {

// printf into a std::string; emitted C++ is full of braces, which rules out std::format
std::string F(const char* fmt, ...);

// the status flags the liveness analysis tracks
// must match rt::kBaseCheck: a base below kMmioBase - kBaseDispLimit plus a displacement below the limit is RAM
constexpr uint32_t kBaseDispLimit = 0x01000000;

// switches in the environment of fci-recomp (generated code changes, so the PGO profile with it):
//   FCI_RECOMP_BASE_CHECK=0  [base + disp] accesses with the hardware check on each address again instead of through
//                            the base register (BaseR32 ...; default on: 5 % less code, Deck guest -2.4 %)
//   FCI_RECOMP_XMM_LIVE=1    xmm liveness across calls/returns (xmm_live.cpp; off: no measurable gain:
//                            the unions over indirect calls keep nearly every register live)
//   FCI_RECOMP_EBP_FRAME=0   [ebp + disp] accesses where ebp is a frame pointer checked again like any other base
//                            (default: RAM without a check, as through esp)
inline bool XmmLiveOn() { static const bool on = std::getenv("FCI_RECOMP_XMM_LIVE") != nullptr; return on; }
inline bool EbpFrameOn() {
    static const bool on = [] {
        const char* v = std::getenv("FCI_RECOMP_EBP_FRAME");
        return !v || std::strcmp(v, "0") != 0;
    }();
    return on;
}
inline bool BaseCheckOn() {
    static const bool on = [] {
        const char* v = std::getenv("FCI_RECOMP_BASE_CHECK");
        return !v || std::strcmp(v, "0") != 0;
    }();
    return on;
}

constexpr uint32_t kStatusFlags = ZYDIS_CPUFLAG_CF | ZYDIS_CPUFLAG_PF | ZYDIS_CPUFLAG_AF | ZYDIS_CPUFLAG_ZF |
                                  ZYDIS_CPUFLAG_SF | ZYDIS_CPUFLAG_OF;

struct Decoded {
    uint32_t va = 0;
    ZydisDecodedInstruction ins{};
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT]{};
    std::vector<uint32_t> succ;  // successors inside the function
    bool falls_through = true;
    bool exits = false;     // control may leave the function without a call (flags pass through Cpu)
    uint32_t live_out = 0;  // status flags read after this instruction
    bool ebp_frame = false;  // FramePointer: ebp holds a stack address (set from esp) on every path to here
};

class FunctionWriter {
public:
    FunctionWriter(Codegen& cg, const Function& fn) : cg_(cg), fn_(fn) {}
    std::string Write();
    void XmmSummarize(XmmFunction* out);  // xmm_live.cpp
    // AnalyzeFlags: the flags this function reads on entry; the direct call targets whose returned flags it reads (with
    // the flags), every function it calls or jumps into, and those it jumps (or falls) into
    uint32_t FlagSummary(std::vector<std::pair<uint32_t, uint32_t>>* returns, std::vector<uint32_t>* targets,
                         std::vector<uint32_t>* tails);
    // the statements of one instruction with every flag it defines computed (for the semantics test);
    // empty when it has no translation
    std::string EmitOne(Decoded d);

private:
    void Prepare();
    void Liveness();
    void FramePointer();
    uint32_t ExitFlags(const Decoded& d) const;  // flags live where d leaves the function without a call

    // instruction level, codegen_insn.cpp; false when the instruction has no translation
    bool EmitInsn(const Decoded& d);
    bool EmitBranch(const Decoded& d, bool* handled);
    bool EmitSse(const Decoded& d, bool* handled);
    bool EmitMmx(const Decoded& d, bool* handled);
    bool EmitX87(const Decoded& d, bool* handled);

    void S(const std::string& stmt) { body_ += stmt + " "; }
    static std::string Localize(const std::string& in, uint32_t* used, uint32_t* written);
    // accesses through esp, and through ea when ea_ram_, without the hardware register check
    std::string RamAccesses(const std::string& in) const;
    std::string Jump(uint32_t target) const;
    std::string Branch(uint32_t from, uint32_t target) const;  // Jump, plus a preemption point on loops
    std::string CallTo(uint32_t target, uint32_t ret) const;
    int KernelOrdinal(const ZydisDecodedOperand& op) const;  // -1 unless op is a kernel thunk slot

    // operand access for the current instruction
    std::string Ea(const ZydisDecodedOperand& op) const;
    std::string Rd(int i, int bits = 0) const;
    std::string Wr(int i, const std::string& v) const;
    std::string Xf(int i) const;   // scalar float of an xmm or m32 operand
    std::string Xv(int i) const;   // whole xmm or m128 operand
    std::string Mv(int i) const;   // mm, m64 or m32 operand as uint64_t
    std::string Fv(int i) const;   // x87 operand as double
    std::string Iv(int i) const;   // x87 integer memory operand as double
    int Xmm(int i) const { return cur_->ops[i].reg.value - ZYDIS_REGISTER_XMM0; }
    int Mm(int i) const { return cur_->ops[i].reg.value - ZYDIS_REGISTER_MM0; }
    int St(int i) const { return cur_->ops[i].reg.value - ZYDIS_REGISTER_ST0; }

    Codegen& cg_;
    const Function& fn_;
    std::vector<Decoded> insns_;
    std::unordered_map<uint32_t, size_t> index_;
    std::set<uint32_t> labels_;
    std::string out_;

    const Decoded* cur_ = nullptr;
    uint32_t need_ = 0;  // flags the current instruction has to produce
    bool ea_ram_ = false;  // the explicit memory operand is RAM for sure (stack, constant address)
    bool ea_base_ = false;  // it is [eb + displacement below kBaseDispLimit]: checked through the base register
    std::string body_;
    uint32_t entry_flags_ = 0;  // Liveness: the status flags read on entry before they are set
};

const char* Cond(ZydisMnemonic m);  // C++ condition over `f` for jcc/setcc/cmovcc/fcmovcc, nullptr otherwise

}  // namespace recomp
