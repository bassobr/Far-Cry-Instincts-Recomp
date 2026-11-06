// FCI_PROFILE=<seconds>[@<start>]: from <start> seconds on, samples every host thread each millisecond for
// <seconds> seconds and logs, per busy thread, the hottest functions (self time and inclusive time).
// Stacks are unwound with RtlVirtualUnwind while the thread is suspended; nothing is allocated meanwhile.
#include "runtime/profiler.h"

#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "runtime/guest_time.h"  // PC comfort features: focus pause
#include "runtime/log.h"
#include "runtime/thread.h"

namespace rt {

namespace {

constexpr int kDepth = 24;

struct Sample {
    DWORD thread;
    uint8_t depth;
    uint64_t pc[kDepth];
};

// a copy of the top of a thread's stack, taken while it was suspended; unwinding happens on the copy after
// the thread runs again, so the profiler never waits on a lock a suspended thread holds (the loader's,
// the heap's) and never touches memory that can fault
constexpr size_t kStackCopy = 32 * 1024;

struct StackCopy {
    uint64_t original = 0;  // the stack pointer the copy starts at
    size_t size = 0;
    alignas(16) uint8_t bytes[kStackCopy + 4096];  // slack: unwinding may read a little past the copied part

    // an address of the original stack as an address in the copy, or 0 outside it
    uint64_t Map(uint64_t address) const {
        if (address >= original && address < original + size)
            return reinterpret_cast<uint64_t>(bytes) + (address - original);
        return 0;
    }
    bool InCopy(uint64_t address) const {
        return address >= reinterpret_cast<uint64_t>(bytes) && address + 8 <= reinterpret_cast<uint64_t>(bytes) + size;
    }
};

int Unwind(CONTEXT ctx, const StackCopy& stack, uint64_t* out, int depth = kDepth) {
    ctx.Rsp = stack.Map(ctx.Rsp);
    if (uint64_t rbp = stack.Map(ctx.Rbp)) ctx.Rbp = rbp;
    int n = 0;
    while (n < depth && ctx.Rip) {
        out[n++] = ctx.Rip;
        if (!stack.InCopy(ctx.Rsp)) break;
        DWORD64 base = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &base, nullptr);
        if (!fn) {  // leaf function: the return address is on top of the stack
            ctx.Rip = *reinterpret_cast<const uint64_t*>(ctx.Rsp);
            ctx.Rsp += 8;
            continue;
        }
        void* handler_data = nullptr;
        DWORD64 frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, fn, &ctx, &handler_data, &frame, nullptr);
        // restored frame pointers point into the original stack
        if (uint64_t rbp = stack.Map(ctx.Rbp)) ctx.Rbp = rbp;
        if (uint64_t rsp = stack.Map(ctx.Rsp)) ctx.Rsp = rsp;
    }
    return n;
}

// the top of a thread's stack (NT_TIB.StackBase), from its TEB
uint64_t StackBase(HANDLE thread) {
    struct BasicInformation {
        LONG status;
        PVOID teb;
        PVOID process, thread_id;
        ULONG_PTR affinity;
        LONG priority, base_priority;
    } info{};
    using Query = LONG(WINAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
    static Query query = reinterpret_cast<Query>(GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread"));
    if (!query || query(thread, 0, &info, sizeof info, nullptr) < 0 || !info.teb) return 0;
    return reinterpret_cast<NT_TIB*>(info.teb)->StackBase ? uint64_t(reinterpret_cast<NT_TIB*>(info.teb)->StackBase) : 0;
}

// function name without the offset
std::string Function(uint64_t pc, std::unordered_map<uint64_t, std::string>& cache) {
    auto it = cache.find(pc);
    if (it != cache.end()) return it->second;
    std::string name = SymbolAt(pc);
    if (size_t plus = name.rfind("+0x"); plus != std::string::npos) name.resize(plus);
    // no symbol (or an export far away): the module and offset say more
    HMODULE module = nullptr;
    char path[MAX_PATH];
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<const char*>(pc), &module) &&
        module != GetModuleHandleA(nullptr) && GetModuleFileNameA(module, path, MAX_PATH)) {
        const char* file = std::strrchr(path, '\\');
        char buf[400];
        snprintf(buf, sizeof buf, "%s!%s", file ? file + 1 : path, name.c_str());
        if (name.size() == 16 && name.find_first_not_of("0123456789ABCDEF") == std::string::npos)
            snprintf(buf, sizeof buf, "%s+%llX", file ? file + 1 : path,
                     (unsigned long long)(pc - reinterpret_cast<uint64_t>(module)));
        name = buf;
    }
    return cache[pc] = name;
}

// source line of an address (of the inlined code there, as the line table has it), "file:line", or ""
std::string LineOf(uint64_t pc, std::unordered_map<uint64_t, std::string>& cache) {
    auto it = cache.find(pc);
    if (it != cache.end()) return it->second;
    IMAGEHLP_LINE64 line{sizeof line};
    DWORD disp = 0;
    std::string out;
    if (SymGetLineFromAddr64(GetCurrentProcess(), pc, &disp, &line) && line.FileName) {
        const char* file = std::strrchr(line.FileName, '\\');
        char buf[300];
        snprintf(buf, sizeof buf, "%s:%lu", file ? file + 1 : line.FileName, line.LineNumber);
        out = buf;
    }
    return cache[pc] = out;
}

bool IsWait(const std::string& name) {
    static const char* const kWaits[] = {"ZwWaitFor", "NtWaitFor", "ZwDelayExecution", "NtDelayExecution",
                                         "ZwRemoveIoCompletion", "NtRemoveIoCompletion", "ZwSignalAndWait",
                                         "NtWaitForAlertByThreadId", "ZwWaitForAlertByThreadId", "NtUserMsgWait",
                                         "NtUserGetMessage", "ZwYieldExecution", "NtYieldExecution"};
    for (const char* w : kWaits)
        if (name.find(w) != std::string::npos) return true;
    return false;
}

void Report(const std::vector<Sample>& samples, double seconds) {
    std::unordered_map<uint64_t, std::string> names;
    std::map<DWORD, std::vector<const Sample*>> by_thread;
    for (const Sample& s : samples) by_thread[s.thread].push_back(&s);
    size_t ticks = 0;
    for (auto& [id, list] : by_thread) ticks = std::max(ticks, list.size());
    Log("profile: %zu samples over %.1f s, %zu threads", samples.size(), seconds, by_thread.size());
    for (auto& [id, list] : by_thread) {
        std::unordered_map<std::string, size_t> self, inclusive, waits, sites, stacks, lines;
        static std::unordered_map<uint64_t, std::string> line_names;
        size_t busy = 0;
        std::string entry;
        for (const Sample* s : list) {
            if (!s->depth) continue;
            std::string top = Function(s->pc[0], names);
            if (IsWait(top)) {
                std::unordered_set<std::string> seen;
                std::string site;
                for (int i = 1; i < s->depth; i++) {
                    std::string f = Function(s->pc[i], names);
                    if (seen.insert(f).second) waits[f]++;
                    // the wait site: the first of our own functions and its caller
                    bool own = f.rfind("f_", 0) == 0 || f.find("::") != std::string::npos;
                    if (own && f.find("std::") == std::string::npos && site.size() < 200 &&
                        std::count(site.begin(), site.end(), '<') < 2)
                        site += (site.empty() ? "" : " < ") + f;
                }
                if (!site.empty()) sites[site]++;
                std::string stack;
                for (int i = 0; i < s->depth && i < 12; i++) {
                    std::string f = Function(s->pc[i], names);
                    if (f.size() > 40) f = f.substr(0, 40);
                    stack += (i ? " < " : "") + f;
                }
                stacks[stack]++;
                continue;
            }
            busy++;
            self[top]++;
            // where in our own code (with inlining: the inlined source line)
            for (int i = 0; i < s->depth && i < 4; i++) {
                std::string l = LineOf(s->pc[i], line_names);
                if (l.empty()) continue;
                lines[l + (i ? " (caller)" : "")]++;
                break;
            }
            std::unordered_set<std::string> seen;
            for (int i = 0; i < s->depth; i++) {
                std::string f = Function(s->pc[i], names);
                if (seen.insert(f).second) inclusive[f]++;
            }
        }
        if (busy * 50 < ticks) continue;  // under 2 % busy
        // the thread's entry: the deepest frame below the system thread start
        for (const Sample* s : list) {
            for (int i = s->depth - 1; i >= 0; i--) {
                std::string f = Function(s->pc[i], names);
                if (f.find("ThreadStart") != std::string::npos || f.find("BaseThreadInit") != std::string::npos ||
                    f.find("thread_start") != std::string::npos || f.find("invoke") != std::string::npos)
                    continue;
                entry = f;
                break;
            }
            if (!entry.empty()) break;
        }
        Log("profile: thread %lu (%s): busy %.1f %%", id, entry.c_str(), 100.0 * busy / ticks);
        auto top = [&](std::unordered_map<std::string, size_t>& counts, const char* what, size_t limit) {
            std::vector<std::pair<size_t, std::string>> sorted;
            for (auto& [name, n] : counts) sorted.push_back({n, name});
            std::sort(sorted.rbegin(), sorted.rend());
            for (size_t i = 0; i < sorted.size() && i < limit; i++)
                Log("profile:   %s %5.1f %%  %s", what, 100.0 * sorted[i].first / ticks, sorted[i].second.c_str());
        };
        top(self, "self", 25);
        top(lines, "line", 40);
        top(inclusive, "incl", 40);
        top(waits, "wait", 25);
        top(sites, "site", 15);
        top(stacks, "stack", 8);
    }
}

void ProfilerThread(double start, double seconds) {
    Sleep(DWORD(start * 1000));
    Log("profile: sampling for %.1f s", seconds);
    std::vector<Sample> samples;
    samples.reserve(size_t(seconds * 1000 * 16));
    struct Thread {
        DWORD id;
        HANDLE handle;
        uint64_t stack_base;
    };
    std::vector<Thread> threads;
    static StackCopy stack;
    // FCI_PROFILE_FCI=1: only our named threads (guest threads, the render thread), every FCI_PROFILE_MS (default 1)
    // milliseconds; under Wine suspending every thread each millisecond stalled the game
    const bool only_fci = GetEnvironmentVariableA("FCI_PROFILE_FCI", nullptr, 0) > 0;
    char period_text[16] = {};
    const DWORD period = GetEnvironmentVariableA("FCI_PROFILE_MS", period_text, sizeof period_text)
                             ? DWORD(std::max(1, std::atoi(period_text)))
                             : 1;
    ULONGLONG begin = GetTickCount64(), refreshed = 0;
    DWORD self = GetCurrentThreadId(), process = GetCurrentProcessId();
    timeBeginPeriod(1);
    while (GetTickCount64() - begin < ULONGLONG(seconds * 1000)) {
        if (GetTickCount64() - refreshed > 200) {
            refreshed = GetTickCount64();
            for (auto& t : threads) CloseHandle(t.handle);
            threads.clear();
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 te{sizeof te};
            for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
                if (te.th32OwnerProcessID != process || te.th32ThreadID == self) continue;
                if (HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                                          te.th32ThreadID)) {
                    if (only_fci) {
                        PWSTR name = nullptr;
                        bool ours = SUCCEEDED(GetThreadDescription(h, &name)) && name && !wcsncmp(name, L"FCI", 3);
                        if (name) LocalFree(name);
                        if (!ours) {
                            CloseHandle(h);
                            continue;
                        }
                    }
                    threads.push_back({te.th32ThreadID, h, StackBase(h)});
                }
            }
            CloseHandle(snap);
        }
        for (auto& t : threads) {
            if (samples.size() == samples.capacity()) break;
            Sample s{};
            s.thread = t.id;
            if (SuspendThread(t.handle) == DWORD(-1)) continue;
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            bool ok = GetThreadContext(t.handle, &ctx) != 0;
            stack.original = ctx.Rsp, stack.size = 0;
            if (ok && t.stack_base > ctx.Rsp) {
                SIZE_T want = SIZE_T(std::min<uint64_t>(t.stack_base - ctx.Rsp, kStackCopy)), got = 0;
                if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(ctx.Rsp), stack.bytes, want, &got))
                    stack.size = got;
            }
            ResumeThread(t.handle);
            if (ok) s.depth = uint8_t(Unwind(ctx, stack, s.pc));
            samples.push_back(s);
        }
        Sleep(period);
    }
    timeEndPeriod(1);
    for (auto& t : threads) CloseHandle(t.handle);
    Report(samples, (GetTickCount64() - begin) / 1000.0);
}

// the stacks of all host threads but the caller, unwound as by the profiler (on copies, so a thread that
// holds a lock while suspended cannot block this)
void HostStacks() {
    constexpr int kDeep = 40;
    struct Taken {
        DWORD id;
        int depth;
        uint64_t pc[kDeep];
    };
    std::vector<Taken> taken;
    static StackCopy stack;
    DWORD self = GetCurrentThreadId(), process = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{sizeof te};
    std::vector<DWORD> ids;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
        if (te.th32OwnerProcessID == process && te.th32ThreadID != self) ids.push_back(te.th32ThreadID);
    CloseHandle(snap);
    taken.reserve(ids.size());
    for (DWORD id : ids) {
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id);
        if (!h) continue;
        uint64_t base = StackBase(h);
        if (SuspendThread(h) != DWORD(-1)) {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            bool ok = GetThreadContext(h, &ctx) != 0;
            stack.original = ctx.Rsp, stack.size = 0;
            if (ok && base > ctx.Rsp) {
                SIZE_T want = SIZE_T(std::min<uint64_t>(base - ctx.Rsp, kStackCopy)), got = 0;
                if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(ctx.Rsp), stack.bytes, want, &got))
                    stack.size = got;
            }
            ResumeThread(h);
            Taken t{id, 0, {}};
            if (ok) t.depth = Unwind(ctx, stack, t.pc, kDeep);
            taken.push_back(t);
        }
        CloseHandle(h);
    }
    std::unordered_map<uint64_t, std::string> names;
    for (const Taken& t : taken) {
        std::string line;
        for (int i = 0; i < t.depth; i++) {
            if (i) line += " < ";
            line += Function(t.pc[i], names);
        }
        Log("watchdog: thread %lu: %s", t.id, line.c_str());
    }
}

std::atomic<uint64_t> g_beats[int(Beat::kCount)];
std::atomic<void (*)()> g_hang_report{nullptr};

void WatchdogThread() {
    static const char* const kNames[] = {"vblank", "flip"};
    uint64_t last[int(Beat::kCount)] = {};
    ULONGLONG since[int(Beat::kCount)] = {};
    bool stalled[int(Beat::kCount)] = {};
    int reports = 0;
    for (;;) {
        Sleep(1000);
        ULONGLONG now = GetTickCount64();
        // ---- PC comfort features: a focus pause holds the guest on purpose, no frames are due ----
        if (GuestTimeHeld()) {
            for (int i = 0; i < int(Beat::kCount); i++) since[i] = now;
            continue;
        }
        // ---- end PC comfort features ----
        for (int i = 0; i < int(Beat::kCount); i++) {
            uint64_t beats = g_beats[i].load(std::memory_order_relaxed);
            if (beats != last[i] || !beats) {
                if (stalled[i]) Log("watchdog: %s runs again", kNames[i]);
                last[i] = beats, since[i] = now, stalled[i] = false;
                continue;
            }
            if (stalled[i] || now - since[i] < 5000) continue;
            stalled[i] = true;
            Log("watchdog: no %s for %.0f s", kNames[i], (now - since[i]) / 1000.0);
            if (reports++ < 3) {
                HostStacks();
                DumpThreads();
                if (auto report = g_hang_report.load()) report();
            }
        }
    }
}

}  // namespace

void SetHangReport(void (*report)()) { g_hang_report = report; }

void DumpHostStacks() { HostStacks(); }

std::string ThreadStack(void* thread_handle) {
    HANDLE h = static_cast<HANDLE>(thread_handle);
    static StackCopy stack;  // (one caller: the hitch sampler)
    uint64_t base = StackBase(h);
    uint64_t pc[40];
    int depth = 0;
    if (SuspendThread(h) == DWORD(-1)) return "(not suspended)";
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    bool ok = GetThreadContext(h, &ctx) != 0;
    stack.original = ctx.Rsp, stack.size = 0;
    if (ok && base > ctx.Rsp) {
        SIZE_T want = SIZE_T(std::min<uint64_t>(base - ctx.Rsp, kStackCopy)), got = 0;
        if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(ctx.Rsp), stack.bytes, want, &got))
            stack.size = got;
    }
    ResumeThread(h);
    if (ok) depth = Unwind(ctx, stack, pc, 40);
    static std::unordered_map<uint64_t, std::string> names, lines;
    std::string out;
    for (int i = 0; i < depth; i++) {
        if (i) out += " < ";
        out += Function(pc[i], names);
        std::string line = LineOf(pc[i] - (i ? 1 : 0), lines);
        if (!line.empty()) out += " (" + line + ")";
    }
    return out;
}

void Heartbeat(Beat which) { g_beats[int(which)].fetch_add(1, std::memory_order_relaxed); }

void StartProfiler() {
    if (!GetEnvironmentVariableA("FCI_NO_WATCHDOG", nullptr, 0)) std::thread(WatchdogThread).detach();
    char value[32] = {};
    if (!GetEnvironmentVariableA("FCI_PROFILE", value, sizeof value)) return;
    double seconds = std::atof(value), start = 0;
    if (const char* at = std::strchr(value, '@')) start = std::atof(at + 1);
    if (seconds <= 0) seconds = 10;
    std::thread(ProfilerThread, start, seconds).detach();
}

}  // namespace rt
