// fci-semtest: runs each instruction both as recompiled C++ and natively on the host CPU with random
// inputs and reports where registers or flags differ.
#include <windows.h>

#include <Zydis/Zydis.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "semtest/harness.h"

using rt::Cpu;
using rt::Flags;
using rt::Xmm;

// the runtime functions generated code may reference
namespace rt {
volatile int g_preempt = 0;
void Preempt(Cpu& FCI_NOESCAPE) {}
void Call(Cpu& FCI_NOESCAPE, uint32_t) {}
DispatchEntry const* g_dispatch = nullptr;
uint32_t g_dispatch_base = 0, g_dispatch_size = 0;
void KernelCall(Cpu& FCI_NOESCAPE, uint32_t) {}
void Unimplemented(Cpu& FCI_NOESCAPE, uint32_t, const char*) {}
void DivideError(Cpu& FCI_NOESCAPE) {}
uint64_t ReadTsc() { return 0; }
void Cpuid(Cpu& FCI_NOESCAPE) {}
void DebugService(Cpu& FCI_NOESCAPE) {}
uint32_t PortIn(Cpu& FCI_NOESCAPE, uint16_t, int) { return 0; }
void PortOut(Cpu& FCI_NOESCAPE, uint16_t, uint32_t, int) {}
extern "C" uint32_t FCI_MMIO_CC MmioRead(uint32_t, unsigned) { return 0; }
extern "C" void FCI_MMIO_CC MmioWrite(uint32_t, uint32_t, unsigned) {}
extern "C" uint32_t FCI_MMIO_CC GuestRead(uint32_t a, unsigned size) {
    uint32_t v = 0;
    std::memcpy(&v, Host(a), size);
    return v;
}
extern "C" void FCI_MMIO_CC GuestWrite(uint32_t a, uint32_t v, unsigned size) { std::memcpy(Host(a), &v, size); }
double LoadF80(uint32_t) { return 0; }
void StoreF80(uint32_t, double) {}
void FSave(Cpu& FCI_NOESCAPE, uint32_t) {}
void FRestore(Cpu& FCI_NOESCAPE, uint32_t) {}
void SetMxcsr(Cpu& FCI_NOESCAPE c, uint32_t v) { c.mxcsr = v; }
int32_t CvtToInt(const Cpu& FCI_NOESCAPE c, float v) { return FToInt<int32_t>(std::nearbyint(double(v))); }
const FunctionEntry kFunctions[1] = {};
const size_t kFunctionCount = 0;
}  // namespace rt

namespace {

// register file the native stub loads from and stores to
struct State {
    uint32_t r[8];  // eax ecx edx ebx esp ebp esi edi (esp unused)
    uint64_t flags;
    uint64_t pad;
    Xmm xmm[8];
    uint64_t mm[8];
};
constexpr int kFlagsOff = offsetof(State, flags), kXmmOff = offsetof(State, xmm), kMmOff = offsetof(State, mm);

struct Emitter {
    std::vector<uint8_t> b;
    void Bytes(std::initializer_list<int> v) {
        for (int x : v) b.push_back(uint8_t(x));
    }
    void Disp32(int d) {
        for (int i = 0; i < 4; i++) b.push_back(uint8_t(d >> (8 * i)));
    }
    // op [r15 + disp32] with the reg field `reg`
    void R15(std::initializer_list<int> prefix_and_op, int reg, int disp) {
        Bytes(prefix_and_op);
        b.push_back(uint8_t(0x80 | ((reg & 7) << 3) | 7));
        Disp32(disp);
    }
};

using Stub = void (*)(State*);

Stub BuildStub(const TestCase& t) {
    Emitter e;
    e.Bytes({0x53, 0x55, 0x56, 0x57, 0x41, 0x57});              // push rbx, rbp, rsi, rdi, r15
    e.Bytes({0x48, 0x83, 0xEC, 0x28});                          // sub rsp, 0x28
    e.Bytes({0x0F, 0x11, 0x34, 0x24});                          // movups [rsp], xmm6
    e.Bytes({0x0F, 0x11, 0x7C, 0x24, 0x10});                    // movups [rsp+16], xmm7
    e.Bytes({0x49, 0x89, 0xCF});                                // mov r15, rcx
    for (int i = 0; i < 8; i++) e.R15({0x41, 0x0F, 0x10}, i, kXmmOff + 16 * i);  // movups xmmi, [r15+..]
    for (int i = 0; i < 8; i++) e.R15({0x41, 0x0F, 0x6F}, i, kMmOff + 8 * i);    // movq mmi, [r15+..]
    e.R15({0x41, 0xFF}, 6, kFlagsOff);                          // push qword [r15+flags]
    e.Bytes({0x9D});                                            // popfq
    for (int i = 0; i < 8; i++)
        if (i != 4) e.R15({0x41, 0x8B}, i, 4 * i);              // mov r32, [r15+..]
    for (size_t i = 0; i < t.length; i++) e.b.push_back(t.native[i]);
    e.Bytes({0x9C});                                            // pushfq
    e.R15({0x41, 0x8F}, 0, kFlagsOff);                          // pop qword [r15+flags]
    for (int i = 0; i < 8; i++)
        if (i != 4) e.R15({0x41, 0x89}, i, 4 * i);              // mov [r15+..], r32
    for (int i = 0; i < 8; i++) e.R15({0x41, 0x0F, 0x11}, i, kXmmOff + 16 * i);
    for (int i = 0; i < 8; i++) e.R15({0x41, 0x0F, 0x7F}, i, kMmOff + 8 * i);
    e.Bytes({0x0F, 0x77, 0xFC});                                // emms, cld
    e.Bytes({0x0F, 0x10, 0x34, 0x24});                          // movups xmm6, [rsp]
    e.Bytes({0x0F, 0x10, 0x7C, 0x24, 0x10});                    // movups xmm7, [rsp+16]
    e.Bytes({0x48, 0x83, 0xC4, 0x28});                          // add rsp, 0x28
    e.Bytes({0x41, 0x5F, 0x5F, 0x5E, 0x5D, 0x5B, 0xC3});        // pop r15, rdi, rsi, rbp, rbx; ret
    void* mem = VirtualAlloc(nullptr, e.b.size(), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    std::memcpy(mem, e.b.data(), e.b.size());
    return reinterpret_cast<Stub>(mem);
}

uint64_t g_seed = 0x9E3779B97F4A7C15ull;
uint64_t Next() {
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 7;
    g_seed ^= g_seed << 17;
    return g_seed;
}

uint32_t RandomValue() {
    static const uint32_t kEdges[] = {0, 1, 2, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF, 0x7FFFFFFF, 0x80000000,
                                      0xFFFFFFFF, 0xFFFFFFFE, 31, 32, 33};
    switch (Next() % 4) {
    case 0: return kEdges[Next() % (sizeof kEdges / sizeof kEdges[0])];
    case 1: return uint32_t(Next() % 64);
    default: return uint32_t(Next());
    }
}

float RandomFloat() {
    static const float kEdges[] = {0.0f, -0.0f, 1.0f, -1.0f, 0.5f, 3.0f, 1e30f, -1e-30f, INFINITY, -INFINITY,
                                   NAN, 2147483648.0f, -2147483904.0f, 1e-40f};
    if (Next() % 3 == 0) return kEdges[Next() % (sizeof kEdges / sizeof kEdges[0])];
    return float(int32_t(Next())) / float(1 + Next() % 1000);
}

uint32_t PackFlags(const Flags& f) {
    return (f.cf ? 0x1 : 0) | (f.pf ? 0x4 : 0) | (f.af ? 0x10 : 0) | (f.zf ? 0x40 : 0) | (f.sf ? 0x80 : 0) | (f.of ? 0x800 : 0);
}

bool SameXmm(const Xmm& a, const Xmm& b, bool* nan_only) {
    for (int i = 0; i < 4; i++) {
        if (a.u[i] == b.u[i]) continue;
        if (std::isnan(a.f[i]) && std::isnan(b.f[i])) {
            *nan_only = true;
            continue;
        }
        return false;
    }
    return true;
}

bool IsShift(int m) {
    return m == ZYDIS_MNEMONIC_SHL || m == ZYDIS_MNEMONIC_SHR || m == ZYDIS_MNEMONIC_SAR || m == ZYDIS_MNEMONIC_ROL ||
           m == ZYDIS_MNEMONIC_ROR || m == ZYDIS_MNEMONIC_RCL || m == ZYDIS_MNEMONIC_RCR || m == ZYDIS_MNEMONIC_SHLD ||
           m == ZYDIS_MNEMONIC_SHRD;
}

}  // namespace

int main(int argc, char** argv) {
    int iterations = argc > 1 ? std::atoi(argv[1]) : 2000;
    int failed_cases = 0;
    for (size_t ci = 0; ci < kCaseCount; ci++) {
        const TestCase& t = kCases[ci];
        Stub stub = BuildStub(t);
        int failures = 0;
        for (int it = 0; it < iterations && failures < 3; it++) {
            State in{};
            for (int i = 0; i < 8; i++) in.r[i] = RandomValue();
            if (t.mnemonic == ZYDIS_MNEMONIC_DIV) {
                in.r[2] = in.r[1] ? uint32_t(Next() % in.r[1]) : 0;  // edx < divisor
                if (!in.r[1]) in.r[1] = 3;
            } else if (t.mnemonic == ZYDIS_MNEMONIC_IDIV) {
                in.r[2] = int32_t(in.r[0]) < 0 ? 0xFFFFFFFF : 0;  // edx:eax = sign extended eax
                if (in.r[3] == 0 || in.r[3] == 0xFFFFFFFF) in.r[3] = 7;
            }
            in.flags = 0x202 | (Next() & 0x8D5);
            for (int i = 0; i < 8; i++)
                for (int l = 0; l < 4; l++) in.xmm[i].f[l] = RandomFloat();
            for (int i = 0; i < 8; i++) in.mm[i] = (uint64_t(RandomValue()) << 32) | RandomValue();

            State native = in;
            stub(&native);

            Cpu c{};
            uint32_t* regs[8] = {&c.eax, &c.ecx, &c.edx, &c.ebx, &c.esp, &c.ebp, &c.esi, &c.edi};
            for (int i = 0; i < 8; i++) *regs[i] = in.r[i];
            for (int i = 0; i < 8; i++) c.xmm[i] = in.xmm[i];
            for (int i = 0; i < 8; i++) c.mm[i] = in.mm[i];
            Flags f;
            f.cf = in.flags & 0x1;
            f.pf = in.flags & 0x4;
            f.af = in.flags & 0x10;
            f.zf = in.flags & 0x40;
            f.sf = in.flags & 0x80;
            f.of = in.flags & 0x800;
            t.emulate(c, f);

            uint32_t compare = 0x8D5 & ~t.undefined;
            if (IsShift(t.mnemonic)) {
                uint32_t count = (t.mnemonic == ZYDIS_MNEMONIC_SHLD || t.mnemonic == ZYDIS_MNEMONIC_SHRD) ? 0 : 1;
                (void)count;
                compare &= ~0x800u;  // OF is only defined for single bit shifts
                compare &= ~0x10u;
            }
            bool bad = false;
            char what[512] = {};
            for (int i = 0; i < 8; i++) {
                if (i == 4) continue;
                if (t.mnemonic == ZYDIS_MNEMONIC_BSF && in.r[3] == 0) continue;  // destination undefined
                if (*regs[i] != native.r[i]) {
                    bad = true;
                    snprintf(what + strlen(what), sizeof what - strlen(what), " r%d=%08X/%08X", i, *regs[i], native.r[i]);
                }
            }
            uint32_t ef = PackFlags(f) & compare, nf = uint32_t(native.flags) & compare;
            if (ef != nf) {
                bad = true;
                snprintf(what + strlen(what), sizeof what - strlen(what), " flags=%03X/%03X", ef, nf);
            }
            for (int i = 0; i < 8; i++) {
                bool nan_only = false;
                if (!SameXmm(c.xmm[i], native.xmm[i], &nan_only)) {
                    bad = true;
                    snprintf(what + strlen(what), sizeof what - strlen(what), " xmm%d", i);
                }
                if (c.mm[i] != native.mm[i]) {
                    bad = true;
                    snprintf(what + strlen(what), sizeof what - strlen(what), " mm%d=%016llX/%016llX", i,
                             (unsigned long long)c.mm[i], (unsigned long long)native.mm[i]);
                }
            }
            if (bad) {
                if (!failures) failed_cases++;
                failures++;
                std::printf("FAIL %-32s in r=%08X %08X %08X %08X flags %03X:%s\n", t.text, in.r[0], in.r[1], in.r[2],
                            in.r[3], unsigned(in.flags & 0x8D5), what);
            }
        }
    }
    std::printf("%zu cases, %d failing\n", kCaseCount, failed_cases);
    return failed_cases ? 1 : 0;
}
