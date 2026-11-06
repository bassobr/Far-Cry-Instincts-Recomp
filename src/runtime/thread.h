// Guest threads run on host threads, each with its own Cpu, guest stack, KPCR and KTHREAD.
#pragma once

#include <cstdint>
#include <initializer_list>

#include "runtime/guest.h"

namespace rt {

struct GuestThread {
    Cpu cpu{};
    uint32_t kpcr = 0;     // fs points here
    uint32_t kthread = 0;  // KPCR.PrcbData.CurrentThread
    uint32_t stack_base = 0, stack_limit = 0;
    uint32_t tls = 0;  // KTHREAD.TlsData
    uint32_t start = 0, context = 0, system_routine = 0;
    uint32_t id = 0;
    void* host = nullptr;  // host thread HANDLE, waitable
};

// thrown by PsTerminateSystemThread to unwind the host thread
struct ThreadExit {
    uint32_t status;
};

// guest KPCR and KTHREAD layout the XDK libraries read through fs
constexpr uint32_t kKpcrPrcb = 0x20, kKpcrIrql = 0x24, kKpcrCurrentThread = 0x28, kKpcrDpcActive = 0x58;
constexpr uint32_t kKthreadTlsData = 0x28;

GuestThread* CurrentThread();

// Starts system_routine(start, context) when system_routine is set (XAPI threads), else start(context).
GuestThread* CreateGuestThread(uint32_t stack_size, uint32_t tls_size, uint32_t start, uint32_t context,
                               uint32_t system_routine, bool suspended);
GuestThread* FindThreadByKthread(uint32_t kthread);

// a context for host-initiated guest calls (DPCs, timers) with its own guest stack
GuestThread* CreateServiceContext(const char* name);
void BindServiceContext(GuestThread* t);  // makes t the current guest thread of this host thread

// logs the host stack of every guest thread (for hangs)
void DumpThreads();

// calls a stdcall or cdecl guest function from the host; esp is restored afterwards
uint32_t GuestCall(Cpu& c, uint32_t function, std::initializer_list<uint32_t> args);

}  // namespace rt
