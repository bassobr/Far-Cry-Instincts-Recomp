// Adapted from SCDA-XBERecomp (MIT, Copyright (c) 2026 TGP482, Joshhhuaaa): src/kernel/xboxkrnl.cpp
// (GuestNow, HoldGuestTime). Here on the performance counter, for the four clocks of this runtime.
#include "runtime/guest_time.h"

#include <windows.h>

#include <atomic>
#include <mutex>

namespace rt {

namespace {

// a seqlock: the version is odd while a hold starts or ends
std::atomic<uint32_t> g_version{0};
std::atomic<int64_t> g_held_at{-1};   // host QPC the clocks stopped at, -1 while they run
std::atomic<int64_t> g_held_total{0};  // QPC ticks of the holds that ended
std::mutex g_writer;

int64_t Qpc() {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

int64_t Frequency() {
    static const int64_t f = [] {
        LARGE_INTEGER v;
        QueryPerformanceFrequency(&v);
        return v.QuadPart;
    }();
    return f;
}

// QPC ticks held so far, up to now
int64_t HeldTicks(int64_t now) {
    for (;;) {
        uint32_t version = g_version.load(std::memory_order_acquire);
        int64_t at = g_held_at.load(std::memory_order_acquire);
        int64_t total = g_held_total.load(std::memory_order_acquire);
        if (!(version & 1) && version == g_version.load(std::memory_order_acquire))
            return total + (at >= 0 ? now - at : 0);
    }
}

}  // namespace

int64_t GuestQpc() {
    int64_t now = Qpc();
    return now - HeldTicks(now);
}

uint64_t GuestFileTime() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t host = (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    int64_t held = HeldTicks(Qpc());
    if (!held) return host;
    return host - uint64_t(held / Frequency() * 10000000 + held % Frequency() * 10000000 / Frequency());
}

uint32_t GuestTickCount() {
    DWORD host = GetTickCount();
    int64_t held = HeldTicks(Qpc());
    if (!held) return host;
    return host - uint32_t(held * 1000 / Frequency());
}

void HoldGuestTime(bool held) {
    std::lock_guard lock(g_writer);
    if (held == (g_held_at.load() >= 0)) return;
    g_version.fetch_add(1, std::memory_order_acq_rel);
    int64_t now = Qpc();
    if (held) {
        g_held_at.store(now, std::memory_order_release);
    } else {
        g_held_total.store(g_held_total.load() + now - g_held_at.load(), std::memory_order_release);
        g_held_at.store(-1, std::memory_order_release);
    }
    g_version.fetch_add(1, std::memory_order_acq_rel);
}

bool GuestTimeHeld() { return g_held_at.load(std::memory_order_relaxed) >= 0; }

}  // namespace rt
