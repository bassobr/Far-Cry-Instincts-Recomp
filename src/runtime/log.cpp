#include "runtime/log.h"

#include <windows.h>
#include <dbghelp.h>

#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

namespace rt {

namespace {

FILE* g_log_file = nullptr;

// ---- performance: the log is written by a thread of its own ----
// A line's write (console or pipe, fci.log, flushed) took 12-19 ms now and then on the Steam Deck (Proton, the 5 s
// statistics line on the render thread: single frames of 22 ms); callers now only queue the text.
std::timed_mutex g_io_lock;  // the writing
std::mutex g_queue_lock;     // the text not yet written
std::condition_variable g_queue_cv;
std::string g_pending;
bool g_writer = false;  // the thread runs (InitLog)

void WriteNow(const char* text) {
    std::fputs(text, stdout);
    if (g_log_file) {
        std::fputs(text, g_log_file);
        std::fflush(g_log_file);
    }
}

// writes what is queued; false when the writer did not let go in time (a crash on it)
bool Drain() {
    std::unique_lock io(g_io_lock, std::defer_lock);
    if (!io.try_lock_for(std::chrono::milliseconds(500))) return false;
    std::string text;
    {
        std::lock_guard q(g_queue_lock);
        text.swap(g_pending);
    }
    if (!text.empty()) WriteNow(text.c_str());
    return true;
}

void Writer() {
    for (;;) {
        {
            std::unique_lock q(g_queue_lock);
            g_queue_cv.wait(q, [] { return !g_pending.empty(); });
        }
        Drain();
    }
}

void Write(const char* text) {
    if (!g_writer) {
        std::lock_guard io(g_io_lock);
        WriteNow(text);
        return;
    }
    size_t queued;
    {
        std::lock_guard q(g_queue_lock);
        g_pending += text;
        queued = g_pending.size();
    }
    g_queue_cv.notify_one();
    if (queued > (4u << 20)) Drain();  // a flood of trace lines: the callers wait for the file, not for memory
}
// ---- end performance ----

}  // namespace

void InitLog() {
    // The exe is a GUI program (no console window of its own, also none under Wine/Proton). Started from a console
    // (cmd, PowerShell), the log also goes there; FCI_CONSOLE=1 opens a console window for it.
    bool console = AttachConsole(ATTACH_PARENT_PROCESS) != 0;
    if (!console && GetEnvironmentVariableA("FCI_CONSOLE", nullptr, 0) > 0) console = AllocConsole() != 0;
    if (console) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (char* slash = std::strrchr(path, '\\')) std::strcpy(slash + 1, "fci.log");
    // a relaunched title (kernel::Relaunch, the game's reboot between levels) goes on in the same log
    bool relaunched = GetEnvironmentVariableA("FCI_RELAUNCHED", nullptr, 0) > 0;
    // always appending: during a relaunch handover both instances write, each at the file's current end
    if (!relaunched)
        if (FILE* f = std::fopen(path, "w")) std::fclose(f);
    g_log_file = std::fopen(path, "a");
    if (relaunched && g_log_file) std::fputs("---- relaunch ----\n", g_log_file);
    g_writer = true;  // performance: the log thread
    std::thread(Writer).detach();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
}

void RefreshSymbols() { SymRefreshModuleList(GetCurrentProcess()); }

void Log(const char* fmt, ...) {
    char buf[4096];
    // FCI_LOG_TIME=1: every line starts with the seconds since start (to compare timings between runs)
    static const bool timed = GetEnvironmentVariableA("FCI_LOG_TIME", nullptr, 0) > 0;
    static const ULONGLONG start = GetTickCount64();
    int at = timed ? snprintf(buf, 32, "[%8.3f] ", (GetTickCount64() - start) / 1000.0) : 0;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + at, sizeof buf - 2 - at, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    n += at;
    if (n > int(sizeof buf - 2)) n = sizeof buf - 2;
    buf[n] = '\n';
    buf[n + 1] = 0;
    Write(buf);
}

std::string SymbolAt(uint64_t host_address) {
    alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 256];
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(storage);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    DWORD64 disp = 0;
    char buf[300];
    if (SymFromAddr(GetCurrentProcess(), host_address, &disp, sym))
        snprintf(buf, sizeof buf, "%s+0x%llX", sym->Name, (unsigned long long)disp);
    else
        snprintf(buf, sizeof buf, "%016llX", (unsigned long long)host_address);
    return buf;
}

// a crash's stack: what is queued first, then the stack, both before the caller ends the process
void PrintStack(void* context, void* thread) {
    std::string stack = FormatStack(context, thread);
    if (!Drain()) {  // the writer crashed while writing: past its lock
        WriteNow(stack.c_str());
        return;
    }
    std::lock_guard io(g_io_lock);
    WriteNow(stack.c_str());
}

void FlushLog() { Drain(); }

std::string FormatStack(void* context, void* thread) {
    std::string out;
    HANDLE process = GetCurrentProcess();
    if (!thread) thread = GetCurrentThread();
    CONTEXT ctx;
    if (context) ctx = *static_cast<CONTEXT*>(context);
    else RtlCaptureContext(&ctx);

    STACKFRAME64 frame{};
    frame.AddrPC.Offset = ctx.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;

    alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 256];
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(storage);
    for (int i = 0; i < 48; i++) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &ctx, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
            !frame.AddrPC.Offset)
            break;
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        char line[320];
        if (SymFromAddr(process, frame.AddrPC.Offset, &disp, sym))
            snprintf(line, sizeof line, "  %2d %s+0x%llX\n", i, sym->Name, (unsigned long long)disp);
        else
            snprintf(line, sizeof line, "  %2d %016llX\n", i, (unsigned long long)frame.AddrPC.Offset);
        out += line;
    }
    return out;
}

void Fatal(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    Log("FATAL: %s", buf);
    PrintStack(nullptr);
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 1);
    for (;;) {
    }
}

}  // namespace rt
