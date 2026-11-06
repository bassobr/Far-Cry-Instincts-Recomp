// PC water: the sea's surface with a pixel shader of our own instead of the game's register combiners
// (FarCryInstincts.ini / FCI_<KEY>, see README): water_quality = console | medium | high.
//
// The renderer (gpu/renderer.cpp, block "PC water") recognises the ocean's draw (the projected grid of the game's
// OceanOver vertex program) and, unless the setting is console, draws it with the HLSL of water.cpp: the game's
// reflection target, its water colour and fog, the sun's direction from its sky sprite, a wave map (Far Cry PC's
// Waterbump when installed, gpu/fc1_data.h, else our own), the scene's depth and (high) a copy of the scene.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gpu::water {

enum Quality : int { kConsole = 0, kMedium = 1, kHigh = 2 };

// as read at start (water_quality; the HLE graphics only)
int BootQuality();
// ---- settings menu: the quality now (BootQuality until changed); false when it needs a restart ----
int Current();
bool SetQuality(int quality);
// ---- end settings menu ----

// the wave map (height gradients x 4 per texel of level 0, R16G16 snorm, full mip chain), built on a thread from Far
// Cry's Waterbump (gpu/fc1_data.h) or our own waves; Prepare starts it, Waves is null until it is done
struct WaveMap {
    uint32_t size = 0;
    std::vector<std::vector<uint32_t>> levels;
    bool from_fc1 = false;
};
void Prepare();
const WaveMap* Waves();

// the pixel shader's HLSL for a quality (medium/high), with the scene depth multisampled (samples > 1) or not and
// the reversed depth or not; fog_mode: SET_FOG_MODE of the draw (0: no fog)
std::string PixelShader(int quality, uint32_t samples, uint32_t fog_mode);

}  // namespace gpu::water
