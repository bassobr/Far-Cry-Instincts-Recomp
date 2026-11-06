// Loads game_code.dll, the recompiled game code (built by CMake, or from the player's xbe on the first start,
// app/game_code.cpp), and checks it fits this executable before anything of it runs.
#include <windows.h>

#include <algorithm>

#include "fci_abi.h"
#include "runtime/log.h"
#include "runtime/runtime.h"

namespace rt {

namespace {

HMODULE g_module = nullptr;
const GameCode* g_code = nullptr;

std::string Hex(uint64_t v) {
    char buf[24];
    snprintf(buf, sizeof buf, "%016llX", (unsigned long long)v);
    return buf;
}

}  // namespace

const GameCode* OpenGameCode(const std::filesystem::path& dll, std::string* why) {
    CloseGameCode();
    std::error_code ec;
    if (!std::filesystem::exists(dll, ec)) {
        *why = "not built yet";
        return nullptr;
    }
    // a DLL that imports something this executable no longer exports fails here: no message box, a reason
    const UINT mode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    HMODULE m = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    const DWORD error = GetLastError();
    SetErrorMode(mode);
    if (!m) {
        *why = error == ERROR_PROC_NOT_FOUND ? "it imports a function this executable does not export"
                                             : "it does not load (error " + std::to_string(error) + ")";
        return nullptr;
    }
    auto* code = reinterpret_cast<const GameCode*>(GetProcAddress(m, "fci_game_code"));
    if (!code || code->version != kGameCodeVersion || code->size != sizeof(GameCode)) {
        *why = "it has another interface version";
    } else if (code->abi != FCI_ABI_HASH) {
        *why = "it was built for another executable (ABI " + Hex(code->abi) + ", this one " + Hex(FCI_ABI_HASH) + ")";
    } else {
        g_module = m;
        g_code = code;
        RefreshSymbols();  // crash stacks and the profiler name f_<address> in it
        return code;
    }
    FreeLibrary(m);
    return nullptr;
}

void CloseGameCode() {
    if (g_module) FreeLibrary(g_module);
    g_module = nullptr;
    g_code = nullptr;
}

const GameCode* LoadedGameCode() { return g_code; }

GuestFunction Original(uint32_t address) {
    if (g_code) {
        const FunctionEntry* end = g_code->originals + g_code->original_count;
        const FunctionEntry* it = std::lower_bound(g_code->originals, end, address,
                                                   [](const FunctionEntry& e, uint32_t a) { return e.address < a; });
        if (it != end && it->address == address) return it->function;
    }
    Fatal("no recompiled original of %08X in the game code (config/hle.txt and the DLL disagree)", address);
}

}  // namespace rt
