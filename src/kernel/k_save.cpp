// Save games (GitHub #29): every profile checked at boot, a damaged one put back to its last good state, and no
// exit of the process in the middle of a save.
//
// A profile is the folder UDATA\55530008\<id>\ on partition 1 (data\hdd\partition1): SaveMeta.xbx (its name),
// profile.sav, renamer.ren, one <level>.mt and exactly two checkpoints *.fcs. All but SaveMeta.xbx end with a
// signature: HMAC-SHA1 over the rest of the file with the key XboxSignatureKey (XAPI's XCalculateSignature).
// The profile check of the game (0x0018EC20) loads every *.fcs and *.mt with its signature and wants exactly two
// checkpoints (0x0018EDB3); otherwise the profile "appears to be damaged and cannot be used" for good: the game
// only offers to delete it.
//
// The game saves in several steps, each one a separate file operation (fci.log: "file: delete", "file: wrote"):
//   checkpoint:  delete the older *.fcs, write the new one (open, set the end, 14 writes of 128 KB, close), rewrite
//                profile.sav in place;
//   level end:   the same for the level's start, then delete the old *.mt and write the next level's, then the
//                reboot (kernel::Relaunch); the new process saves the level start the same way;
//   new profile: SaveMeta.xbx, profile.sav, fcs0.fcs, fcs1.fcs, tmp.mt, renamer.ren.
// A process that ends between the steps (window closed, Alt+F4, a crash, killed, power lost) leaves one or three
// checkpoints or a file with a bad signature: the profile is gone. On the console the same holds (power off while
// saving), but there the save is the only thing running; here the player ends the program at any moment.
//
// So here:
//   - the host notes every change of a save file (k_file.cpp: NoteSaveChange, SaveWriterOpened/Closed);
//   - once a profile was quiet for a while and checks good, its files are copied to data\hdd\save_backup\ (the
//     game never sees that folder: it is not below a partition);
//   - before the process ends on its own (window closed, relaunch, quit), FinishSaves waits until the save in
//     progress is done and lets no new one start;
//   - at boot every profile is checked with the game's rules; a damaged one gets its last good state back, the
//     damaged files go to data\hdd\save_damaged\<id>-<time>\ (nothing is deleted). fci.log says what was found.
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "kernel/kernel.h"
#include "runtime/scheduler.h"

namespace kernel {

namespace {

namespace fs = std::filesystem;

constexpr uint32_t kOrdXboxSignatureKey = 325;
constexpr const wchar_t* kTitle = L"55530008";
constexpr uint32_t kBackupQuietMs = 1500;  // a profile unchanged this long is between two saves

fs::path g_hdd;
uint8_t g_key[16];
bool g_have_key = false;

std::mutex g_lock;
std::condition_variable g_changed;
int g_writers = 0;              // save files open for writing
uint64_t g_last_change = 0;     // GetTickCount64 of the last change
std::set<fs::path> g_dirty;     // profile folders changed since their last backup
bool g_ending = false;          // FinishSaves: no new save starts
uint32_t g_ending_quiet = 0;    // ... once none changed for this long
bool g_guard_started = false;

std::wstring Lower(std::wstring s) {
    for (auto& ch : s) ch = wchar_t(towlower(ch));
    return s;
}

fs::path UserData() { return g_hdd / "partition1" / "UDATA" / kTitle; }
fs::path BackupRoot() { return g_hdd / "save_backup" / "UDATA" / kTitle; }
fs::path DamagedRoot() { return g_hdd / "save_damaged" / "UDATA" / kTitle; }

bool ReadAll(const fs::path& p, std::vector<uint8_t>* out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !in.bad();
}

// the file types the game signs
bool Signed(const fs::path& p) {
    std::wstring ext = Lower(p.extension().wstring());
    return ext == L".fcs" || ext == L".sav" || ext == L".mt" || ext == L".ren";
}

struct Check {
    bool ok = false;           // the game would use the profile
    bool foreign_key = false;  // no file verifies with this build's key: not judged
    int checkpoints = 0, mts = 0, files = 0;
    std::string why;           // what is wrong
    std::string name;          // from SaveMeta.xbx
};

std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t ch : w) s += ch < 128 ? char(ch) : '?';
    return s;
}

// the profile's name: SaveMeta.xbx is UTF-16 "Name=<name>\r\n"
std::string ProfileName(const fs::path& dir) {
    std::vector<uint8_t> b;
    if (!ReadAll(dir / "SaveMeta.xbx", &b) || b.size() < 2) return "?";
    std::wstring w;
    for (size_t i = 0; i + 1 < b.size(); i += 2) w += wchar_t(b[i] | b[i + 1] << 8);
    size_t at = w.find(L"Name=");
    if (at == std::wstring::npos) return "?";
    w = w.substr(at + 5);
    w = w.substr(0, w.find_first_of(L"\r\n"));
    return Narrow(w);
}

// the profile check of the game, on the host
Check CheckProfile(const fs::path& dir) {
    Check r;
    r.name = ProfileName(dir);
    std::error_code ec;
    int good = 0, bad = 0;
    std::string bad_files;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec)) continue;
        r.files++;
        const fs::path& p = e.path();
        std::wstring ext = Lower(p.extension().wstring());
        if (ext == L".fcs") r.checkpoints++;
        if (ext == L".mt") r.mts++;
        if (!Signed(p)) continue;
        std::vector<uint8_t> d;
        bool valid = false;
        if (ReadAll(p, &d) && d.size() > 20 && g_have_key) {
            uint8_t digest[20];
            HmacSha1(g_key, sizeof g_key, d.data(), d.size() - 20, digest);
            valid = std::memcmp(digest, d.data() + d.size() - 20, 20) == 0;
        }
        if (valid) {
            good++;
        } else {
            bad++;
            bad_files += (bad_files.empty() ? "" : ", ") + Narrow(p.filename().wstring()) + " (" + std::to_string(d.size()) +
                         " bytes)";
        }
    }
    if (!good && bad) {
        r.foreign_key = true;
        r.why = "no file carries this build's signature";
        return r;
    }
    // the game's verdicts on crafted profiles (tested for GitHub #29): damaged with a bad
    // signature in any of these files, with 0, 1 or 3 checkpoints, without or with two *.mt, without profile.sav or
    // renamer.ren; a file of another kind does not matter
    auto add = [&](const std::string& s) { r.why += (r.why.empty() ? "" : "; ") + s; };
    if (bad) add("bad signature: " + bad_files);
    if (r.checkpoints != 2) add(std::to_string(r.checkpoints) + " checkpoints (*.fcs), 2 expected");
    if (r.mts != 1) add(std::to_string(r.mts) + " *.mt, 1 expected");
    if (!fs::exists(dir / "profile.sav", ec)) add("no profile.sav");
    if (!fs::exists(dir / "renamer.ren", ec)) add("no renamer.ren");
    r.ok = r.why.empty();
    return r;
}

std::string Describe(const Check& c) {
    return "'" + c.name + "', " + std::to_string(c.files) + " files, " + std::to_string(c.checkpoints) + " checkpoints";
}

// copies a folder's files (flat) and flushes them; false on any error
bool CopyFolder(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_directories(to, ec);
    for (const auto& e : fs::directory_iterator(from, ec)) {
        if (!e.is_regular_file(ec)) continue;
        std::vector<uint8_t> d;
        if (!ReadAll(e.path(), &d)) return false;
        HANDLE h = CreateFileW((to / e.path().filename()).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD n = 0;
        BOOL ok = d.empty() || WriteFile(h, d.data(), DWORD(d.size()), &n, nullptr);
        // the file's times go along: the game takes the newest checkpoint (by its time, k_file.cpp AccessTime) as
        // the current one, and a copy in name order made the wrong one the newest (the profile then counts as
        // damaged)
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (ok && GetFileAttributesExW(e.path().c_str(), GetFileExInfoStandard, &a))
            ok = SetFileTime(h, &a.ftCreationTime, &a.ftLastWriteTime, &a.ftLastWriteTime);
        ok = ok && n == d.size() && FlushFileBuffers(h);
        CloseHandle(h);
        if (!ok) return false;
    }
    return !ec;
}

// the same files in both folders: names, sizes and the last 20 bytes (the signature, a hash of the rest)
bool SameFiles(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    if (!fs::exists(b, ec)) return false;
    auto list = [](const fs::path& dir) {
        std::vector<std::pair<std::wstring, std::vector<uint8_t>>> r;
        std::error_code e;
        for (const auto& f : fs::directory_iterator(dir, e)) {
            if (!f.is_regular_file(e)) continue;
            std::vector<uint8_t> tail;
            std::ifstream in(f.path(), std::ios::binary);
            uint64_t size = f.file_size(e);
            if (size >= 20) {
                tail.resize(20);
                in.seekg(std::streamoff(size - 20));
                in.read(reinterpret_cast<char*>(tail.data()), 20);
            }
            tail.insert(tail.end(), reinterpret_cast<uint8_t*>(&size), reinterpret_cast<uint8_t*>(&size) + 8);
            r.emplace_back(Lower(f.path().filename().wstring()), tail);
        }
        std::sort(r.begin(), r.end());
        return r;
    };
    return list(a) == list(b);
}

// the profile's files as its last good state: written beside the old backup, checked, then swapped in
void Backup(const fs::path& profile) {
    std::error_code ec;
    const fs::path id = profile.filename();
    const fs::path dst = BackupRoot() / id, tmp = BackupRoot() / (id.wstring() + L".new"),
                   old = BackupRoot() / (id.wstring() + L".old");
    if (!fs::exists(profile, ec)) {  // the player deleted the profile
        if (fs::exists(dst, ec)) {
            fs::remove_all(dst, ec);
            rt::Log("save: profile %s deleted, its backup too", Narrow(id.wstring()).c_str());
        }
        return;
    }
    const Check c = CheckProfile(profile);
    if (!c.ok) {
        // between two steps of a save or broken: the last good backup stays
        rt::Log("save: profile %s %s not backed up (%s)", Narrow(id.wstring()).c_str(), Describe(c).c_str(), c.why.c_str());
        return;
    }
    if (SameFiles(profile, dst)) return;  // the backup holds this state already
    fs::remove_all(tmp, ec);
    if (!CopyFolder(profile, tmp) || !CheckProfile(tmp).ok) {  // changed while copying: next time
        fs::remove_all(tmp, ec);
        rt::Log("save: backup of profile %s failed or changed meanwhile, kept the previous one", Narrow(id.wstring()).c_str());
        return;
    }
    fs::remove_all(old, ec);
    if (fs::exists(dst, ec)) fs::rename(dst, old, ec);
    fs::rename(tmp, dst, ec);
    if (ec) {
        rt::Log("save: backup of profile %s could not be put in place (%s)", Narrow(id.wstring()).c_str(), ec.message().c_str());
        return;
    }
    fs::remove_all(old, ec);
    rt::Log("save: profile %s %s good, backed up", Narrow(id.wstring()).c_str(), Describe(c).c_str());
}

// host thread: backs up each changed profile once it is quiet
void Guard() {
    for (;;) {
        std::vector<fs::path> todo;
        {
            std::unique_lock lock(g_lock);
            g_changed.wait(lock, [] { return !g_dirty.empty(); });
            for (;;) {
                uint64_t quiet = GetTickCount64() - g_last_change;
                if (g_writers == 0 && quiet >= kBackupQuietMs) break;
                g_changed.wait_for(lock, std::chrono::milliseconds(kBackupQuietMs - std::min<uint64_t>(quiet, kBackupQuietMs) + 50));
            }
            todo.assign(g_dirty.begin(), g_dirty.end());
            g_dirty.clear();
        }
        for (const fs::path& p : todo) Backup(p);
    }
}

// another instance on the same hard disk folder that may be saving right now (not the one this relaunch replaces:
// it saved before it started this one)
bool OtherInstanceRunning() {
    wchar_t own[MAX_PATH];
    GetModuleFileNameW(nullptr, own, MAX_PATH);
    const DWORD self = GetCurrentProcessId();
    DWORD parent = 0;
    bool other = false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{sizeof pe};
    for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe))
        if (pe.th32ProcessID == self) parent = pe.th32ParentProcessID;
    for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe)) {
        if (pe.th32ProcessID == self || _wcsicmp(pe.szExeFile, fs::path(own).filename().c_str())) continue;
        if (pe.th32ProcessID == parent && GetEnvironmentVariableA("FCI_RELAUNCHED", nullptr, 0) > 0) continue;
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!p) continue;
        wchar_t path[MAX_PATH];
        DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(p, 0, path, &n) && _wcsicmp(path, own) == 0) {
            // one that never ran cannot save: still its first thread only, no time in its own code, for more than
            // 30 s (a start has its loader threads within milliseconds). Such a process was left suspended by builds
            // before 0.1.9 when the previous one ended in the middle of a level change
            FILETIME created, exited, kernel_time, user_time;
            ULARGE_INTEGER c{}, u{}, now{};
            FILETIME ft;
            GetSystemTimeAsFileTime(&ft);
            now.LowPart = ft.dwLowDateTime, now.HighPart = ft.dwHighDateTime;
            const bool times = GetProcessTimes(p, &created, &exited, &kernel_time, &user_time) != 0;
            if (times) {
                c.LowPart = created.dwLowDateTime, c.HighPart = created.dwHighDateTime;
                u.LowPart = user_time.dwLowDateTime, u.HighPart = user_time.dwHighDateTime;
            }
            const uint64_t age_s = times && now.QuadPart > c.QuadPart ? (now.QuadPart - c.QuadPart) / 10000000 : 0;
            if (times && pe.cntThreads == 1 && u.QuadPart == 0 && age_s > 30)
                rt::Log("save: process %lu of this executable never ran (one thread, no time, %llu s old): not counted",
                        pe.th32ProcessID, (unsigned long long)age_s);
            else
                other = true;
        }
        CloseHandle(p);
    }
    CloseHandle(snap);
    return other;
}

std::wstring Now() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t s[32];
    swprintf(s, 32, L"%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return s;
}

// a damaged profile: its files to save_damaged, the backup's in their place
bool Restore(const fs::path& profile, const fs::path& backup) {
    std::error_code ec;
    const fs::path keep = DamagedRoot() / (profile.filename().wstring() + L"-" + Now());
    fs::create_directories(keep, ec);
    for (const auto& e : fs::directory_iterator(profile, ec)) {
        if (!e.is_regular_file(ec)) continue;
        std::error_code mv;
        fs::rename(e.path(), keep / e.path().filename(), mv);
        if (mv) {
            rt::Log("save: could not move %s aside (%s)", Narrow(e.path().wstring()).c_str(), mv.message().c_str());
            return false;
        }
    }
    rt::Log("save: the damaged files are kept in %s", Narrow(keep.wstring()).c_str());
    return CopyFolder(backup, profile) && CheckProfile(profile).ok;
}

// Tests of an end in the middle of a save, at the n-th change of a save file of this process (counted from 1):
//   FCI_TEST_KILL_SAVE=<n>   the process is killed right there (as by a crash, the task manager, power loss);
//   FCI_TEST_CLOSE_SAVE=<n>  the window gets WM_CLOSE right there (the player closes it while the game saves).
// With "<n>+" the n-th change of the first process; plain <n> counts in every process (a relaunch starts at 1).
void TestEnd(const fs::path& host) {
    static const auto setting = [](const char* name) {
        char v[32] = {};
        if (!GetEnvironmentVariableA(name, v, sizeof v)) return 0;
        if (std::strchr(v, '+') && GetEnvironmentVariableA("FCI_RELAUNCHED", nullptr, 0) > 0) return 0;
        return std::atoi(v);
    };
    static const int kill = setting("FCI_TEST_KILL_SAVE"), close = setting("FCI_TEST_CLOSE_SAVE");
    static int count = 0;
    if (!kill && !close) return;
    count++;
    if (count == kill) {
        rt::Log("save: FCI_TEST_KILL_SAVE: killed at change %d (%s)", count, Narrow(host.wstring()).c_str());
        rt::FlushLog();
        TerminateProcess(GetCurrentProcess(), 9);
    }
    if (count == close) {
        rt::Log("save: FCI_TEST_CLOSE_SAVE: window closed at change %d (%s)", count, Narrow(host.wstring()).c_str());
        EnumWindows(
            [](HWND w, LPARAM) -> BOOL {
                DWORD pid = 0;
                wchar_t cls[32] = {};
                GetWindowThreadProcessId(w, &pid);
                if (pid != GetCurrentProcessId() || !GetClassNameW(w, cls, 32) || wcscmp(cls, L"FarCryInstincts")) return TRUE;
                PostMessageW(w, WM_CLOSE, 0, 0);
                return FALSE;
            },
            0);
    }
}

}  // namespace

// FCI_SAVE_GUARD=0: none of this (tests, and a way out should it ever stand in the way): profiles are only checked
bool SaveGuardOff() {
    static const bool off = [] {
        char v[8] = {};
        return GetEnvironmentVariableA("FCI_SAVE_GUARD", v, sizeof v) > 0 && v[0] == '0';
    }();
    return off;
}

void CheckSaves() {
    g_hdd = HddFolder();  // set by SetDevicePaths
    std::memcpy(g_key, rt::Host(DataAddress(kOrdXboxSignatureKey)), sizeof g_key);
    g_have_key = true;
    std::error_code ec;
    // a backup cut while it was swapped in: the previous one is still complete
    for (const auto& e : fs::directory_iterator(BackupRoot(), ec)) {
        std::wstring n = e.path().filename().wstring();
        std::error_code ec2;
        if (n.size() > 4 && n.compare(n.size() - 4, 4, L".old") == 0) {
            fs::path dst = BackupRoot() / n.substr(0, n.size() - 4);
            if (!fs::exists(dst, ec2)) fs::rename(e.path(), dst, ec2);
        }
    }
    const bool other = OtherInstanceRunning();
    int profiles = 0;
    for (const auto& e : fs::directory_iterator(UserData(), ec)) {
        if (!e.is_directory(ec)) continue;
        profiles++;
        const fs::path& dir = e.path();
        const std::string id = Narrow(dir.filename().wstring());
        const Check c = CheckProfile(dir);
        const fs::path backup = BackupRoot() / dir.filename();
        if (c.foreign_key) {
            rt::Log("save: profile %s %s: %s, not checked", id.c_str(), Describe(c).c_str(), c.why.c_str());
            continue;
        }
        if (c.ok) {
            rt::Log("save: profile %s %s good", id.c_str(), Describe(c).c_str());
            // the good state as the backup (the first run with this check; or the last process ended within the
            // quiet time after a save, before its backup)
            std::lock_guard lock(g_lock);
            g_dirty.insert(dir);
            continue;
        }
        rt::Log("save: profile %s %s is DAMAGED: %s", id.c_str(), Describe(c).c_str(), c.why.c_str());
        if (SaveGuardOff()) continue;
        if (other) {
            rt::Log("save: another FarCryInstincts.exe runs on these saves, profile %s left as it is", id.c_str());
            continue;
        }
        const Check b = fs::exists(backup, ec) ? CheckProfile(backup) : Check{};
        if (!b.ok) {
            rt::Log("save: no good backup of profile %s%s; the game will call it damaged", id.c_str(),
                    fs::exists(backup, ec) ? (" (" + b.why + ")").c_str() : "");
            continue;
        }
        FILETIME ft{};
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (GetFileAttributesExW((backup / "profile.sav").c_str(), GetFileExInfoStandard, &a)) ft = a.ftLastWriteTime;
        SYSTEMTIME st{}, lt{};
        FileTimeToSystemTime(&ft, &st);
        SystemTimeToTzSpecificLocalTime(nullptr, &st, &lt);
        const bool done = Restore(dir, backup);
        rt::Log("save: profile %s %s from its last good state (%s, saved %04u-%02u-%02u %02u:%02u:%02u)", id.c_str(),
                done ? "RESTORED" : "could NOT be restored", Describe(b).c_str(), lt.wYear, lt.wMonth, lt.wDay, lt.wHour,
                lt.wMinute, lt.wSecond);
    }
    if (!profiles) rt::Log("save: no saved profile yet");
    if (SaveGuardOff()) return rt::Log("save: FCI_SAVE_GUARD=0: no backups, no repair, no wait for a save at the end");
    std::lock_guard lock(g_lock);
    if (!g_guard_started) {
        g_guard_started = true;
        std::thread(Guard).detach();
        g_changed.notify_all();
    }
}

void NoteSaveChange(const fs::path& host) {
    std::unique_lock lock(g_lock);
    if (!SaveGuardOff() && g_ending && g_writers == 0 && GetTickCount64() - g_last_change >= g_ending_quiet) {
        // the process ends and the last save is done: a new one would be cut, so it never starts (a change right
        // after the last one is the next step of the same save and goes on)
        lock.unlock();
        rt::Log("save: %s not changed, the program is ending", Narrow(host.wstring()).c_str());
        rt::CpuUnlock unlock;
        Sleep(INFINITE);
    }
    g_last_change = GetTickCount64();
    TestEnd(host);
    // the profile folder: UDATA\55530008\<id>\<file> (the host path is the device folder and the guest's path)
    const std::wstring root = Lower(UserData().wstring()) + L"\\", path = Lower(host.wstring());
    if (path.size() > root.size() && path.compare(0, root.size(), root) == 0) {
        size_t end = path.find(L'\\', root.size());
        if (end != std::wstring::npos) g_dirty.insert(host.wstring().substr(0, end));
    }
    g_changed.notify_all();
}

void SaveWriterOpened() {
    std::lock_guard lock(g_lock);
    g_writers++;
    g_last_change = GetTickCount64();
}

void SaveWriterClosed() {
    std::lock_guard lock(g_lock);
    g_writers--;
    g_last_change = GetTickCount64();
    g_changed.notify_all();
}

void FinishSaves(const char* why, uint32_t max_ms, uint32_t quiet_ms) {
    if (SaveGuardOff()) return;
    rt::CpuUnlock unlock;  // a guest thread that saves may need the CPU to finish
    const uint64_t start = GetTickCount64();
    std::unique_lock lock(g_lock);
    g_ending = true;
    g_ending_quiet = quiet_ms;
    bool waited = false;
    for (;;) {
        const uint64_t now = GetTickCount64();
        if (g_writers == 0 && now - g_last_change >= quiet_ms) break;
        if (now - start >= max_ms) {
            rt::Log("save: %s: a save still runs after %u ms (%d files open), ending anyway", why, max_ms, g_writers);
            return;
        }
        waited = true;
        g_changed.wait_for(lock, std::chrono::milliseconds(20));
    }
    if (waited) rt::Log("save: %s: waited %llu ms for the save to finish", why, (unsigned long long)(GetTickCount64() - start));
}

}  // namespace kernel
