// Host implementations of XDK library functions (HLE). A function named in config/hle.txt is replaced: the
// recompiler emits f_<address> as a call to hle::<name> (exported by the executable, hle_hooks.h) and keeps the
// recompiled body as o_<address> in the game code DLL.
// The macros follow SCDA-XBERecomp's (MIT, TGP482 and Joshhhuaaa) so its code ports with few changes.
#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>

#include "hle/original.h"
#include "image_symbols.h"
#include "kernel/kernel.h"
#include "runtime/guest.h"
#include "runtime/log.h"
#include "runtime/runtime.h"
#include "runtime/thread.h"

#define XDK_CAT_(a, b) a##b
#define XDK_CAT(a, b) XDK_CAT_(a, b)
#define SYM(name) XDK_CAT(0x, SYM_##name)           // address of a symbol of config/symbols*.txt
#define GUEST(name) (::rt::Lookup(SYM(name)))        // the function as the guest calls it (a host one if replaced)
#define GUEST_ORIG(name) GUEST_ORIG_AT(SYM(name))   // the recompiled body of a replaced function

#include "xdk/xdk.h"

namespace xdk {

// stdcall/thiscall return: result in eax, return address and stack arguments popped
inline void Return(rt::Cpu& c, uint32_t value, int stack_args) {
    c.eax = value;
    c.esp += 4 + 4 * stack_args;
}

inline float ArgF(const kernel::Args& a, int i) {
    uint32_t v = a[i];
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}
inline float GF(uint32_t va) {
    float f;
    std::memcpy(&f, rt::Host(va), 4);
    return f;
}
inline void* HostPtr(uint32_t va) { return rt::Host(va); }
// performance (xdk/d3d8.cpp): runs fn on the render thread in push buffer order (values the renderer reads per
// frame), at once without HLE graphics
void RenderPost(std::function<void()> fn);
// PC shadows: the same, after everything the guest pushed so far (drains first): a mark between two draws
void RenderMark(std::function<void()> fn);
// performance: frames the guest may finish ahead of the screen (frame_queue, 1-3)
void SetFrameQueue(int frames);
inline uint32_t CallGuest(rt::Cpu& c, uint32_t fn, std::initializer_list<uint32_t> args) {
    return rt::GuestCall(c, fn, args);
}
// from inside an HLE body: runs the recompiled original on the caller's arguments (still on the stack) and
// returns its eax; the stack is left as the HLE wrapper expects it
inline uint32_t CallOriginal(rt::Cpu& c, rt::GuestFunction original) {
    uint32_t esp = c.esp;
    original(c);
    c.esp = esp;
    return c.eax;
}

}  // namespace xdk

// A translation unit with HLE functions defines `static bool HleOff()` first: true sends every call of the
// unit to the recompiled original (the LLE path).
#define HLE_STDCALL(name, nargs)                                                               \
    static uint32_t hle_impl_##name(rt::Cpu& c, const kernel::Args& a);                        \
    void hle::name(rt::Cpu& c) {                                                               \
        if (HleOff()) return GUEST_ORIG(name)(c);                                              \
        kernel::Args args{c};                                                                  \
        uint32_t r = hle_impl_##name(c, args);                                                 \
        xdk::Return(c, r, nargs);                                                              \
    }                                                                                          \
    static uint32_t hle_impl_##name([[maybe_unused]] rt::Cpu& c, [[maybe_unused]] const kernel::Args& a)

// __fastcall: the first two arguments in ecx and edx, `stack_args` more on the stack (a[0] is the first of those)
#define HLE_FASTCALL(name, stack_args) HLE_STDCALL(name, stack_args)
// __thiscall: this in ecx, the arguments on the stack
#define HLE_THISCALL(name, stack_args) HLE_STDCALL(name, stack_args)
// __cdecl: the caller pops the arguments
#define HLE_CDECL(name) HLE_STDCALL(name, 0)

#define LOG_FIRST(n, ...)                    \
    do {                                     \
        static int logged_ = 0;              \
        if (logged_++ < (n)) rt::Log(__VA_ARGS__); \
    } while (0)
