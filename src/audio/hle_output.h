// Adapted from SCDA-XBERecomp (MIT, Copyright (c) 2026 TGP482, Joshhhuaaa): src/audio/output.h.
// The host output of the DirectSound HLE (src/xdk/dsound.cpp): a 5.1 float mix at 48 kHz through XAudio2.
#pragma once

#include <cstdint>
#include <functional>

namespace audio {

constexpr uint32_t kRate = 48000;
constexpr uint32_t kChannels = 6;

// channel order of the frames the mixer renders
enum Speaker : uint32_t {
    FrontLeft,
    FrontRight,
    Centre,
    Sub,
    BackLeft,
    BackRight,
};

// Starts the platform output, which pulls interleaved float frames of kChannels from render.
// Returns once the device is open and IsSurround is known. Without a device a host clock pulls the mix at
// the same pace, so streams still complete. FCI_RECORD_AUDIO=1 writes the stereo fold to audio.raw.
void StartOutput(std::function<void(float* out, uint32_t frames)> render);

// the mix is 5.1: by FCI_AUDIO_OUTPUT (auto, stereo, surround, mono), automatic when the default device has 5.1
bool IsSurround();
// the output setting is mono: the stereo mix plays as one channel on the front pair
bool IsMono();

// ---- PC comfort features: pause on focus loss stops the output and the mix (no render calls) ----
void SetPaused(bool paused);

}  // namespace audio
