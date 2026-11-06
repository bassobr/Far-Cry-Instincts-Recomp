// fmv_scale, see fmv.h. The scaling follows SCDA-XBERecomp's Refit (MIT, Copyright (c) 2026 TGP482, Joshhhuaaa),
// src/game/hud.cpp: as large as the frame allows without cutting the picture, its shape kept.
#include "gpu/fmv.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "app/pcmode.h"
#include "app/settings.h"
#include "runtime/log.h"
#include "xdk/xdk.h"

namespace gpu::fmv {

namespace {

constexpr float kConsole = 4.0f / 3.0f;

// The picture rows of FCI's letterboxed videos (first, one past the last, of a picture height), measured with
// FCI_FMV_ROWS over the whole video (FCI_TEST_VIDEO plays any of them at boot). Videos not listed have no bars.
struct Rows {
    const char* name;
    uint32_t height, top, bottom;
};
// The others fill their frame (Attract_Mode, FCI_02, MULTI, PREDATOR, PREDATOR_MULTI, PREY, MAP_EDITOR,
// Previews_R6L, Crytek 800x600; MenusBack_sun is the 640x346 band itself).
constexpr Rows kRows[] = {
    {"ubisoft.bik", 480, 27, 453},
    {"intro_fci.bik", 480, 113, 367},
    {"startmenu_boat.bik", 480, 67, 413},  // title background: the menus' 640x346 band inside black
    {"credits.bik", 480, 67, 413},
    {"intro.bik", 480, 65, 415},             // dark grey bars; its first 4 s (Ubisoft logo on grey) fill the frame
    {"previews_gr3_ntsc.bik", 480, 64, 411},  // dark grey bars
};

struct Video {
    std::string name;  // lower case file name
    uint32_t width = 0, height = 0;
};

std::mutex g_lock;
Video g_videos[4];  // the latest opened first
// FCI_FMV_ROWS
std::vector<uint32_t> g_row_seen;  // per row: frames in which it showed something (not even, or bright)
uint32_t g_row_w = 0, g_row_h = 0;
uint64_t g_row_frames = 0;

void LogRows(bool end = true) {
    if (!g_row_frames) return;
    // picture: a row that showed something in more than 1 % of the frames (bars are even, some dark grey instead of
    // black; a flash or a fade now and then must not count)
    int top = -1, bottom = -1;
    for (uint32_t y = 0; y < g_row_h; y++)
        if (g_row_seen[y] * 100 > g_row_frames) (top < 0 ? top = int(y) : 0), bottom = int(y) + 1;
    rt::Log("fmv: rows %s: %ux%u, %llu frames, picture rows %d..%d (%d rows)", g_videos[0].name.c_str(), g_row_w,
            g_row_h, (unsigned long long)g_row_frames, top, bottom, bottom - top);
    if (end) g_row_frames = 0;
}

const Rows* Find(const std::string& name) {
    for (const Rows& r : kRows)
        if (name == r.name) return &r;
    return nullptr;
}

}  // namespace

bool Fill() {
    static const bool fill = [] {
        if (!xdk::HleGraphics()) return false;
        std::string v = settings::String("fmv_scale", "fill");
        for (char& ch : v) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        bool on = !(v == "4:3" || v == "1" || v == "console" || v == "off");
        rt::Log("fmv: fmv_scale %s%s", on ? "fill" : "4:3",
                on && pcmode::Widen() <= 1.0f ? " (no effect: aspect 4:3)" : "");
        return on && pcmode::Widen() > 1.0f;
    }();
    return fill;
}

void Opened(const char* name, uint32_t width, uint32_t height) {
    std::lock_guard lock(g_lock);
    static const bool rows = GetEnvironmentVariableA("FCI_FMV_ROWS", nullptr, 0) > 0;
    if (rows) LogRows(), g_row_w = g_row_h = 0;  // the next video measures afresh
    std::string n(name ? name : "");
    for (char& ch : n) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    if (size_t slash = n.find_last_of("\\/"); slash != std::string::npos) n = n.substr(slash + 1);
    // FCI_TEST_VIDEO (kernel/k_file.cpp): the first boot video's file plays another video, which is the one shown
    static char test[MAX_PATH] = {};
    static const bool tested = GetEnvironmentVariableA("FCI_TEST_VIDEO", test, sizeof test) > 0;
    if (tested && n == "ubisoft.bik") {
        n = test;
        for (char& ch : n) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    }
    for (int i = 3; i > 0; i--) g_videos[i] = g_videos[i - 1];
    g_videos[0] = {n, width, height};
    const Rows* r = Find(n);
    rt::Log("fmv: video %s %ux%u%s", n.c_str(), width, height,
            r ? (" (picture rows " + std::to_string(r->top) + ".." + std::to_string(r->bottom) + ")").c_str() : "");
}

bool IsVideo(uint32_t width, uint32_t height) {
    std::lock_guard lock(g_lock);
    for (const Video& v : g_videos)
        if (v.width && v.width == width && v.height == height) return true;
    return false;
}

float Transform(float x0, float y0, float x1, float y1, uint32_t video_h, float s[4]) {
    s[0] = 1, s[1] = 0, s[2] = 1, s[3] = 0;
    if (!Fill()) return 1;
    const float aspect = pcmode::Aspect(), wide = aspect / kConsole;
    // not across the whole width (a video in a window): in the 4:3 middle like 2D
    if (x0 > 1.0f || x1 < 639.0f || y1 - y0 < 240.0f) {
        s[0] = 1 / wide, s[1] = 320 * (1 - 1 / wide);
        return 1;
    }
    // the picture inside the bars: rows of the video that was opened last with this height
    float top = 0, bottom = 1;
    {
        std::lock_guard lock(g_lock);
        for (const Video& v : g_videos) {
            if (!v.width || v.height != video_h) continue;
            if (const Rows* r = Find(v.name)) top = float(r->top) / r->height, bottom = float(r->bottom) / r->height;
            break;
        }
    }
    float py0 = y0 + (y1 - y0) * top, py1 = y0 + (y1 - y0) * bottom;
    float pw = x1 - x0, ph = py1 - py0;
    // the frame is 480 high and aspect * 480 wide in console pixels: as large as fits, never smaller than drawn
    float z = std::max(1.0f, std::min(aspect * 480 / pw, 480 / ph));
    float cx = (x0 + x1) * 0.5f, cy = (py0 + py1) * 0.5f;
    // centred; x in the anamorphic target, where 640 is the whole (wide) width
    s[0] = z / wide, s[1] = 320 - cx * s[0];
    s[2] = z, s[3] = 240 - cy * z;
    return z;
}

void MeasureRows(const uint8_t* yuv, uint32_t width, uint32_t height, uint32_t pitch, bool uyvy) {
    static const bool on = GetEnvironmentVariableA("FCI_FMV_ROWS", nullptr, 0) > 0;
    if (!on) return;
    std::lock_guard lock(g_lock);
    if (width != g_row_w || height != g_row_h) {
        LogRows();
        g_row_w = width, g_row_h = height;
        g_row_seen.assign(height, 0);
    }
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* row = yuv + size_t(y) * pitch + (uyvy ? 1 : 0);
        uint8_t lo = 255, hi = 0;
        for (uint32_t x = 0; x < width; x++) lo = std::min(lo, row[2 * x]), hi = std::max(hi, row[2 * x]);
        if (hi - lo > 24 || hi > 64) g_row_seen[y]++;
    }
    if (++g_row_frames % 600 == 0) LogRows(false);  // long videos: so far
}

}  // namespace gpu::fmv
