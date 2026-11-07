// Control flow analysis of the XBE: finds functions, their instructions and jump tables.
#pragma once

#include <Zydis/Zydis.h>

#include <cstdint>
#include <iosfwd>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "xbe/xbe.h"

namespace recomp {

struct Function {
    uint32_t start = 0;
    uint32_t end = 0;  // one past the highest decoded byte
    uint32_t insn_count = 0;
    std::set<uint32_t> blocks;  // basic block starts, including `start`
    bool from_pointer = false;  // only found through a code pointer in data or an immediate
    bool suspicious = false;    // contains instructions no Xbox title executes, or undecodable bytes
    std::set<uint32_t> calls;     // direct call and tail jump targets
    std::set<uint32_t> pointers;  // code addresses used as immediates
};

struct Insn {
    uint8_t length = 0;
    ZydisMnemonic mnemonic = ZYDIS_MNEMONIC_INVALID;
};

class Analysis {
public:
    Analysis(const xbe::Image& image, std::vector<std::string> code_sections);

    void Run();
    // functions known by name (XDK symbols) that are entry points even if nothing calls them directly
    void AddEntry(uint32_t va) { extra_entries_.push_back(va); }
    void WriteReport(const std::string& path) const;
    // every distinct mnemonic and operand shape with an example, to check codegen coverage
    void WriteShapes(const std::string& path) const;

    bool IsCode(uint32_t va) const;
    bool Decode(uint32_t va, ZydisDecodedInstruction* ins, ZydisDecodedOperand* ops) const;
    // where execution continues after a non-branching instruction
    uint32_t NextAddress(uint32_t va, const ZydisDecodedInstruction& ins, const ZydisDecodedOperand* ops) const;

    std::map<uint32_t, Function> functions;
    std::map<uint32_t, Insn> insns;  // every decoded instruction, by address
    std::map<uint32_t, std::vector<uint32_t>> jump_tables;  // table address -> targets
    std::set<uint32_t> bad_decodes;
    std::set<uint32_t> kernel_calls;  // call sites through the kernel thunk
    size_t dropped = 0;  // functions discarded as data decoded as code
    std::map<uint32_t, uint32_t> packed_pointers;  // code pointers found in packed structures: where -> target

private:
    // what a code address points at
    enum class Target { kFunction, kInsn, kMidInsn, kUnreached, kGarbage, kNoCode };
    Target Classify(uint32_t va) const;
    // the first instructions decode as plausible code (no zero fill, no int3, nothing implausible)
    bool DecodesAsCode(uint32_t va) const;
    void WritePointerAudit(std::ofstream& out) const;
    bool LooksLikeFunctionStart(uint32_t va) const;
    void Queue(uint32_t va, bool from_pointer);
    void Explore(Function& fn);
    void ScanDataPointers();
    size_t ScanUnreachedPointers();
    // code pointers in byte-packed structures the code takes the address of: where -> code address
    std::map<uint32_t, uint32_t> PackedStructPointers() const;
    bool InsideInstruction(uint32_t va) const;
    void Drain();
    void DropDataAsCode();

    std::set<uint32_t> data_pointer_roots_;
    std::vector<uint32_t> extra_entries_;
    std::vector<uint32_t> ReadJumpTable(uint32_t table, uint32_t jump) const;

    const xbe::Image& image_;
    std::vector<const xbe::Section*> code_;
    std::vector<std::pair<uint32_t, bool>> queue_;
    ZydisDecoder decoder_;
};

}  // namespace recomp
