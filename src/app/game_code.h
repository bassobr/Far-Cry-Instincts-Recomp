// The recompiled game code (game_code\game_code.dll beside the executable): loaded if it is current, else built
// from the player's default.xbe first (src/app/game_code.cpp).
#pragma once

#include <filesystem>

#include "xbe/xbe.h"

namespace app {

// true with the DLL loaded (rt::LoadedGameCode); false when it could not be built or loaded, or the player stopped
// the build (the reason was shown and logged)
bool PrepareGameCode(const std::filesystem::path& exe_dir, const xbe::Image& image);

}  // namespace app
