// Game patch for the PC shadow feature (app/pcmode.h): how far the sun's shadows reach.
//
// FCI has one dynamic shadow map per frame (1024x1024, 16 bit depth; four of 512x512 in another mode), for the
// strongest shadow casting light: the sun, else a spot light. The engine ([0x006B3900], 0x0072E230 in the first
// level) holds the shadow radius R, "DynamicShadowRadius" (+0x10B4: level property or Lua SetDynamicShadowRadius,
// default 10 m, also in the first level). It is used twice:
//  - the view's environment ([engine+0x3348], 0x0072FC88) eases +0x468 towards it (or towards a zone's own value)
//    at ShadowRadiusSpeed (0x00463183); the frame copies that to +0x34 just before the shadow pass (0x00475247,
//    0 with r_DynamicShadow off). The shadow pass 0x00458C80 (Game_ShadowPass) picks the shadow casting lights
//    and renders their maps; for the sun (light flag 2) 0x004585D0 puts an orthographic camera over a square of
//    side S = max(R, 2 R sin(fov / 2)) (fov: the view camera's horizontal one), centred S/2 ahead of the camera,
//    snapped to whole texels. The casters' culling and the receivers' texture matrix come from that camera.
//  - which terrain sectors receive the shadow: 0x0047A2C0 (Game_ShadowReceiveTest, per sector) lets those with
//    (distance to the camera - 46 m) <= R through.
// With R = 10 m the square is 16-20 m wide: shadows end some 15 m ahead. shadow_distance scales R at both places
// (only while they run: the eased value and the level's value stay the game's, so easing and zones work as
// before); the renderer raises the map's resolution by the same factor (gpu/renderer.cpp), so a shadow texel
// keeps the size it has at shadow_scale.
#include <windows.h>

#include <cmath>
#include <cstring>

#include "app/pcmode.h"
#include "xdk/hle.h"

namespace {

bool HleOff() { return !xdk::HleGraphics(); }

using rt::R32;

constexpr uint32_t kEngine = 0x006B3900;  // the 3D engine's pointer
constexpr uint32_t kEngineRadius = 0x10B4, kEnvCamera = 0, kEnvRadius = 0x34;

void SetF(uint32_t va, float v) { std::memcpy(rt::Host(va), &v, 4); }

}  // namespace

HLE_STDCALL(Game_ShadowPass, 7) {
    const uint32_t engine = a[0], env = a[2];
    const float k = pcmode::ShadowDistance();
    const float radius = xdk::GF(env + kEnvRadius);  // 0: no sun shadow (r_DynamicShadow off)
    // the frame copies the eased radius here before every pass, so this does not compound
    if (k != 1.0f && radius > 0) SetF(env + kEnvRadius, radius * k);
    {
        static float logged = -1;  // the game's radius, when it changes by a metre or more
        if (std::fabs(radius - logged) >= 1.0f) {
            logged = radius;
            const uint32_t cam = R32(env + kEnvCamera);
            const float fov = cam ? xdk::GF(cam + 0x38) : 0, r = radius * k;
            rt::Log("shadows: sun shadow radius %g m (level %g m) x%.2f: square of %.1f m", radius,
                    xdk::GF(engine + kEngineRadius), k, std::fmax(r, 2 * r * std::sin(fov * 0.5f)));
        }
    }
    return xdk::CallOriginal(c, GUEST_ORIG(Game_ShadowPass));
}

// eax = the sector, ecx = the camera position; 5 stack arguments
HLE_STDCALL(Game_ShadowReceiveTest, 5) {
    const float k = pcmode::ShadowDistance();
    const uint32_t engine = R32(kEngine);
    if (k == 1.0f || !engine) return xdk::CallOriginal(c, GUEST_ORIG(Game_ShadowReceiveTest));
    const float radius = xdk::GF(engine + kEngineRadius);
    SetF(engine + kEngineRadius, radius * k);
    uint32_t r = xdk::CallOriginal(c, GUEST_ORIG(Game_ShadowReceiveTest));
    SetF(engine + kEngineRadius, radius);
    return r;
}
