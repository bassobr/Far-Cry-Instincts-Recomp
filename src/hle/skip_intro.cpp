// skip_intro: the boot videos (Ubisoft, Crytek, the FCI intro) are not opened. The game's video player
// (0x00220200) checks BinkOpen's result and, when it is NULL, signals its loader event, switches the overlay off
// (D3DDevice_EnableOverlay(FALSE)) and returns as if the video had ended; the boot goes on to the title screen.
// Failing the file open instead is no good: the file system reports that as a disc error and the game shows its
// "dirty disc" screen forever.
// Only these three logo videos: the story's cutscenes (the opening on the boat in 00_Training, ~2 minutes) are the
// level's own scripted sequences in the engine, not videos, and the game has no skip for them (no pad button ends
// the opening; Start opens the pause menu, which offers no skip either).
// The setting is read at every video, so a change in the F10 menu holds from the next video on (the boot videos
// still to come, or the next boot after "Quit to main menu").
#include <cctype>
#include <cstring>
#include <string>

#include "app/settings.h"
#include "hle/original.h"
#include "gpu/fmv.h"  // PC features: fmv_scale
#include "runtime/guest.h"
#include "runtime/log.h"

namespace hle {

namespace {

constexpr uint32_t kBinkFileHandle = 0x00800000;  // BINKFILEHANDLE: the name argument is a file handle

bool SkipVideo(uint32_t name, uint32_t flags) {
    if (!name || (flags & kBinkFileHandle)) return false;
    const bool skip = settings::Bool("skip_intro", false);
    static bool logged = false;  // the state once in the log, with the first video (tester logs)
    if (!logged) {
        logged = true;
        rt::Log("video: skip_intro %s", skip ? "on" : "off");
    }
    if (!skip) return false;
    std::string path(reinterpret_cast<const char*>(rt::Host(name)));
    for (char& ch : path) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    // the file name, whichever folder or separator: Ubisoft.bik, Crytek.bik, Intro_FCI.bik (and variants of them,
    // e.g. a language suffix, should a release name them so)
    const std::string file = path.substr(path.find_last_of("\\/") + 1);
    if (file.size() < 4 || file.compare(file.size() - 4, 4, ".bik") != 0) return false;
    for (const char* video : {"ubisoft", "crytek", "intro_fci"})
        if (file.compare(0, std::strlen(video), video) == 0) return true;
    return false;
}

}  // namespace

// BinkOpen(const char* name, uint32_t flags), stdcall
void BinkOpen(rt::Cpu& c) {
    uint32_t name = rt::R32(c.esp + 4), flags = rt::R32(c.esp + 8);
    if (!SkipVideo(name, flags)) {
        std::string path(name && !(flags & kBinkFileHandle) ? reinterpret_cast<const char*>(rt::Host(name)) : "");
        GUEST_ORIG_AT(0x005A9A20u)(c);
        // PC features, fmv_scale: which video is open and its size (struct BINK: +0 width, +4 height)
        if (c.eax) gpu::fmv::Opened(path.c_str(), rt::R32(c.eax), rt::R32(c.eax + 4));
        return;
    }
    rt::Log("video: %s skipped (skip_intro)", reinterpret_cast<const char*>(rt::Host(name)));
    c.eax = 0;
    c.esp += 12;
}

}  // namespace hle
