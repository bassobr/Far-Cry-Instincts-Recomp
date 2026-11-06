// Emits C++ for every function the analysis found.
#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "analysis.h"

namespace recomp {

// xmm liveness (xmm_live.cpp): masks with bit r = lane 0 of xmm r, bit 8 + r = its lanes 1-3
struct XmmExit {
    enum Kind : uint8_t { kRet, kTail, kIndirect, kUnknown } kind = kUnknown;
    uint32_t target = 0;  // kTail: the function jumped to
};
struct XmmNode {
    enum Call : uint8_t { kNone, kDirect, kSetjmp, kIndirect, kUnknown, kKernel } call = kNone;
    uint16_t use = 0, def = 0;
    uint32_t target = 0;          // kDirect, kSetjmp
    std::vector<uint32_t> succ;   // instruction indices inside the function
    std::vector<XmmExit> exits;   // leaving the function in the instruction itself (ret, branch out)
    XmmExit fall;                 // falling through out of the function (into the next one)
    bool has_fall = false;
};
struct XmmFunction {
    uint32_t entry = 0;
    std::vector<XmmNode> nodes;
    std::vector<uint32_t> vas;  // guest address of each node
};
struct XmmSummary {
    uint16_t reads = 0, exit = 0;
};

// what the executable needs from the configuration alone (no xbe): image_symbols.h (SYM_<name>, config/symbols*.txt)
// and hle_hooks.h/.cpp (the host replacements it exports to the DLL, config/hle.txt and hooks.h)
void WriteHostHeaders(const std::filesystem::path& dir);

class Codegen {
public:
    Codegen(const xbe::Image& image, const Analysis& analysis) : image_(image), analysis_(analysis) {}

    // writes funcs.h, table.cpp and code_NNN.cpp into `dir`; xbe_hash (xbe::ContentHash of the
    // file translated) goes into the DLL's rt::GameCode
    void Write(const std::filesystem::path& dir, int partitions, uint64_t xbe_hash);

    // instructions emitted as runtime traps, by mnemonic
    std::map<std::string, size_t> unimplemented;

    const xbe::Image& image() const { return image_; }
    const Analysis& analysis() const { return analysis_; }

    // whether kFtol2 holds the CRT's _ftol2 (its first instructions), so calls to it can be emitted inline
    bool IsFtol2();

    // xmm liveness over the whole program (xmm_live.cpp); then, per function, what is live where
    void AnalyzeXmm();
    const XmmFunction* XmmOf(uint32_t start) const {
        auto it = xmm_functions_.find(start);
        return it == xmm_functions_.end() ? nullptr : &it->second;
    }
    // with_exit: what callers need after this function returns counts too (masks for the code); without: only what it
    // reads itself before writing (its reads summary; callers keep what they need after a call themselves)
    void XmmFlow(uint32_t start, std::vector<uint16_t>* in, std::vector<uint16_t>* out, bool with_exit = true) const;
    uint16_t XmmExitLive(const XmmExit& e, uint32_t self, bool with_exit = true) const;
    uint16_t XmmCallReads(const XmmNode& n) const;
    uint16_t XmmReads(uint32_t start) const { return xmm_[start].reads; }

private:
    const xbe::Image& image_;
    const Analysis& analysis_;
    int ftol2_ = -1;
    std::unordered_map<uint32_t, XmmFunction> xmm_functions_;
    mutable std::unordered_map<uint32_t, XmmSummary> xmm_;
    uint16_t xmm_indirect_reads_ = 0, xmm_indirect_exit_ = 0;
    std::unordered_map<uint32_t, bool> xmm_address_taken_;  // may be reached through a code pointer
};

}  // namespace recomp
