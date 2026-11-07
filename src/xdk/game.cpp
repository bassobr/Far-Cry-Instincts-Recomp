// Game patches for the PC display features (app/pcmode.h): Hor+ widescreen and the field of view.
//
// FCI's camera (CryEngine's CCamera) keeps its horizontal field of view at +0x38 (radians) and its aspect ratio
// (width / height) at +0x384. Its update, 0x002A84E0 (camera in ebx), builds the projection with
// D3DXMatrixPerspectiveFovRH(fov / aspect, aspect, near, far) and the culling frustum from tan(fov / 2) and
// tan(fov / aspect / 2). Both come from the same two values, so widening them here widens what is drawn, what is
// culled and every pass derived from the camera (water reflections, shadow frusta) consistently. The widened values
// stay in the camera afterwards (the water culls later from them), and the next update starts again from the
// game's own values (see Widened), so nothing compounds when the game animates them (zoom, cutscenes).
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "app/pcmode.h"
#include "gpu/ao.h"
#include "hle/mouse_look.h"  // direct mouse look
#include "xdk/hle.h"

namespace {

bool HleOff() { return !xdk::HleGraphics(); }

using rt::R32;

const bool trace_view = GetEnvironmentVariableA("FCI_TRACE_VIEW", nullptr, 0) > 0;

constexpr uint32_t kCamFov = 0x38, kCamOrtho = 0x364, kCamAspect = 0x384;

void SetF(uint32_t va, float v) { std::memcpy(rt::Host(va), &v, 4); }

// The widened values stay in the camera after its update: the game culls its water patches later from the
// camera's own field of view and aspect (put back to 4:3, the patches beside the 4:3 middle were dropped and the
// sea floor showed through). Per camera the console's values and the widened ones written: a camera that still
// holds what was written is still at the console's values; anything else is the game's new value (zoom).
struct Widened {
    uint32_t cam;
    float fov, aspect;            // the game's own
    float wide_fov, wide_aspect;  // what was written
};
std::vector<Widened> g_widened;

// FCI_TEST_CRT_MATH=1: runs the game's x87 tanh (0x005001F5) and exp (0x004ED12C) once, on the game thread at the
// first camera update, for an argument of each class (normal, zero, infinite, NaN, denormal) and logs them beside
// the host's. The CRT dispatches each class to its own handler through a byte-packed table (fci-recomp,
// PackedStructPointers); tanh of a normal number is the call of GitHub #7 (0x00500275).
void TestCrtMath(rt::Cpu& c) {
    const rt::Cpu saved = c;
    struct Fn { const char* name; uint32_t va; double (*host)(double); } fns[] = {
        {"tanh", 0x005001F5u, [](double x) { return std::tanh(x); }},
        {"exp", 0x004ED12Cu, [](double x) { return std::exp(x); }},
    };
    const double inf = INFINITY, args[] = {0.5, -0.25, 3.0, -20.0, 0.0, -0.0, 1e-310, inf, -inf, NAN};
    int bad = 0;
    for (const auto& fn : fns)
        for (double x : args) {
            rt::FPush(c, x);
            rt::GuestCall(c, fn.va, {});
            const double y = rt::ST(c, 0), want = fn.host(x);
            rt::FPop(c);
            const bool ok = (std::isnan(y) && std::isnan(want)) || y == want || std::fabs(y - want) <= 1e-12 * std::fabs(want);
            bad += !ok;
            rt::Log("crt math: %s(%g) = %.17g, host %.17g%s", fn.name, x, y, want, ok ? "" : "  MISMATCH");
        }
    rt::Log("crt math: %d mismatches", bad);
    c = saved;
}

}  // namespace

HLE_STDCALL(Game_CameraUpdate, 0) {
    static bool test_math = GetEnvironmentVariableA("FCI_TEST_CRT_MATH", nullptr, 0) > 0;
    if (test_math) {
        test_math = false;
        TestCrtMath(c);
    }
    const uint32_t cam = c.ebx;
    const float widen = pcmode::Widen(), fov_scale = pcmode::FovScale();
    float fov = xdk::GF(cam + kCamFov), aspect = xdk::GF(cam + kCamAspect);
    auto known = std::find_if(g_widened.begin(), g_widened.end(), [cam](const Widened& w) { return w.cam == cam; });
    if (known != g_widened.end() && fov == known->wide_fov && aspect == known->wide_aspect)
        fov = known->fov, aspect = known->aspect;  // still widened from the last update: the game's values
    // the screen's cameras (4:3, the split screen's views); not square render to texture views or ortho ones
    const bool screen = R32(cam + kCamOrtho) == 0 && aspect > 1.2f && aspect < 3.6f && fov > 0.05f && fov < 3.0f;
    const bool patch = (widen != 1.0f || fov_scale != 1.0f) && screen;
    // ---- direct mouse look (hle/mouse_look.cpp): the client's view (built by 0x000F0500) shows this frame's
    // mouse turn already; the game's own fov (before the widening) gives the zoom ----
    if (R32(c.esp) == 0x000F0C55u) hle::PredictView(cam, fov);
    // ---- end direct mouse look ----
    if (screen && trace_view) {  // FCI_TRACE_VIEW=1: where the screen's camera is, every 5 s (compare runs)
        static ULONGLONG last = 0;
        if (GetTickCount64() - last >= 5000) {
            last = GetTickCount64();
            rt::Log("game: view at %.2f %.2f %.2f", xdk::GF(cam), xdk::GF(cam + 4), xdk::GF(cam + 8));
        }
    }
    {
        static std::vector<uint64_t> seen;  // each camera shape and caller once
        uint32_t from = R32(c.esp);
        uint64_t key = (uint64_t(from) << 32) ^ (uint32_t(aspect * 1000) << 12) ^ uint32_t(fov * 1000);
        if (seen.size() < 32 && std::find(seen.begin(), seen.end(), key) == seen.end()) {
            seen.push_back(key);
            rt::Log("game: camera %08X from %08X: fov %.4f aspect %.4f near %g far %g ortho %u%s", cam, from, fov,
                    aspect, xdk::GF(cam + 0x30), xdk::GF(cam + 0x34), R32(cam + kCamOrtho), patch ? " (widened)" : "");
        }
    }
    // ---- fov beyond the engine's limit ----
    // The vertical field of view (fov / aspect) stays, times fov_scale; the horizontal one grows with the screen.
    // The engine's angle (vertical x aspect) gives the frustum's sides at tan(fov / 2), so it has to stay below pi:
    // in play (game fov pi / 2) that held fov only up to ~1.45x at 16:9 and ~1.08x at 21:9, and on 32:9 below 1.
    // Beyond it the engine gets a culling angle that stays valid and at least as wide as the view (the true
    // horizontal angle, not less than the limit) with the aspect that keeps the vertical angle exact, and after
    // the update the projection's x scale is set to the real aspect (column 0 of the projection matrices; the
    // planes come from the angles, not from these). proj_fovy / proj_aspect: the picture's projection, for AO.
    float proj_fovy = fov / aspect, proj_aspect = aspect, x_fix = 1.0f;
    if (patch) {
        constexpr float kEngineLimit = 3.05f;
        const float want = fov * widen * fov_scale, wide_aspect = aspect * widen;
        proj_aspect = wide_aspect;
        if (want <= kEngineLimit) {
            proj_fovy = want / wide_aspect;
            SetF(cam + kCamFov, want);
            SetF(cam + kCamAspect, wide_aspect);
        } else {
            proj_fovy = std::min(fov / aspect * fov_scale, 2.8f);  // 160 degrees vertically at most
            const float horizontal = 2.0f * std::atan(wide_aspect * std::tan(proj_fovy * 0.5f));
            const float cull = std::clamp(horizontal, kEngineLimit, 3.1f);
            SetF(cam + kCamFov, cull);
            SetF(cam + kCamAspect, cull / proj_fovy);
            x_fix = cull / proj_fovy / wide_aspect;
            static float logged = 0;  // once per requested value: a tester's log shows it
            if (logged != fov * fov_scale) {
                logged = fov * fov_scale;
                rt::Log("game: fov x%.2f at aspect %.3f beyond the engine's angle (game fov %.4f): projection %.1f x "
                        "%.1f degrees, culling %.1f degrees wide", fov_scale, wide_aspect, fov,
                        horizontal * 57.29578f, proj_fovy * 57.29578f, cull * 57.29578f);
            }
        }
    }
    // ---- end fov beyond the engine's limit ----
    // ---- ambient occlusion (gpu/ao.h): the screen camera's projection, PerspectiveFovRH(fov / aspect, aspect) ----
    if (screen) {
        const float f = proj_fovy, a = proj_aspect;
        const float y_scale = 1.0f / std::tan(f * 0.5f);
        // (performance: in push buffer order, the render thread may still draw the frame before)
        const float x_scale = y_scale / a, far_plane = xdk::GF(cam + 0x34);
        xdk::RenderPost([=] { gpu::ao::SetProjection(x_scale, y_scale, far_plane); });
    }
    // ---- end ambient occlusion ----
    uint32_t r = xdk::CallOriginal(c, GUEST_ORIG(Game_CameraUpdate));
    // ---- fov beyond the engine's limit: projection +0x234, with depth scale +0x274, view x projection +0x2B4/+0x2F4
    // (row vectors: x_clip = column 0; the depth scale matrix of +0x274 is diagonal, so the same factor holds) ----
    if (x_fix != 1.0f)
        for (uint32_t m : {0x234u, 0x274u, 0x2B4u, 0x2F4u})
            for (uint32_t row = 0; row < 4; row++) SetF(cam + m + row * 16, xdk::GF(cam + m + row * 16) * x_fix);
    // ---- end fov beyond the engine's limit ----
    if (patch) {
        Widened w{cam, fov, aspect, xdk::GF(cam + kCamFov), xdk::GF(cam + kCamAspect)};
        if (known != g_widened.end()) *known = w;
        else if (g_widened.size() < 64) g_widened.push_back(w);
    } else if (known != g_widened.end() && xdk::GF(cam + kCamFov) == known->wide_fov) {
        SetF(cam + kCamFov, fov), SetF(cam + kCamAspect, aspect);  // no longer widened (setting off, not a screen camera)
    }
    return r;
}

// The renderer's second projection, 0x002469E0 (renderer in eax), for draw items with flag bit 0 (the first
// person view): PerspectiveFovRH(fov +0x698, width +0x6B8 / height +0x6BC x pixel aspect +0x6AC, 1e-8, far +0x6C8),
// independent of the camera. Its aspect widens the same way; 0x00246BC0 restores the camera's matrices after.
HLE_STDCALL(Game_FirstPersonProjection, 0) {
    const uint32_t renderer = c.eax;
    const float widen = pcmode::Widen();
    const float pixel_aspect = xdk::GF(renderer + 0x6AC);
    LOG_FIRST(4, "game: first person projection, fov %.4f viewport %ux%u pixel aspect %.4f far %g%s",
              xdk::GF(renderer + 0x698), R32(renderer + 0x6B8), R32(renderer + 0x6BC), pixel_aspect,
              xdk::GF(renderer + 0x6C8), widen != 1.0f ? " (widened)" : "");
    if (widen != 1.0f) SetF(renderer + 0x6AC, pixel_aspect * widen);
    // ---- direct mouse look (hle/mouse_look.cpp): the hands and the weapon with the view they were placed for ----
    float view[16];
    const bool fp_view = hle::FirstPersonView(renderer, view);
    // ---- end direct mouse look ----
    uint32_t r = xdk::CallOriginal(c, GUEST_ORIG(Game_FirstPersonProjection));
    if (fp_view) hle::FirstPersonViewDone(renderer, view);
    if (widen != 1.0f) SetF(renderer + 0x6AC, pixel_aspect);
    return r;
}
