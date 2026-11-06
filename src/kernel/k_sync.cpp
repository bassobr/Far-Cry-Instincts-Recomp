// Dispatcher objects, waits, critical sections, IRQL, DPCs, timers and interrupts.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "gpu/nv2a.h"
#include "kernel/kernel.h"
#include "runtime/guest_time.h"  // PC comfort features: focus pause
#include "runtime/memory.h"
#include "runtime/scheduler.h"
#include "runtime/thread.h"

namespace kernel {

namespace {

// DISPATCHER_HEADER.Type
enum : uint8_t {
    kNotificationEvent = 0,
    kSynchronizationEvent = 1,
    kMutant = 2,
    kSemaphore = 5,
    kThread = 6,
    kNotificationTimer = 8,
    kSynchronizationTimer = 9,
};

struct EventObject : Object {
    HANDLE h;
    explicit EventObject(HANDLE h) : h(h) {}
    ~EventObject() override { ::CloseHandle(h); }
    void* Waitable() override { return h; }
    const char* Kind() const override { return "event"; }
};

// performance: a mutant without a host mutex. FCI takes and releases one ~75 times a frame, each a system call (and
// under Wine a server round trip). Guest code, and with it every kernel call, only runs while it holds the guest CPU,
// which serializes all operations on the mutant, so ownership is plain data; the host event only wakes a thread that
// blocked on it (system calls only when there is contention).
struct MutantObject : Object {
    HANDLE wake = CreateEventA(nullptr, FALSE, FALSE, nullptr);  // auto reset: set by a release while someone waits
    const rt::GuestThread* owner = nullptr;
    uint32_t count = 0;    // recursion
    uint32_t waiters = 0;  // threads blocked in a wait for it
    ~MutantObject() override { ::CloseHandle(wake); }
    void* Waitable() override { return nullptr; }  // waits go through TryTake/Wait, never to the host
    const char* Kind() const override { return "mutant"; }

    bool TryTake() {
        const rt::GuestThread* me = rt::CurrentThread();
        if (owner && owner != me) return false;
        owner = me;
        count++;
        return true;
    }
    bool Release() {
        if (owner != rt::CurrentThread() || !count) return false;
        if (--count == 0) {
            owner = nullptr;
            if (waiters) SetEvent(wake);
        }
        return true;
    }
};

// guest dispatcher objects living in guest memory, by address
std::mutex g_dispatcher_lock;
std::unordered_map<uint32_t, HANDLE> g_dispatcher;

bool IsEventLike(uint8_t type) {
    return type == kNotificationEvent || type == kSynchronizationEvent || type == kNotificationTimer ||
           type == kSynchronizationTimer;
}
bool IsAutoReset(uint8_t type) { return type == kSynchronizationEvent || type == kSynchronizationTimer; }

// critical sections by address
std::shared_mutex g_cs_lock;
std::unordered_map<uint32_t, CRITICAL_SECTION*> g_cs;

CRITICAL_SECTION* FindCs(uint32_t addr) {
    {
        std::shared_lock lock(g_cs_lock);
        auto it = g_cs.find(addr);
        if (it != g_cs.end()) return it->second;
    }
    std::unique_lock lock(g_cs_lock);
    auto& cs = g_cs[addr];
    if (!cs) {
        cs = new CRITICAL_SECTION;
        InitializeCriticalSection(cs);
    }
    return cs;
}

uint8_t& Irql() {
    auto* t = rt::CurrentThread();
    return *rt::Host(t->kpcr + rt::kKpcrIrql);
}

// DPCs run one at a time on their own guest context, as they would at DISPATCH_LEVEL
struct DpcRequest {
    uint32_t dpc, arg1, arg2;
};
std::mutex g_dpc_lock;
std::condition_variable g_dpc_cv;
std::deque<DpcRequest> g_dpc_queue;

void DpcWorker() {
    rt::GuestThread* ctx = rt::CreateServiceContext("dpc");
    rt::BindServiceContext(ctx);
    W8(ctx->kpcr + rt::kKpcrIrql, 2);
    W32(ctx->kpcr + rt::kKpcrDpcActive, 1);
    for (;;) {
        DpcRequest req;
        {
            std::unique_lock lock(g_dpc_lock);
            g_dpc_cv.wait(lock, [] { return !g_dpc_queue.empty(); });
            req = g_dpc_queue.front();
            g_dpc_queue.pop_front();
        }
        rt::AcquireCpu(true);
        W8(req.dpc + 2, 0);  // Inserted
        uint32_t routine = R32(req.dpc + 0x0C);
        uint32_t context = R32(req.dpc + 0x10);
        if (routine) rt::GuestCall(ctx->cpu, routine, {req.dpc, context, req.arg1, req.arg2});
        rt::ReleaseCpu();
    }
}

std::once_flag g_dpc_start;

// KTIMER: Header (0x10), DueTime (+0x10), TimerListEntry (+0x18), Dpc (+0x20), Period (+0x24)
struct Timer {
    HANDLE queue_timer = nullptr;
    uint32_t dpc = 0;
};
std::mutex g_timer_lock;
std::unordered_map<uint32_t, Timer> g_timers;
HANDLE g_timer_queue = nullptr;

void CALLBACK TimerFired(void* param, BOOLEAN) {
    uint32_t timer = uint32_t(reinterpret_cast<uintptr_t>(param));
    uint32_t dpc;
    {
        std::lock_guard lock(g_timer_lock);
        auto it = g_timers.find(timer);
        if (it == g_timers.end()) return;
        dpc = it->second.dpc;
    }
    W32(timer + 4, 1);  // SignalState
    SetEvent(DispatcherWaitable(timer));
    // ---- PC comfort features: a periodic timer's DPC is queued once, as KeInsertQueueDpc does on the
    // console; while the guest is held (focus pause) its expirations would otherwise pile up ----
    if (dpc) {
        std::lock_guard lock(g_dpc_lock);
        for (const DpcRequest& r : g_dpc_queue)
            if (r.dpc == dpc) return;
    }
    // ---- end PC comfort features ----
    if (dpc) QueueDpc(dpc, 0, 0);
}

bool CancelTimer(uint32_t timer) {
    HANDLE t = nullptr;
    {
        std::lock_guard lock(g_timer_lock);
        auto it = g_timers.find(timer);
        if (it == g_timers.end()) return false;
        t = it->second.queue_timer;
        g_timers.erase(it);
    }
    DeleteTimerQueueTimer(g_timer_queue, t, nullptr);
    return true;
}

uint32_t SetTimer(uint32_t timer, int64_t due, uint32_t period_ms, uint32_t dpc) {
    bool was_set = CancelTimer(timer);
    uint32_t due_ms;
    if (due < 0) {
        due_ms = uint32_t(-due / 10000);
    } else {
        int64_t cur = int64_t(rt::GuestFileTime());  // PC comfort features: absolute times are guest time
        due_ms = due <= cur ? 0 : uint32_t((due - cur) / 10000);
    }
    W32(timer + 4, 0);
    ResetEvent(DispatcherWaitable(timer));
    W32(timer + 0x20, dpc);
    W32(timer + 0x24, period_ms);
    std::lock_guard lock(g_timer_lock);
    Timer& t = g_timers[timer];
    t.dpc = dpc;
    CreateTimerQueueTimer(&t.queue_timer, g_timer_queue, TimerFired, reinterpret_cast<void*>(uintptr_t(timer)), due_ms,
                          period_ms, WT_EXECUTEDEFAULT);
    return was_set;
}

}  // namespace

void InitTimers() {
    g_timer_queue = CreateTimerQueue();
    std::call_once(g_dpc_start, [] { std::thread(DpcWorker).detach(); });
}

void QueueDpc(uint32_t dpc, uint32_t arg1, uint32_t arg2) {
    {
        std::lock_guard lock(g_dpc_lock);
        g_dpc_queue.push_back({dpc, arg1, arg2});
    }
    g_dpc_cv.notify_one();
}

void* DispatcherWaitable(uint32_t object) {
    uint8_t type = R8(object);
    if (type == kThread) {
        auto* t = rt::FindThreadByKthread(object);
        return t ? t->host : nullptr;
    }
    std::lock_guard lock(g_dispatcher_lock);
    HANDLE& h = g_dispatcher[object];
    int32_t signal = int32_t(R32(object + 4));
    if (!h) {
        if (IsEventLike(type)) h = CreateEventA(nullptr, !IsAutoReset(type), signal != 0, nullptr);
        else if (type == kSemaphore) h = CreateSemaphoreA(nullptr, signal, std::max<int32_t>(int32_t(R32(object + 0x10)), 1), nullptr);
        else if (type == kMutant) h = CreateMutexA(nullptr, FALSE, nullptr);  // KMUTANT in guest memory: host mutex
        else rt::Fatal("kernel: wait on dispatcher object %08X of type %u", object, type);
    } else if (IsEventLike(type)) {
        // XDK macros such as KeClearEvent write SignalState directly
        if (signal) SetEvent(h);
        else ResetEvent(h);
    }
    return h;
}

// ---------------------------------------------------------------------------------------------
// handle based objects

KERNEL_EXPORT(189, NtCreateEvent) {
    HANDLE h = CreateEventA(nullptr, a[2] == 0, a[3] != 0, nullptr);
    W32(a[0], NewHandle(std::make_shared<EventObject>(h)));
    return X_STATUS_SUCCESS;
}

KERNEL_EXPORT(225, NtSetEvent) {
    auto e = Get<EventObject>(a[0]);
    if (!e) return X_STATUS_INVALID_HANDLE;
    if (a[1]) W32(a[1], WaitForSingleObject(e->h, 0) == WAIT_OBJECT_0);
    SetEvent(e->h);
    return X_STATUS_SUCCESS;
}

KERNEL_EXPORT(186, NtClearEvent) {
    auto e = Get<EventObject>(a[0]);
    if (!e) return X_STATUS_INVALID_HANDLE;
    ResetEvent(e->h);
    return X_STATUS_SUCCESS;
}

KERNEL_EXPORT(192, NtCreateMutant) {
    auto m = std::make_shared<MutantObject>();
    if (a[2]) m->TryTake();
    W32(a[0], NewHandle(std::move(m)));
    return X_STATUS_SUCCESS;
}

KERNEL_EXPORT(221, NtReleaseMutant) {
    auto m = Get<MutantObject>(a[0]);
    if (!m) return X_STATUS_INVALID_HANDLE;
    if (a[1]) W32(a[1], 0);
    return m->Release() ? X_STATUS_SUCCESS : X_STATUS_MUTANT_NOT_OWNED;
}

namespace {

// a wait for a mutant another thread owns: blocks without the CPU until a release wakes it, then takes it
uint32_t WaitMutant(MutantObject& m, uint32_t ms, bool alertable) {
    if (m.TryTake()) return X_STATUS_SUCCESS;
    if (!ms) return X_STATUS_TIMEOUT;
    const ULONGLONG until = ms == INFINITE ? 0 : GetTickCount64() + ms;
    m.waiters++;
    uint32_t status = X_STATUS_TIMEOUT;
    for (;;) {
        DWORD left = INFINITE;
        if (until) {
            const ULONGLONG now = GetTickCount64();
            left = now >= until ? 0 : DWORD(until - now);
        }
        DWORD r;
        {
            rt::CpuUnlock unlock;
            r = WaitForSingleObjectEx(m.wake, left, alertable);
        }
        if (m.TryTake()) {
            status = X_STATUS_SUCCESS;
            break;
        }
        if (r == WAIT_IO_COMPLETION) {
            status = X_STATUS_USER_APC;
            break;
        }
        if (r == WAIT_TIMEOUT || r == WAIT_FAILED) break;
    }
    m.waiters--;
    if (m.waiters && !m.owner) SetEvent(m.wake);  // a release this waiter consumed but could not use
    return status;
}

// a multiple wait with a mutant among the objects: polled (rare; FCI waits for its mutexes one at a time)
uint32_t WaitMultipleWithMutants(const std::vector<std::shared_ptr<Object>>& objects, bool wait_all, bool alertable,
                                 uint32_t ms) {
    static std::atomic<int> reports{0};
    if (reports++ < 4) rt::Log("kernel: multiple wait (%zu objects, %s) with a mutant: polled", objects.size(),
                               wait_all ? "all" : "any");
    const ULONGLONG until = ms == INFINITE ? 0 : GetTickCount64() + ms;
    for (;;) {
        if (wait_all) {
            bool free = true;
            std::vector<HANDLE> handles;
            for (const auto& o : objects) {
                if (auto* m = dynamic_cast<MutantObject*>(o.get())) free = free && (!m->owner || m->owner == rt::CurrentThread());
                else handles.push_back(o->Waitable());
            }
            if (free && (handles.empty() || WaitForMultipleObjects(DWORD(handles.size()), handles.data(), TRUE, 0) <
                                                WAIT_OBJECT_0 + handles.size())) {
                for (const auto& o : objects)
                    if (auto* m = dynamic_cast<MutantObject*>(o.get())) m->TryTake();
                return X_STATUS_WAIT_0;
            }
        } else {
            for (size_t i = 0; i < objects.size(); i++) {
                auto* m = dynamic_cast<MutantObject*>(objects[i].get());
                if (m ? m->TryTake() : WaitForSingleObject(objects[i]->Waitable(), 0) == WAIT_OBJECT_0)
                    return X_STATUS_WAIT_0 + uint32_t(i);
            }
        }
        if (until && GetTickCount64() >= until) return X_STATUS_TIMEOUT;
        rt::CpuUnlock unlock;
        if (SleepEx(1, alertable) == WAIT_IO_COMPLETION) return X_STATUS_USER_APC;
    }
}

// a wait that can block gives the guest CPU to the other threads meanwhile
uint32_t WaitOne(HANDLE h, uint32_t ms, bool alertable) {
    // performance: a wait that is satisfied at once keeps the CPU (FCI takes and releases a mutex ~75 times a frame;
    // each gave the CPU to whichever thread was ready, the frame's thread waited to get it back)
    DWORD now = WaitForSingleObjectEx(h, 0, alertable);
    if (!ms || now != WAIT_TIMEOUT) return WaitStatus(now, 1);
    rt::CpuUnlock unlock;
    return WaitStatus(WaitForSingleObjectEx(h, ms, alertable), 1);
}

uint32_t WaitHandle(uint32_t handle, bool alertable, uint32_t timeout_ptr) {
    auto obj = FindObject(handle);
    if (!obj) return X_STATUS_INVALID_HANDLE;
    if (auto* m = dynamic_cast<MutantObject*>(obj.get())) return WaitMutant(*m, TimeoutMs(timeout_ptr), alertable);
    HANDLE h = obj->Waitable();
    if (!h) return X_STATUS_SUCCESS;  // files and other objects that complete synchronously
    return WaitOne(h, TimeoutMs(timeout_ptr), alertable);
}

uint32_t WaitMultiple(uint32_t count, const std::vector<HANDLE>& handles, bool wait_all, bool alertable,
                      uint32_t timeout_ptr) {
    if (handles.size() != count || count > MAXIMUM_WAIT_OBJECTS) return X_STATUS_INVALID_PARAMETER;
    uint32_t ms = TimeoutMs(timeout_ptr);
    if (!ms) return WaitStatus(WaitForMultipleObjectsEx(count, handles.data(), wait_all, 0, alertable), count);
    rt::CpuUnlock unlock;
    return WaitStatus(WaitForMultipleObjectsEx(count, handles.data(), wait_all, ms, alertable), count);
}

}  // namespace

KERNEL_EXPORT(233, NtWaitForSingleObject) { return WaitHandle(a[0], a[1] != 0, a[2]); }
KERNEL_EXPORT(234, NtWaitForSingleObjectEx) { return WaitHandle(a[0], a[2] != 0, a[3]); }

KERNEL_EXPORT(235, NtWaitForMultipleObjectsEx) {
    uint32_t count = a[0];
    std::vector<HANDLE> handles;
    std::vector<std::shared_ptr<Object>> objects;
    bool mutants = false;
    for (uint32_t i = 0; i < count; i++) {
        auto obj = FindObject(R32(a[1] + i * 4));
        const bool mutant = obj && dynamic_cast<MutantObject*>(obj.get());
        if (!obj || (!mutant && !obj->Waitable())) return X_STATUS_INVALID_HANDLE;
        mutants = mutants || mutant;
        handles.push_back(obj->Waitable());
        objects.push_back(std::move(obj));
    }
    if (mutants) {
        if (count > MAXIMUM_WAIT_OBJECTS) return X_STATUS_INVALID_PARAMETER;
        // XAPI's WaitForSingleObject(Ex) comes here with one object: the same as a single wait
        if (count == 1) return WaitMutant(static_cast<MutantObject&>(*objects[0]), TimeoutMs(a[5]), a[4] != 0);
        return WaitMultipleWithMutants(objects, a[2] == 0, a[4] != 0, TimeoutMs(a[5]));
    }
    return WaitMultiple(count, handles, a[2] == 0, a[4] != 0, a[5]);
}

// ---------------------------------------------------------------------------------------------
// guest dispatcher objects

const bool g_trace_waits = GetEnvironmentVariableA("FCI_TRACE_WAITS", nullptr, 0) > 0;

KERNEL_EXPORT(145, KeSetEvent) {
    uint32_t event = a[0];
    if (g_trace_waits) rt::Log("kernel: KeSetEvent(%08X) type %u state %u", event, R8(event), R32(event + 4));
    uint32_t previous = R32(event + 4);
    HANDLE h = DispatcherWaitable(event);
    W32(event + 4, 1);
    SetEvent(h);
    return previous;
}

KERNEL_EXPORT(159, KeWaitForSingleObject) {
    uint32_t object = a[0];
    if (g_trace_waits) rt::Log("kernel: KeWaitForSingleObject(%08X) type %u state %u", object, R8(object), R32(object + 4));
    // D3D's BlockOnTime: the time record is in ebp, the time two words above the saved registers
    if (R32(c.esp) == 0x0050AFFF) gpu::TraceBlockOnTime(c.ebp, R32(c.esp + 11 * 4));
    HANDLE h = DispatcherWaitable(object);
    if (!h) return X_STATUS_SUCCESS;
    uint32_t status = WaitOne(h, TimeoutMs(a[4]), a[3] != 0);
    if (status == X_STATUS_SUCCESS && IsAutoReset(R8(object))) W32(object + 4, 0);
    return status;
}

KERNEL_EXPORT(158, KeWaitForMultipleObjects) {
    uint32_t count = a[0];
    std::vector<HANDLE> handles;
    for (uint32_t i = 0; i < count; i++) handles.push_back(DispatcherWaitable(R32(a[1] + i * 4)));
    uint32_t status = WaitMultiple(count, handles, a[2] == 0, a[5] != 0, a[6]);
    if (status < count && IsAutoReset(R8(R32(a[1] + status * 4)))) W32(R32(a[1] + status * 4) + 4, 0);
    return status;
}

KERNEL_EXPORT(99, KeDelayExecutionThread) {
    uint32_t ms = TimeoutMs(a[2]);
    rt::CpuUnlock unlock;  // a zero delay still lets the other threads run
    if (ms == 0) SwitchToThread();
    else SleepEx(ms, a[1] != 0);
    return X_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------------------------
// critical sections: RTL_CRITICAL_SECTION is an event header, LockCount (+0x10),
// RecursionCount (+0x14) and OwningThread (+0x18)

KERNEL_EXPORT(291, RtlInitializeCriticalSection) {
    uint32_t cs = a[0];
    FindCs(cs);
    W32(cs + 0x10, 0xFFFFFFFF);
    W32(cs + 0x14, 0);
    W32(cs + 0x18, 0);
    return 0;
}

KERNEL_EXPORT(277, RtlEnterCriticalSection) {
    uint32_t cs = a[0];
    CRITICAL_SECTION* host = FindCs(cs);
    if (!TryEnterCriticalSection(host)) {
        rt::CpuUnlock unlock;  // the owner needs the CPU to leave it
        EnterCriticalSection(host);
    }
    W32(cs + 0x10, R32(cs + 0x10) + 1);
    W32(cs + 0x14, R32(cs + 0x14) + 1);
    if (auto* t = rt::CurrentThread()) W32(cs + 0x18, t->kthread);
    return 0;
}

KERNEL_EXPORT(294, RtlLeaveCriticalSection) {
    uint32_t cs = a[0];
    W32(cs + 0x10, R32(cs + 0x10) - 1);
    uint32_t rec = R32(cs + 0x14) - 1;
    W32(cs + 0x14, rec);
    if (!rec) W32(cs + 0x18, 0);
    LeaveCriticalSection(FindCs(cs));
    return 0;
}

// ---------------------------------------------------------------------------------------------
// irql

KERNEL_EXPORT(160, KfRaiseIrql) {
    uint8_t old = Irql();
    Irql() = uint8_t(c.ecx);
    return old;
}

KERNEL_EXPORT(161, KfLowerIrql) {
    Irql() = uint8_t(c.ecx);
    rt::Preempt(c);  // interrupts and DPCs held back by the raised level may run now
    return 0;
}

KERNEL_EXPORT(129, KeRaiseIrqlToDpcLevel) {
    uint8_t old = Irql();
    Irql() = 2;
    return old;
}

// ---------------------------------------------------------------------------------------------
// DPCs: Type (+0), Inserted (+2), DpcListEntry (+4), DeferredRoutine (+0xC), DeferredContext (+0x10)

KERNEL_EXPORT(107, KeInitializeDpc) {
    uint32_t dpc = a[0];
    W16(dpc, 19);  // DpcObject
    W8(dpc + 2, 0);
    W32(dpc + 0x0C, a[1]);
    W32(dpc + 0x10, a[2]);
    return 0;
}

KERNEL_EXPORT(119, KeInsertQueueDpc) {
    uint32_t dpc = a[0];
    if (R8(dpc + 2)) return 0;
    W8(dpc + 2, 1);
    QueueDpc(dpc, a[1], a[2]);
    return 1;
}

KERNEL_EXPORT(137, KeRemoveQueueDpc) {
    uint32_t dpc = a[0];
    std::lock_guard lock(g_dpc_lock);
    for (auto it = g_dpc_queue.begin(); it != g_dpc_queue.end(); ++it) {
        if (it->dpc == dpc) {
            g_dpc_queue.erase(it);
            W8(dpc + 2, 0);
            return 1;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------
// timers

KERNEL_EXPORT(113, KeInitializeTimerEx) {
    uint32_t timer = a[0];
    W8(timer, a[1] ? kSynchronizationTimer : kNotificationTimer);
    W32(timer + 4, 0);
    W32(timer + 0x20, 0);
    W32(timer + 0x24, 0);
    return 0;
}

KERNEL_EXPORT(149, KeSetTimer) { return SetTimer(a[0], int64_t((uint64_t(a[2]) << 32) | a[1]), 0, a[3]); }
KERNEL_EXPORT(150, KeSetTimerEx) { return SetTimer(a[0], int64_t((uint64_t(a[2]) << 32) | a[1]), a[3], a[4]); }
KERNEL_EXPORT(97, KeCancelTimer) { return CancelTimer(a[0]); }

// ---------------------------------------------------------------------------------------------
// interrupts: no device raises any, so connecting only records the routine

namespace {
std::mutex g_interrupt_lock;
std::unordered_map<uint32_t, uint32_t> g_interrupts;  // vector -> connected KINTERRUPT
}  // namespace

bool GetInterrupt(uint32_t vector, uint32_t* object, uint32_t* routine, uint32_t* context) {
    std::lock_guard lock(g_interrupt_lock);
    auto it = g_interrupts.find(vector);
    if (it == g_interrupts.end()) return false;
    *object = it->second;
    *routine = R32(it->second + 0);
    *context = R32(it->second + 4);
    return true;
}

KERNEL_EXPORT(109, KeInitializeInterrupt) {
    uint32_t intr = a[0];
    W32(intr + 0, a[1]);  // ServiceRoutine
    W32(intr + 4, a[2]);  // ServiceContext
    W32(intr + 8, a[3]);  // BusInterruptLevel
    W32(intr + 12, a[4]);
    rt::Log("kernel: interrupt %08X for vector %u, routine %08X", intr, a[3], a[1]);
    return 0;
}

KERNEL_EXPORT(98, KeConnectInterrupt) {
    std::lock_guard lock(g_interrupt_lock);
    g_interrupts[R32(a[0] + 8)] = a[0];
    return 1;
}

KERNEL_EXPORT(100, KeDisconnectInterrupt) {
    std::lock_guard lock(g_interrupt_lock);
    g_interrupts.erase(R32(a[0] + 8));
    return 1;
}

KERNEL_EXPORT(153, KeSynchronizeExecution) { return rt::GuestCall(c, a[1], {a[2]}) & 0xFF; }

}  // namespace kernel
