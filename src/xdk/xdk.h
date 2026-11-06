// Which side drives graphics and sound: HLE (host implementations of the XDK libraries, src/xdk) or LLE
// (the recompiled XDK on the emulated NV2A / MCPX APU). FCI_LLE_GFX=1 / FCI_LLE_AUDIO=1 select LLE.
#pragma once

namespace xdk {

bool HleAudio();
bool HleGraphics();
// PC level of detail (xdk/lod.cpp): the engine's LOD/sprite and view distance ratios times these, at once
void SetLodScales(float lod_distance, float view_distance);

}  // namespace xdk
