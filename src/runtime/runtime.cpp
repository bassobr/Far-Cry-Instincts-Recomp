// Dispatch of indirect calls and the helpers generated code calls out to.
#include <intrin.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <unordered_map>
#include <cmath>
#include <string>
#include <vector>

#include "runtime/guest.h"
#include "runtime/guest_time.h"  // PC comfort features: focus pause
#include "runtime/log.h"
#include "runtime/runtime.h"
#include "runtime/thread.h"

// The C runtime functions the game code DLL calls (it links without one, see runtime/freestanding): the executable's
// own static C runtime, exported by name. The generated setjmp and the longjmp in Call below are thus one
// implementation, and the math the same code as when the game code was linked into the executable.
#pragma comment(linker, "/EXPORT:_setjmp")
#pragma comment(linker, "/EXPORT:memcpy")
#pragma comment(linker, "/EXPORT:memmove")
#pragma comment(linker, "/EXPORT:memset")
#pragma comment(linker, "/EXPORT:memcmp")
#pragma comment(linker, "/EXPORT:sqrt")
#pragma comment(linker, "/EXPORT:sqrtf")
#pragma comment(linker, "/EXPORT:sin")
#pragma comment(linker, "/EXPORT:sinf")
#pragma comment(linker, "/EXPORT:cos")
#pragma comment(linker, "/EXPORT:cosf")
#pragma comment(linker, "/EXPORT:tan")
#pragma comment(linker, "/EXPORT:tanf")
#pragma comment(linker, "/EXPORT:atan")
#pragma comment(linker, "/EXPORT:atan2")
#pragma comment(linker, "/EXPORT:atan2f")
#pragma comment(linker, "/EXPORT:exp")
#pragma comment(linker, "/EXPORT:exp2")
#pragma comment(linker, "/EXPORT:log")
#pragma comment(linker, "/EXPORT:log2")
#pragma comment(linker, "/EXPORT:log10")
#pragma comment(linker, "/EXPORT:pow")
#pragma comment(linker, "/EXPORT:floor")
#pragma comment(linker, "/EXPORT:floorf")
#pragma comment(linker, "/EXPORT:ceil")
#pragma comment(linker, "/EXPORT:ceilf")
#pragma comment(linker, "/EXPORT:trunc")
#pragma comment(linker, "/EXPORT:truncf")
#pragma comment(linker, "/EXPORT:round")
#pragma comment(linker, "/EXPORT:nearbyint")
#pragma comment(linker, "/EXPORT:nearbyintf")
#pragma comment(linker, "/EXPORT:rint")
#pragma comment(linker, "/EXPORT:fmod")
#pragma comment(linker, "/EXPORT:fmodf")
#pragma comment(linker, "/EXPORT:remainder")
#pragma comment(linker, "/EXPORT:ldexp")
#pragma comment(linker, "/EXPORT:frexp")
#pragma comment(linker, "/EXPORT:__chkstk")

namespace rt {

namespace {

std::vector<GuestFunction> g_table;  // indexed by guest address - g_base
uint32_t g_base = 0;
volatile int g_preempt_stand_in = 0;

}  // namespace

volatile int* g_preempt_flag = &g_preempt_stand_in;

void InitDispatch(const GameCode& code, uint32_t image_base, uint32_t image_size) {
    g_table.assign(image_size, nullptr);
    for (uint32_t i = 0; i < code.function_count; i++) {
        uint32_t a = code.functions[i].address;
        if (a >= image_base && a - image_base < image_size) g_table[a - image_base] = code.functions[i].function;
    }
    g_base = image_base;
    // the generated code reads the DLL's copies
    *code.dispatch = g_table.data();
    *code.dispatch_base = image_base;
    *code.dispatch_size = image_size;
    *code.preempt = *g_preempt_flag;
    g_preempt_flag = code.preempt;
    Log("dispatch: %u functions", code.function_count);
}

GuestFunction Lookup(uint32_t address) {
    uint32_t i = address - g_base;
    return i < g_table.size() ? g_table[i] : nullptr;
}

namespace {
struct LandingPad {
    alignas(16) jmp_buf host;
};
// Built on a thread's first use, not at namespace scope: there its constructor (which allocates) ran from the
// executable's TLS callback for every new thread, also for the system's thread pool workers that start while the
// main thread is still initializing the C runtime, before its heap exists. Access violation in RtlAllocateHeap
// (heap handle 0) on almost every start: the loader swallowed it, Windows logged an "Application Error".
using Landings = std::unordered_map<uint64_t, std::unique_ptr<LandingPad>>;
Landings& ThreadLandings() {
    thread_local Landings landings;
    return landings;
}
uint64_t LandingKey(uint32_t pc, uint32_t esp) { return (uint64_t(pc) << 32) | esp; }
}  // namespace

jmp_buf* Landing(Cpu& FCI_NOESCAPE c, uint32_t pc) {
    auto& pad = ThreadLandings()[LandingKey(pc, c.esp)];
    if (!pad) pad = std::make_unique<LandingPad>();
    return &pad->host;
}

void Call(Cpu& FCI_NOESCAPE c, uint32_t target) {
    if (GuestFunction f = Lookup(target)) return f(c);
    if (target >= kKernelThunkBase && target < kKernelThunkBase + 0x1000) return KernelCall(c, (target - kKernelThunkBase) / 4);
    // the guest's longjmp: back into the frame of its setjmp, whose host frame is still live below us
    Landings& landings = ThreadLandings();
    if (auto it = landings.find(LandingKey(target, c.esp)); it != landings.end()) {
        static std::atomic<uint32_t> landed{0};
        if (uint32_t n = ++landed; n <= 20 || n % 1000 == 0)
            Log("longjmp %u to %08X (esp %08X, eax %08X)", n, target, c.esp, c.eax);
        longjmp(it->second->host, 1);
    }
    Log("bad call: eax %08X ecx %08X edx %08X ebx %08X esp %08X ebp %08X esi %08X edi %08X", c.eax, c.ecx, c.edx,
        c.ebx, c.esp, c.ebp, c.esi, c.edi);
    // the memory the registers point at, in case it was an object or an array of them
    for (uint32_t a : {c.eax, c.ecx, c.esi, c.edi, c.esp}) {
        if (a < 0x10000 || a >= 0xF0000000) continue;
        uint32_t base = a & ~0xFu;
        for (uint32_t row = 0; row < 4; row++) {
            uint32_t at = base - 0x10 + row * 0x10;
            Log("  %08X: %08X %08X %08X %08X", at, R32(at), R32(at + 4), R32(at + 8), R32(at + 12));
        }
    }
    Fatal("call to %08X, which is no recompiled function (return address %08X)", target, R32(c.esp));
}

void DumpTrace(const Cpu& c) {
    std::string line;
    for (uint32_t i = 64; i > 0; i--) {
        uint32_t a = c.trace[(c.trace_pos - i) & 255];
        if (!a) continue;
        char buf[16];
        snprintf(buf, sizeof buf, " %08X", a);
        line += buf;
    }
    Log("  last functions entered (oldest first):%s", line.c_str());
    line.clear();
    for (uint32_t i = 48; i > 0; i--) {
        const uint32_t* r = c.rets[(c.ret_pos - i) & 127];
        if (!r[0]) continue;
        char buf[24];
        snprintf(buf, sizeof buf, " %08X=%X", r[0], r[1]);
        line += buf;
    }
    Log("  last returns (function=eax, oldest first):%s", line.c_str());
}

void Unimplemented(Cpu& FCI_NOESCAPE c, uint32_t address, const char* text) {
    Fatal("unimplemented instruction at %08X: %s", address, text);
}

void DivideError(Cpu& FCI_NOESCAPE c) { Fatal("guest divide error"); }

// The Xbox CPU runs at 733.33 MHz and XAPI's QueryPerformanceFrequency (0x004F7FB6) returns that constant,
// while its QueryPerformanceCounter is a bare rdtsc. The host TSC runs several times faster, so the game's
// clock (frame times, animation, cutscene timing) ran several times too fast. The emulated TSC counts at
// the Xbox rate, derived from the host's performance counter.
uint64_t ReadTsc() {
    constexpr uint64_t kXboxTsc = 733333333;
    static const auto clock = [] {
        LARGE_INTEGER f, start;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&start);
        return std::pair<uint64_t, uint64_t>(uint64_t(f.QuadPart), uint64_t(start.QuadPart));
    }();
    // ---- PC comfort features: guest time stops during a focus pause (runtime/guest_time.h) ----
    const uint64_t ticks = uint64_t(GuestQpc()) - clock.second;
    return ticks / clock.first * kXboxTsc + (ticks % clock.first) * kXboxTsc / clock.first;
}

void Cpuid(Cpu& FCI_NOESCAPE c) {
    switch (c.eax) {
    case 0:
        c.eax = 2;
        c.ebx = 0x756E6547;  // "GenuineIntel"
        c.edx = 0x49656E69;
        c.ecx = 0x6C65746E;
        break;
    case 1:
        c.eax = 0x0000068A;  // Pentium III (Coppermine)
        c.ebx = 0;
        c.ecx = 0;
        c.edx = 0x0383F9FF;  // fpu, tsc, cx8, cmov, mmx, fxsr, sse
        break;
    default:
        c.eax = c.ebx = c.ecx = c.edx = 0;
        break;
    }
}

void DebugService(Cpu& FCI_NOESCAPE c) {
    if (c.eax == 1) {  // BREAKPOINT_PRINT, ecx is an ANSI_STRING
        uint16_t len = R16(c.ecx);
        uint32_t buf = R32(c.ecx + 4);
        std::string text(reinterpret_cast<const char*>(Host(buf)), len);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
        Log("[guest] %s", text.c_str());
    } else {
        Log("[guest] debug service %u", c.eax);
    }
}

// I/O ports of the MCPX (ACPI, SMBus); nothing behind them is emulated yet
uint32_t PortIn(Cpu& FCI_NOESCAPE c, uint16_t port, int bits) {
    static std::atomic<int> reports{0};
    if (reports++ < 32) Log("port: in%d %04X", bits, port);
    return 0;
}

void PortOut(Cpu& FCI_NOESCAPE c, uint16_t port, uint32_t value, int bits) {
    static std::atomic<int> reports{0};
    if (reports++ < 32) Log("port: out%d %04X = %X", bits, port, value);
}

double LoadF80(uint32_t a) {
    uint64_t mant = R64(a);
    uint16_t se = R16(a + 8);
    bool sign = se >> 15;
    int exp = se & 0x7FFF;
    double v;
    if (exp == 0 && mant == 0) v = 0.0;
    else if (exp == 0x7FFF) v = (mant << 1) ? NAN : INFINITY;
    else v = std::ldexp(double(mant), exp - 16383 - 63);
    return sign ? -v : v;
}

void StoreF80(uint32_t a, double v) {
    uint64_t mant = 0;
    uint16_t se = std::signbit(v) ? 0x8000 : 0;
    if (std::isnan(v)) {
        mant = 0xC000000000000000ull;
        se |= 0x7FFF;
    } else if (std::isinf(v)) {
        mant = 1ull << 63;
        se |= 0x7FFF;
    } else if (v != 0.0) {
        int e = 0;
        double m = std::frexp(std::fabs(v), &e);  // [0.5, 1)
        mant = uint64_t(std::ldexp(m, 64));
        se |= uint16_t(e - 1 + 16383);
    }
    W64(a, mant);
    W16(a + 8, se);
}

// FNSAVE layout: 28 byte environment, then ST0..ST7 as 80-bit values
void FSave(Cpu& FCI_NOESCAPE c, uint32_t a) {
    W32(a + 0, c.fcw);
    W32(a + 4, FStatus(c));
    W32(a + 8, 0);
    for (int i = 0; i < 4; i++) W32(a + 12 + i * 4, 0);
    for (int i = 0; i < 8; i++) StoreF80(a + 28 + i * 10, ST(c, i));
    c.fcw = 0x037F;
    c.fsw = 0;
    c.top = 0;
}

void FRestore(Cpu& FCI_NOESCAPE c, uint32_t a) {
    c.fcw = uint16_t(R32(a));
    uint16_t sw = uint16_t(R32(a + 4));
    c.top = (sw >> 11) & 7;
    c.fsw = sw & ~0x3800;
    for (int i = 0; i < 8; i++) ST(c, i) = LoadF80(a + 28 + i * 10);
}

int32_t CvtToInt(const Cpu& FCI_NOESCAPE c, float v) {
    double d = v;
    switch ((c.mxcsr >> 13) & 3) {
    case 0: d = std::nearbyint(d); break;
    case 1: d = std::floor(d); break;
    case 2: d = std::ceil(d); break;
    default: d = std::trunc(d); break;
    }
    return FToInt<int32_t>(d);
}

void SetMxcsr(Cpu& FCI_NOESCAPE c, uint32_t v) {
    c.mxcsr = v;
    _mm_setcsr(v | 0x1F80);  // the guest's rounding, FTZ and DAZ apply to host SSE math; exceptions stay masked
}

}  // namespace rt
