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
//
// Shadow cascades (GitHub #16): the pass picks its casters and their level of detail by their size in the map, so
// with the square k times larger it leaves out small casters (ferns) and draws palm crowns with their low detail
// meshes: near the player the shadows no longer match the objects. With shadow_cascades (and k >= 2) the pass
// alternates per view: one frame with the console's radius, which the renderer draws into a near map of its own,
// the next with k times it, as before (a second pass in the same frame draws no casters: tried first, the far map
// stayed empty). After each pass the pass's sun camera (a local of the pass, read where its frame was) gives its
// view-projection; the renderer gets the map from this pass's texture coordinates to the other map's, drawn the
// frame before (gpu/renderer.cpp, "shadow cascades"). The radius goes back to k times the game's after every pass:
// the rest of the frame reads it too, and with the console's radius left there after a near pass the receivers a few
// metres away were shaded without their shadow in every other frame (GitHub #16, flicker in 0.1.5).
#include <windows.h>

#include <cmath>
#include <cstring>

#include "app/pcmode.h"
#include "gpu/renderer.h"
#include "xdk/hle.h"

namespace {

bool HleOff() { return !xdk::HleGraphics(); }

using rt::R32;

constexpr uint32_t kEngine = 0x006B3900;  // the 3D engine's pointer
constexpr uint32_t kEngineRadius = 0x10B4, kEnvCamera = 0, kEnvRadius = 0x34;
// the shadow pass's light camera: a local at [esp + 0x700] inside the pass (sub esp 0xAC0 and four pushes), so
// 0x3D0 below the stack pointer the pass is called with; CCamera +0x364 orthographic, +0x37C/+0x380 its width and
// height, +0x2B4 view x projection (row vectors)
constexpr uint32_t kPassCamera = 0x3D0, kCamOrtho = 0x364, kCamWidth = 0x37C, kCamHeight = 0x380, kCamViewProj = 0x2B4;

void SetF(uint32_t va, float v) { std::memcpy(rt::Host(va), &v, 4); }

// the side of the sun's square for radius r (0x004585D0)
float SquareSide(uint32_t env, float r) {
    const uint32_t cam = R32(env + kEnvCamera);
    const float fov = cam ? xdk::GF(cam + 0x38) : 0;
    return std::fmax(r, 2 * r * std::sin(fov * 0.5f));
}

// the pass's camera, if the sun's for radius r was set up: its clip from world (column vectors, c = m p)
bool SunCamera(uint32_t cam, float side, double m[4][4]) {
    const float w = xdk::GF(cam + kCamWidth), h = xdk::GF(cam + kCamHeight);
    if (R32(cam + kCamOrtho) != 1 || !(std::fabs(w - side) <= 0.01f * side) || w != h) return false;
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) m[k][r] = xdk::GF(cam + kCamViewProj + 4 * (4 * r + k));  // transposed
    return true;
}

bool Invert(const double a[4][4], double out[4][4]) {
    double m[4][8];
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) m[r][k] = a[r][k], m[r][k + 4] = r == k ? 1 : 0;
    for (int col = 0; col < 4; col++) {
        int pivot = col;
        for (int r = col + 1; r < 4; r++)
            if (std::fabs(m[r][col]) > std::fabs(m[pivot][col])) pivot = r;
        if (std::fabs(m[pivot][col]) < 1e-12) return false;
        for (int k = 0; k < 8; k++) std::swap(m[col][k], m[pivot][k]);
        const double d = m[col][col];
        for (int k = 0; k < 8; k++) m[col][k] /= d;
        for (int r = 0; r < 4; r++)
            if (r != col) {
                const double f = m[r][col];
                for (int k = 0; k < 8; k++) m[r][k] -= f * m[col][k];
            }
    }
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) out[r][k] = m[r][k + 4];
    return true;
}

void Mul(const double a[4][4], const double b[4][4], double out[4][4]) {
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) {
            double s = 0;
            for (int j = 0; j < 4; j++) s += a[r][j] * b[j][k];
            out[r][k] = s;
        }
}

// texture coordinates of a sun camera's map: u = 0.5 + 0.5 x, v = 0.5 - 0.5 y, depth = z
void ToTexture(const double clip[4][4], double out[4][4]) {
    static const double kTex[4][4] = {{0.5, 0, 0, 0.5}, {0, -0.5, 0, 0.5}, {0, 0, 1, 0}, {0, 0, 0, 1}};
    Mul(kTex, clip, out);
}

// shadow cascades: per view (split screen: one pass each, told apart by the view's environment)
struct View {
    uint32_t env = 0;
    uint64_t passes = 0;        // this view's passes so far
    uint64_t done[2] = {};      // the pass that last drew each map (near, far) with the sun, 0 = none
    double clip[2][4][4] = {};  // and its camera
};
View g_views[4];

View& ViewOf(uint32_t env) {
    View* v = nullptr;
    for (View& x : g_views)
        if (x.env == env || (!v && !x.env)) v = &x;
    if (!v) v = &g_views[0], *v = View{};
    v->env = env;
    return *v;
}

}  // namespace

HLE_STDCALL(Game_ShadowPass, 7) {
    const uint32_t engine = a[0], env = a[2];
    const float k = pcmode::ShadowDistance();
    const float radius = xdk::GF(env + kEnvRadius);  // 0: no sun shadow (r_DynamicShadow off)
    {
        static float logged = -1;  // the game's radius, when it changes by a metre or more
        if (std::fabs(radius - logged) >= 1.0f) {
            logged = radius;
            rt::Log("shadows: sun shadow radius %g m (level %g m) x%.2f: square of %.1f m", radius,
                    xdk::GF(engine + kEngineRadius), k, SquareSide(env, radius * k));
        }
    }
    // ---- shadow cascades: the near pass (the console's radius) and the far one on alternate frames ----
    const bool cascades = gpu::render::ShadowCascades();
    static bool cascades_before = false;
    if (cascades_before && !cascades) {  // switched off in the F10 menu: the game's own maps from here on
        for (View& x : g_views) x = View{};
        xdk::RenderMark([] { gpu::render::DropSunShadows(); });
    }
    cascades_before = cascades;
    if (k >= 2.0f && radius > 0 && cascades) {
        View* v = &ViewOf(env);
        const int view = int(v - g_views);
        const uint64_t pass = ++v->passes;
        const int which = pass & 1;  // 0: near, 1: far
        const float r_pass = which ? radius * k : radius;
        const uint32_t cam = c.esp - kPassCamera;
        SetF(cam + kCamWidth, 0);  // (so that a pass without the sun is told apart)
        SetF(env + kEnvRadius, r_pass);
        xdk::RenderMark([view, which] { gpu::render::BeginSunShadowPass(view, !which); });
        const uint32_t r = xdk::CallOriginal(c, GUEST_ORIG(Game_ShadowPass));
        // the rest of the frame sees the far radius in every frame, whichever pass ran: the receivers' shading
        // depends on it (GitHub #16, second report: with the near pass's radius left there, objects and ground a few
        // metres away lost their shadow in every other frame although both maps held it).
        // FCI_SHADOW_CASCADE_RADIUS=pass keeps the pass's radius (0.1.5, to reproduce)
        static const bool pass_radius = [] {
            char v[16] = {};
            return GetEnvironmentVariableA("FCI_SHADOW_CASCADE_RADIUS", v, sizeof v) && !std::strcmp(v, "pass");
        }();
        if (!pass_radius) SetF(env + kEnvRadius, radius * k);
        double clip[4][4];
        const bool sun = SunCamera(cam, SquareSide(env, r_pass), clip);
        bool ok = sun;
        const float far_plane = xdk::GF(cam + 0x34);
        // the other map's texture coordinates from this one's: T other_clip this_clip^-1 T^-1 (the other map was drawn
        // by the view's pass before, with its camera)
        float m[3][4] = {};
        if (ok) {
            std::memcpy(v->clip[which], clip, sizeof clip);
            v->done[which] = pass;
            double ot[4][4], tt[4][4], ti[4][4], map[4][4];
            ok = v->done[!which] && v->done[!which] == pass - 1;
            if (ok) {
                ToTexture(v->clip[!which], ot), ToTexture(clip, tt);
                ok = Invert(tt, ti);
            }
            if (ok) {
                Mul(ot, ti, map);
                for (int i = 0; i < 3; i++)
                    for (int j = 0; j < 4; j++) m[i][j] = float(map[i][j]);
            }
        } else {
            v->done[which] = 0;
        }
        static int logged = 0;
        if (logged < 4) {
            logged++;
            // (far plane: the depth range, over which the game's constant depth bias spreads)
            rt::Log("shadows: cascades: %s pass, camera %s (square %.1f m, far plane %.1f m); the other map: u = %.4f u "
                    "%+.4f v %+.4f d %+.4f, v = %.4f u %+.4f v %+.4f d %+.4f, d = %.4f u %+.4f v %+.4f d %+.4f",
                    which ? "far" : "near", ok ? "sun" : "not the sun (or no other map)", SquareSide(env, r_pass),
                    far_plane, m[0][0], m[0][1], m[0][2], m[0][3], m[1][0], m[1][1], m[1][2], m[1][3], m[2][0],
                    m[2][1], m[2][2], m[2][3]);
        }
        xdk::RenderMark([sun, ok, m] { gpu::render::EndSunShadowPass(sun, ok, m); });
        return r;
    }
    if (k >= 2.0f && cascades) {  // no sun this frame: the receivers read the game's maps
        View& v = ViewOf(env);
        v.done[0] = v.done[1] = 0;
        const int view = int(&v - g_views);
        xdk::RenderMark([view] { gpu::render::DropSunShadow(view); });
    }
    // ---- end shadow cascades ----
    // the frame copies the eased radius here before every pass, so this does not compound
    if (k != 1.0f && radius > 0) SetF(env + kEnvRadius, radius * k);
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
