// Which side drives graphics and sound: HLE (host implementations of the XDK libraries, src/xdk) or LLE
// (the recompiled XDK on the emulated NV2A / MCPX APU). FCI_LLE_GFX=1 / FCI_LLE_AUDIO=1 select LLE.
#pragma once

namespace rt {
struct Cpu;
}

namespace xdk {

bool HleAudio();
bool HleGraphics();
// PC level of detail (xdk/lod.cpp): the engine's LOD/sprite and view distance ratios times these, at once
void SetLodScales(float lod_distance, float view_distance);
// frame-rate audit (xdk/game.cpp, FCI_INPUT_SCRIPT_CLOCK=level): seconds of the game's own clock since the first
// frame of the level's main camera, negative before it
double LevelSeconds();
// tests (hle/start_level.cpp, FCI_TEST_COMMAND_DELAY): called by the physics step every frame, pushes the delayed
// FCI_TEST_COMMAND once the level has run that long
void StartLevelTick(rt::Cpu& c);

}  // namespace xdk
