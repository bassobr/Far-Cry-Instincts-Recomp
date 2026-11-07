// Videos behind the menus (title screen StartMenu_boat.bik, the credits) at the video's own frame rate whatever the
// game's frame rate (GitHub #13).
//
// The game plays Bink videos two ways:
// - full screen (logos, intro, attract mode, the bonus movies): the player 0x00220200 runs its own loop, BinkWait
//   (0x005AB260) before each BinkDoFrame/BinkNextFrame. BinkWait times against QueryPerformanceCounter/GetTickCount,
//   which are the guest clocks (runtime/guest_time.cpp): these play at their own rate at any frame rate already.
// - behind the menus: the game's frame (0x00049223) calls the update 0x0021FFA0 once per frame for each video object
//   (eax = object: +4 BINK, +8 handle or -1, +0x1C ended, +0x1D paused, +0x1E pausable, +0x21 still to open;
//   [esp+4] = pause; stdcall, ret 4). With a video below 25 fps (FrameRate / FrameRateDiv, BINK +0x14 / +0x18;
//   MenusBack_sun.bik, 15 fps) it asks BinkWait first. From 25 fps on (StartMenu_boat.bik and Credits.bik,
//   29.976 fps) it does not: one BinkDoFrame/BinkNextFrame per game frame, and the copy 0x0021FDB0 then lets Bink
//   skip frames while it is behind. On the Xbox the menus ran at 30 fps, so that was the video's rate; at 60, 120
//   or unlimited fps the video ran 2, 4 or many times too fast.
// The hook paces all menu videos on the guest clock (stops with a focus pause): while the next frame is not due,
// the update ends as it does after a BinkWait "wait" (nothing advanced, the menu texture keeps its picture); once
// due, the original runs unchanged (advance, copy, Bink's frame skipping when the game is slower than the video),
// and for a video below 25 fps its BinkWait (hooked too) answers "go" for this call. Opening, pausing and the end
// of a video that does not loop go to the original as is.
// Why not BinkWait: it advances exactly when the game frame's time passes the due time. At 60 fps two game frames
// (33.33 ms) are within 0.04 ms of a frame of these videos (33.36 ms), so every ~17 s the due times drift across
// the game frames, and for ~3 s the jitter of the frame times (~1 ms) decided frame by frame: 1 and 3 game frames
// per video frame in turn instead of 2 (measured: 90 of 900 frames in 30 s). The 15 fps video is a whole number
// of game frames at 30/60/120 fps and so sits on that boundary all the time (at 30 fps: 17 of 121 frames shown
// for 1 or 3 game frames instead of 2). Here a frame also counts as due when a video frame's time (less 1/8 of it)
// has passed since the last one and the video is at most 1/4 frame ahead of its schedule; so the cadence stays
// even (2, 2, 2, ...), and when the lead runs out it slips once by a game frame and is then a whole game frame
// away from the boundary (one 3 per ~17 s at 60 fps, like a TV's pulldown).
// FCI_NO_VIDEO_PACE=1 keeps the game's behaviour (comparison).
//
// FCI_TRACE_BINK=1: every open video's frames advanced per second (a host thread reads BINK +0x0C FrameNum), and
// for the menu videos the game frames per second and the gaps between advances (in game frames and ms).
#include "hle/video_pace.h"

#include <windows.h>

#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hle/original.h"
#include "runtime/guest.h"
#include "runtime/guest_time.h"
#include "runtime/log.h"

namespace hle {

namespace {

constexpr uint32_t kMenuVideoUpdate = 0x0021FFA0, kBinkWait = 0x005AB260, kMenuBinkWaitReturn = 0x00220016;
// struct BINK
constexpr uint32_t kWidth = 0x00, kHeight = 0x04, kFrames = 0x08, kFrameNum = 0x0C, kFrameRate = 0x14,
                   kFrameRateDiv = 0x18;
// the menu video object of 0x0021FFA0
constexpr uint32_t kObjBink = 0x04, kObjHandle = 0x08, kObjEnded = 0x1C, kObjPaused = 0x1D, kObjPausable = 0x1E;

bool Env(const char* name) { return GetEnvironmentVariableA(name, nullptr, 0) > 0; }
bool PaceOn() {
    static const bool on = !Env("FCI_NO_VIDEO_PACE");
    return on;
}
bool Tracing() {
    static const bool on = Env("FCI_TRACE_BINK");
    return on;
}

using Clock = std::chrono::steady_clock;
double Seconds(Clock::duration d) { return std::chrono::duration<double>(d).count(); }

// ---- FCI_TRACE_BINK ----
struct Open {
    uint32_t bink, width, height, frames;
    std::string name;
    uint32_t last;  // FrameNum at the last sample
};
std::mutex g_lock;
std::vector<Open> g_open;

std::string BaseName(const char* name) {
    std::string n(name ? name : "");
    if (size_t slash = n.find_last_of("\\/"); slash != std::string::npos) n = n.substr(slash + 1);
    return n;
}

// every second: how far each open video advanced. A closed video's memory goes back to the heap; an entry whose
// header no longer reads as the video it was is dropped.
void Sampler() {
    auto last = Clock::now();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const auto now = Clock::now();
        const double dt = Seconds(now - last);
        last = now;
        std::lock_guard lock(g_lock);
        for (size_t i = 0; i < g_open.size();) {
            Open& o = g_open[i];
            const uint32_t frame = rt::R32(o.bink + kFrameNum);
            if (rt::R32(o.bink + kWidth) != o.width || rt::R32(o.bink + kHeight) != o.height ||
                rt::R32(o.bink + kFrames) != o.frames || frame > o.frames) {
                g_open.erase(g_open.begin() + i);
                continue;
            }
            if (frame != o.last) {
                // a looping video wraps from its last frame to 1
                const uint32_t advanced = frame > o.last ? frame - o.last : frame + o.frames - o.last;
                rt::Log("bink: %s frame %u/%u, %u in %.2f s = %.2f fps", o.name.c_str(), frame, o.frames, advanced, dt,
                        advanced / dt);
                o.last = frame;
            }
            i++;
        }
    }
}

// the menu videos, from the update hook (game thread): game frames, advances, gaps between advances
struct MenuStats {
    uint32_t bink = 0, frame = 0;
    uint32_t calls = 0, advances = 0, since = 0;  // since: game frames since the last advance
    uint32_t gaps[5] = {};                        // gaps of 1, 2, 3, 4, 5+ game frames
    double min_ms = 1e9, max_ms = 0;
    Clock::time_point start, last_advance;
} g_menu;

void TraceMenu(uint32_t bink) {
    MenuStats& s = g_menu;
    const auto now = Clock::now();
    const uint32_t frame = rt::R32(bink + kFrameNum);
    if (s.bink != bink) {
        s = {};
        s.bink = bink, s.frame = frame, s.start = s.last_advance = now;
        return;
    }
    s.calls++, s.since++;
    if (frame != s.frame) {
        s.advances++;
        s.gaps[s.since < 5 ? s.since - 1 : 4]++;
        const double ms = Seconds(now - s.last_advance) * 1000;
        if (ms < s.min_ms) s.min_ms = ms;
        if (ms > s.max_ms) s.max_ms = ms;
        s.frame = frame, s.since = 0, s.last_advance = now;
    }
    const double dt = Seconds(now - s.start);
    if (dt >= 1.0) {
        rt::Log("bink: menu video %ux%u: %u game frames, %u advances in %.2f s = %.2f fps; gaps 1:%u 2:%u 3:%u 4:%u 5+:%u "
                "frames, %.1f..%.1f ms",
                rt::R32(bink + kWidth), rt::R32(bink + kHeight), s.calls, s.advances, dt, s.advances / dt, s.gaps[0],
                s.gaps[1], s.gaps[2], s.gaps[3], s.gaps[4], s.advances ? s.min_ms : 0.0, s.max_ms);
        s.calls = s.advances = 0;
        std::memset(s.gaps, 0, sizeof s.gaps);
        s.min_ms = 1e9, s.max_ms = 0, s.start = now;
    }
}

}  // namespace

namespace {
void ForgetPace(uint32_t bink);
}

void VideoOpened(uint32_t bink, const char* name, uint32_t caller) {
    if (!bink) return;
    ForgetPace(bink);  // the game reopens the menu videos, often at the same address
    if (!Tracing()) return;
    const uint32_t rate = rt::R32(bink + kFrameRate), div = rt::R32(bink + kFrameRateDiv);
    rt::Log("bink: open %s at %08X, %ux%u, %u frames, %u/%u = %.3f fps, %s player (%08X)", BaseName(name).c_str(), bink,
            rt::R32(bink + kWidth), rt::R32(bink + kHeight), rt::R32(bink + kFrames), rate, div,
            div ? double(rate) / div : 0.0, caller == 0x00220273u ? "full screen" : "menu", caller);
    std::lock_guard lock(g_lock);
    static bool started = false;
    if (!started) {
        started = true;
        std::thread(Sampler).detach();
    }
    for (size_t i = 0; i < g_open.size(); i++)
        if (g_open[i].bink == bink) g_open.erase(g_open.begin() + i--);
    g_open.push_back({bink, rt::R32(bink + kWidth), rt::R32(bink + kHeight), rt::R32(bink + kFrames), BaseName(name),
                      rt::R32(bink + kFrameNum)});
}

namespace {

// the schedule of one menu video (the game thread only)
struct Pace {
    uint32_t bink = 0;
    bool running = false;  // false: (re)start the schedule at the next due frame (new video, after a pause or the end)
    uint32_t frame = 0;    // FrameNum after the last advance
    double due = 0;        // guest seconds: when the next frame is due
    double last = 0;       // guest seconds of the last advance
};
Pace g_pace[4];  // the game keeps one or two menu videos open
uint32_t g_go;   // the BINK whose update runs now with its frame due

Pace& PaceOf(uint32_t bink) {
    for (Pace& p : g_pace)
        if (p.bink == bink) return p;
    static unsigned next = 0;
    Pace& p = g_pace[next++ % 4];
    p = {};
    p.bink = bink;
    return p;
}

void ForgetPace(uint32_t bink) {
    for (Pace& p : g_pace)
        if (p.bink == bink) p = {};
}

double GuestSeconds() {
    static const double frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return double(f.QuadPart);
    }();
    return double(rt::GuestQpc()) / frequency;
}

// frames from FrameNum a to b; a looping video goes from its last frame on to 1
uint32_t FramesBetween(uint32_t a, uint32_t b, uint32_t frames) { return b >= a ? b - a : b + frames - a; }

}  // namespace

// 0x0021FFA0: eax = video object, [esp+4] = pause (stdcall, ret 4); preserves ebx, ecx, esi, edi, ebp
void MenuVideoUpdate(rt::Cpu& c) {
    const uint32_t obj = c.eax;
    const uint32_t bink = rt::R32(obj + kObjHandle) != 0xFFFFFFFFu ? rt::R32(obj + kObjBink) : 0;
    Pace* pace = nullptr;
    if (bink && PaceOn()) {
        const uint8_t pause = uint8_t(rt::R32(c.esp + 4));
        const bool pause_change = rt::R8(obj + kObjPausable) && rt::R8(obj + kObjPaused) != pause;
        const uint32_t rate = rt::R32(bink + kFrameRate), div = rt::R32(bink + kFrameRateDiv);
        Pace& p = PaceOf(bink);
        if (!pause_change && !rt::R8(obj + kObjEnded) && !rt::R8(obj + kObjPaused) && rate && div) {
            const double now = GuestSeconds(), period = double(div) / rate;
            if (!p.running || now - p.due > 1.0) {
                // a new start, or the game stood still for a while (loading): no racing to catch up
                p.running = true;
                p.due = now;
                p.last = now - period;
            }
            const bool due = now >= p.due ||
                             (now - p.last >= period * (7.0 / 8) && p.due - now <= period * (1.0 / 4));
            if (!due) {
                if (Tracing()) TraceMenu(bink);
                c.esp += 8;  // return address and the argument
                return;
            }
            p.frame = rt::R32(bink + kFrameNum);
            p.last = now;
            pace = &p;
        } else {
            p.running = false;
        }
    }
    g_go = pace ? bink : 0;  // below 25 fps the update asks BinkWait: this frame is due
    GUEST_ORIG_AT(kMenuVideoUpdate)(c);
    g_go = 0;
    if (pace) {
        // the frames this update went on (more than one when Bink skipped to catch up)
        const uint32_t frame = rt::R32(bink + kFrameNum);
        const uint32_t rate = rt::R32(bink + kFrameRate), div = rt::R32(bink + kFrameRateDiv);
        const uint32_t frames = FramesBetween(pace->frame, frame, rt::R32(bink + kFrames));
        if (rate && div && frames <= rate / div)
            pace->due += frames * (double(div) / rate);
        else
            pace->running = false;  // more than a second's frames at once: not a skip to catch up, start afresh
        pace->frame = frame;
    }
    if (bink && Tracing()) TraceMenu(bink);
}

// 0x005AB260 BinkWait(bink), stdcall: nonzero = the next frame is not due yet. The menu update (call at 0x00220011,
// videos below 25 fps) gets "go" when the hook above found the frame due; every other caller (the full screen
// player) the original.
void BinkWait(rt::Cpu& c) {
    if (g_go && rt::R32(c.esp) == kMenuBinkWaitReturn && rt::R32(c.esp + 4) == g_go) {
        c.eax = 0;
        c.esp += 8;
        return;
    }
    GUEST_ORIG_AT(kBinkWait)(c);
}

}  // namespace hle
