#include "runtime/scheduler.h"

#include <windows.h>
#include <timeapi.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include "runtime/guest.h"
#include "runtime/hitch.h"
#include "runtime/thread.h"

namespace rt {

namespace {

// IRQL decides who may take the CPU from whom, as on the real kernel: a context at DISPATCH_LEVEL or above
// is not preempted by threads or DPCs, only by interrupts of a higher level. Guest code relies on that:
// DirectSound raises the IRQL to keep its APU DPC away from the voice lists it is editing.
constexpr int kDispatch = 2;

struct Waiter {
    std::condition_variable cv;
    bool granted = false;
    int level = 0;  // IRQL of the waiting context
};

std::mutex g_lock;
bool g_held = false;
std::deque<Waiter*> g_interrupts, g_threads;  // interrupt, DPC and raised contexts; threads at PASSIVE/APC
thread_local bool t_holds = false;
thread_local int64_t t_wait_ticks = 0;  // performance: CpuWaitStats
thread_local uint64_t t_waits = 0;

// ---- performance: FCI_TRACE_HITCHES, who held the CPU how long ----
std::atomic<int64_t> g_hold_ticks[kHoldSlots];
thread_local int64_t t_hold_since = 0;
int HoldSlot() {
    GuestThread* t = CurrentThread();
    return t ? 1 + int(t->id % (kHoldSlots - 1)) : 0;
}
void Took() {
    if (hitch::Enabled()) t_hold_since = hitch::Now();
}
void Gave() {
    if (!t_hold_since) return;
    g_hold_ticks[HoldSlot()].fetch_add(hitch::Now() - t_hold_since, std::memory_order_relaxed);
    t_hold_since = 0;
}
// ---- end performance ----

int CurrentLevel() {
    GuestThread* t = CurrentThread();
    return t && t->kpcr ? R8(t->kpcr + kKpcrIrql) : 0;
}

// the waiter to run next when the CPU holder is at IRQL 'above' (-1: it is giving the CPU up)
Waiter* Pick(int above) {
    auto best = g_interrupts.end();
    for (auto it = g_interrupts.begin(); it != g_interrupts.end(); ++it)
        if ((*it)->level > above && (best == g_interrupts.end() || (*it)->level > (*best)->level)) best = it;
    if (best != g_interrupts.end()) {
        Waiter* w = *best;
        g_interrupts.erase(best);
        return w;
    }
    if (above < kDispatch && !g_threads.empty()) {
        Waiter* w = g_threads.front();
        g_threads.pop_front();
        return w;
    }
    return nullptr;
}

void Grant(Waiter* next) {
    if (g_interrupts.empty() && g_threads.empty()) *g_preempt_flag = 0;
    next->granted = true;  // the CPU passes over without ever being free
    next->cv.notify_one();
}

// queues the caller and waits for the CPU (g_lock held)
void Wait(std::unique_lock<std::mutex>& lock, int level, bool front) {
    Waiter w;
    w.level = level;
    if (level >= kDispatch) g_interrupts.push_back(&w);
    else if (front) g_threads.push_front(&w);
    else g_threads.push_back(&w);
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    w.cv.wait(lock, [&] { return w.granted; });
    QueryPerformanceCounter(&b);
    t_wait_ticks += b.QuadPart - a.QuadPart, t_waits++;
    t_holds = true;
    Took();
}

void QuantumTimer() {
    timeBeginPeriod(1);
    for (;;) {
        Sleep(2);
        std::lock_guard lock(g_lock);
        if (!g_threads.empty() || !g_interrupts.empty()) *g_preempt_flag = 1;
    }
}

}  // namespace

void StartScheduler() { std::thread(QuantumTimer).detach(); }

void AcquireCpu(bool interrupt) {
    int level = CurrentLevel();
    if (interrupt && level < kDispatch) level = kDispatch;
    std::unique_lock lock(g_lock);
    if (!g_held && g_interrupts.empty() && g_threads.empty()) {
        g_held = true;
        t_holds = true;
        Took();
        return;
    }
    if (level >= kDispatch) *g_preempt_flag = 1;
    Wait(lock, level, false);
}

void ReleaseCpu() {
    std::lock_guard lock(g_lock);
    t_holds = false;
    Gave();
    if (Waiter* next = Pick(-1)) Grant(next);
    else g_held = false;
}

bool HoldsCpu() { return t_holds; }

void CpuWaitStats(int64_t* ticks, uint64_t* waits) { *ticks = t_wait_ticks, *waits = t_waits; }

void CpuHoldTicks(int64_t out[kHoldSlots]) {
    if (t_hold_since) {  // the caller's hold so far counts now (it may hold the CPU over many frames)
        const int64_t now = hitch::Now();
        g_hold_ticks[HoldSlot()].fetch_add(now - t_hold_since, std::memory_order_relaxed);
        t_hold_since = now;
    }
    for (int i = 0; i < kHoldSlots; i++) out[i] = g_hold_ticks[i].load(std::memory_order_relaxed);
}
int CpuHoldSlot() { return HoldSlot(); }

void Preempt(Cpu& FCI_NOESCAPE) {
    int level = CurrentLevel();
    std::unique_lock lock(g_lock);
    Waiter* next = Pick(level);
    if (!next) {
        // nothing may run above this level now; lowering the IRQL or the quantum timer looks again
        *g_preempt_flag = 0;
        return;
    }
    // an interrupt or DPC returns the CPU to the context it interrupted; threads take turns
    bool interrupted = next->level >= kDispatch;
    t_holds = false;
    Gave();
    Grant(next);
    Wait(lock, level, interrupted);
}

CpuUnlock::CpuUnlock() : held_(t_holds) {
    if (held_) ReleaseCpu();
}

CpuUnlock::~CpuUnlock() {
    if (held_) AcquireCpu();
}

}  // namespace rt
