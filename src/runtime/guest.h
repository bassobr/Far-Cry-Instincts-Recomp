// Everything the recompiled code needs: the guest CPU state, guest memory and the x86 semantics
// that are easier to write once here than to emit inline.
#pragma once

#include <setjmp.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#define RT_INLINE __forceinline

// The Cpu a recompiled function gets is never kept by anything it calls (noescape = LLVM nocapture). Together with
// __restrict (FCI_RESTRICT) and -capture-tracking-max-uses-to-explore (CMakeLists.txt) this lets the compiler prove
// that guest memory, reached through integer addresses (Host), never aliases the Cpu, so guest registers stay in host
// registers between calls instead of being stored and reloaded around every guest memory access. True in practice:
// every Cpu lives in host memory (GuestThread), never in the guest window.
#define FCI_NOESCAPE __attribute__((noescape))

// Recompiled functions preserve no host registers (clang's preserve_none): the guest registers are written back to the
// Cpu at every call anyway, so across a call between them nothing lives in host registers, and the 8 general and 10 xmm
// registers the Windows ABI makes callee-saved would only be pushed and popped by every function. Host code calling
// one (the dispatcher, hooks) saves them once itself.
#define FCI_GUEST_CC __attribute__((preserve_none))

// Recompiled functions hold the guest registers in locals (FCI_REG) and write those they change back to the Cpu only
// where control leaves the function: calls, returns, tail jumps (FCI_SAVE before, FCI_LOAD after; both defined per
// function by fci-recomp).
// FCI_PRECISE (the "precise" build) keeps the guest state in memory current and records the guest
// address of every instruction in c.pc, so faults report exactly where they happened; its locals are references
// into the Cpu.
#ifdef FCI_PRECISE
#define FCI_REG(type) type&
#define FCI_REG_UNREAD(type, name, member) type& name = member
#define FCI_RESTRICT
#define PC(address) (c.pc = (address))
#define ENTER(address) (c.trace[c.trace_pos++ & 255] = (address))
#define RETURN(function) (c.rets[c.ret_pos & 127][0] = (function), c.rets[c.ret_pos++ & 127][1] = c.eax)
#else
#define FCI_REG(type) type
// a register the function writes before it reads it (fci-recomp's xmm liveness): no load
#define FCI_REG_UNREAD(type, name, member) type name
#define FCI_RESTRICT __restrict
#define PC(address) ((void)0)
#define ENTER(address) ((void)0)
#define RETURN(function) ((void)0)
#endif

namespace rt {

// ---------------------------------------------------------------------------------------------
// memory: the 4 GB guest address space is reserved at a fixed host address, so a guest address
// is a constant offset away and needs no base register

constexpr uintptr_t kGuestBase = 0x2000000000ull;

RT_INLINE uint8_t* Host(uint32_t a) { return reinterpret_cast<uint8_t*>(kGuestBase + a); }

// Everything from here up is hardware: the NV2A GPU, the APU, USB and the flash. Registers have side
// effects (write-one-to-clear interrupt bits, FIFO pointers), so accesses of up to 4 bytes take a slow path.
constexpr uint32_t kMmioBase = 0xFD000000;
// They keep every host register (preserve_all, which needs C linkage), so the rarely taken check in each guest memory
// access costs the hot code around it no spills.
#define FCI_MMIO_CC __attribute__((preserve_all))
extern "C" uint32_t FCI_MMIO_CC MmioRead(uint32_t a, unsigned size);
extern "C" void FCI_MMIO_CC MmioWrite(uint32_t a, uint32_t v, unsigned size);
// a guest access of up to 4 bytes, RAM or hardware (the cold path of LoadBase/StoreBase: one call, no spills)
extern "C" uint32_t FCI_MMIO_CC GuestRead(uint32_t a, unsigned size);
extern "C" void FCI_MMIO_CC GuestWrite(uint32_t a, uint32_t v, unsigned size);

template <typename T>
RT_INLINE T Load(uint32_t a) {
    T v;
    if constexpr (sizeof(T) <= 4) {
        if (a >= kMmioBase) [[unlikely]] {
            uint32_t raw = MmioRead(a, sizeof(T));
            std::memcpy(&v, &raw, sizeof(T));
            return v;
        }
    }
    std::memcpy(&v, Host(a), sizeof(T));
    return v;
}
template <typename T>
RT_INLINE void Store(uint32_t a, T v) {
    if constexpr (sizeof(T) <= 4) {
        if (a >= kMmioBase) [[unlikely]] {
            uint32_t raw = 0;
            std::memcpy(&raw, &v, sizeof(T));
            return MmioWrite(a, raw, sizeof(T));
        }
    }
    std::memcpy(Host(a), &v, sizeof(T));
}

// accesses that cannot reach the hardware (the stack, constant addresses below kMmioBase): no check
template <typename T>
RT_INLINE T LoadRam(uint32_t a) {
    T v;
    std::memcpy(&v, Host(a), sizeof(T));
    return v;
}
template <typename T>
RT_INLINE void StoreRam(uint32_t a, T v) {
    std::memcpy(Host(a), &v, sizeof(T));
}
RT_INLINE uint8_t RamR8(uint32_t a) { return LoadRam<uint8_t>(a); }
RT_INLINE uint16_t RamR16(uint32_t a) { return LoadRam<uint16_t>(a); }
RT_INLINE uint32_t RamR32(uint32_t a) { return LoadRam<uint32_t>(a); }
RT_INLINE uint64_t RamR64(uint32_t a) { return LoadRam<uint64_t>(a); }
RT_INLINE float RamRF32(uint32_t a) { return LoadRam<float>(a); }
RT_INLINE double RamRF64(uint32_t a) { return LoadRam<double>(a); }
RT_INLINE void RamW8(uint32_t a, uint8_t v) { StoreRam(a, v); }
RT_INLINE void RamW16(uint32_t a, uint16_t v) { StoreRam(a, v); }
RT_INLINE void RamW32(uint32_t a, uint32_t v) { StoreRam(a, v); }
RT_INLINE void RamW64(uint32_t a, uint64_t v) { StoreRam(a, v); }
RT_INLINE void RamWF32(uint32_t a, float v) { StoreRam(a, v); }
RT_INLINE void RamWF64(uint32_t a, double v) { StoreRam(a, v); }

// [base + displacement < 16 MB] (fci-recomp's kBaseDispLimit): a base below kBaseCheck cannot reach the hardware, and the
// test is the same for every access through that base register, so the compiler keeps one for a run of them
constexpr uint32_t kBaseCheck = kMmioBase - 0x01000000;
template <typename T>
RT_INLINE T LoadBase(uint32_t base, uint32_t a) {
    if constexpr (sizeof(T) <= 4) {
        if (base >= kBaseCheck) [[unlikely]] {
            const uint32_t raw = GuestRead(a, sizeof(T));
            T v;
            std::memcpy(&v, &raw, sizeof(T));
            return v;
        }
    }
    return LoadRam<T>(a);
}
template <typename T>
RT_INLINE void StoreBase(uint32_t base, uint32_t a, T v) {
    if constexpr (sizeof(T) <= 4) {
        if (base >= kBaseCheck) [[unlikely]] {
            uint32_t raw = 0;
            std::memcpy(&raw, &v, sizeof(T));
            return GuestWrite(a, raw, sizeof(T));
        }
    }
    StoreRam(a, v);
}
RT_INLINE uint8_t BaseR8(uint32_t b, uint32_t a) { return LoadBase<uint8_t>(b, a); }
RT_INLINE uint16_t BaseR16(uint32_t b, uint32_t a) { return LoadBase<uint16_t>(b, a); }
RT_INLINE uint32_t BaseR32(uint32_t b, uint32_t a) { return LoadBase<uint32_t>(b, a); }
RT_INLINE uint64_t BaseR64(uint32_t b, uint32_t a) { return LoadBase<uint64_t>(b, a); }
RT_INLINE float BaseRF32(uint32_t b, uint32_t a) { return LoadBase<float>(b, a); }
RT_INLINE double BaseRF64(uint32_t b, uint32_t a) { return LoadBase<double>(b, a); }
RT_INLINE void BaseW8(uint32_t b, uint32_t a, uint8_t v) { StoreBase(b, a, v); }
RT_INLINE void BaseW16(uint32_t b, uint32_t a, uint16_t v) { StoreBase(b, a, v); }
RT_INLINE void BaseW32(uint32_t b, uint32_t a, uint32_t v) { StoreBase(b, a, v); }
RT_INLINE void BaseW64(uint32_t b, uint32_t a, uint64_t v) { StoreBase(b, a, v); }
RT_INLINE void BaseWF32(uint32_t b, uint32_t a, float v) { StoreBase(b, a, v); }
RT_INLINE void BaseWF64(uint32_t b, uint32_t a, double v) { StoreBase(b, a, v); }

RT_INLINE uint8_t R8(uint32_t a) { return Load<uint8_t>(a); }
RT_INLINE uint16_t R16(uint32_t a) { return Load<uint16_t>(a); }
RT_INLINE uint32_t R32(uint32_t a) { return Load<uint32_t>(a); }
RT_INLINE uint64_t R64(uint32_t a) { return Load<uint64_t>(a); }
RT_INLINE float RF32(uint32_t a) { return Load<float>(a); }
RT_INLINE double RF64(uint32_t a) { return Load<double>(a); }
RT_INLINE void W8(uint32_t a, uint8_t v) { Store(a, v); }
RT_INLINE void W16(uint32_t a, uint16_t v) { Store(a, v); }
RT_INLINE void W32(uint32_t a, uint32_t v) { Store(a, v); }
RT_INLINE void W64(uint32_t a, uint64_t v) { Store(a, v); }
RT_INLINE void WF32(uint32_t a, float v) { Store(a, v); }
RT_INLINE void WF64(uint32_t a, double v) { Store(a, v); }

// ---------------------------------------------------------------------------------------------
// cpu state

// 128-bit vectors (clang's vector extension; lanes subscript like arrays), so a register a function holds in a local
// stays one host xmm register instead of four scalars
using F32x4 = float __attribute__((vector_size(16)));
using U32x4 = uint32_t __attribute__((vector_size(16)));
using U64x2 = uint64_t __attribute__((vector_size(16)));
union Xmm {
    F32x4 f;
    U32x4 u;
    U64x2 q;
};

// Status flags live in locals of each generated function; they only pass through Cpu where control
// leaves a function without a call (tail jumps, falling into the next function).
struct Flags {
    bool cf = false, pf = false, af = false, zf = false, sf = false, of = false;
};

struct alignas(16) Cpu {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    Flags flags;
    uint32_t df;       // direction flag
    uint32_t eflags_id;  // AC and ID bits: software toggles ID through pushfd/popfd to detect cpuid
    uint32_t fs_base;  // guest address of the thread's KPCR
    Xmm xmm[8];
    uint64_t mm[8];
    double st[8];  // x87 stack, held in double precision
    uint32_t top;
    uint16_t fcw = 0x027F;
    uint16_t fsw;  // condition codes only, TOP comes from `top`
    uint32_t mxcsr = 0x1F80;
    uint32_t pc;   // guest address of the current instruction, kept in the precise build only
    void* thread;  // runtime data of the guest thread
    // precise build: the last functions entered, for rt::DumpTrace
    uint32_t trace_pos;
    uint32_t trace[256];
    uint32_t ret_pos;
    uint32_t rets[128][2];  // function, eax at its return
};

void DumpTrace(const Cpu& c);

// eflags bits, the same masks Zydis uses
enum : uint32_t { CF = 1u << 0, PF = 1u << 2, AF = 1u << 4, ZF = 1u << 6, SF = 1u << 7, OF = 1u << 11 };

// implemented by the runtime
void Call(Cpu& FCI_NOESCAPE c, uint32_t target);  // indirect call or jump; the return address is already pushed
// the dispatch table Call looks in (runtime.cpp): recompiled function by guest address - g_dispatch_base
using DispatchEntry = void(FCI_GUEST_CC*)(Cpu& FCI_NOESCAPE);
extern DispatchEntry const* g_dispatch;
extern uint32_t g_dispatch_base, g_dispatch_size;
// Call with the table lookup inline: a recompiled target is called straight from the generated code
RT_INLINE void CallIndirect(Cpu& c, uint32_t target) {
    const uint32_t i = target - g_dispatch_base;
    if (i < g_dispatch_size) [[likely]]
        if (DispatchEntry f = g_dispatch[i]) [[likely]]
            return f(c);
    Call(c, target);
}
// a host setjmp buffer for a guest setjmp that returned to pc with the stack at c.esp; a jump to pc with the
// same stack (the end of the guest's longjmp) resumes there
jmp_buf* Landing(Cpu& FCI_NOESCAPE c, uint32_t pc);
// after setjmp: a second return (the guest's longjmp) comes back with the Cpu changed behind the compiler's back
// (nothing it can see touched it, see FCI_NOESCAPE), so everything it holds in registers is read again from c
RT_INLINE void SetjmpBarrier(Cpu& c) { asm volatile("" : : "r"(&c) : "memory"); }

// set when another guest context waits for the CPU; checked at loop back-edges (scheduler.h)
extern volatile int g_preempt;
void Preempt(Cpu& FCI_NOESCAPE c);
void KernelCall(Cpu& FCI_NOESCAPE c, uint32_t ordinal);
void Unimplemented(Cpu& FCI_NOESCAPE c, uint32_t address, const char* text);
uint64_t ReadTsc();
void Cpuid(Cpu& FCI_NOESCAPE c);         // answers as the Xbox's Pentium III
void DebugService(Cpu& FCI_NOESCAPE c);  // int 0x2D: eax selects print or breakpoint
uint32_t PortIn(Cpu& FCI_NOESCAPE c, uint16_t port, int bits);
void PortOut(Cpu& FCI_NOESCAPE c, uint16_t port, uint32_t value, int bits);

// ---------------------------------------------------------------------------------------------
// integer alu

template <typename T> constexpr int kBits = sizeof(T) * 8;
template <typename T> struct Signed;
template <> struct Signed<uint8_t> { using type = int8_t; };
template <> struct Signed<uint16_t> { using type = int16_t; };
template <> struct Signed<uint32_t> { using type = int32_t; };
template <typename T> using S = typename Signed<T>::type;

template <typename T>
RT_INLINE bool Msb(T v) { return (v >> (kBits<T> - 1)) & 1; }

template <typename T>
RT_INLINE void SZP(Flags& f, uint32_t m, T r) {
    if (m & ZF) f.zf = r == 0;
    if (m & SF) f.sf = Msb(r);
    if (m & PF) f.pf = !__builtin_parity(uint8_t(r));
}

template <typename T>
RT_INLINE T Add(Flags& f, uint32_t m, T a, T b, bool carry = false) {
    T r = T(a + b + carry);
    if (m & CF) f.cf = carry ? r <= a : r < a;
    if (m & OF) f.of = Msb(T((a ^ r) & (b ^ r)));
    if (m & AF) f.af = ((a ^ b ^ r) >> 4) & 1;
    SZP(f, m, r);
    return r;
}

template <typename T>
RT_INLINE T Sub(Flags& f, uint32_t m, T a, T b, bool borrow = false) {
    T r = T(a - b - borrow);
    if (m & CF) f.cf = uint64_t(a) < uint64_t(b) + borrow;
    if (m & OF) f.of = Msb(T((a ^ b) & (a ^ r)));
    if (m & AF) f.af = ((a ^ b ^ r) >> 4) & 1;
    SZP(f, m, r);
    return r;
}

template <typename T>
RT_INLINE T Logic(Flags& f, uint32_t m, T r) {
    if (m & CF) f.cf = false;
    if (m & OF) f.of = false;
    if (m & AF) f.af = false;
    SZP(f, m, r);
    return r;
}

template <typename T>
RT_INLINE T Shl(Flags& f, uint32_t m, T a, uint32_t n) {
    n &= 31;
    if (!n) return a;
    uint64_t wide = uint64_t(a) << n;
    T r = T(wide);
    if (m & CF) f.cf = (wide >> kBits<T>) & 1;
    if (m & OF) f.of = Msb(r) ^ bool((wide >> kBits<T>) & 1);
    if (m & AF) f.af = false;
    SZP(f, m, r);
    return r;
}

template <typename T>
RT_INLINE T Shr(Flags& f, uint32_t m, T a, uint32_t n) {
    n &= 31;
    if (!n) return a;
    T r = n >= uint32_t(kBits<T>) ? T(0) : T(a >> n);
    if (m & CF) f.cf = n <= uint32_t(kBits<T>) && ((uint64_t(a) >> (n - 1)) & 1);
    if (m & OF) f.of = Msb(a);
    if (m & AF) f.af = false;
    SZP(f, m, r);
    return r;
}

template <typename T>
RT_INLINE T Sar(Flags& f, uint32_t m, T a, uint32_t n) {
    n &= 31;
    if (!n) return a;
    int32_t sa = S<T>(a);
    uint32_t k = n < uint32_t(kBits<T>) ? n : kBits<T> - 1;
    T r = T(sa >> k);
    if (m & CF) f.cf = (sa >> (n - 1 < uint32_t(kBits<T>) ? n - 1 : kBits<T> - 1)) & 1;
    if (m & OF) f.of = false;
    if (m & AF) f.af = false;
    SZP(f, m, r);
    return r;
}

template <typename T>
RT_INLINE T Rol(Flags& f, uint32_t m, T a, uint32_t n) {
    n &= 31;
    if (!n) return a;
    uint32_t k = n % kBits<T>;
    T r = k ? T((a << k) | (a >> (kBits<T> - k))) : a;
    if (m & CF) f.cf = r & 1;
    if (m & OF) f.of = Msb(r) ^ bool(r & 1);
    return r;
}

template <typename T>
RT_INLINE T Ror(Flags& f, uint32_t m, T a, uint32_t n) {
    n &= 31;
    if (!n) return a;
    uint32_t k = n % kBits<T>;
    T r = k ? T((a >> k) | (a << (kBits<T> - k))) : a;
    if (m & CF) f.cf = Msb(r);
    if (m & OF) f.of = Msb(r) ^ bool((r >> (kBits<T> - 2)) & 1);
    return r;
}

template <typename T>
RT_INLINE T Rcl(Flags& f, uint32_t m, T a, uint32_t n) {
    n = (n & 31) % (kBits<T> + 1);
    bool cf = f.cf;
    for (uint32_t i = 0; i < n; i++) {
        bool out = Msb(a);
        a = T((a << 1) | cf);
        cf = out;
    }
    if (n) {
        f.cf = cf;
        if (m & OF) f.of = Msb(a) ^ cf;
    }
    return a;
}

template <typename T>
RT_INLINE T Rcr(Flags& f, uint32_t m, T a, uint32_t n) {
    n = (n & 31) % (kBits<T> + 1);
    bool cf = f.cf;
    if (n && (m & OF)) f.of = Msb(a) ^ cf;
    for (uint32_t i = 0; i < n; i++) {
        bool out = a & 1;
        a = T((a >> 1) | (T(cf) << (kBits<T> - 1)));
        cf = out;
    }
    if (n) f.cf = cf;
    return a;
}

RT_INLINE uint32_t Shld(Flags& f, uint32_t m, uint32_t a, uint32_t b, uint32_t n) {
    n &= 31;
    if (!n) return a;
    uint32_t r = (a << n) | (b >> (32 - n));
    if (m & CF) f.cf = (a >> (32 - n)) & 1;
    if (m & OF) f.of = Msb(r) ^ Msb(a);
    SZP(f, m, r);
    return r;
}

RT_INLINE uint32_t Shrd(Flags& f, uint32_t m, uint32_t a, uint32_t b, uint32_t n) {
    n &= 31;
    if (!n) return a;
    uint32_t r = (a >> n) | (b << (32 - n));
    if (m & CF) f.cf = (a >> (n - 1)) & 1;
    if (m & OF) f.of = Msb(r) ^ Msb(a);
    SZP(f, m, r);
    return r;
}

// two and three operand imul
template <typename T>
RT_INLINE T Imul(Flags& f, uint32_t m, T a, T b) {
    int64_t p = int64_t(S<T>(a)) * S<T>(b);
    T r = T(p);
    bool overflow = p != int64_t(S<T>(r));
    if (m & CF) f.cf = overflow;
    if (m & OF) f.of = overflow;
    SZP(f, m, r);
    return r;
}

// the general registers (and df) of a recompiled function's locals, for the helpers below, which take either a Cpu or
// this view
struct GprView {
    uint32_t &eax, &ecx, &edx, &ebx, &esp, &ebp, &esi, &edi, &df;
};
#define FCI_GPRS (::rt::GprView{eax, ecx, edx, ebx, esp, ebp, esi, edi, c.df})

template <typename C>
RT_INLINE void Mul32(C&& c, Flags& f, uint32_t m, uint32_t b) {
    uint64_t p = uint64_t(c.eax) * b;
    c.eax = uint32_t(p);
    c.edx = uint32_t(p >> 32);
    f.cf = f.of = c.edx != 0;
}

template <typename C>
RT_INLINE void Imul32(C&& c, Flags& f, uint32_t m, uint32_t b) {
    int64_t p = int64_t(int32_t(c.eax)) * int32_t(b);
    c.eax = uint32_t(p);
    c.edx = uint32_t(uint64_t(p) >> 32);
    f.cf = f.of = p != int64_t(int32_t(c.eax));
}

template <typename C>
RT_INLINE void Imul16(C&& c, Flags& f, uint32_t m, uint16_t b) {
    int32_t p = int32_t(int16_t(c.eax)) * int16_t(b);
    c.eax = (c.eax & 0xFFFF0000u) | uint16_t(p);
    c.edx = (c.edx & 0xFFFF0000u) | uint16_t(uint32_t(p) >> 16);
    f.cf = f.of = p != int32_t(int16_t(p));
}

template <typename C>
RT_INLINE void Imul8(C&& c, Flags& f, uint32_t m, uint8_t b) {
    int16_t p = int16_t(int8_t(c.eax) * int8_t(b));
    c.eax = (c.eax & 0xFFFF0000u) | uint16_t(p);
    f.cf = f.of = p != int16_t(int8_t(p));
}

void DivideError(Cpu& FCI_NOESCAPE c);

template <typename C>
RT_INLINE void Div32(Cpu& cpu, C&& c, uint32_t b) {
    if (!b) return DivideError(cpu);
    uint64_t n = (uint64_t(c.edx) << 32) | c.eax;
    c.eax = uint32_t(n / b);
    c.edx = uint32_t(n % b);
}

template <typename C>
RT_INLINE void Idiv32(Cpu& cpu, C&& c, uint32_t b) {
    if (!b) return DivideError(cpu);
    int64_t n = int64_t((uint64_t(c.edx) << 32) | c.eax);
    int64_t d = int32_t(b);
    if (n == INT64_MIN && d == -1) return DivideError(cpu);
    c.eax = uint32_t(n / d);
    c.edx = uint32_t(n % d);
}

// ---------------------------------------------------------------------------------------------
// string instructions

template <typename T, typename C>
RT_INLINE void Movs(C&& c, bool rep) {
    int32_t step = c.df ? -int32_t(sizeof(T)) : int32_t(sizeof(T));
    uint32_t n = rep ? c.ecx : 1;
    if (!c.df && rep && n && c.edi < kMmioBase && c.esi < kMmioBase &&
        (c.edi >= c.esi + n * sizeof(T) || c.edi + n * sizeof(T) <= c.esi)) {
        std::memcpy(Host(c.edi), Host(c.esi), size_t(n) * sizeof(T));
        c.esi += n * sizeof(T);
        c.edi += n * sizeof(T);
    } else {
        for (uint32_t i = 0; i < n; i++) {
            Store<T>(c.edi, Load<T>(c.esi));
            c.esi += step;
            c.edi += step;
        }
    }
    if (rep) c.ecx = 0;
}

template <typename T, typename C>
RT_INLINE void Stos(C&& c, bool rep) {
    int32_t step = c.df ? -int32_t(sizeof(T)) : int32_t(sizeof(T));
    uint32_t n = rep ? c.ecx : 1;
    T v = T(c.eax);
    if (sizeof(T) == 1 && !c.df && rep && c.edi < kMmioBase) {
        std::memset(Host(c.edi), uint8_t(v), n);
        c.edi += n;
    } else {
        for (uint32_t i = 0; i < n; i++) {
            Store<T>(c.edi, v);
            c.edi += step;
        }
    }
    if (rep) c.ecx = 0;
}

template <typename T, typename C>
RT_INLINE void Lods(C&& c) {
    T v = Load<T>(c.esi);
    c.esi += c.df ? -int32_t(sizeof(T)) : int32_t(sizeof(T));
    if constexpr (sizeof(T) == 4) c.eax = v;
    else if constexpr (sizeof(T) == 2) c.eax = (c.eax & 0xFFFF0000u) | v;
    else c.eax = (c.eax & 0xFFFFFF00u) | v;
}

// rep: 0 none, 1 repe, 2 repne
template <typename T, typename C>
RT_INLINE void Cmps(C&& c, Flags& f, int rep) {
    int32_t step = c.df ? -int32_t(sizeof(T)) : int32_t(sizeof(T));
    if (!rep) {
        Sub<T>(f, ~0u, Load<T>(c.esi), Load<T>(c.edi));
        c.esi += step;
        c.edi += step;
        return;
    }
    while (c.ecx) {
        Sub<T>(f, ~0u, Load<T>(c.esi), Load<T>(c.edi));
        c.esi += step;
        c.edi += step;
        c.ecx--;
        if ((rep == 1) != f.zf) break;
    }
}

template <typename T, typename C>
RT_INLINE void Scas(C&& c, Flags& f, int rep) {
    int32_t step = c.df ? -int32_t(sizeof(T)) : int32_t(sizeof(T));
    if (!rep) {
        Sub<T>(f, ~0u, T(c.eax), Load<T>(c.edi));
        c.edi += step;
        return;
    }
    while (c.ecx) {
        Sub<T>(f, ~0u, T(c.eax), Load<T>(c.edi));
        c.edi += step;
        c.ecx--;
        if ((rep == 1) != f.zf) break;
    }
}

// ---------------------------------------------------------------------------------------------
// eflags as a value (pushfd, lahf)

RT_INLINE uint32_t PackFlags(const Cpu& c, const Flags& f) {
    return 0x202u | (f.cf ? CF : 0) | (f.pf ? PF : 0) | (f.af ? AF : 0) | (f.zf ? ZF : 0) | (f.sf ? SF : 0) |
           (f.of ? OF : 0) | (c.df ? 0x400u : 0) | c.eflags_id;
}

RT_INLINE void UnpackFlags(Cpu& c, Flags& f, uint32_t v, bool low_byte_only) {
    f.cf = v & CF;
    f.pf = v & PF;
    f.af = v & AF;
    f.zf = v & ZF;
    f.sf = v & SF;
    if (!low_byte_only) {
        f.of = v & OF;
        c.df = (v >> 10) & 1;
        c.eflags_id = v & 0x00240000u;
    }
}

// ---------------------------------------------------------------------------------------------
// x87, kept in double precision

RT_INLINE double& ST(Cpu& c, int i) { return c.st[(c.top + i) & 7]; }
RT_INLINE void FPush(Cpu& c, double v) {
    c.top = (c.top - 1) & 7;
    c.st[c.top] = v;
}
RT_INLINE void FPop(Cpu& c) { c.top = (c.top + 1) & 7; }

constexpr uint16_t kC0 = 0x100, kC1 = 0x200, kC2 = 0x400, kC3 = 0x4000;

RT_INLINE void FCompare(Cpu& c, double a, double b) {
    uint16_t cc;
    if (std::isnan(a) || std::isnan(b)) cc = kC0 | kC2 | kC3;
    else if (a < b) cc = kC0;
    else if (a == b) cc = kC3;
    else cc = 0;
    c.fsw = (c.fsw & ~(kC0 | kC1 | kC2 | kC3)) | cc;
}

// fcomi/fucomi and comiss/ucomiss report into eflags
RT_INLINE void CompareToFlags(Flags& f, double a, double b) {
    bool unordered = std::isnan(a) || std::isnan(b);
    f.zf = unordered || a == b;
    f.pf = unordered;
    f.cf = unordered || a < b;
    f.of = f.sf = f.af = false;
}

RT_INLINE uint16_t FStatus(const Cpu& c) { return uint16_t((c.fsw & ~0x3800u) | ((c.top & 7) << 11)); }

RT_INLINE double FRound(const Cpu& c, double v) {
    switch ((c.fcw >> 10) & 3) {
    case 0: return std::nearbyint(v);
    case 1: return std::floor(v);
    case 2: return std::ceil(v);
    default: return std::trunc(v);
    }
}

template <typename I>
RT_INLINE I FToInt(double v) {
    constexpr double lo = -double(uint64_t(1) << (sizeof(I) * 8 - 1));
    if (!(v >= lo && v < -lo)) return I(uint64_t(1) << (sizeof(I) * 8 - 1));  // integer indefinite
    return I(v);
}

RT_INLINE void FExamine(Cpu& c) {
    double v = ST(c, 0);
    uint16_t cc = std::signbit(v) ? kC1 : 0;
    switch (std::fpclassify(v)) {
    case FP_NAN: cc |= kC0; break;
    case FP_INFINITE: cc |= kC0 | kC2; break;
    case FP_ZERO: cc |= kC3; break;
    case FP_SUBNORMAL: cc |= kC2 | kC3; break;
    default: cc |= kC2; break;
    }
    c.fsw = (c.fsw & ~(kC0 | kC1 | kC2 | kC3)) | cc;
}

double LoadF80(uint32_t a);
void StoreF80(uint32_t a, double v);
void FSave(Cpu& FCI_NOESCAPE c, uint32_t a);
void FRestore(Cpu& FCI_NOESCAPE c, uint32_t a);

// ---------------------------------------------------------------------------------------------
// sse

RT_INLINE Xmm LoadX(uint32_t a) { return Load<Xmm>(a); }
RT_INLINE void StoreX(uint32_t a, const Xmm& v) { Store<Xmm>(a, v); }

RT_INLINE Xmm Shufps(const Xmm& a, const Xmm& b, uint8_t imm) {
    return Xmm{.f = {a.f[imm & 3], a.f[(imm >> 2) & 3], b.f[(imm >> 4) & 3], b.f[(imm >> 6) & 3]}};
}
RT_INLINE float MaxF(float a, float b) { return a > b ? a : b; }
RT_INLINE float MinF(float a, float b) { return a < b ? a : b; }

int32_t CvtToInt(const Cpu& FCI_NOESCAPE c, float v);  // rounds per mxcsr
RT_INLINE int32_t CvttToInt(float v) { return FToInt<int32_t>(v); }
void SetMxcsr(Cpu& FCI_NOESCAPE c, uint32_t v);

// ---------------------------------------------------------------------------------------------
// mmx: 64-bit registers handled lane by lane

template <typename L, typename F>
RT_INLINE uint64_t Lanes(uint64_t a, uint64_t b, F op) {
    L la[8 / sizeof(L)], lb[8 / sizeof(L)];
    std::memcpy(la, &a, 8);
    std::memcpy(lb, &b, 8);
    for (size_t i = 0; i < 8 / sizeof(L); i++) la[i] = L(op(la[i], lb[i]));
    uint64_t r;
    std::memcpy(&r, la, 8);
    return r;
}

template <typename L>
RT_INLINE L Saturate(int64_t v) {
    constexpr int64_t lo = L(-1) < 0 ? -(int64_t(1) << (sizeof(L) * 8 - 1)) : 0;
    constexpr int64_t hi = L(-1) < 0 ? (int64_t(1) << (sizeof(L) * 8 - 1)) - 1 : (int64_t(1) << (sizeof(L) * 8)) - 1;
    return L(v < lo ? lo : v > hi ? hi : v);
}

template <typename L>
RT_INLINE uint64_t ShiftLeft(uint64_t a, uint64_t n) {
    return n >= sizeof(L) * 8 ? 0 : Lanes<L>(a, 0, [n](L x, L) { return L(x << n); });
}
template <typename L>
RT_INLINE uint64_t ShiftRight(uint64_t a, uint64_t n) {
    if (L(-1) < 0) n = n >= sizeof(L) * 8 ? sizeof(L) * 8 - 1 : n;
    else if (n >= sizeof(L) * 8) return 0;
    return Lanes<L>(a, 0, [n](L x, L) { return L(x >> n); });
}

template <typename Wide, typename Narrow>
RT_INLINE uint64_t Unpack(uint64_t a, uint64_t b, bool high) {
    constexpr size_t n = 8 / sizeof(Narrow);
    Narrow la[n], lb[n], r[n];
    std::memcpy(la, &a, 8);
    std::memcpy(lb, &b, 8);
    size_t base = high ? n / 2 : 0;
    for (size_t i = 0; i < n / 2; i++) {
        r[2 * i] = la[base + i];
        r[2 * i + 1] = lb[base + i];
    }
    uint64_t out;
    std::memcpy(&out, r, 8);
    return out;
}

template <typename From, typename To>
RT_INLINE uint64_t Pack(uint64_t a, uint64_t b) {
    From la[8 / sizeof(From)], lb[8 / sizeof(From)];
    std::memcpy(la, &a, 8);
    std::memcpy(lb, &b, 8);
    constexpr size_t n = 8 / sizeof(From);
    To r[2 * n];
    for (size_t i = 0; i < n; i++) {
        r[i] = Saturate<To>(la[i]);
        r[n + i] = Saturate<To>(lb[i]);
    }
    uint64_t out;
    std::memcpy(&out, r, 8);
    return out;
}

RT_INLINE uint64_t Pmaddwd(uint64_t a, uint64_t b) {
    int16_t la[4], lb[4];
    std::memcpy(la, &a, 8);
    std::memcpy(lb, &b, 8);
    int32_t r[2] = {la[0] * lb[0] + la[1] * lb[1], la[2] * lb[2] + la[3] * lb[3]};
    uint64_t out;
    std::memcpy(&out, r, 8);
    return out;
}

RT_INLINE uint64_t Pshufw(uint64_t a, uint8_t imm) {
    uint16_t l[4], r[4];
    std::memcpy(l, &a, 8);
    for (int i = 0; i < 4; i++) r[i] = l[(imm >> (2 * i)) & 3];
    uint64_t out;
    std::memcpy(&out, r, 8);
    return out;
}

RT_INLINE uint32_t Pmovmskb(uint64_t a) {
    uint32_t r = 0;
    for (int i = 0; i < 8; i++) r |= uint32_t((a >> (8 * i + 7)) & 1) << i;
    return r;
}

// ---------------------------------------------------------------------------------------------
// table of recompiled functions, emitted by fci-recomp

using GuestFunction = DispatchEntry;
struct FunctionEntry {
    uint32_t address;
    GuestFunction function;
};
extern const FunctionEntry kFunctions[];
extern const size_t kFunctionCount;

}  // namespace rt
