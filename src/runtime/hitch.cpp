#include "runtime/hitch.h"

#include <windows.h>
#include <timeapi.h>

#include <algorithm>
#include <mutex>

#include <atomic>
#include <string>
#include <thread>

#include "runtime/log.h"
#include "runtime/profiler.h"
#include <cstdio>
#include <cstdlib>

namespace rt::hitch {

namespace {
std::atomic<uint64_t> g_n[kCount];
std::atomic<int64_t> g_ticks[kCount];
std::atomic<uint64_t> g_bytes[kCount];
const int64_t g_freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
}();
}  // namespace

bool Enabled() {
    static const bool on = GetEnvironmentVariableA("FCI_TRACE_HITCHES", nullptr, 0) > 0;
    return on;
}

double Factor() {
    static const double f = [] {
        char v[32] = {};
        GetEnvironmentVariableA("FCI_TRACE_HITCHES", v, sizeof v);
        double x = atof(v);
        return x > 1.0 ? x : 2.0;
    }();
    return f;
}

std::atomic<uint64_t> g_frame{0};
uint64_t FrameNumber() { return g_frame.load(std::memory_order_relaxed); }
void CountFrame() { g_frame.fetch_add(1, std::memory_order_relaxed); }

void Note(Event e, int64_t ticks, uint64_t bytes) {
    if (!Enabled()) return;
    g_n[e].fetch_add(1, std::memory_order_relaxed);
    g_ticks[e].fetch_add(ticks, std::memory_order_relaxed);
    g_bytes[e].fetch_add(bytes, std::memory_order_relaxed);
}

Counts Take() {
    Counts c;
    for (int i = 0; i < kCount; i++) {
        c.n[i] = g_n[i].exchange(0, std::memory_order_relaxed);
        c.ticks[i] = g_ticks[i].exchange(0, std::memory_order_relaxed);
        c.bytes[i] = g_bytes[i].exchange(0, std::memory_order_relaxed);
    }
    return c;
}

const char* Describe(const Counts& c, char* out, int size) {
    static const char* const kNames[] = {"shaders compiled", "textures made", "targets made", "file reads",
                                         "target uploads", "shaders made (cache, generation, D3D11 object)",
                                         "draws left out (shader not ready)"};
    int at = 0;
    out[0] = 0;
    for (int i = 0; i < kCount && at < size; i++) {
        if (!c.n[i]) continue;
        at += snprintf(out + at, size - at, "%s%llu %s %.2f ms", at ? ", " : "", (unsigned long long)c.n[i], kNames[i],
                       Ms(c.ticks[i]));
        if (c.bytes[i] && at < size) at += snprintf(out + at, size - at, " %.2f MB", c.bytes[i] / 1048576.0);
    }
    return out;
}

// ---- FCI_HITCH_STACKS ----
namespace {
const double g_stack_ms = [] {
    char v[32] = {};
    GetEnvironmentVariableA("FCI_HITCH_STACKS", v, sizeof v);
    return atof(v);
}();
std::atomic<int64_t> g_decode_since{0}, g_flip_at{0};
std::atomic<HANDLE> g_render{nullptr}, g_guest{nullptr};
std::atomic<DWORD> g_guest_id{0};

HANDLE OwnHandle() {
    HANDLE h = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS);
    return h;
}

// up to 4 samples of a long decode or frame
struct Watch {
    int64_t last = 0;
    int taken = 0;
    bool Want(int64_t since) {
        if (since != last) last = since, taken = 0;
        return taken++ < 4;
    }
};

void Sampler() {
    timeBeginPeriod(1);
    const int64_t limit = int64_t(g_stack_ms * double(g_freq) / 1000.0);
    int left = 600;  // samples in all
    Watch decode, frame;
    while (left > 0) {
        Sleep(DWORD(std::max(1, int(g_stack_ms / 4))));
        const int64_t now = Now();
        if (int64_t since = g_decode_since.load(); since && now - since > limit && g_render.load() && decode.Want(since)) {
            std::string s = rt::ThreadStack(g_render.load());
            rt::Log("hitch stack: render thread %.1f ms into a decode: %s", Ms(now - since), s.c_str());
            left--;
        }
        if (int64_t at = g_flip_at.load(); at && now - at > limit && g_guest.load() && frame.Want(at)) {
            std::string s = rt::ThreadStack(g_guest.load());
            rt::Log("hitch stack: guest thread %.1f ms into a frame: %s", Ms(now - at), s.c_str());
            left--;
        }
    }
}

void StartSampler() {
    static std::once_flag once;
    std::call_once(once, [] { std::thread(Sampler).detach(); });
}
}  // namespace

void DecodeBegin() {
    if (g_stack_ms <= 0) return;
    if (!g_render.load()) g_render = OwnHandle(), StartSampler();
    g_decode_since = Now();
}

void DecodeEnd() {
    if (g_stack_ms > 0) g_decode_since = 0;
}

void GuestFlip() {
    if (g_stack_ms <= 0) return;
    const DWORD id = GetCurrentThreadId();
    if (g_guest_id.load() != id) {
        g_guest = OwnHandle();  // (the old handle stays open: the presenting thread rarely changes)
        g_guest_id = id;
        StartSampler();
    }
    g_flip_at = Now();
}
// ---- end FCI_HITCH_STACKS ----

Timed::Timed(Event e) : e(e), t0(Enabled() ? Now() : 0) {}
Timed::~Timed() {
    if (t0) Note(e, Now() - t0);
}

int64_t Now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

double Ms(int64_t ticks) { return double(ticks) * 1000.0 / double(g_freq); }

}  // namespace rt::hitch
