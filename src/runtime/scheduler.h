// The Xbox has one CPU core and titles rely on that: no two guest threads ever run at the same time.
// Guest threads hand a single "CPU" around: blocking kernel calls give it up, and a quantum timer asks
// the running thread to yield at its next loop back-edge (rt::Preempt, emitted by the recompiler).
#pragma once

#include <cstdint>

namespace rt {

void StartScheduler();

// interrupt, DPC and timer contexts pass the queue of guest threads
void AcquireCpu(bool interrupt = false);
void ReleaseCpu();
bool HoldsCpu();
// performance: QueryPerformanceCounter ticks the calling thread waited for the CPU while it could run (another
// guest thread had it), and how often; totals since start
void CpuWaitStats(int64_t* ticks, uint64_t* waits);
// performance, FCI_TRACE_HITCHES: ticks each context held the CPU since start: [0] host contexts (interrupts, DPCs,
// timers), [1 + guest thread id % (kHoldSlots - 1)] guest threads
constexpr int kHoldSlots = 64;
void CpuHoldTicks(int64_t out[kHoldSlots]);
int CpuHoldSlot();  // the calling thread's

// gives the CPU up around a blocking host call
class CpuUnlock {
public:
    CpuUnlock();
    ~CpuUnlock();
    CpuUnlock(const CpuUnlock&) = delete;
    CpuUnlock& operator=(const CpuUnlock&) = delete;

private:
    bool held_;
};

}  // namespace rt
