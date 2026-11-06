#include "runtime/thread.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <iterator>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "runtime/log.h"
#include "runtime/memory.h"
#include "runtime/scheduler.h"

namespace rt {

namespace {

constexpr uint32_t kMinGuestStack = 256 * 1024;
constexpr size_t kHostStack = 64 * 1024 * 1024;  // recompiled calls nest on the host stack

thread_local GuestThread* t_current = nullptr;
std::atomic<uint32_t> g_next_id{1};
std::mutex g_threads_lock;
std::vector<GuestThread*> g_threads;

void SetupContext(GuestThread* t, uint32_t stack_size, uint32_t tls_size) {
    stack_size = (std::max(stack_size, kMinGuestStack) + 0xFFF) & ~0xFFFu;
    uint32_t tls_area = (tls_size + 15) & ~15u;
    uint32_t stack = mem::AllocSystem(stack_size + tls_area);
    t->stack_limit = stack;
    t->stack_base = stack + stack_size + tls_area;
    // XAPI addresses TLS as StackBase + TlsIndex * 4 with TlsIndex = -TlsDataSize / 4, so the block has to
    // end exactly at StackBase
    t->tls = t->stack_base - tls_size;

    uint32_t block = mem::AllocSystem(0x1000);
    t->kpcr = block;
    t->kthread = block + 0x400;
    W32(t->kpcr + 0x00, 0xFFFFFFFF);  // NtTib.ExceptionList
    W32(t->kpcr + 0x04, t->stack_base);
    W32(t->kpcr + 0x08, t->stack_limit);
    W32(t->kpcr + 0x18, t->kpcr);  // NtTib.Self
    W32(t->kpcr + 0x1C, t->kpcr);  // SelfPcr
    W32(t->kpcr + kKpcrPrcb, t->kpcr + 0x28);
    W32(t->kpcr + kKpcrCurrentThread, t->kthread);
    W32(t->kthread + kKthreadTlsData, t->tls);
    W8(t->kthread + 0x00, 6);  // DISPATCHER_HEADER.Type = ThreadObject

    t->id = g_next_id++;
    t->cpu.fs_base = t->kpcr;
    t->cpu.esp = (t->tls - 16) & ~15u;
    t->cpu.thread = t;
    std::lock_guard lock(g_threads_lock);
    g_threads.push_back(t);
}

DWORD WINAPI ThreadMain(void* param) {
    auto* t = static_cast<GuestThread*>(param);
    t_current = t;
    {  // a name for debuggers and FCI_PROFILE_FCI=1 (the profiler's filter)
        wchar_t name[32];
        swprintf(name, 32, L"FCI guest %u", t->id);
        SetThreadDescription(GetCurrentThread(), name);
    }
    AcquireCpu();
    try {
        if (t->system_routine) GuestCall(t->cpu, t->system_routine, {t->start, t->context});
        else GuestCall(t->cpu, t->start, {t->context});
    } catch (const ThreadExit& e) {
        ReleaseCpu();
        Log("thread %u exited with %08X", t->id, e.status);
        return e.status;
    }
    ReleaseCpu();
    Log("thread %u returned", t->id);
    return 0;
}

}  // namespace

GuestThread* CurrentThread() { return t_current; }

GuestThread* CreateGuestThread(uint32_t stack_size, uint32_t tls_size, uint32_t start, uint32_t context,
                               uint32_t system_routine, bool suspended) {
    auto* t = new GuestThread;
    SetupContext(t, stack_size, tls_size);
    t->start = start;
    t->context = context;
    t->system_routine = system_routine;
    t->host = CreateThread(nullptr, kHostStack, ThreadMain, t, STACK_SIZE_PARAM_IS_A_RESERVATION | CREATE_SUSPENDED, nullptr);
    if (!t->host) Fatal("CreateThread failed (%lu)", GetLastError());
    Log("thread %u created: start %08X context %08X system %08X stack %08X-%08X tls %08X", t->id, start, context,
        system_routine, t->stack_limit, t->stack_base, t->tls);
    if (!suspended) ResumeThread(t->host);
    return t;
}

GuestThread* FindThreadByKthread(uint32_t kthread) {
    std::lock_guard lock(g_threads_lock);
    for (auto* t : g_threads)
        if (t->kthread == kthread) return t;
    return nullptr;
}

GuestThread* CreateServiceContext(const char* name) {
    auto* t = new GuestThread;
    SetupContext(t, kMinGuestStack, 0);
    Log("service context %s: thread %u", name, t->id);
    return t;
}

void BindServiceContext(GuestThread* t) {
    t_current = t;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &t->host, 0, FALSE, DUPLICATE_SAME_ACCESS);
}

void DumpThreads() {
    std::vector<GuestThread*> threads;
    {
        std::lock_guard lock(g_threads_lock);
        threads = g_threads;
    }
    for (auto* t : threads) {
        if (!t->host || WaitForSingleObject(t->host, 0) == WAIT_OBJECT_0) continue;
        if (SuspendThread(t->host) == DWORD(-1)) continue;
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_FULL;
        std::string stack;
        if (GetThreadContext(t->host, &ctx)) stack = FormatStack(&ctx, t->host);
        ResumeThread(t->host);
        Log("thread %u (pc %08X esp %08X eax %08X ecx %08X edx %08X esi %08X edi %08X):\n%s", t->id, t->cpu.pc,
            t->cpu.esp, t->cpu.eax, t->cpu.ecx, t->cpu.edx, t->cpu.esi, t->cpu.edi, stack.c_str());
        // the top of its guest stack: arguments and return addresses of the guest functions above
        std::string words;
        for (uint32_t i = 0; i < 48 && t->cpu.esp; i++) {
            char part[16];
            snprintf(part, sizeof part, "%s%08X", i % 8 ? " " : "\n   ", R32(t->cpu.esp + 4 * i));
            words += part;
        }
        Log("thread %u guest stack:%s", t->id, words.c_str());
    }
}

uint32_t GuestCall(Cpu& c, uint32_t function, std::initializer_list<uint32_t> args) {
    uint32_t saved = c.esp;
    for (auto it = std::rbegin(args); it != std::rend(args); ++it) {
        c.esp -= 4;
        W32(c.esp, *it);
    }
    c.esp -= 4;
    W32(c.esp, 0);  // return address; recompiled code returns on the host instead
    Call(c, function);
    c.esp = saved;
    return c.eax;
}

}  // namespace rt
