// skip_intro: the boot videos (Ubisoft, Crytek, the FCI intro) are not opened. The game's video player
// (0x00220200) checks BinkOpen's result and, when it is NULL, signals its loader event, switches the overlay off
// (D3DDevice_EnableOverlay(FALSE)) and returns as if the video had ended; the boot goes on to the title screen.
// Failing the file open instead is no good: the file system reports that as a disc error and the game shows its
// "dirty disc" screen forever.
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

bool SkipVideo(uint32_t name) {
    static const bool skip = settings::Bool("skip_intro", false);
    if (!skip || !name) return false;
    std::string path(reinterpret_cast<const char*>(rt::Host(name)));
    for (char& ch : path) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    for (const char* video : {"videos\\ubisoft.bik", "videos\\crytek.bik", "videos\\intro_fci.bik"}) {
        size_t n = std::strlen(video);
        if (path.size() >= n && path.compare(path.size() - n, n, video) == 0) return true;
    }
    return false;
}

}  // namespace

// BinkOpen(const char* name, uint32_t flags), stdcall
void BinkOpen(rt::Cpu& c) {
    uint32_t name = rt::R32(c.esp + 4);
    if (!SkipVideo(name)) {
        std::string path(name ? reinterpret_cast<const char*>(rt::Host(name)) : "");
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
