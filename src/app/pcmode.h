// PC display features of the HLE graphics path (FarCryInstincts.ini / FCI_<KEY>, see README):
//   aspect     = auto | 4:3 | 16:9 | 16:10 | 21:9 | <width>:<height> | <ratio>   picture shape of 3D frames (Hor+)
//   fov        = scale of the game's vertical field of view (1 = as on the console)
//   fps_limit  = auto (the refresh rate of the window's monitor, at most 120) | 30 (the console's rate) | 60 | ... | 120
//                (the game logic is verified up to 120 fps: kMaxFps; 0 or more than 120 is taken as 120)
//   vsync      = 0 | 1  wait for the monitor's refresh when presenting
//   shadow_distance = how far the sun's shadows reach, times the console's (1 = as on the console)
//   lod_distance  = distances at which objects and vegetation drop to lower LODs and to sprites, times the console's
//   view_distance = how far objects and vegetation are drawn, times the console's (the fog still ends the view)
// With FCI_LLE_GFX=1 everything here is off (4:3, 30 fps), so the LLE path stays as it was.
#pragma once

namespace pcmode {

// the picture shape of frames that show the 3D world (width / height); 4/3 when widescreen is off
float Aspect();
// Aspect() / (4/3): how much wider than the console's picture the 3D view is; 1 when off
float Widen();
// scale of the vertical field of view (1 = the game's own)
float FovScale();
// the most frames per second the game may show, at most kMaxFps. 30 is the console's (flip every second vblank).
// 0 = no limit only with FCI_UNCAPPED_FPS=1 (developer measurements; the game logic is not verified above 120)
int FpsLimit();
constexpr int kMaxFps = 120;
// unsupported_uncapped_fps = 1 (ini) or FCI_UNSUPPORTED_UNCAPPED_FPS=1: fps_limit 0 (no limit) and above kMaxFps
// allowed. Not supported: the game logic is only verified up to kMaxFps
bool UncappedAllowed();
// fps_limit = auto: the refresh rate of the monitor the window is on (gpu/present.cpp reports it, Hz)
void SetDisplayRefresh(int hz);
constexpr int kFpsAuto = -2;  // SetFpsLimit: follow the display
// ---- settings menu: take effect at once (the camera's next update, the next frame) ----
void SetFovScale(float scale);
void SetFpsLimit(int limit);
// ---- end settings menu ----
// ---- widescreen HUD (GitHub #20, experimental): hud_aspect = 4:3 (default) | 16:9 | 16:10 | 21:9 | auto | <w>:<h> ----
// The shape of the box whose left and right edges the HUD's edge elements (radar, health, ammo) sit at, as a
// multiple of the console's 4:3 width: 1 = the 4:3 middle (as before), at most Widen() (the screen). gpu/renderer.cpp
float HudWiden();
// settings menu: takes effect with the next frame (value as in the ini)
void SetHudAspect(const char* value);
// ---- end widescreen HUD ----
bool Vsync();
// ---- PC shadows (xdk/shadows.cpp, gpu/renderer.cpp) ----
// the sun's shadow radius as a multiple of the game's (1 = as on the console)
float ShadowDistance();
// ---- end PC shadows ----
// ---- PC level of detail (xdk/lod.cpp) ----
// multiples of the engine's e_obj_lod_ratio and e_obj_sprite_switch_dist_ratio, and of e_obj_view_dist_ratio; the
// defaults on a PC (measured cheap there); the Steam Deck's preset keeps the console's (app/settings.cpp)
constexpr float kDefaultLodDistance = 4.0f, kDefaultViewDistance = 2.0f;
float LodDistance();
float ViewDistance();
// settings menu: takes effect with the next frame (the game is held while the menu is open)
void SetLodDistance(float scale);
void SetViewDistance(float scale);
// ---- end PC level of detail ----

}  // namespace pcmode
