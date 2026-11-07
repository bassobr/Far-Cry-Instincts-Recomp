// The recompiled game code is built on the player's PC, so a release carries none of it: fci-recomp translates the
// player's default.xbe to C++, clang compiles that and lld links it into game_code\game_code.dll, all from the
// compiler folder beside the executable, with the flags and the profile (PGO) the developer build uses. The first
// start does this behind a progress window, once; later starts (and the relaunch between levels) load the DLL.
// Finished objects are kept until the DLL is linked, so a closed window or a crash loses only the files in progress
// and the next start carries on.
//
// The build driver, the progress window and the error handling are adapted from SCDA-XBERecomp's
// src/app/game_code.cpp (MIT License, Copyright (c) TGP482 and Joshhhuaaa).
#include "app/game_code.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "fci_abi.h"
#include "runtime/log.h"
#include "runtime/runtime.h"

namespace app {

namespace {

namespace fs = std::filesystem;

constexpr wchar_t kTitle[] = L"Far Cry Instincts";
// a compiler's peak working set on the biggest file with the profile is ~340 MB (PC); with room to spare
constexpr uint64_t kJobMemory = 640ull << 20;
constexpr uint64_t kDiskNeeded = 2ull << 30;  // sources, objects and the DLL, with room to spare

// the compiler folder's tools and inputs
fs::path g_exe_dir;
fs::path CompilerDir() { return g_exe_dir / "compiler"; }
fs::path OutDir() { return g_exe_dir / "game_code"; }
fs::path WorkDir() { return OutDir() / "work"; }
fs::path GenDir() { return WorkDir() / "gen"; }
fs::path ObjDir() { return WorkDir() / "obj"; }
fs::path DllPath() { return OutDir() / "game_code.dll"; }
fs::path BuildLog() { return OutDir() / "build.log"; }
fs::path Profile() { return CompilerDir() / "config" / "game_code.profdata"; }
std::wstring Quote(const fs::path& path) { return L"\"" + path.wstring() + L"\""; }

std::wstring Hex(uint64_t v) {
    wchar_t buf[24];
    swprintf(buf, 24, L"0x%016llXull", (unsigned long long)v);
    return buf;
}

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

bool FileBytes(const fs::path& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

// What a first-start DLL is built with besides the xbe and the ABI: the flags, the tools (size and time: a new
// release brings new ones) and the profile. Never 0, which marks a DLL from the developer build.
uint64_t BuildKey() {
    std::string text = "flags ";
    for (const wchar_t* p = FCI_GAME_FLAGS; *p; p++) text += char(*p);
    char parts[16] = {};
    GetEnvironmentVariableA("FCI_GAME_CODE_PARTITIONS", parts, sizeof parts);  // another split is another build
    text += std::string("\npartitions ") + parts;
    for (const char* tool : {"clang.exe", "lld-link.exe", "fci-recomp.exe", "FarCryInstincts.lib"}) {
        WIN32_FILE_ATTRIBUTE_DATA a{};
        GetFileAttributesExW((CompilerDir() / tool).c_str(), GetFileExInfoStandard, &a);
        char line[160];
        snprintf(line, sizeof line, "\n%s %lu %lu %lu %lu", tool, a.nFileSizeHigh, a.nFileSizeLow,
                 a.ftLastWriteTime.dwHighDateTime, a.ftLastWriteTime.dwLowDateTime);
        text += line;
    }
    for (const char* config : {"game_code.profdata", "hle.txt", "symbols.txt", "symbols_fci.txt"}) {
        std::vector<uint8_t> bytes;
        uint64_t h = FileBytes(CompilerDir() / "config" / config, bytes) ? xbe::ContentHash(bytes.data(), bytes.size()) : 0;
        char line[96];
        snprintf(line, sizeof line, "\n%s %016llX", config, (unsigned long long)h);
        text += line;
    }
    return xbe::ContentHash(reinterpret_cast<const uint8_t*>(text.data()), text.size()) | 1;
}

// ---- the build ----

enum class Stage { Translating, Compiling, Linking, Done };

// written by the build thread, read by the window
struct Progress {
    std::atomic<Stage> stage{Stage::Translating};
    std::atomic<uint64_t> bytes_done{0};
    std::atomic<uint64_t> bytes_total{0};
    std::atomic<uint32_t> files_done{0};
    std::atomic<uint32_t> files_total{0};
    std::atomic<uint32_t> first_wave{1};  // files compiled before the time left is worth showing
    std::atomic<int64_t> compile_start{0};
    std::atomic<bool> cancel{false};
};

Progress g_progress;
std::wstring g_failure;  // the step that failed, written by the build thread before it ends
constexpr UINT kBuildDone = WM_APP + 1;

// The progress window lives on a thread of its own and the build never waits on it: when it cannot be made (no
// display: a session without one, an X server that refuses the connection under Proton) or its thread hangs
// in the window system, the build runs to the end all the same.
enum class WindowState { Starting, Shown, Failed };
std::atomic<WindowState> g_window_state{WindowState::Starting};
std::atomic<HWND> g_window{nullptr};
std::atomic<bool> g_build_over{false};  // the window closes itself if it comes up after the build

const char* WindowNote() {
    switch (g_window_state.load()) {
        case WindowState::Shown: return "";
        case WindowState::Failed: return ", without the progress window";
        default: return ", the progress window not up yet";
    }
}

constexpr int64_t kNoteMs = 15000;  // a line in the log this often while compiling

// CPU time a process has used, seconds
double CpuSeconds(HANDLE process) {
    FILETIME created, ended, kernel, user;
    if (!GetProcessTimes(process, &created, &ended, &kernel, &user)) return 0;
    auto s = [](const FILETIME& f) { return double((uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 1e7; };
    return s(kernel) + s(user);
}

bool RecordFailure(const std::wstring& what) {
    g_failure = what;
    rt::Log("game code: %s", fs::path(what).string().c_str());
    return false;
}

// starts and waits on the build's processes; closing the job, or this process ending, ends them all
class Processes {
public:
    Processes() {
        job_ = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
        limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limit, sizeof(limit));
        SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
        log_ = CreateFileW(BuildLog().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    ~Processes() {
        CloseHandle(job_);
        if (log_ != INVALID_HANDLE_VALUE) CloseHandle(log_);
    }

    void Note(const std::string& line) {
        DWORD n = 0;
        std::string text = line + "\r\n";
        if (log_ != INVALID_HANDLE_VALUE) WriteFile(log_, text.data(), DWORD(text.size()), &n, nullptr);
    }

    // null when it did not start; its output goes to build.log. Without a console (DETACHED_PROCESS): the output
    // goes to the log's handle anyway, and under Wine/Proton no console host is started for each compiler.
    HANDLE Start(std::wstring command, const fs::path& dir) {
        Note(fs::path(command).string());
        STARTUPINFOW si{sizeof(si)};
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = nullptr;
        si.hStdOutput = log_;
        si.hStdError = log_;
        PROCESS_INFORMATION pi{};
        DWORD flags = DETACHED_PROCESS | CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS;
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, flags, nullptr, dir.c_str(), &si, &pi)) {
            rt::Log("game code: could not start %s (%lu)", fs::path(command).string().c_str(), GetLastError());
            return nullptr;
        }
        if (!AssignProcessToJobObject(job_, pi.hProcess))
            rt::Log("game code: a build process is not in the job (%lu); it is ended by hand if the build stops",
                    GetLastError());
        if (ResumeThread(pi.hThread) == DWORD(-1)) {  // a process that never runs would hold the build forever
            rt::Log("game code: could not resume %s (%lu)", fs::path(command).string().c_str(), GetLastError());
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return nullptr;
        }
        CloseHandle(pi.hThread);
        return pi.hProcess;
    }

    // closes the process; its exit code
    static DWORD ExitCode(HANDLE process) {
        DWORD code = 1;
        GetExitCodeProcess(process, &code);
        CloseHandle(process);
        return code;
    }

    // false when cancelled, which ends every process of the build
    bool Wait(HANDLE process) {
        while (WaitForSingleObject(process, 250) == WAIT_TIMEOUT)
            if (g_progress.cancel) return Cancel();
        return true;
    }

    bool Cancel() {
        TerminateJobObject(job_, 1);
        return false;
    }

private:
    HANDLE job_ = nullptr;
    HANDLE log_ = INVALID_HANDLE_VALUE;
};

// a source to compile and the object it makes
struct Job {
    fs::path source;
    fs::path object;
    uint64_t bytes = 0;
};

// compilers that run at once: one a logical processor, as many as the free memory holds (FCI_GAME_CODE_JOBS=<n>)
size_t Slots() {
    MEMORYSTATUSEX mem{sizeof(mem)};
    GlobalMemoryStatusEx(&mem);
    size_t by_memory = size_t(mem.ullAvailPhys / kJobMemory);
    size_t by_cpu = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    char env[16] = {};
    if (GetEnvironmentVariableA("FCI_GAME_CODE_JOBS", env, sizeof env) && std::atoi(env) > 0) return size_t(std::atoi(env));
    return std::clamp<size_t>(std::min(by_memory, by_cpu), 1, MAXIMUM_WAIT_OBJECTS);
}

struct Build {
    const xbe::Image* image = nullptr;
    uint64_t key = 0;
    double translate_s = 0, compile_s = 0, link_s = 0;
    size_t slots = 0, compiled = 0;
};

std::string KeyText(const Build& b) {
    char text[96];
    snprintf(text, sizeof text, "%016llX %016llX %016llX", (unsigned long long)FCI_ABI_HASH,
             (unsigned long long)b.image->content_hash, (unsigned long long)b.key);
    return text;
}

// fci-recomp writes the C++ into work\gen; skipped when an earlier, interrupted build of the same inputs did it
bool Translate(Build& b, Processes& run) {
    std::error_code ec;
    const fs::path key_file = WorkDir() / "key.txt";
    std::string old_key;
    if (std::ifstream f(key_file); f) std::getline(f, old_key);
    if (old_key != KeyText(b)) {
        fs::remove_all(WorkDir(), ec);  // another xbe, executable or compiler: nothing of it is of use
        fs::create_directories(ObjDir(), ec);
        std::ofstream(key_file) << KeyText(b) << "\n";
    }
    if (fs::exists(GenDir() / "done.txt", ec)) return true;
    fs::remove_all(GenDir(), ec);
    fs::create_directories(GenDir(), ec);
    const fs::path xbe = WorkDir() / "default.xbe";
    const auto& bytes = b.image->File();
    std::ofstream(xbe, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    std::wstring command = Quote(CompilerDir() / "fci-recomp.exe") + L" " + Quote(xbe) + L" " + Quote(GenDir()) + L" " +
                           Quote(CompilerDir() / "config");
    char parts[16] = {};
    if (GetEnvironmentVariableA("FCI_GAME_CODE_PARTITIONS", parts, sizeof parts))
        command += L" --partitions " + std::to_wstring(std::atoi(parts));
    const int64_t start = NowMs();
    HANDLE process = run.Start(command, WorkDir());
    if (!process) return RecordFailure(L"fci-recomp.exe could not be started.");
    if (!run.Wait(process)) return false;
    const DWORD code = Processes::ExitCode(process);
    if (code == 0xC0000135) return RecordFailure(L"fci-recomp.exe is missing a DLL it needs.");  // STATUS_DLL_NOT_FOUND
    if (code) return RecordFailure(L"Translating default.xbe failed (fci-recomp.exe exit code " + std::to_wstring(code) + L").");
    fs::remove(xbe, ec);
    std::ofstream(GenDir() / "done.txt") << "translated\n";
    b.translate_s = (NowMs() - start) / 1000.0;
    return true;
}

std::wstring CompileCommand(const Build& b, const Job& job) {
    const fs::path include = CompilerDir() / "include";
    // /vctoolsdir: the code needs nothing of Visual Studio (no system headers, no linking here), and without it
    // clang-cl searches for an installation, through COM (the Setup Configuration API) among others; under
    // Wine/Proton COM needs the window system, which a session without a display does not have
    std::wstring command = Quote(CompilerDir() / "clang.exe") + L" --driver-mode=cl /nologo -TP /vctoolsdir " +
                           Quote(CompilerDir()) + L" " FCI_GAME_FLAGS;
    command += L" -DFCI_ABI_HASH=" + Hex(FCI_ABI_HASH) + L" -DFCI_BUILD_KEY=" + Hex(b.key);
    std::error_code ec;
    if (fs::exists(Profile(), ec))
        command += L" \"-fprofile-instr-use=" + Profile().wstring() +
                   L"\" -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date";
    command += L" /I" + Quote(include / "freestanding") + L" /I" + Quote(include) + L" /I" + Quote(GenDir());
    return command + L" -c " + Quote(job.source) + L" /Fo" + Quote(fs::path(job.object.wstring() + L".tmp"));
}

// an object is written beside its final name and moved there once whole
bool FinishJob(const Job& job, HANDLE process) {
    const std::wstring name = job.source.filename().wstring();
    if (DWORD code = Processes::ExitCode(process))
        return RecordFailure(L"Compiling " + name + L" failed (clang.exe exit code " + std::to_wstring(code) + L").");
    std::error_code ec;
    fs::rename(job.object.wstring() + L".tmp", job.object, ec);
    if (ec) return RecordFailure(L"Saving the compiled " + name + L" failed.");
    g_progress.bytes_done += job.bytes;
    g_progress.files_done++;
    return true;
}

// the biggest files first, as many at once as Slots allows; objects an interrupted build left are kept
bool Compile(Build& b, Processes& run, std::vector<Job>& jobs) {
    std::error_code ec;
    std::vector<Job> todo;
    for (const auto& e : fs::directory_iterator(GenDir(), ec)) {
        const fs::path& p = e.path();
        // the DLL is table.cpp and code_NNN.cpp; the rest (semtest_cases.cpp) is not part of it
        const std::wstring name = p.filename().wstring();
        if (p.extension() != ".cpp" || (name != L"table.cpp" && name.rfind(L"code_", 0) != 0)) continue;
        Job job{p, ObjDir() / (p.stem().wstring() + L".obj"), e.file_size(ec)};
        jobs.push_back(job);
        g_progress.bytes_total += job.bytes;
        if (fs::exists(job.object, ec)) g_progress.bytes_done += job.bytes;
        else todo.push_back(job);
    }
    if (jobs.empty()) return RecordFailure(L"fci-recomp.exe wrote no code.");
    std::sort(todo.begin(), todo.end(), [](const Job& x, const Job& y) { return x.bytes > y.bytes; });
    b.slots = Slots();
    b.compiled = todo.size();
    rt::Log("game code: compiling %zu of %zu files, %zu at a time", todo.size(), jobs.size(), b.slots);
    g_progress.files_total = uint32_t(todo.size());
    g_progress.first_wave = uint32_t(std::min(b.slots, todo.size()));
    g_progress.compile_start = NowMs();
    g_progress.stage = Stage::Compiling;

    std::vector<HANDLE> running;
    std::vector<const Job*> running_jobs;
    size_t next = 0;
    int64_t next_note = NowMs() + kNoteMs;
    while (next < todo.size() || !running.empty()) {
        if (NowMs() >= next_note) {
            // the log shows the build going on without the window too, and compilers that do not run
            next_note += kNoteMs;
            double cpu = 0;
            for (HANDLE p : running) cpu += CpuSeconds(p);
            rt::Log("game code: %u of %zu files compiled, %zu compilers running (%.1f s CPU so far)%s",
                    g_progress.files_done.load(), todo.size(), running.size(), cpu, WindowNote());
        }
        for (; running.size() < b.slots && next < todo.size(); next++) {
            HANDLE process = run.Start(CompileCommand(b, todo[next]), GenDir());
            if (!process) {
                RecordFailure(L"clang.exe could not be started.");
                return run.Cancel();
            }
            running.push_back(process);
            running_jobs.push_back(&todo[next]);
        }
        DWORD wait = WaitForMultipleObjects(DWORD(running.size()), running.data(), FALSE, 250);
        if (g_progress.cancel) return run.Cancel();
        if (wait == WAIT_TIMEOUT) continue;
        size_t done = wait - WAIT_OBJECT_0;
        if (done >= running.size()) {
            RecordFailure(L"Waiting on the compilers failed.");
            return run.Cancel();
        }
        if (!FinishJob(*running_jobs[done], running[done])) return run.Cancel();
        running.erase(running.begin() + done);
        running_jobs.erase(running_jobs.begin() + done);
    }
    b.compile_s = (NowMs() - g_progress.compile_start) / 1000.0;
    return true;
}

// lld links the objects against the executable's exports into the DLL
bool Link(Build& b, Processes& run, const std::vector<Job>& jobs) {
    g_progress.stage = Stage::Linking;
    const int64_t start = NowMs();
    std::error_code ec;
    fs::remove(DllPath(), ec);
    const fs::path rsp = WorkDir() / "link.rsp";
    {
        std::wofstream r(rsp);
        // /lldignoreenv: no search for Visual Studio and the Windows SDK either (as for clang above)
        r << L"/nologo /dll /noentry /nodefaultlib /lldignoreenv /machine:x64 /debug /incremental:no\n";
        r << L"/pdb:" << Quote(OutDir() / "game_code.pdb") << L"\n/out:" << Quote(DllPath()) << L"\n";
        r << L"/implib:" << Quote(WorkDir() / "game_code.lib") << L"\n";
        r << Quote(CompilerDir() / "FarCryInstincts.lib") << L"\n";
        for (const Job& job : jobs) r << Quote(job.object) << L"\n";
    }
    HANDLE process = run.Start(Quote(CompilerDir() / "lld-link.exe") + L" @" + Quote(rsp), WorkDir());
    if (!process) return RecordFailure(L"lld-link.exe could not be started.");
    if (!run.Wait(process)) return false;
    if (DWORD code = Processes::ExitCode(process))
        return RecordFailure(L"Linking game_code.dll failed (lld-link.exe exit code " + std::to_wstring(code) + L").");
    b.link_s = (NowMs() - start) / 1000.0;
    return true;
}

bool RunBuild(Build& b) {
    Processes run;
    run.Note("---- " + KeyText(b) + " ----");
    if (!Translate(b, run)) return false;
    std::vector<Job> jobs;
    if (!Compile(b, run, jobs)) return false;
    if (!Link(b, run, jobs)) return false;
    // what is left is only of use to a build of the same inputs, which the DLL makes unnecessary
    char keep[4] = {};
    if (!GetEnvironmentVariableA("FCI_KEEP_GAME_CODE_BUILD", keep, sizeof keep)) {
        std::error_code ec;
        fs::remove_all(WorkDir(), ec);
    }
    return true;
}

// what stops a build before it starts, for the player to fix; empty when nothing does
std::wstring Blocker() {
    for (const wchar_t* tool : {L"clang.exe", L"lld-link.exe", L"fci-recomp.exe", L"FarCryInstincts.lib"}) {
        std::error_code ec;
        if (!fs::exists(CompilerDir() / tool, ec))
            return L"The game's code has to be compiled for this PC, but the compiler folder is missing " +
                   std::wstring(tool) + L". Extract the whole release again, then start the game.";
    }
    std::error_code ec;
    fs::create_directories(OutDir(), ec);
    if (ec) return L"The game's folder cannot be written to. Move it somewhere you can write to, such as Documents.";
    ULARGE_INTEGER free{};
    if (GetDiskFreeSpaceExW(OutDir().c_str(), &free, nullptr, nullptr) && free.QuadPart < kDiskNeeded)
        return L"Compiling the game's code for this PC needs 2 GB of free space on the drive it is on.";
    return {};
}

// ---- the progress window ----

constexpr COLORREF kBackground = RGB(14, 16, 14);
constexpr COLORREF kText = RGB(232, 236, 226);
constexpr COLORREF kDim = RGB(150, 158, 144);
constexpr COLORREF kBarEmpty = RGB(34, 40, 32);
constexpr COLORREF kBarFull = RGB(214, 150, 52);

HFONT g_font = nullptr, g_heading_font = nullptr;

float Fraction() {
    // the translation is a few percent of the work, the link the last few
    switch (g_progress.stage.load()) {
        case Stage::Translating: return 0.0f;
        case Stage::Compiling:
            return 0.03f + 0.94f * (g_progress.bytes_total ? float(g_progress.bytes_done) / float(g_progress.bytes_total) : 0.0f);
        case Stage::Linking: return 0.97f;
        default: return 1.0f;
    }
}

std::wstring StageText() {
    switch (g_progress.stage.load()) {
        case Stage::Translating: return L"Translating the game's code (default.xbe)";
        case Stage::Compiling:
            return L"Compiling, file " + std::to_wstring(std::min(g_progress.files_done + 1, g_progress.files_total.load())) +
                   L" of " + std::to_wstring(g_progress.files_total);
        case Stage::Linking: return L"Linking";
        default: return L"Starting the game";
    }
}

// from the rate the files compiled so far went at, once every compiler has finished one
std::wstring TimeLeftText() {
    if (g_progress.stage != Stage::Compiling) return {};
    if (g_progress.files_done < g_progress.first_wave) return L"Working out the time left";
    uint64_t done = 0, total = g_progress.bytes_total;
    done = g_progress.bytes_done;
    if (!done || done >= total) return {};
    double elapsed = double(NowMs() - g_progress.compile_start);
    double left = elapsed * double(total - done) / double(done);
    int minutes = int(left / 60000.0 + 0.5);
    if (minutes < 1) return L"Less than a minute left";
    return L"About " + std::to_wstring(minutes) + (minutes == 1 ? L" minute left" : L" minutes left");
}

void DrawLine(HDC dc, RECT row, const std::wstring& text, COLORREF color, UINT align) {
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), int(text.size()), &row, align | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
}

void FillBox(HDC dc, const RECT& box, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &box, brush);
    DeleteObject(brush);
}

int Unit(HWND hwnd) { return MulDiv(10, GetDpiForWindow(hwnd), 96); }

// a heading, the stage, the bar, then the percentage and time left
void Paint(HDC dc, const RECT& client, int unit) {
    FillBox(dc, client, kBackground);
    SetBkMode(dc, TRANSPARENT);
    RECT heading{client.left + 2 * unit, client.top + 2 * unit, client.right - 2 * unit, client.top + 5 * unit};
    RECT stage = heading;
    OffsetRect(&stage, 0, 3 * unit);
    RECT bar{stage.left, stage.bottom + unit, stage.right, stage.bottom + unit + unit * 3 / 2};
    RECT bottom{bar.left, bar.bottom + unit / 2, bar.right, bar.bottom + unit / 2 + 3 * unit};
    SelectObject(dc, g_heading_font);
    const int percent = int(Fraction() * 100.0f);
    DrawLine(dc, heading, L"Compiling game code for your PC (one time) … " + std::to_wstring(percent) + L" %", kText,
             DT_LEFT);
    SelectObject(dc, g_font);
    DrawLine(dc, stage, StageText(), kDim, DT_LEFT);
    FillBox(dc, bar, kBarEmpty);
    RECT filled = bar;
    filled.right = bar.left + LONG(float(bar.right - bar.left) * Fraction());
    FillBox(dc, filled, kBarFull);
    DrawLine(dc, bottom, L"The game starts by itself when this is done.", kDim, DT_LEFT);
    DrawLine(dc, bottom, TimeLeftText(), kDim, DT_RIGHT);
}

// painted off screen, then copied, so the bar does not flicker
void PaintWindow(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT client;
    GetClientRect(hwnd, &client);
    HDC memory = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
    HGDIOBJ old_bitmap = SelectObject(memory, bitmap);
    HGDIOBJ old_font = SelectObject(memory, g_font);
    Paint(memory, client, Unit(hwnd));
    BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
    SelectObject(memory, old_font);
    SelectObject(memory, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: PaintWindow(hwnd); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_TIMER: InvalidateRect(hwnd, nullptr, FALSE); return 0;
        case WM_CLOSE: {
            const wchar_t* question =
                L"Stop compiling the game's code? What is done so far is kept, and the next start carries on.";
            if (MessageBoxW(hwnd, question, kTitle, MB_YESNO | MB_ICONQUESTION) == IDYES) g_progress.cancel = true;
            return 0;
        }
        case kBuildDone: DestroyWindow(hwnd); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// centred on the primary monitor, sized for its DPI, in the message font
HWND OpenWindow() {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
    wc.lpszClassName = L"FCIGameCode";
    RegisterClassExW(&wc);
    UINT dpi = GetDpiForSystem();
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r{0, 0, MulDiv(600, dpi, 96), MulDiv(150, dpi, 96)};
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi);
    int w = r.right - r.left;
    int h = r.bottom - r.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, kTitle, style, x, y, w, h, nullptr, nullptr, inst, nullptr);
    if (!hwnd) return nullptr;
    NONCLIENTMETRICSW metrics{sizeof(metrics)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi);
    g_font = CreateFontIndirectW(&metrics.lfMessageFont);
    LOGFONTW heading = metrics.lfMessageFont;
    heading.lfHeight = heading.lfHeight * 4 / 3;
    heading.lfWeight = FW_SEMIBOLD;
    g_heading_font = CreateFontIndirectW(&heading);
    SetTimer(hwnd, 1, 100, nullptr);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    return hwnd;
}

// the last lines of build.log, where a compiler's error is
std::wstring LogTail() {
    constexpr size_t kTailBytes = 1500;
    std::ifstream f(BuildLog(), std::ios::binary | std::ios::ate);
    if (!f) return {};
    std::streamoff size = f.tellg();
    std::streamoff start = size > std::streamoff(kTailBytes) ? size - std::streamoff(kTailBytes) : 0;
    std::string tail(size_t(size - start), '\0');
    f.seekg(start);
    f.read(tail.data(), std::streamsize(tail.size()));
    if (start) tail.erase(0, tail.find('\n') + 1);  // from a whole line
    int length = MultiByteToWideChar(CP_UTF8, 0, tail.data(), int(tail.size()), nullptr, 0);
    std::wstring text(size_t(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, tail.data(), int(tail.size()), text.data(), length);
    return text;
}

void ShowFailure() {
    std::wstring text = L"Compiling the game's code for this PC failed. " + g_failure;
    if (std::wstring tail = LogTail(); !tail.empty()) text += L"\n\nThe last lines of the log:\n\n" + tail;
    text += L"\n\nThe full details are in " + BuildLog().wstring() + L".";
    MessageBoxW(nullptr, text.c_str(), kTitle, MB_ICONERROR);
}

// the progress window's thread: the window and its messages until the build is over
void WindowThread() {
    // FCI_TEST_PROGRESS_WINDOW=fail|hang (tests): the window cannot be made / its thread never comes back
    char test[8] = {};
    GetEnvironmentVariableA("FCI_TEST_PROGRESS_WINDOW", test, sizeof test);
    if (!strcmp(test, "hang")) {
        rt::Log("game code: FCI_TEST_PROGRESS_WINDOW=hang: the progress window's thread blocks");
        Sleep(INFINITE);
    }
    HWND hwnd = strcmp(test, "fail") ? OpenWindow() : nullptr;
    if (!hwnd) {
        const DWORD error = GetLastError();
        g_window_state = WindowState::Failed;
        rt::Log("game code: no progress window (%s, error %lu); the build goes on without it",
                test[0] ? "FCI_TEST_PROGRESS_WINDOW=fail" : "CreateWindowEx failed", error);
        return;
    }
    g_window = hwnd;
    g_window_state = WindowState::Shown;
    rt::Log("game code: progress window shown");
    if (g_build_over) DestroyWindow(hwnd);  // the build ended while the window was being made
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    DeleteObject(g_font);
    DeleteObject(g_heading_font);
}

// the build on this thread, the window on its own; false when it failed or the player stopped it
bool BuildWithWindow(Build& b) {
    if (std::wstring blocker = Blocker(); !blocker.empty()) {
        rt::Log("game code: %s", fs::path(blocker).string().c_str());
        MessageBoxW(nullptr, blocker.c_str(), kTitle, MB_ICONERROR);
        return false;
    }
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);  // no sleep while it works
    std::thread window;
    try {
        window = std::thread(WindowThread);
    } catch (const std::exception&) {
        g_window_state = WindowState::Failed;
        rt::Log("game code: no thread for the progress window; the build goes on without it");
    }
    const bool ok = RunBuild(b);
    if (ok) g_progress.stage = Stage::Done;
    g_build_over = true;
    if (HWND hwnd = g_window) PostMessageW(hwnd, kBuildDone, ok, 0);
    if (window.joinable()) {
        // a window thread stuck in the window system is left behind (it holds nothing of the build)
        HANDLE thread = window.native_handle();
        if (WaitForSingleObject(thread, 3000) == WAIT_OBJECT_0) {
            window.join();
        } else {
            rt::Log("game code: the progress window's thread did not end; left behind");
            window.detach();
        }
    }
    SetThreadExecutionState(ES_CONTINUOUS);
    if (ok) rt::Log("game code: build finished%s", WindowNote());
    if (!ok && !g_progress.cancel) ShowFailure();
    if (g_progress.cancel) rt::Log("game code: build stopped by the player");
    return ok;
}

// the DLL serves this executable (OpenGameCode checked the ABI), the xbe found and, if built here, these tools
bool Current(const rt::GameCode& code, const xbe::Image& image, uint64_t key, std::string* why) {
    if (code.xbe != image.content_hash) *why = "it was translated from another default.xbe";
    else if (code.build && code.build != key) *why = "it was built with other tools, flags or profile";
    else return true;
    return false;
}

}  // namespace

bool PrepareGameCode(const fs::path& exe_dir, const xbe::Image& image) {
    g_exe_dir = exe_dir;
    std::string why;
    const int64_t start = NowMs();
    // the relaunch between levels runs the DLL the first process ran, whatever has changed since: never a build
    if (GetEnvironmentVariableA("FCI_RELAUNCHED", nullptr, 0) > 0) {
        if (rt::OpenGameCode(DllPath(), &why)) {
            rt::Log("game code: %s (relaunch: as the first process ran it)", DllPath().string().c_str());
            return true;
        }
        rt::Log("game code: %s cannot be loaded after the relaunch: %s", DllPath().string().c_str(), why.c_str());
        MessageBoxW(nullptr, L"The game's code (game_code\\game_code.dll) cannot be loaded. Start the game again.",
                    kTitle, MB_ICONERROR);
        return false;
    }
    // one build at a time: a second copy of the game started meanwhile waits for it, then finds the DLL current
    HANDLE lock = CreateMutexW(nullptr, FALSE, L"FarCryInstinctsRecomp-GameCode");
    WaitForSingleObject(lock, INFINITE);
    Build b;
    b.image = &image;
    b.key = BuildKey();
    bool built = false;
    const rt::GameCode* code = rt::OpenGameCode(DllPath(), &why);
    const bool force = GetEnvironmentVariableA("FCI_REBUILD_GAME_CODE", nullptr, 0) > 0;
    if (force) {
        why = "FCI_REBUILD_GAME_CODE";
        std::error_code ec;
        fs::remove_all(WorkDir(), ec);  // from scratch, not from what an interrupted build left
    }
    if (!code || force || !Current(*code, image, b.key, &why)) {
        rt::CloseGameCode();
        rt::Log("game code: building %s (%s)", DllPath().string().c_str(), why.c_str());
        built = BuildWithWindow(b);
        code = built ? rt::OpenGameCode(DllPath(), &why) : nullptr;
        if (built && !code) {
            rt::Log("game code: the new DLL does not load: %s", why.c_str());
            g_failure = L"The new game_code.dll does not load.";
            ShowFailure();
        } else if (code && !Current(*code, image, b.key, &why)) {
            rt::Log("game code: the new DLL is not current: %s", why.c_str());
            rt::CloseGameCode();
            code = nullptr;
        }
    }
    ReleaseMutex(lock);
    CloseHandle(lock);
    if (!code) return false;
    if (built)
        rt::Log("game code: built in %.1f s (translate %.1f s, compile %zu files %.1f s with %zu at a time, link %.1f s)",
                (NowMs() - start) / 1000.0, b.translate_s, b.compiled, b.compile_s, b.slots, b.link_s);
    rt::Log("game code: %s (%s build, %u functions)", DllPath().string().c_str(), code->build ? "first-start" : "developer",
            code->function_count);
    return true;
}

}  // namespace app
