#pragma once

#include <string>

namespace rt {

// FCI_PROFILE=<seconds>[@<start>]: sampling profiler over all host threads, report in fci.log
void StartProfiler();

// a sign of life from a part that should keep going (the watchdog logs every thread's stack when one stops
// for 5 seconds after it started)
enum class Beat { kVblank, kFlip, kCount };
void Heartbeat(Beat which);
// called by the watchdog after the stacks, for state that explains a hang
void SetHangReport(void (*report)());
// every host thread's stack (as the watchdog logs them)
void DumpHostStacks();
// performance, FCI_HITCH_STACKS: one thread's stack as "f < g < h" with source lines (suspends it for a copy only)
std::string ThreadStack(void* thread_handle);

}  // namespace rt
