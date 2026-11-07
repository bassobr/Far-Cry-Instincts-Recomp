// FarCryInstincts.exe: loads default.xbe from the game files (extracted or a disc image) and runs its recompiled code.
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <vector>

#include "app/game_code.h"
#include "audio/apu.h"
#include "gpu/nv2a.h"
#include "kernel/disc.h"
#include "kernel/kernel.h"
#include "runtime/log.h"
#include "runtime/memory.h"
#include "runtime/profiler.h"
#include "runtime/runtime.h"
#include "runtime/scheduler.h"
#include "runtime/thread.h"
#include "video/display.h"
#include "xbe/xbe.h"

namespace fs = std::filesystem;

namespace {

fs::path ExeDir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return fs::path(path).parent_path();
}

// The game: its files extracted into assets\ (default.xbe with the disc's folders), or its disc image (.iso: a full
// disc dump or an xiso) in assets\ or right in the folder. Searched beside the executable and in the folders above
// it (builds inside the source tree); extracted files anywhere win over an image. FCI_ISO=<file> picks an image; a
// relaunch (kernel/k_launch.cpp) inherits it for the image this process mounted.
constexpr uint32_t kTitleId = 0x55530008;  // Far Cry Instincts (US-008)

struct Game {
    fs::path dvd;  // the folder behind D:, or the image's folder
    std::unique_ptr<kernel::Disc> disc;
    std::vector<uint8_t> xbe;  // default.xbe from the image
};

std::vector<fs::path> SearchDirs(const fs::path& exe_dir) {
    std::vector<fs::path> dirs;
    fs::path dir = exe_dir;
    for (int i = 0; i < 5; i++) {
        dirs.push_back(dir);
        if (!dir.has_parent_path() || dir.parent_path() == dir) break;
        dir = dir.parent_path();
    }
    return dirs;
}

std::vector<fs::path> Images(const fs::path& dir) {
    std::vector<fs::path> isos;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        std::wstring ext = e.path().extension().wstring();
        for (wchar_t& ch : ext) ch = wchar_t(std::towlower(ch));
        if (ext == L".iso" && e.is_regular_file(ec)) isos.push_back(e.path());
    }
    std::sort(isos.begin(), isos.end());
    return isos;
}

// opens an image and reads its default.xbe; logs why not
bool OpenImage(const fs::path& iso, Game* game) {
    auto disc = std::make_unique<kernel::Disc>();
    std::string error;
    if (!disc->Open(iso, error)) {
        rt::Log("game: %s: %s", iso.string().c_str(), error.c_str());
        return false;
    }
    std::vector<uint8_t> xbe;
    if (!disc->Load("default.xbe", xbe)) {
        rt::Log("game: %s has no default.xbe", iso.string().c_str());
        return false;
    }
    game->dvd = iso.parent_path();
    game->disc = std::move(disc);
    game->xbe = std::move(xbe);
    return true;
}

uint32_t XbeTitleId(const std::vector<uint8_t>& xbe) {
    auto get = [&](size_t at) {
        uint32_t v = 0;
        if (at + 4 <= xbe.size()) std::memcpy(&v, xbe.data() + at, 4);
        return v;
    };
    // the certificate's address (+0x118) is a virtual address in the headers, which map at the base (+0x104)
    return get(size_t(get(0x118) - get(0x104)) + 8);
}

bool FindGame(const fs::path& exe_dir, Game* game) {
    wchar_t chosen[MAX_PATH * 2] = {};
    if (GetEnvironmentVariableW(L"FCI_ISO", chosen, MAX_PATH * 2) > 0) {
        if (OpenImage(chosen, game)) return true;
        rt::Log("game: FCI_ISO=%s cannot be used, searching the assets folders", fs::path(chosen).string().c_str());
    }
    std::vector<fs::path> dirs = SearchDirs(exe_dir);
    std::error_code ec;
    for (const fs::path& dir : dirs)
        if (fs::exists(dir / "assets" / "default.xbe", ec)) {
            game->dvd = dir / "assets";
            return true;
        }
    // images: this title's first, another title's only when there is nothing else
    std::vector<fs::path> isos;
    for (const fs::path& dir : dirs) {
        for (const fs::path& iso : Images(dir / "assets")) isos.push_back(iso);
        for (const fs::path& iso : Images(dir)) isos.push_back(iso);
    }
    Game other;
    for (const fs::path& iso : isos) {
        Game g;
        if (!OpenImage(iso, &g)) continue;
        if (XbeTitleId(g.xbe) == kTitleId) {
            *game = std::move(g);
            return true;
        }
        rt::Log("game: %s holds title %08X, not Far Cry Instincts", iso.string().c_str(), XbeTitleId(g.xbe));
        if (!other.disc) other = std::move(g);
    }
    if (!other.disc) return false;
    *game = std::move(other);
    return true;
}

LONG WINAPI OnCrash(EXCEPTION_POINTERS* info) {
    auto* rec = info->ExceptionRecord;
    rt::Log("host exception %08lX at %p", rec->ExceptionCode, rec->ExceptionAddress);
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2)
        rt::Log("  %s of %016llX", rec->ExceptionInformation[0] ? "write" : "read",
                (unsigned long long)rec->ExceptionInformation[1]);
    rt::PrintStack(info->ContextRecord);
    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

int main() {
    rt::InitLog();
    SetUnhandledExceptionFilter(OnCrash);
    // FCI_TEST_RELAUNCH_FAIL=1: a relaunched process ends at once (test of the level change's error message)
    if (GetEnvironmentVariableA("FCI_TEST_RELAUNCH_FAIL", nullptr, 0) &&
        GetEnvironmentVariableA("FCI_RELAUNCHED", nullptr, 0)) {
        rt::Log("FCI_TEST_RELAUNCH_FAIL: the relaunched process ends");
        rt::FlushLog();
        return 3;
    }

    fs::path exe_dir = ExeDir();
    Game game;
    if (!FindGame(exe_dir, &game)) {
        rt::Log("game not found: neither assets\\default.xbe nor a disc image (.iso) beside the executable or above it");
        MessageBoxW(nullptr,
                    L"The game was not found.\n\nPut its disc image (.iso) or the disc's extracted files (default.xbe "
                    L"with the folders beside it) into an \"assets\" folder beside FarCryInstincts.exe (or in a folder "
                    L"above it).",
                    L"Far Cry Instincts", MB_ICONERROR | MB_OK);
        return 1;
    }

    xbe::Image image;
    std::string error;
    bool loaded;
    if (game.disc) {
        rt::Log("game files: disc image %s (%zu files)", game.disc->Path().string().c_str(), game.disc->FileCount());
        // a relaunch of the title (kernel/k_launch.cpp) mounts the same image
        SetEnvironmentVariableW(L"FCI_ISO", game.disc->Path().c_str());
        loaded = image.Load(std::move(game.xbe), game.disc->Path().string() + "\\default.xbe", &error);
    } else {
        rt::Log("game files: %s", game.dvd.string().c_str());
        loaded = image.Load((game.dvd / "default.xbe").string(), &error);
    }
    if (!loaded) {
        rt::Log("%s", error.c_str());
        return 1;
    }
    rt::Log("%s (%08X), entry %08X", image.title.c_str(), image.title_id, image.entry);
    if (image.title_id != kTitleId)
        rt::Log("game: title %08X is not the Far Cry Instincts this build was made from (%08X)", image.title_id, kTitleId);

    // the guest window first, so nothing loaded from here on can land in it
    if (!mem::Init()) return 1;
    // the recompiled code: game_code\game_code.dll, built from this xbe first if it is missing or stale
    if (!app::PrepareGameCode(exe_dir, image)) return 1;
    const rt::GameCode& code = *rt::LoadedGameCode();

    mem::LoadImage(image);
    rt::InitDispatch(code, image.base, uint32_t(image.memory.size()));

    kernel::MountDisc(std::move(game.disc));
    kernel::SetDevicePaths(game.dvd, exe_dir / "data" / "hdd");
    kernel::Init(image);
    rt::StartScheduler();
    rt::StartProfiler();
    kernel::InitTimers();
    kernel::StartTickCount();
    display::Init();
    gpu::Init();
    apu::Init();

    // instrumented game code (cmake -DFCI_PGO=generate): the game never exits, so FCI_PGO_WRITE=<seconds> writes the
    // counters of the recompiled code then (to LLVM_PROFILE_FILE, default default.profraw next to the exe)
    char pgo_write[16] = {};
    if (code.write_profile && GetEnvironmentVariableA("FCI_PGO_WRITE", pgo_write, sizeof pgo_write)) {
        const DWORD ms = DWORD(std::atof(pgo_write) * 1000);
        CreateThread(nullptr, 0, [](void* p) -> DWORD {
            Sleep(DWORD(uintptr_t(p)));
            const int r = rt::LoadedGameCode()->write_profile();
            rt::Log("pgo: profile %s", r == 0 ? "written" : "NOT written");
            return 0;
        }, reinterpret_cast<void*>(uintptr_t(ms)), 0, nullptr);
    }

    // the kernel runs the entry point on a fresh system thread
    rt::GuestThread* boot = rt::CreateGuestThread(1024 * 1024, 0, image.entry, 0, 0, false);
    WaitForSingleObject(boot->host, INFINITE);
    rt::Log("boot thread finished");

    // FCI_DUMP_AFTER=<seconds> logs every guest thread's stack, to see where a hang sits
    char dump_after[16] = {};
    if (GetEnvironmentVariableA("FCI_DUMP_AFTER", dump_after, sizeof dump_after)) {
        Sleep(DWORD(std::atoi(dump_after)) * 1000);
        rt::DumpThreads();
        rt::DumpHostStacks();
    }
    Sleep(INFINITE);
    return 0;
}
