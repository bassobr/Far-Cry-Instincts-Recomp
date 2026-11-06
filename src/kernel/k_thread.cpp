// Ps* thread exports.
#include <windows.h>

#include <algorithm>

#include "kernel/kernel.h"
#include "runtime/thread.h"

namespace kernel {

namespace {

struct ThreadObject : Object {
    rt::GuestThread* thread;
    explicit ThreadObject(rt::GuestThread* t) : thread(t) { guest_body = t->kthread; }
    void* Waitable() override { return thread->host; }
    const char* Kind() const override { return "thread"; }
};

// Xbox base priorities run from -2 (lowest) to 2 (highest), plus the saturated ±16 values
int HostPriority(int32_t increment) {
    if (increment >= 16) return THREAD_PRIORITY_TIME_CRITICAL;
    if (increment <= -16) return THREAD_PRIORITY_IDLE;
    return std::clamp(increment, -2, 2);
}

}  // namespace

// ThreadHandle*, ThreadExtensionSize, KernelStackSize, TlsDataSize, ThreadId*, StartRoutine, StartContext,
// CreateSuspended, DebuggerThread, SystemRoutine
KERNEL_EXPORT(255, PsCreateSystemThreadEx) {
    auto* t = rt::CreateGuestThread(a[2], a[3], a[5], a[6], a[9], true);
    if (a[0]) W32(a[0], NewHandle(std::make_shared<ThreadObject>(t)));
    if (a[4]) W32(a[4], t->id);
    if (!a[7]) ResumeThread(t->host);
    return X_STATUS_SUCCESS;
}

KERNEL_EXPORT(258, PsTerminateSystemThread) { throw rt::ThreadExit{a[0]}; }

KERNEL_EXPORT(224, NtResumeThread) {
    auto t = Get<ThreadObject>(a[0]);
    if (!t) return X_STATUS_INVALID_HANDLE;
    DWORD prev = ResumeThread(t->thread->host);
    if (a[1]) W32(a[1], prev);
    return X_STATUS_SUCCESS;
}

KERNEL_EXPORT(231, NtSuspendThread) {
    auto t = Get<ThreadObject>(a[0]);
    if (!t) return X_STATUS_INVALID_HANDLE;
    DWORD prev = SuspendThread(t->thread->host);
    if (a[1]) W32(a[1], prev);
    return X_STATUS_SUCCESS;
}

KERNEL_EXPORT(143, KeSetBasePriorityThread) {
    if (auto* t = rt::FindThreadByKthread(a[0])) {
        SetThreadPriority(t->host, HostPriority(int32_t(a[1])));
        rt::Log("kernel: thread %u (start %08X) base priority %d", t->id, t->start, int32_t(a[1]));
    }
    return 0;
}

// x87/SSE state is per host thread already
KERNEL_EXPORT(142, KeSaveFloatingPointState) { return X_STATUS_SUCCESS; }
KERNEL_EXPORT(139, KeRestoreFloatingPointState) { return X_STATUS_SUCCESS; }

}  // namespace kernel
