// Launch data (the kernel's LaunchDataPage) and title relaunch.
//
// FCI starts its levels through its own launch data: the main loop (0x00012270) reads XGetLaunchInfo
// (0x004A96F0: LAUNCH_DATA_PAGE at [LaunchDataPage], type at +0, data at +0x400, freed with
// MmFreeContiguousMemory) and accepts
//   - type 0 (LDT_TITLE, its own reboot): data = the 0x488-byte state block of 0x00047CF0, magic 'FCI1' at +4,
//     command line at +8;
//   - type 3 (LDT_FROM_DEBUGGER_CMDLINE): data = the command line itself.
// A command line with "-map <level>" (and optionally "-lpc<n>") becomes the game command
// "StartLevel level=<level> mission=<level> [localplayers=<n>]" (0x00012511..0x000125D6), i.e. the level loads
// right after boot without the menus.
//
// FCI_START_LEVEL=<level>[:<mission>[:<players>]] (debug switch) puts such a command line into a type-3 page at
// boot; the mission and the player count are applied by hle::Game_PushCommand (src/hle/start_level.cpp).
//
// Relaunch: between levels, on "return to menu" and after the credits FCI reboots itself (0x00047CF0:
// XLaunchNewImage("D:\default.xbe", state block) -> HalReturnToFirmware(ReturnFirmwareQuickReboot)).
// Here that starts the executable again with the page in FCI_LAUNCH_DATA (hex) and ends this process, so the
// next level loads as on the console. Without that the process just quit at the end of a level.
// The new process runs from its start (no CREATE_SUSPENDED); the window handover and a close of the window during
// the relaunch go through two named events made before it (FCI_HANDOVER_EVENT, kernel.h).
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app/settings.h"
#include "gpu/present.h"  // GPU memory at the handover
#include "kernel/kernel.h"
#include "video/display.h"
#include "runtime/memory.h"

namespace kernel {

namespace {

constexpr uint32_t kPageSize = 0x1000;
constexpr uint32_t kDataOffset = 0x400, kDataSize = 0xC00;
constexpr uint32_t kTypeTitle = 0, kTypeDebuggerCmdline = 3;
constexpr uint32_t kOrdLaunchDataPage = 164;

// a relaunch in progress (Relaunch on the guest thread, CancelRelaunch on the display thread)
std::timed_mutex g_relaunch;           // held while the new process is made
std::atomic<bool> g_cancelled{false};  // the window was closed: no relaunch, the new process ends
HANDLE g_child = nullptr;              // the new process (set under g_relaunch)
DWORD g_child_id = 0;
HANDLE g_close = nullptr;              // its <name>-close event
unsigned g_counter = 0;                // for the event names
bool g_started_relaunched = false;     // this process is a relaunch (FCI_LAUNCH_DATA at boot)

std::string Env(const char* name) {
    char buf[16384];
    DWORD n = GetEnvironmentVariableA(name, buf, sizeof buf);
    return n && n < sizeof buf ? std::string(buf, n) : std::string();
}

// the page at the top of the contiguous range, so the game's own allocations keep their addresses
uint32_t AllocPage() {
    uint32_t at = mem::kContiguousEnd - kPageSize;
    if (!mem::Contiguous().AllocAt(at, kPageSize, true)) at = mem::Contiguous().Alloc(kPageSize);
    if (at) std::memset(rt::Host(at), 0, kPageSize);
    return at;
}

std::string ToHex(const uint8_t* p, size_t n) {
    static const char* digits = "0123456789ABCDEF";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; i++) {
        s += digits[p[i] >> 4];
        s += digits[p[i] & 15];
    }
    return s;
}

std::vector<uint8_t> FromHex(const std::string& s) {
    std::vector<uint8_t> v;
    for (size_t i = 0; i + 1 < s.size(); i += 2) v.push_back(uint8_t(std::strtoul(s.substr(i, 2).c_str(), nullptr, 16)));
    return v;
}

// After the handover: while this window still is in front, the new one comes in front from here (a foreground
// process may hand the foreground to any window). The new process asks for it too (display::FinishHandover), but
// Windows lets a background process do that only within its foreground rules; this way the keyboard and the mouse
// surely go to the new window. Nothing happens when the player has switched to another program meanwhile.
void ForegroundToNewWindow(DWORD process) {
    DWORD front = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &front);
    if (front != GetCurrentProcessId()) return;
    struct Find {
        DWORD process;
        HWND window;
    } find{process, nullptr};
    EnumWindows(
        [](HWND w, LPARAM p) -> BOOL {
            auto* f = reinterpret_cast<Find*>(p);
            DWORD pid = 0;
            wchar_t cls[32] = {};
            GetWindowThreadProcessId(w, &pid);
            if (pid != f->process || !IsWindowVisible(w) || !GetClassNameW(w, cls, 32) || wcscmp(cls, L"FarCryInstincts"))
                return TRUE;
            f->window = w;
            return FALSE;
        },
        LPARAM(&find));
    if (!find.window) return;
    const bool done = SetForegroundWindow(find.window) != 0;
    rt::Log("kernel: the new window %s in front", done ? "brought" : "could not be brought");
}

}  // namespace

// FCI_START_LEVEL=<level>[:<mission>[:<players>]]: the parts
bool StartLevelSetting(std::string* level, std::string* mission, int* players) {
    static const std::string value = Env("FCI_START_LEVEL");
    if (value.empty() || !Env("FCI_LAUNCH_DATA").empty()) return false;  // a relaunch carries its own data
    size_t c1 = value.find(':');
    size_t c2 = c1 == std::string::npos ? std::string::npos : value.find(':', c1 + 1);
    if (level) *level = value.substr(0, c1);
    if (mission) *mission = c1 == std::string::npos ? std::string() : value.substr(c1 + 1, c2 - c1 - 1);
    if (players) *players = c2 == std::string::npos ? 0 : std::atoi(value.c_str() + c2 + 1);
    return true;
}

// called by Init after the data exports are set up (LaunchDataPage = 0 = cold boot)
void InitLaunchData() {
    std::string relaunch = Env("FCI_LAUNCH_DATA");
    std::string level, mission;
    int players = 0;
    if (!relaunch.empty()) {
        g_started_relaunched = true;
        std::vector<uint8_t> bytes = FromHex(relaunch);
        uint32_t page = AllocPage();
        if (!page) return;
        std::memcpy(rt::Host(page), bytes.data(), std::min<size_t>(bytes.size(), kPageSize));
        W32(DataAddress(kOrdLaunchDataPage), page);
        rt::Log("kernel: relaunched, launch data type %u, command line '%s'", R32(page),
                ReadCString(page + kDataOffset + (R32(page) == kTypeTitle ? 8 : 0)).c_str());
        return;
    }
    if (!StartLevelSetting(&level, &mission, &players)) return;
    if (level.empty() || level.find(' ') != std::string::npos) {
        rt::Log("kernel: FCI_START_LEVEL '%s' has no usable level name", level.c_str());
        return;
    }
    // The map editor templates (Levels\MET_*) are no level of their own: the start and respawn points come from
    // the user map built on them (map editor, ige.xbe). Alone the level start finds none (0x0004D600: list
    // [level+0x24] empty, 0x000D7BD0 fails, position 0,0,0), and 0x004768A0 reads the sector of a null pointer
    // (0x00476901, address 0xA1) - the same code on the console. The menus never offer them (the level list skips
    // "MET_" at 0x0004AA39/0x0004ABDF), so only this switch could get there: the game boots to its menus instead.
    if (_strnicmp(level.c_str(), "MET_", 4) == 0) {
        rt::Log("kernel: FCI_START_LEVEL '%s' is a map editor template (no start points without a user map, the "
                "game would crash at the level start): ignored, normal boot",
                level.c_str());
        return;
    }
    std::string cmdline = "-map " + level;
    if (players >= 2 && players <= 4) cmdline += " -lpc" + std::to_string(players);
    uint32_t page = AllocPage();
    if (!page) return;
    W32(page, kTypeDebuggerCmdline);
    std::memcpy(rt::Host(page + kDataOffset), cmdline.c_str(), std::min<size_t>(cmdline.size(), kDataSize - 1));
    W32(DataAddress(kOrdLaunchDataPage), page);
    rt::Log("kernel: FCI_START_LEVEL: launch data '%s' (mission %s)", cmdline.c_str(),
            mission.empty() ? "= level name, the game's default" : mission.c_str());
}

// HalReturnToFirmware(ReturnFirmwareQuickReboot) after XLaunchNewImage: true if this process was replaced by a
// new one that boots with the launch data (the caller exits)
bool Relaunch(uint32_t routine) {
    uint32_t page = R32(DataAddress(kOrdLaunchDataPage));
    if (routine != 2 || !page) return false;
    std::string path = ReadCString(page + 8);
    std::string lower = path;
    for (char& ch : lower) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    // XLaunchNewImage writes "<device>;<path>", here "\Device\CdRom0;default.xbe"
    const std::string own = "default.xbe", device = "\\device\\cdrom0";
    size_t at = lower.size() - std::min(lower.size(), own.size());
    bool self = lower.size() > own.size() && lower.compare(at, own.size(), own) == 0 &&
                (lower[at - 1] == ';' || lower[at - 1] == '\\') && lower.rfind(device, 0) == 0;
    rt::Log("kernel: launch of '%s', type %u, title %08X, command line '%s'", path.c_str(), R32(page), R32(page + 4),
            ReadCString(page + kDataOffset + (R32(page) == kTypeTitle ? 8 : 0)).c_str());
    if (!self) return false;
    if (GetEnvironmentVariableA("FCI_NO_RELAUNCH", nullptr, 0) > 0) return false;
    // what the settings menu changed in this process: the new one starts with it (its own log follows)
    const std::string changed = settings::ChangedSinceStart();
    rt::Log("kernel: settings changed since the start: %s", changed.empty() ? "none" : changed.c_str());

    // trailing zeros of the page need not travel
    const uint8_t* bytes = rt::Host(page);
    size_t n = kPageSize;
    while (n > 0 && !bytes[n - 1]) n--;
    SetEnvironmentVariableA("FCI_LAUNCH_DATA", ToHex(bytes, n).c_str());
    SetEnvironmentVariableA("FCI_RELAUNCHED", "1");  // the log goes on in the same fci.log
    // the window as the player left it (F11, dragged to another monitor, resized, maximized): the new process opens
    // its window the same way (GitHub #10); without it, the settings decide (also after the menu changed them)
    const std::string window = display::RelaunchWindowState();
    SetEnvironmentVariableA("FCI_WINDOW_STATE", window.empty() ? nullptr : window.c_str());
    rt::Log("kernel: window for the new process: %s", window.empty() ? "from the settings" : window.c_str());

    // Handover (as in SCDA-XBERecomp, MIT): this window stays on screen with its last picture until the new instance
    // has presented its first frames (it opens its window hidden and shows it then, video/display.cpp), so no gap
    // and no desktop show in between. Not with exclusive fullscreen: two swap chains cannot hold the output.
    // Not either when both instances would not fit the GPU memory Windows gives a process: the new one makes its
    // render targets (most of its memory: 2.2 of 2.5 GB at scale 6x, msaa 8, shadows 8x) before its first frame,
    // so during the handover both hold about this one's usage. Without it this process ends at once and its
    // memory is free before the new one has a device; the desktop shows for a moment.
    int budget = 0;
    const int used = gpu::present::VideoMemoryUsage(&budget);
    if (std::string test = Env("FCI_TEST_GPU_BUDGET"); !test.empty()) budget = std::atoi(test.c_str());  // test: a small GPU
    const bool fits = used < 0 || budget <= 0 || 2 * used <= budget * 9 / 10;
    const bool handover = !display::WantExclusive() && fits;

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = GetCommandLineW();
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    HANDLE ready = nullptr;
    {
        // the window was closed meanwhile (CancelRelaunch, display thread): no new process. Held while the process
        // is made, so a close in between waits for it and then tells it to end
        std::lock_guard lock(g_relaunch);
        if (g_cancelled) {
            rt::Log("kernel: the window was closed: no relaunch");
            return false;
        }
        // The events exist before the new process, which runs at once (never suspended: a process left suspended by
        // an end of this one in between stayed for good, without a window or a log, and held off the save repair of
        // the next start, GitHub #29). Named after this process and a counter; a name still held by an earlier
        // instance (its process id used again) is skipped.
        std::wstring name;
        for (int tries = 0; tries < 16 && !g_close; tries++) {
            name = L"Local\\FCIHandover-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++g_counter);
            g_close = CreateEventW(nullptr, TRUE, FALSE, HandoverCloseEvent(name).c_str());
            if (g_close && GetLastError() == ERROR_ALREADY_EXISTS) {
                CloseHandle(g_close), g_close = nullptr;
            } else if (g_close && handover) {
                ready = CreateEventW(nullptr, TRUE, FALSE, HandoverReadyEvent(name).c_str());
                if (!ready || GetLastError() == ERROR_ALREADY_EXISTS) {
                    if (ready) CloseHandle(ready), ready = nullptr;
                    CloseHandle(g_close), g_close = nullptr;
                }
            }
        }
        if (!g_close) rt::Log("kernel: no handover events (%lu): the new process starts as any other", GetLastError());
        SetEnvironmentVariableW(L"FCI_HANDOVER_EVENT", g_close ? name.c_str() : nullptr);
        if (!CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            rt::Log("kernel: relaunch failed (%lu)", GetLastError());
            if (ready) CloseHandle(ready);
            if (g_close) CloseHandle(g_close), g_close = nullptr;
            return false;
        }
        g_child = pi.hProcess;
        g_child_id = pi.dwProcessId;
    }
    CloseHandle(pi.hThread);
    AllowSetForegroundWindow(pi.dwProcessId);
    // FCI_TEST_CLOSE_RELAUNCH=<ms> (tests): in a relaunched process, the window gets WM_CLOSE <ms> after this relaunch
    // made its new process (the player closes the window during the level change; the first process of a chain
    // hands over normally)
    if (std::string t = Env("FCI_TEST_CLOSE_RELAUNCH"); !t.empty() && g_started_relaunched) {
        const int ms = std::atoi(t.c_str());
        std::thread([ms] {
            Sleep(DWORD(std::max(ms, 0)));
            rt::Log("kernel: FCI_TEST_CLOSE_RELAUNCH: window closed %d ms after the new process was made", ms);
            PostMessageW(static_cast<HWND>(display::Window()), WM_CLOSE, 0, 0);
        }).detach();
    }
    rt::Log("kernel: relaunched the title as process %lu%s (GPU memory of this process %d MB, budget %d MB)",
            pi.dwProcessId,
            ready ? ", keeping this window until its first frames"
                  : fits ? "" : ", no handover: two instances would not fit the GPU memory",
            used, budget);
    if (ready) {
        HANDLE wait[] = {ready, pi.hProcess};
        DWORD r = WaitForMultipleObjects(2, wait, FALSE, 15000);
        // the window was closed meanwhile: the new process ends because it was told to (CancelRelaunch)
        const bool closed = g_cancelled;
        rt::Log("kernel: handover %s", closed                   ? "ended: the window was closed"
                                       : r == WAIT_OBJECT_0     ? "done"
                                       : r == WAIT_OBJECT_0 + 1 ? "failed (new instance ended)"
                                                                : "timed out");
        CloseHandle(ready);
        if (r == WAIT_OBJECT_0 && !closed) ForegroundToNewWindow(pi.dwProcessId);
        if (r == WAIT_OBJECT_0 + 1 && !closed) {  // the next level's process ended before its first frames: say so
            DWORD code = 0;
            GetExitCodeProcess(pi.hProcess, &code);
            rt::Log("kernel: the relaunched process %lu ended with %08lX before its first frames", pi.dwProcessId, code);
            rt::FlushLog();
            wchar_t text[400];
            swprintf(text, 400,
                     L"The next level could not be started: the new game process ended with code %08lX.\n\n"
                     L"fci.log beside FarCryInstincts.exe says more (the log goes on after \"---- relaunch ----\").",
                     code);
            MessageBoxW(nullptr, text, L"Far Cry Instincts", MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
        }
    }
    // the process and its close event stay open (this process ends now): a close of the window until then still
    // reaches the new one
    return true;
}

void CancelRelaunch(const char* why) {
    g_cancelled = true;  // a relaunch not yet begun does not begin
    std::unique_lock lock(g_relaunch, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::milliseconds(2000))) {
        rt::Log("kernel: %s: the relaunch still makes its process after 2 s, ending anyway", why);
        return;
    }
    if (!g_child) return;
    rt::Log("kernel: %s during the relaunch: the new process %lu is told to end", why, g_child_id);
    if (g_close) SetEvent(g_close);
    // until it has ended: the event lives as long as this process, and a new process that opens it only now must
    // still find it set
    const uint64_t start = GetTickCount64();
    const DWORD r = WaitForSingleObject(g_child, 5000);
    rt::Log("kernel: the new process %s after %llu ms", r == WAIT_OBJECT_0 ? "ended" : "STILL RUNS",
            (unsigned long long)(GetTickCount64() - start));
}

}  // namespace kernel
