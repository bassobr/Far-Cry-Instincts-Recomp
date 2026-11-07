// Bink videos behind the menus advance with the video's own clock at any frame rate (GitHub #13), and the
// FCI_TRACE_BINK diagnostics (frames advanced per second of every open video). See video_pace.cpp.
#pragma once

#include <cstdint>

namespace hle {

// BinkOpen (skip_intro.cpp) opened a video: the BINK in guest memory, the name the game passed, the return
// address into the game's player (which player opened it)
void VideoOpened(uint32_t bink, const char* name, uint32_t caller);

}  // namespace hle
