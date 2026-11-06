// Guest time: the host clocks less the time the guest was held (pause on focus loss, app/focus_pause.cpp).
// Every clock the title reads (rdtsc, KeQueryPerformanceCounter, KeQuerySystemTime, KeTickCount) and the
// kernel's absolute timeouts and timers go through here, so a pause leaves no gap in the game's time.
#pragma once

#include <cstdint>

namespace rt {

int64_t GuestQpc();          // QueryPerformanceCounter less the time held (same frequency)
uint64_t GuestFileTime();    // GetSystemTimeAsFileTime (100 ns units) less the time held
uint32_t GuestTickCount();   // GetTickCount less the time held, in ms

// stops (true) or restarts (false) the guest clocks; they resume where they stopped
void HoldGuestTime(bool held);
bool GuestTimeHeld();

}  // namespace rt
