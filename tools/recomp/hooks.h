// Guest functions the runtime replaces with host code (src/hle). For each, codegen emits f_<address> as a
// call to hle::<name>, and the recompiled original as o_<address> so the host side can still use it.
#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace recomp {

struct Hook {
    uint32_t address;
    const char* name;
};

inline constexpr Hook kHooks[] = {
    // XPP (XDK 5849): controllers are host devices instead of USB devices behind the OHCI stack
    {0x005BEE47, "XGetDevices"},
    {0x005BEE69, "XGetDeviceChanges"},
    {0x005C5926, "XInputOpen"},
    {0x005C597C, "XInputClose"},
    {0x005C5988, "XInputGetState"},
    {0x005C59FB, "XInputSetState"},
    {0x005C6220, "XInputPoll"},
    // ---- PC comfort features: skip_intro (src/hle/skip_intro.cpp) ----
    {0x005A9A20, "BinkOpen"},  // BinkOpen(name, flags), stdcall, called by the game's video player 0x00220200
};

// The CRT's _setjmp3. longjmp ends in a jump to the address after a call to it, in the middle of the caller
// (Lua's protected calls: a script error unwinds to luaD_rawrunprotected). Codegen puts a host setjmp after
// each call, registered under that address and the stack pointer, and rt::Call lands there.
inline constexpr uint32_t kSetjmp = 0x00500030;

// The CRT's _ftol2: st0 truncated toward zero as a 64-bit integer in edx:eax (the integer indefinite when out of
// range), st0 popped; the code of every (int)float. Calls to it are emitted inline (Codegen::IsFtol2 checks the code
// first): its result is exactly that for any rounding mode.
inline constexpr uint32_t kFtol2 = 0x004ECC18;

// symbols of the XBE (config/symbols.txt, config/symbols_fci.txt): name -> address, functions and data
struct Symbol {
    uint32_t address;
    bool function;
};
inline std::map<std::string, Symbol>& Symbols() {
    static std::map<std::string, Symbol> symbols;
    return symbols;
}

// guest functions replaced by host code: the static ones above and the symbols config/hle.txt names
inline std::map<uint32_t, std::string>& Hooks() {
    static std::map<uint32_t, std::string> hooks = [] {
        std::map<uint32_t, std::string> h;
        for (const auto& hook : kHooks) h[hook.address] = hook.name;
        return h;
    }();
    return hooks;
}

inline const char* HookName(uint32_t address) {
    auto it = Hooks().find(address);
    return it == Hooks().end() ? nullptr : it->second.c_str();
}

}  // namespace recomp
