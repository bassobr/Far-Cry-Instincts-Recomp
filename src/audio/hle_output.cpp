// Adapted from SCDA-XBERecomp (MIT, Copyright (c) 2026 TGP482, Joshhhuaaa): src/audio/xaudio2.cpp.
// Mixer output through XAudio2. FCI additions: FCI_AUDIO_OUTPUT picks the mode (no settings menu here), a
// limiter after the fold keeps the stereo sum out of clipping, FCI_RECORD_AUDIO=1 records what the stereo fold
// plays (audio.raw, 48 kHz stereo float, like the LLE path), the level is logged every 5 s, and without a
// device a host clock keeps pulling the mix so the title's streams still complete.
#include "audio/hle_output.h"

#include <windows.h>
#include <mmreg.h>
#include <xaudio2.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <future>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "runtime/log.h"

namespace audio {

namespace {

// the console's stereo downmix in the DOLBY image: centre and back pair at -3 dB, no sub
constexpr float kCentreFold = 0.707f;
constexpr float kBackFold = 0.707f;
constexpr DWORD kFront = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY;
constexpr DWORD kBack = SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
constexpr DWORD kSide = SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;
// the speaker of each source channel, in Speaker order
constexpr DWORD kSourceSpeaker[kChannels] = {SPEAKER_FRONT_LEFT, SPEAKER_FRONT_RIGHT, SPEAKER_FRONT_CENTER,
                                             SPEAKER_LOW_FREQUENCY, SPEAKER_BACK_LEFT, SPEAKER_BACK_RIGHT};
constexpr float kLeft[kChannels] = {1, 0, kCentreFold, 0, kBackFold, 0};
constexpr float kRight[kChannels] = {0, 1, kCentreFold, 0, 0, kBackFold};

constexpr uint32_t kOutputAuto = 0;
constexpr uint32_t kOutputStereo = 1;
constexpr uint32_t kOutputSurround = 2;
constexpr uint32_t kOutputMono = 3;

std::atomic<bool> g_surround{false};
std::atomic<bool> g_mono{false};
std::atomic<bool> g_paused{false};  // PC comfort features: focus pause

uint32_t Mode() {
    char v[32] = {};
    GetEnvironmentVariableA("FCI_AUDIO_OUTPUT", v, sizeof v);
    std::string s = v;
    if (s == "stereo") return kOutputStereo;
    if (s == "surround" || s == "5.1") return kOutputSurround;
    if (s == "mono") return kOutputMono;
    return kOutputAuto;
}

struct Output {
    IXAudio2* xa = nullptr;
    IXAudio2MasteringVoice* master = nullptr;
    IXAudio2SourceVoice* voice = nullptr;
};

// the master's speakers have a centre, a sub and a back or side pair
bool HasSurround(DWORD mask) {
    bool back = (mask & kBack) == kBack;
    bool side = (mask & kSide) == kSide;
    return (mask & kFront) == kFront && (back || side);
}

bool WantSurround(uint32_t mode, DWORD mask) {
    if (mode == kOutputAuto) return HasSurround(mask);
    return mode == kOutputSurround;
}

WAVEFORMATEXTENSIBLE MixFormat() {
    // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT: ksmedia.h needs ks.h, which breaks GUID_NULL
    static const GUID kFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
    WAVEFORMATEXTENSIBLE wf{};
    wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wf.Format.nChannels = kChannels;
    wf.Format.nSamplesPerSec = kRate;
    wf.Format.wBitsPerSample = 32;
    wf.Format.nBlockAlign = kChannels * sizeof(float);
    wf.Format.nAvgBytesPerSec = kRate * kChannels * sizeof(float);
    wf.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wf.Samples.wValidBitsPerSample = 32;
    wf.dwChannelMask = kFront | kBack;
    wf.SubFormat = kFloat;
    return wf;
}

// output index of a speaker in a channel mask, outputs follow the mask's bit order
int OutputOf(DWORD mask, DWORD speaker) {
    if (!(mask & speaker)) return -1;
    DWORD below = mask & (speaker - 1);
    int index = 0;
    for (; below; below &= below - 1) index++;
    return index;
}

// every source channel on its own speaker, the back pair on the side pair when the master has no back pair
std::vector<float> SurroundMatrix(DWORD mask, uint32_t outputs) {
    std::vector<float> m(outputs * kChannels, 0.0f);
    bool no_back = (mask & kBack) != kBack;
    for (uint32_t s = 0; s < kChannels; s++) {
        DWORD speaker = kSourceSpeaker[s];
        if (no_back && speaker == SPEAKER_BACK_LEFT) speaker = SPEAKER_SIDE_LEFT;
        if (no_back && speaker == SPEAKER_BACK_RIGHT) speaker = SPEAKER_SIDE_RIGHT;
        int o = OutputOf(mask, speaker);
        if (o >= 0) m[o * kChannels + s] = 1.0f;
    }
    return m;
}

// L = FL + kCentreFold C + kBackFold BL and R = FR + kCentreFold C + kBackFold BR without the sub, on the first two
// outputs. Mono, or a mono master, hears the average of the two, as the console's mono downmix.
std::vector<float> FoldToStereo(uint32_t outputs, bool mono) {
    std::vector<float> m(outputs * kChannels, 0.0f);
    bool average = mono || outputs == 1;
    for (uint32_t s = 0; s < kChannels; s++) {
        float left = kLeft[s];
        float right = kRight[s];
        if (average) {
            left = (kLeft[s] + kRight[s]) * 0.5f;
            right = left;
        }
        m[s] = left;
        if (outputs > 1) m[kChannels + s] = right;
    }
    return m;
}

uint32_t MasterOutputs(IXAudio2MasteringVoice* master) {
    XAUDIO2_VOICE_DETAILS details{};
    master->GetVoiceDetails(&details);
    return details.InputChannels;
}

DWORD MasterMask(IXAudio2MasteringVoice* master, uint32_t outputs) {
    DWORD mask = 0;
    master->GetChannelMask(&mask);
    // a stereo device that names no speakers is the front pair
    if (!mask && outputs == 2) mask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    return mask;
}

const char* OutputName() {
    if (g_mono) return "mono";
    return g_surround ? "5.1" : "stereo";
}

void ApplyMatrix(Output& out) {
    uint32_t outputs = MasterOutputs(out.master);
    DWORD mask = MasterMask(out.master, outputs);
    uint32_t mode = Mode();
    g_surround = WantSurround(mode, mask);
    g_mono = mode == kOutputMono;
    // a 5.1 mix on a device without 5.1 speakers is folded to stereo
    bool own_speakers = g_surround && HasSurround(mask);
    std::vector<float> m = own_speakers ? SurroundMatrix(mask, outputs) : FoldToStereo(outputs, g_mono);
    out.voice->SetOutputMatrix(out.master, kChannels, outputs, m.data());
    rt::Log("audio: %s output, %u device channels (DirectSound HLE)", OutputName(), outputs);
}

std::optional<Output> Open() {
    Output out;
    if (FAILED(XAudio2Create(&out.xa, 0, XAUDIO2_DEFAULT_PROCESSOR))) return std::nullopt;
    if (FAILED(out.xa->CreateMasteringVoice(&out.master))) return std::nullopt;
    WAVEFORMATEXTENSIBLE wf = MixFormat();
    if (FAILED(out.xa->CreateSourceVoice(&out.voice, &wf.Format))) return std::nullopt;
    ApplyMatrix(out);
    return out;
}

constexpr uint32_t kFrames = kRate / 100;  // 10 ms blocks

// After the mix: a limiter on the stereo fold (or on each speaker with 5.1), the same gain on every channel,
// so loud scenes get quieter instead of clipping. Instant attack, 150 ms release, inactive below the ceiling.
// FCI_NO_LIMITER=1 leaves it out. Then the level statistics and FCI_RECORD_AUDIO.
void Finish(float* b, uint32_t frames) {
    static const bool no_limiter = GetEnvironmentVariableA("FCI_NO_LIMITER", nullptr, 0) > 0;
    static FILE* record = GetEnvironmentVariableA("FCI_RECORD_AUDIO", nullptr, 0) ? std::fopen("audio.raw", "wb") : nullptr;
    static std::vector<float> stereo;
    static float envelope = 0;
    static const float release = std::exp(-1.0f / (0.15f * kRate));
    static uint64_t blocks = 0;
    static float peak = 0, limited = 1;
    static double energy = 0;
    constexpr float kCeiling = 0.95f;
    stereo.resize(frames * 2);
    bool surround = g_surround;
    for (uint32_t i = 0; i < frames; i++) {
        float* f = b + i * kChannels;
        float l = 0, r = 0;
        for (uint32_t s = 0; s < kChannels; s++) {
            l += kLeft[s] * f[s];
            r += kRight[s] * f[s];
        }
        float level = std::max(std::fabs(l), std::fabs(r));
        if (surround)
            for (uint32_t s = 0; s < kChannels; s++) level = std::max(level, std::fabs(f[s]));
        if (!std::isfinite(level)) {  // never let a NaN reach the device
            for (uint32_t s = 0; s < kChannels; s++) f[s] = 0;
            l = r = level = 0;
        }
        envelope = std::max(level, envelope * release);
        float g = !no_limiter && envelope > kCeiling ? kCeiling / envelope : 1.0f;
        if (g != 1.0f)
            for (uint32_t s = 0; s < kChannels; s++) f[s] *= g;
        limited = std::min(limited, g);
        stereo[i * 2] = l * g;
        stereo[i * 2 + 1] = r * g;
        peak = std::max({peak, std::fabs(stereo[i * 2]), std::fabs(stereo[i * 2 + 1])});
        energy += double(stereo[i * 2]) * stereo[i * 2] + double(stereo[i * 2 + 1]) * stereo[i * 2 + 1];
    }
    if (record) std::fwrite(stereo.data(), sizeof(float), frames * 2, record);
    if (++blocks % 500 == 0) {
        rt::Log("audio: %llu s, peak %.3f, rms %.4f, limiter %.2f", (unsigned long long)(blocks / 100), peak,
                std::sqrt(energy / (500.0 * frames * 2)), limited);
        peak = 0, energy = 0, limited = 1;
    }
}

void Pump(Output& out, const std::function<void(float*, uint32_t)>& render) {
    out.voice->Start();
    constexpr uint32_t kMinQueued = 3;
    constexpr uint32_t kMaxQueued = 10;
    // one slot more than can be queued, so the one rendered is never one XAudio2 still reads
    constexpr uint32_t kSlots = kMaxQueued + 1;
    std::vector<float> bufs(kSlots * kFrames * kChannels);
    uint32_t depth = kMinQueued;
    bool stopped = false;  // PC comfort features: focus pause
    for (uint32_t n = 0;;) {
        // ---- PC comfort features: a focus pause stops the voice where it is and the mix with it ----
        if (g_paused != stopped) {
            stopped = !stopped;
            if (stopped) out.voice->Stop();
            else out.voice->Start();
        }
        if (stopped) {
            Sleep(10);
            continue;
        }
        // ---- end PC comfort features ----
        XAUDIO2_VOICE_STATE st;
        out.voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        // a starved voice crackles: queue one buffer more each time, up to kMaxQueued
        if (n && !st.BuffersQueued && depth < kMaxQueued) {
            depth++;
            rt::Log("audio: output starved, queueing %u ms", depth * 10);
        }
        if (st.BuffersQueued >= depth) {
            Sleep(2);
            continue;
        }
        float* b = &bufs[(n % kSlots) * kFrames * kChannels];
        render(b, kFrames);
        Finish(b, kFrames);
        XAUDIO2_BUFFER xb{};
        xb.AudioBytes = kFrames * kChannels * sizeof(float);
        xb.pAudioData = reinterpret_cast<const BYTE*>(b);
        out.voice->SubmitSourceBuffer(&xb);
        n++;  // only a submitted buffer takes a slot, XAudio2 still reads the queued ones
    }
}

// no device: the mix still runs in real time, so the title's streams and buffers advance
void Clock(const std::function<void(float*, uint32_t)>& render) {
    std::vector<float> b(kFrames * kChannels);
    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    for (uint64_t n = 0;; n++) {
        // ---- PC comfort features: no mix during a focus pause, and no catching up after it ----
        if (g_paused) {
            LARGE_INTEGER paused_at, resumed;
            QueryPerformanceCounter(&paused_at);
            while (g_paused) Sleep(10);
            QueryPerformanceCounter(&resumed);
            start.QuadPart += resumed.QuadPart - paused_at.QuadPart;
        }
        // ---- end PC comfort features ----
        render(b.data(), kFrames);
        Finish(b.data(), kFrames);
        for (;;) {
            QueryPerformanceCounter(&now);
            if (double(now.QuadPart - start.QuadPart) / double(freq.QuadPart) >= double(n + 1) / 100.0) break;
            Sleep(1);
        }
    }
}

}  // namespace

bool IsSurround() { return g_surround; }

bool IsMono() { return g_mono; }

// ---- PC comfort features: focus pause ----
void SetPaused(bool paused) { g_paused = paused; }
// ---- end PC comfort features ----

void StartOutput(std::function<void(float*, uint32_t)> render) {
    std::promise<void> opened;
    std::future<void> known = opened.get_future();
    std::thread([render, opened = std::move(opened)]() mutable {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::optional<Output> out = Open();
        opened.set_value();
        if (!out) {
            rt::Log("audio: XAudio2 unavailable, no sound (the mix runs on a host clock)");
            Clock(render);
            return;
        }
        Pump(*out, render);
    }).detach();
    known.wait();
}

}  // namespace audio
