#include "app/pcmode.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <string>

#include "app/output.h"  // display output options
#include "app/settings.h"
#include "runtime/log.h"
#include "xdk/xdk.h"

namespace pcmode {

namespace {

constexpr float kConsole = 4.0f / 3.0f;
// PC level of detail: the range of lod_distance and view_distance
constexpr float kMinLod = 0.5f, kMaxLod = 8.0f;

struct Mode {
    float aspect = kConsole, fov = 1.0f;
    int fps = 30;  // kFpsAuto: the display's refresh rate
    bool vsync = false;
    float shadow_distance = 1.0f;  // PC shadows
    float lod_distance = 1.0f, view_distance = 1.0f;  // PC level of detail
};

// "16:9", "1.777", "auto" (the output's shape: the resolution, else the chosen monitor; app/output.h)
float ParseAspect(std::string v) {
    for (char& ch : v) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    if (v.empty() || v == "auto") return output::AutoAspect();  // display output options
    if (v == "off" || v == "console") return kConsole;
    size_t colon = v.find_first_of(":x/");
    if (colon != std::string::npos) {
        float w = float(std::atof(v.c_str())), h = float(std::atof(v.c_str() + colon + 1));
        return w > 0 && h > 0 ? w / h : kConsole;
    }
    float r = float(std::atof(v.c_str()));
    return r > 0 ? r : kConsole;
}

const Mode& Get() {
    static const Mode mode = [] {
        Mode m;
        if (!xdk::HleGraphics()) {
            rt::Log("pcmode: LLE graphics: 4:3, 30 fps");
            return m;
        }
        m.aspect = std::clamp(ParseAspect(settings::String("aspect", "auto")), kConsole, 4.0f);
        if (m.aspect < kConsole * 1.01f) m.aspect = kConsole;  // narrower than 4:3 stays 4:3 (letterboxed)
        m.fov = std::clamp(settings::Float("fov", 1.0f), 0.5f, 2.0f);
        // fps_limit = auto (the default): the display's refresh rate (60 on the Steam Deck LCD, 90 on the OLED, 120
        // on the ROG Ally X, 144 on many PC monitors), a number: that, 0: no limit
        const std::string fps = settings::String("fps_limit", "auto");
        if (fps.empty() || fps == "auto") {
            m.fps = kFpsAuto;
        } else {
            m.fps = std::clamp(std::atoi(fps.c_str()), 0, 1000);
            if (m.fps > 0 && m.fps < 10) m.fps = 10;
        }
        m.vsync = settings::Int("vsync", 0) != 0;
        m.shadow_distance = std::clamp(settings::Float("shadow_distance", 1.0f), 1.0f, 8.0f);  // PC shadows
        // PC level of detail
        m.lod_distance = std::clamp(settings::Float("lod_distance", kDefaultLodDistance), kMinLod, kMaxLod);
        m.view_distance = std::clamp(settings::Float("view_distance", kDefaultViewDistance), kMinLod, kMaxLod);
        rt::Log("pcmode: aspect %.4f (Hor+ x%.3f), fov x%.2f, fps limit %s, vsync %d, shadow distance x%.2f, lod distance "
                "x%.2f, view distance x%.2f", m.aspect, m.aspect / kConsole, m.fov,
                m.fps == kFpsAuto ? "auto" : std::to_string(m.fps).c_str(), int(m.vsync), m.shadow_distance,
                m.lod_distance, m.view_distance);
        return m;
    }();
    return mode;
}

}  // namespace

float Aspect() { return Get().aspect; }
float Widen() { return Get().aspect / kConsole; }
// ---- settings menu: fov and fps_limit change while running (SetFovScale, SetFpsLimit) ----
namespace {
std::atomic<float> g_fov{0};  // 0: as read at start
std::atomic<int> g_fps{-1};   // -1: as read at start
}  // namespace
float FovScale() {
    float f = g_fov.load(std::memory_order_relaxed);
    return f > 0 ? f : Get().fov;
}
// ---- fps_limit = auto ----
namespace {
std::atomic<int> g_refresh{0};  // Hz of the window's monitor, 0 until known
// a refresh rate the frame limit can follow: 24-1000 Hz, else 60 (a virtual display may report 0 or 1)
int Sensible(int hz) { return hz >= 24 && hz <= 1000 ? hz : 60; }
}  // namespace
void SetDisplayRefresh(int hz) {
    int now = Sensible(hz), before = g_refresh.exchange(now);
    if (now != before)
        rt::Log("pcmode: display refresh %d Hz%s%s", hz, now != hz ? ", taken as 60" : "",
                FpsLimit() == now ? " (fps limit auto)" : "");
}
// ---- end fps_limit = auto ----
int FpsLimit() {
    int n = g_fps.load(std::memory_order_relaxed);
    if (n < 0 && n != kFpsAuto) n = Get().fps;
    if (n != kFpsAuto) return n;
    int hz = g_refresh.load(std::memory_order_relaxed);
    return hz ? hz : Sensible(output::Get().desktop_hz);  // before the first present: the chosen monitor's
}
void SetFovScale(float scale) {
    if (!xdk::HleGraphics()) return;
    g_fov = std::clamp(scale, 0.5f, 2.0f);
    rt::Log("pcmode: fov x%.2f (settings menu)", g_fov.load());
}
void SetFpsLimit(int limit) {
    if (!xdk::HleGraphics()) return;
    if (limit != kFpsAuto) {
        limit = std::clamp(limit, 0, 1000);
        if (limit > 0 && limit < 10) limit = 10;
    }
    g_fps = limit;
    rt::Log("pcmode: fps limit %s (settings menu)", limit == kFpsAuto ? "auto" : std::to_string(limit).c_str());
}
// ---- end settings menu ----
bool Vsync() { return Get().vsync; }
float ShadowDistance() { return Get().shadow_distance; }  // PC shadows
// ---- PC level of detail ----
namespace {
std::atomic<float> g_lod{0}, g_view{0};  // 0: as read at start
}  // namespace
float LodDistance() {
    float f = g_lod.load(std::memory_order_relaxed);
    return f > 0 ? f : Get().lod_distance;
}
float ViewDistance() {
    float f = g_view.load(std::memory_order_relaxed);
    return f > 0 ? f : Get().view_distance;
}
void SetLodDistance(float scale) {
    if (!xdk::HleGraphics()) return;
    g_lod = std::clamp(scale, kMinLod, kMaxLod);
    xdk::SetLodScales(LodDistance(), ViewDistance());
}
void SetViewDistance(float scale) {
    if (!xdk::HleGraphics()) return;
    g_view = std::clamp(scale, kMinLod, kMaxLod);
    xdk::SetLodScales(LodDistance(), ViewDistance());
}
// ---- end PC level of detail ----

}  // namespace pcmode
