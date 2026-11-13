// Adapted from SCDA-XBERecomp (MIT, Copyright (c) 2026 TGP482, Joshhhuaaa): src/app/focus_pause.cpp.
// SCDA parks the game thread at its swap. Here a host thread takes the single guest CPU instead (as an
// interrupt would, rt::AcquireCpu): every guest thread, DPC and timer stops at its next preemption point,
// wherever the game is (menus, videos, loading). The guest clocks are held (runtime/guest_time.h), the sound
// output stops, rumble is switched off, and the watchdog knows the silence is deliberate.
// The settings menu (app/menu.cpp) holds the game the same way while it is open; this thread, which then owns the
// guest CPU and with it the renderer's device context, draws the menu's frames over the last picture.
#include "app/focus_pause.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "app/menu.h"  // settings menu
#include "app/settings.h"
#include "audio/hle_output.h"
#include "runtime/guest_time.h"
#include "runtime/log.h"
#include "runtime/scheduler.h"

namespace hle {
void PauseRumble(bool paused);  // hle/xinput.cpp
}  // namespace hle

namespace app {

namespace {

std::mutex g_lock;
std::condition_variable g_cv;
bool g_focus_lost = false;  // the window is inactive or minimized
bool g_menu = false;        // settings menu: open
std::atomic<bool> g_paused{false};
std::atomic<int> g_enabled{-1};  // pause_on_focus_loss; -1 until read
bool g_exiting = false;          // ResumeForExit: no pause any more

bool Enabled() {
    if (g_enabled < 0) {
        bool on = settings::Bool("pause_on_focus_loss", false);
        if (on) rt::Log("focus pause: on (pause_on_focus_loss)");
        g_enabled = on;
    }
    return g_enabled > 0;
}

// g_lock held
bool WantPause() { return !g_exiting && ((g_focus_lost && Enabled()) || g_menu); }

void Pause(const char* why) {
    LARGE_INTEGER f, t0, paused_at;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    rt::AcquireCpu(true);  // the guest stops at its next preemption point
    rt::HoldGuestTime(true);
    audio::SetPaused(true);
    hle::PauseRumble(true);
    g_paused = true;
    QueryPerformanceCounter(&paused_at);
    rt::Log("focus pause: %s, game paused (guest stopped after %.1f ms, guest clock %.3f s)", why,
            double(paused_at.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart),
            double(rt::GuestQpc()) / double(f.QuadPart));
}

void Resume(const char* why, double held_s) {
    hle::PauseRumble(false);
    audio::SetPaused(false);
    rt::HoldGuestTime(false);
    g_paused = false;
    rt::ReleaseCpu();
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    rt::Log("focus pause: %s, game resumed after %.1f s (guest clock %.3f s)", why, held_s,
            double(rt::GuestQpc()) / double(f.QuadPart));
}

void Controller() {
    using Clock = std::chrono::steady_clock;
    Clock::time_point paused_at{}, next_frame{};
    constexpr auto kMenuFrame = std::chrono::microseconds(16667);  // the menu's frames: 60 a second
    bool menu_shown = false;   // settings menu: frames of it were drawn (the guest is held since)
    for (;;) {
        bool want, menu, exiting;
        {
            std::unique_lock lock(g_lock);
            auto changed = [&] { return WantPause() != g_paused.load() || menu_shown != g_menu; };
            if (g_paused && g_menu) g_cv.wait_until(lock, next_frame, changed);
            else g_cv.wait(lock, changed);
            want = WantPause();
            menu = g_menu;
            exiting = g_exiting;
        }
        // settings menu: input settles while the guest is still held
        bool closed_now = false;
        if (menu_shown && !menu) {
            menu_shown = false;
            closed_now = true;
            MenuClosed();
        }
        if (want && !g_paused) {
            Pause(menu ? "settings menu open" : "window inactive");
            paused_at = Clock::now();
            next_frame = paused_at;
        } else if (!want && g_paused) {
            Resume(exiting ? "the program ends" : closed_now ? "settings menu closed" : "window active",
                   std::chrono::duration<double>(Clock::now() - paused_at).count());
            continue;
        }
        // ---- settings menu: a frame of it while the game is held ----
        if (g_paused && menu && Clock::now() >= next_frame) {
            menu_shown = true;
            MenuFrame();
            next_frame = std::max(next_frame + kMenuFrame, Clock::now() - kMenuFrame);
        }
    }
}

void Start() {
    static std::once_flag started;
    std::call_once(started, [] { std::thread(Controller).detach(); });
}

}  // namespace

void WindowActivated(bool active) {
    {
        std::lock_guard lock(g_lock);
        g_focus_lost = !active;
        if (!Enabled() && !g_menu) return;
    }
    Start();
    g_cv.notify_one();
}

bool FocusPaused() { return g_paused; }

// ---- settings menu ----
void MenuPause(bool open) {
    {
        std::lock_guard lock(g_lock);
        g_menu = open;
    }
    Start();
    g_cv.notify_one();
}

void SetPauseOnFocusLoss(bool on) {
    {
        std::lock_guard lock(g_lock);
        g_enabled = on;
    }
    rt::Log("focus pause: %s (settings menu)", on ? "on" : "off");
    Start();
    g_cv.notify_one();
}
// ---- end settings menu ----

void ResumeForExit(unsigned max_ms) {
    if (MenuOpen()) ToggleMenu();  // as Escape closes it: input settles, the settings are saved
    {
        std::lock_guard lock(g_lock);
        g_exiting = true;
    }
    if (!g_paused) return;
    Start();
    g_cv.notify_one();
    const ULONGLONG start = GetTickCount64();
    while (g_paused && GetTickCount64() - start < max_ms) {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    rt::Log("focus pause: the program ends, %s", g_paused ? "the game is STILL held" : "the game runs again (a save in progress can finish)");
}

}  // namespace app
