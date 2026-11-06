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

}  // namespace

HLE_STDCALL(Game_CameraUpdate, 0) {
    const uint32_t cam = c.ebx;
    const float widen = pcmode::Widen(), fov_scale = pcmode::FovScale();
    float fov = xdk::GF(cam + kCamFov), aspect = xdk::GF(cam + kCamAspect);
    auto known = std::find_if(g_widened.begin(), g_widened.end(), [cam](const Widened& w) { return w.cam == cam; });
    if (known != g_widened.end() && fov == known->wide_fov && aspect == known->wide_aspect)
        fov = known->fov, aspect = known->aspect;  // still widened from the last update: the game's values
    // the screen's cameras (4:3, the split screen's views); not square render to texture views or ortho ones
    const bool screen = R32(cam + kCamOrtho) == 0 && aspect > 1.2f && aspect < 3.6f && fov > 0.05f && fov < 3.0f;
    const bool patch = (widen != 1.0f || fov_scale != 1.0f) && screen;
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
    if (patch) {
        // the vertical field of view (fov / aspect) stays, times fov_scale; the horizontal one grows with the screen
        float wide_fov = std::min(fov * widen * fov_scale, 3.05f);
        SetF(cam + kCamFov, wide_fov);
        SetF(cam + kCamAspect, aspect * widen);
    }
    // ---- ambient occlusion (gpu/ao.h): the screen camera's projection, PerspectiveFovRH(fov / aspect, aspect) ----
    if (screen) {
        const float f = xdk::GF(cam + kCamFov), a = xdk::GF(cam + kCamAspect);
        const float y_scale = 1.0f / std::tan(f / a * 0.5f);
        // (performance: in push buffer order, the render thread may still draw the frame before)
        const float x_scale = y_scale / a, far_plane = xdk::GF(cam + 0x34);
        xdk::RenderPost([=] { gpu::ao::SetProjection(x_scale, y_scale, far_plane); });
    }
    // ---- end ambient occlusion ----
    uint32_t r = xdk::CallOriginal(c, GUEST_ORIG(Game_CameraUpdate));
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
    uint32_t r = xdk::CallOriginal(c, GUEST_ORIG(Game_FirstPersonProjection));
    if (widen != 1.0f) SetF(renderer + 0x6AC, pixel_aspect);
    return r;
}
