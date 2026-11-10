// PC features: fmv_scale (FarCryInstincts.ini / FCI_FMV_SCALE, see README "Breitbild (Hor+), Sichtfeld, Bildrate"):
//   fmv_scale = fill (default) | 4:3
// fill: full screen Bink videos on a widescreen display are as large as they fit without cutting their picture: the
// black bars FCI encodes into its letterboxed videos are cut, the picture keeps its shape and is centred; 2D drawn over
// a video (title, menus) stays in the 4:3 middle. 4:3: videos as on the console, in the 4:3 middle.
// The idea follows SCDA-XBERecomp's fmv_scale (MIT, Copyright (c) 2026 TGP482, Joshhhuaaa), src/game/hud.cpp (Refit).
//
// Videos reach the screen two ways: the boot videos (and every video of the game's overlay player 0x00220200) through
// the video overlay (gpu/present.cpp), the title and menu backgrounds as a linear texture Bink fills every frame,
// drawn as a pre-transformed 2D quad (gpu/renderer.cpp).
#pragma once

#include <cstdint>

namespace gpu::fmv {

// fill is on and the 3D view is widened (aspect wider than 4:3, HLE graphics)
bool Fill();
// BinkOpen (hle/skip_intro.cpp): a video was opened (name as the game passes it), its picture width x height
void Opened(const char* name, uint32_t width, uint32_t height);
// a video of this size was opened lately (the title's and menus' linear video textures are recognised by it)
bool IsVideo(uint32_t width, uint32_t height);
// a video picture of height video_h drawn into the guest rectangle x0..x1, y0..y1 (of the 640x480 screen) of a frame
// shown at the widescreen aspect: the affine map of its vertices in the anamorphic target, x' = x * s[0] + s[1],
// y' = y * s[2] + s[3]. Full width videos are scaled up about the screen centre to fill; others keep the 4:3 middle
// like 2D. With fill off: identity. Returns the scale of the picture against the 4:3 frame (1 = as on the console).
float Transform(float x0, float y0, float x1, float y1, uint32_t video_h, float s[4]);

// FCI_FMV_ROWS=1 (diagnostics): the rows of overlay videos that were ever brighter than black, logged when a video
// ends; the rows to add to the table in fmv.cpp. luma: one frame's Y bytes, rows of pitch bytes, every second byte
void MeasureRows(const uint8_t* yuv, uint32_t width, uint32_t height, uint32_t pitch, bool uyvy);

}  // namespace gpu::fmv
