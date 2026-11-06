// Host side of the runtime: dispatch setup and constants shared with the kernel.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "runtime/guest.h"

namespace rt {

// kernel thunk slots of function exports hold kKernelThunkBase + ordinal * 4, so indirect calls through
// a loaded slot reach KernelCall as well
constexpr uint32_t kKernelThunkBase = 0xFE000000;

void InitDispatch(const GameCode& code, uint32_t image_base, uint32_t image_size);
GuestFunction Lookup(uint32_t address);

// The game code DLL (runtime/game_dll.cpp). OpenGameCode loads it and checks it was built for this executable
// (struct version, FCI_ABI_HASH); null with the reason in *why otherwise, and then nothing of it stays loaded.
// What it was built from (GameCode::xbe, ::build) the caller compares (app/game_code.cpp).
const GameCode* OpenGameCode(const std::filesystem::path& dll, std::string* why);
void CloseGameCode();
const GameCode* LoadedGameCode();
// the recompiled body of a function host code replaces (GUEST_ORIG); fatal when there is none
GuestFunction Original(uint32_t address);

}  // namespace rt
