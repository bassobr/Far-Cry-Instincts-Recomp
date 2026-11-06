// The MCPX audio processing unit. DirectSound (the XDK library, recompiled) drives it at register level:
// it writes methods to the front end (FE), which keeps voice descriptors in memory, and the voice processor
// (VP) mixes the active voices into 32 mixbins. The global processor (GP, a DSP running dsstdfx.bin) would
// add effects and send the bins to the AC'97 codec; here the speaker and crosstalk (3D) bins go straight to
// XAudio2 as a stereo downmix, the reverb send (bin 10) through a stand-in reverb, and the GP is a stand-in
// that acknowledges the commands DirectSound posts to its mailbox. 3D voices are placed by a reduced HRTF
// (gain and delay per ear).
#include "audio/apu.h"

#include <windows.h>
#include <xaudio2.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "kernel/kernel.h"
#include "runtime/guest.h"
#include "runtime/log.h"
#include "runtime/memory.h"
#include "runtime/scheduler.h"
#include "runtime/thread.h"

namespace xdk {
bool HleAudio();  // src/xdk/xdk.cpp: false with FCI_LLE_AUDIO=1
}

namespace apu {

namespace {

// registers (offsets into the APU range)
constexpr uint32_t kIsts = 0x1000;      // interrupt status, write one to clear
constexpr uint32_t kIen = 0x1004;       // interrupt enable
constexpr uint32_t kIstsGlobal = 1u << 0, kIstsVoice = 1u << 6;  // FEVINTSTS: a voice has a notification
constexpr uint32_t kIstsFeTrap = 1u << 4;  // FETINTSTS: the front end trapped a method
// front end control: method mode (free running, halted, trapped) and the trap reason
constexpr uint32_t kFectl = 0x1100, kFeModeMask = 0xE0, kFeModeTrapped = 0xE0, kFeTrapRequested = 0xF00;
constexpr uint32_t kFedecmeth = 0x1300, kFedecparam = 0x1304;  // the trapped method and its argument
constexpr uint32_t kSe2FeIdleVoice = 0x8000;  // the voice processor tells the front end a voice went idle
constexpr uint32_t kFememaddr = 0x1324, kFememdata = 0x1334;  // FE writes FEMEMDATA to FEMEMADDR (a fence)
constexpr uint32_t kFenaddr = 0x115C;   // FE notifiers: 16 bytes each; two, then 4 per voice (SSL A, SSL B, ...)
constexpr uint32_t kApuVector = 5 + 48; // IRQ 5
constexpr uint32_t kXgscnt = 0x200C;    // global sample counter, 48 kHz
constexpr uint32_t kVpvaddr = 0x202C;   // voice descriptors, physical
constexpr uint32_t kVpsgeaddr = 0x2030; // input buffer scatter-gather entries, physical
constexpr uint32_t kVpssladdr = 0x2034; // stream segment lists, physical
constexpr uint32_t kGpsaddr = 0x2040;   // GP scratch SGEs
constexpr uint32_t kTop[4] = {0, 0x2054, 0x2060, 0x206C};  // voice list heads: 2D, 3D, multipass
constexpr uint32_t kGpReset = 0x3FFFC;
constexpr uint32_t kEpReset = 0x5FFFC;
// the running EP program keeps 0xCCCCCC in this word of its memory; DirectSound's watchdog resets both
// DSPs (and stalls 10 ms) whenever it is missing
constexpr uint32_t kEpAlive = 0x5A018, kEpAliveValue = 0xCCCCCC;
constexpr uint32_t kMailbox = 0x810;  // command word in the first GP scratch page

// front end methods (offsets from 0x20000)
constexpr uint32_t kFe = 0x20000;
enum : uint32_t {
    PIO_FREE = 0x010,
    PIO_SYNCHRONIZE = 0x104,  // the front end reports through its own first notifier once it got this far
    SET_CURRENT_HRTF_ENTRY = 0x160,
    SET_ANTECEDENT_VOICE = 0x120,
    VOICE_ON = 0x124,
    VOICE_OFF = 0x128,
    VOICE_RELEASE = 0x12C,
    VOICE_PAUSE = 0x140,
    SET_CURRENT_SSL = 0x190,
    SET_SUBMIX_HEADROOM = 0x200,  // 32 bins, 6 dB steps
    SET_CURRENT_VOICE = 0x2F8,
    SET_VOICE_CFG_VBIN = 0x300,
    SET_VOICE_CFG_MISC = 0x318,
    SET_VOICE_CFG_HRTF_TARGET = 0x31C,
    SET_VOICE_SSL_A = 0x320,
    SET_VOICE_SSL_B = 0x35C,
    SET_VOICE_TAR_VOLA = 0x360,
    SET_VOICE_TAR_VOLC = 0x368,
    SET_VOICE_LFO_ENV = 0x36C,
    SET_VOICE_TAR_FCB = 0x378,
    SET_VOICE_TAR_PITCH = 0x37C,
    SET_VOICE_CFG_BUF_BASE = 0x3A0,
    SET_VOICE_CFG_BUF_LBO = 0x3A4,
    SET_VOICE_BUF_CBO = 0x3D8,
    SET_VOICE_CFG_BUF_EBO = 0x3DC,
    SET_SSL_SEGMENT = 0x600,  // 0x600 + 8n offset, 0x604 + 8n length
    SET_HRIR = 0x400,    // 15 words: left and right taps 2n (bits 7:0, 15:8) and 2n+1 (23:16, 31:24), signed
    SET_HRIR_X = 0x43C,  // tap 30 (7:0 left, 15:8 right) and the interaural delay (31:16, 1/1024 samples)
    SET_CURRENT_INBUF_SGE = 0x804,
    SET_CURRENT_INBUF_SGE_OFFSET = 0x808,
};

// voice descriptor (0x80 bytes)
enum : uint32_t {
    VBIN = 0x00,
    FMT = 0x04,
    PSL_START = 0x20,   // BA: buffer base, bytes in SGE space
    PSH_SAMPLE = 0x24,  // LBO: loop start, samples
    CUR_VOLA = 0x28,
    HRTF_TARGET = 0x1C,  // the voice's HRTF entry, 0xFFFF for none
    PAR_STATE = 0x54,
    PAR_OFFSET = 0x58,  // CBO: current position, samples
    PAR_NEXT = 0x5C,    // EBO: end, samples
    TAR_VOLA = 0x60,
    TAR_VOLB = 0x64,
    TAR_VOLC = 0x68,
    TAR_PITCH_LINK = 0x7C,  // next voice handle (low 16), pitch (high 16)
};
constexpr uint32_t kStatePaused = 1u << 18, kStateActive = 1u << 21;
constexpr uint32_t kFmtStream = 1u << 24, kFmtLoop = 1u << 25, kFmtStereo = 1u << 27;
constexpr uint16_t kNoVoice = 0xFFFF;
constexpr int kVoices = 256, kBins = 32;
constexpr int kRate = 48000, kChunk = 480;  // 10 ms per mix

std::mutex g_lock;  // voice lists and descriptors between the FE (guest CPU) and the mixer
uint32_t g_current_voice = 0, g_antecedent = 0, g_inbuf_sge = 0, g_current_ssl = 0;
std::condition_variable g_irq_wake;
double g_fraction[kVoices];  // position between samples, per voice
// the volumes each voice was last mixed with: the voice processor ramps current volumes to their targets
// instead of jumping (3D sources change theirs every frame, which would crackle)
float g_gain[kVoices][8];
bool g_gain_primed[kVoices];
// released voices: without envelopes the release fades out over one mix chunk instead of cutting off (a click)
bool g_releasing[kVoices];

// Resampling: the voice processor interpolates between source samples. Repeating each one instead (a 22 kHz
// voice held for two or three output samples) adds images of the whole spectrum above the source's Nyquist
// frequency, which made speech harsh. Each voice keeps the last two source frames it fetched and blends
// them by the position between them (linear interpolation, one source sample late).
struct Interpolator {
    float previous[2], current[2];
    uint64_t consumed;  // source frames the voice has moved past
    uint64_t fetched;   // consumed when current was fetched
    bool primed;
};
Interpolator g_interp[kVoices];

template <typename FetchFrame>
void Interpolate(uint32_t v, FetchFrame fetch, double fraction, float out[2]) {
    Interpolator& in = g_interp[v];
    if (!in.primed || in.fetched != in.consumed) {
        float frame[2];
        fetch(frame);
        for (int c = 0; c < 2; c++) {
            in.previous[c] = in.primed ? in.current[c] : frame[c];
            in.current[c] = frame[c];
        }
        in.fetched = in.consumed;
        in.primed = true;
    }
    float t = float(fraction);
    for (int c = 0; c < 2; c++) out[c] = in.previous[c] + (in.current[c] - in.previous[c]) * t;
}
float g_headroom[kBins];     // attenuation per mixbin

// stream voices play two segment lists in turn; each SSL register holds the first segment (value >> 8)
// and the segment count (low byte), and DirectSound refills a list once its notifier says it is done
struct Stream {
    uint32_t ssl[2];
    int current;       // SSL playing
    uint32_t segment;  // within it
    uint32_t frame;    // within the segment
};
Stream g_streams[kVoices];

// 3D voices (list 2) run through a head related impulse response: DirectSound computes one per position,
// 31 taps for each ear and an interaural delay, and the voice processor filters the mono voice into a left
// and a right signal; volumes 0 and 1 then take the left one and 2 and 3 the right one (the front and back
// pairs of the XTalk mixbins). Here the filter is reduced to what places the sound: a gain per ear, from the
// energy of its taps, and the delay.
constexpr int kHrtfEntries = 256, kHrtfHistory = 64;
struct HrtfEntry {
    float gain[2] = {1, 1};
    int delay[2] = {0, 0};
};
HrtfEntry g_hrtf[kHrtfEntries];
uint32_t g_hrtf_entry = 0, g_hrir[15];
struct HrtfVoice {
    float history[kHrtfHistory];
    uint32_t position;
    float gain[2];  // as last mixed, ramped to the entry's over each chunk
    bool primed;
};
HrtfVoice g_hrtf_voices[kVoices];

void SetHrir(uint32_t x) {
    if (g_hrtf_entry >= kHrtfEntries) return;
    double energy[2] = {0, 0};
    auto tap = [&](uint32_t word, int shift, int ear) { energy[ear] += std::pow(double(int8_t(word >> shift)), 2); };
    for (uint32_t w : g_hrir) tap(w, 0, 0), tap(w, 8, 1), tap(w, 16, 0), tap(w, 24, 1);
    tap(x, 0, 0), tap(x, 8, 1);
    HrtfEntry& e = g_hrtf[g_hrtf_entry];
    double total = energy[0] + energy[1];
    for (int ear = 0; ear < 2; ear++) e.gain[ear] = total > 0 ? float(std::sqrt(2 * energy[ear] / total)) : 1.0f;
    // negative: the right ear hears it later
    int delay = int(std::lround(int16_t(x >> 16) / 1024.0));
    delay = std::clamp(delay, -(kHrtfHistory - 1), kHrtfHistory - 1);
    e.delay[0] = std::max(delay, 0), e.delay[1] = std::max(-delay, 0);
}
std::chrono::steady_clock::time_point g_start;
FILE* g_trace = nullptr;

uint32_t& Reg(uint32_t offset) { return *reinterpret_cast<uint32_t*>(rt::Host(kMmioBase + offset)); }

uint32_t VoiceAddress(uint32_t v) { return Reg(kVpvaddr) + v * 0x80; }
uint32_t& Field(uint32_t v, uint32_t field) {
    return *reinterpret_cast<uint32_t*>(mem::Device(VoiceAddress(v) + field));
}
void SetLow24(uint32_t v, uint32_t field, uint32_t value) {
    Field(v, field) = (Field(v, field) & 0xFF000000u) | (value & 0x00FFFFFFu);
}
// DirectSound unlinks released voices itself and points them at themselves; that ends a list too
uint16_t Next(uint32_t v) {
    uint16_t next = uint16_t(Field(v, TAR_PITCH_LINK));
    return next == v ? kNoVoice : next;
}
void SetNext(uint32_t v, uint16_t next) { Field(v, TAR_PITCH_LINK) = (Field(v, TAR_PITCH_LINK) & 0xFFFF0000u) | next; }

bool Valid(uint32_t v) { return v < kVoices && Reg(kVpvaddr); }

void Unlink(uint32_t v) {
    for (int list = 1; list <= 3; list++) {
        uint32_t& top = Reg(kTop[list]);
        if ((top & 0xFFFF) == v) {
            top = Next(v);
            return;
        }
        for (uint32_t p = top & 0xFFFF, guard = 0; p != kNoVoice && Valid(p) && guard < kVoices; p = Next(p), guard++)
            if (Next(p) == v) {
                SetNext(p, Next(v));
                return;
            }
    }
}

// voices the voice processor finished, reported one at a time: the front end traps SE2FE_IDLE_VOICE and
// DirectSound's interrupt handler frees the voice, then lets the front end run on (g_lock held)
std::deque<uint16_t> g_idle_voices;

void TrapIdleVoice() {
    if (g_idle_voices.empty() || (Reg(kFectl) & kFeModeMask)) return;  // halted or still trapped
    uint16_t v = g_idle_voices.front();
    g_idle_voices.pop_front();
    Reg(kFectl) |= kFeModeTrapped | kFeTrapRequested;
    Reg(kFedecmeth) = kSe2FeIdleVoice;
    Reg(kFedecparam) = v;
    Reg(kIsts) |= kIstsFeTrap | kIstsGlobal;
    g_irq_wake.notify_one();
}

void IdleVoice(uint32_t v) {
    static const bool off = GetEnvironmentVariableA("FCI_NO_IDLE_VOICE", nullptr, 0) > 0;  // diagnostics
    if (off) return;
    g_idle_voices.push_back(uint16_t(v));
    TrapIdleVoice();
}

// FCI_TRACE_VOICES=1: voices starting and stopping, and the active ones every 5 seconds
const bool g_trace_voices = GetEnvironmentVariableA("FCI_TRACE_VOICES", nullptr, 0) > 0;

void TraceVoice(const char* what, uint32_t v) {
    if (!g_trace_voices) return;
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    rt::Log("%8.2f voice %s %02X: fmt %08X vbin %08X pitch %04X vol %08X %08X %08X", t, what, v, Field(v, FMT), Field(v, VBIN),
            Field(v, TAR_PITCH_LINK) >> 16, Field(v, TAR_VOLA), Field(v, TAR_VOLB), Field(v, TAR_VOLC));
}

void Method(uint32_t m, uint32_t value) {
    std::lock_guard lock(g_lock);
    uint32_t v = g_current_voice;
    switch (m) {
    case PIO_SYNCHRONIZE:
        // methods run as they are written, so the front end is always caught up: report at once (DirectSound
        // polls the notifier for up to 667 us around every voice update)
        if (uint32_t base = Reg(kFenaddr)) {
            uint8_t* n = mem::Device(base);
            n[0x0E] = 0;
            n[0x0F] = 0x01;
        }
        return;
    case SET_ANTECEDENT_VOICE: g_antecedent = value; return;
    case VOICE_ON: {
        uint32_t h = value & 0xFFFF;
        if (!Valid(h)) return;
        Unlink(h);  // a voice is on one list at most
        uint32_t list = (g_antecedent >> 16) & 3;
        if (list) {
            SetNext(h, uint16_t(Reg(kTop[list])));
            Reg(kTop[list]) = h;
        } else if (Valid(g_antecedent & 0xFFFF)) {
            SetNext(h, Next(g_antecedent & 0xFFFF));
            SetNext(g_antecedent & 0xFFFF, uint16_t(h));
        }
        Field(h, PAR_STATE) |= kStateActive;
        g_fraction[h] = 0;
        g_hrtf_voices[h].primed = false;
        g_gain_primed[h] = false;
        g_interp[h].primed = false;
        g_releasing[h] = false;
        g_streams[h].current = 0, g_streams[h].segment = 0, g_streams[h].frame = 0;
        TraceVoice("on", h);
        return;
    }
    case VOICE_OFF:
    case VOICE_RELEASE: {  // no envelopes yet: a release ends the voice after a short fade
        uint32_t h = value & 0xFFFF;
        if (!Valid(h)) return;
        bool was_active = Field(h, PAR_STATE) & kStateActive;
        if (was_active) TraceVoice(m == VOICE_OFF ? "off" : "release", h);
        if (m == VOICE_RELEASE && was_active) {
            g_releasing[h] = true;  // the mixer ends it
            return;
        }
        g_releasing[h] = false;
        Field(h, PAR_STATE) &= ~kStateActive;
        Unlink(h);
        // DirectSound waits for the idle report after it turned a voice off
        if (was_active && m == VOICE_OFF) IdleVoice(h);
        return;
    }
    case VOICE_PAUSE: {
        uint32_t h = value & 0xFFFF;
        if (!Valid(h)) return;
        if (value & (1u << 18))
            Field(h, PAR_STATE) |= kStatePaused;
        else
            Field(h, PAR_STATE) &= ~kStatePaused;
        return;
    }
    case SET_CURRENT_VOICE: g_current_voice = value & 0xFFFF; return;
    case SET_CURRENT_INBUF_SGE: g_inbuf_sge = value; return;
    case SET_CURRENT_INBUF_SGE_OFFSET:
        if (Reg(kVpsgeaddr)) *reinterpret_cast<uint32_t*>(mem::Device(Reg(kVpsgeaddr) + g_inbuf_sge * 8)) = value & ~0xFFFu;
        return;
    case SET_CURRENT_SSL: g_current_ssl = value; return;
    case SET_CURRENT_HRTF_ENTRY: g_hrtf_entry = value; return;
    case SET_HRIR_X: SetHrir(value); return;
    default: break;
    }
    if (m >= SET_HRIR && m < SET_HRIR_X) {
        g_hrir[(m - SET_HRIR) / 4] = value;
        return;
    }
    if (m >= SET_SUBMIX_HEADROOM && m < SET_SUBMIX_HEADROOM + 4 * kBins) {
        g_headroom[(m - SET_SUBMIX_HEADROOM) / 4] = std::ldexp(1.0f, -int(value & 7));
        return;
    }
    if (m >= SET_SSL_SEGMENT && m < SET_SSL_SEGMENT + 0x200) {
        uint32_t n = (m - SET_SSL_SEGMENT) / 8;
        if (Reg(kVpssladdr))
            *reinterpret_cast<uint32_t*>(mem::Device(Reg(kVpssladdr) + (g_current_ssl + n) * 8 + (m & 4))) = value;
        return;
    }
    if (!Valid(v)) return;
    if (m == SET_VOICE_SSL_A || m == SET_VOICE_SSL_B) {
        g_streams[v].ssl[m == SET_VOICE_SSL_B] = value;
    } else if (m >= SET_VOICE_CFG_VBIN && m <= SET_VOICE_CFG_HRTF_TARGET) {
        Field(v, m - SET_VOICE_CFG_VBIN) = value;
    } else if (m >= SET_VOICE_TAR_VOLA && m <= SET_VOICE_TAR_VOLC) {
        Field(v, TAR_VOLA + (m - SET_VOICE_TAR_VOLA)) = value;
        Field(v, CUR_VOLA + (m - SET_VOICE_TAR_VOLA)) = value;
    } else if (m >= SET_VOICE_LFO_ENV && m <= SET_VOICE_TAR_FCB) {
        Field(v, m - SET_VOICE_TAR_VOLA + TAR_VOLA) = value;
    } else if (m == SET_VOICE_TAR_PITCH) {
        Field(v, TAR_PITCH_LINK) = (Field(v, TAR_PITCH_LINK) & 0xFFFF) | (value & 0xFFFF0000u);
    } else if (m == SET_VOICE_CFG_BUF_BASE) {
        SetLow24(v, PSL_START, value);
    } else if (m == SET_VOICE_CFG_BUF_LBO) {
        SetLow24(v, PSH_SAMPLE, value);
    } else if (m == SET_VOICE_BUF_CBO) {
        SetLow24(v, PAR_OFFSET, value);
        g_fraction[v] = 0;
        g_interp[v].primed = false;
    } else if (m == SET_VOICE_CFG_BUF_EBO) {
        SetLow24(v, PAR_NEXT, value);
    }
}

// ---- voice processor ----

// SGE space -> host pointer: the buffer is a list of 4 KB pages
const uint8_t* SgeByte(uint32_t address) {
    uint32_t page = *reinterpret_cast<const uint32_t*>(mem::Device(Reg(kVpsgeaddr) + (address >> 12) * 8));
    return mem::Device((page & ~0xFFFu) + (address & 0xFFF));
}

const int kAdpcmSteps[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
    544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
    9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
const int kAdpcmIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

// Xbox ADPCM: per channel a 4 byte header (first sample, step index) and 64 more samples in nibbles,
// channels interleaved by 4 bytes; 36 bytes per block per channel, of which 64 samples are used
// buffers are read through the SGE table, stream segments at their device address
using Source = const uint8_t* (*)(uint32_t);
const uint8_t* DeviceByte(uint32_t address) { return mem::Device(address); }

void DecodeAdpcmBlock(Source byte, uint32_t address, int channels, int16_t out[2][65]) {
    for (int ch = 0; ch < channels; ch++) {
        int predictor = int16_t(byte(address + ch * 4)[0] | (byte(address + ch * 4 + 1)[0] << 8));
        int index = std::clamp<int>(byte(address + ch * 4 + 2)[0], 0, 88);
        out[ch][0] = int16_t(predictor);
        int n = 1;
        for (int word = 0; word < 8; word++) {
            uint32_t base = address + channels * 4 + (word * channels + ch) * 4;
            for (int b = 0; b < 4; b++) {
                uint8_t packed = byte(base + b)[0];
                for (int nib = 0; nib < 2; nib++) {
                    int code = (packed >> (4 * nib)) & 0xF, step = kAdpcmSteps[index];
                    int diff = step >> 3;
                    if (code & 1) diff += step >> 2;
                    if (code & 2) diff += step >> 1;
                    if (code & 4) diff += step;
                    predictor = std::clamp(predictor + ((code & 8) ? -diff : diff), -32768, 32767);
                    index = std::clamp(index + kAdpcmIndex[code], 0, 88);
                    out[ch][n++] = int16_t(predictor);
                }
            }
        }
    }
}

// bytes per frame, or per 64 frame block for ADPCM
uint32_t FrameBytes(uint32_t fmt) {
    uint32_t channels = (fmt & kFmtStereo) ? 2 : 1, container = fmt >> 30;
    return container == 2 ? 36 * channels : (container == 0 ? 1 : container == 1 ? 2 : 4) * channels;
}

// one sample frame, as floats in [-1, 1]
void Fetch(Source byte, uint32_t fmt, uint32_t base, uint32_t index, float& left, float& right) {
    bool stereo = fmt & kFmtStereo;
    int channels = stereo ? 2 : 1;
    uint32_t container = fmt >> 30, size = (fmt >> 28) & 3;
    if (container == 2) {  // ADPCM
        static thread_local uint32_t cached = ~0u;
        static thread_local Source cached_source = nullptr;
        static thread_local int16_t block[2][65];
        uint32_t address = base + (index / 64) * FrameBytes(fmt);
        if (address != cached || byte != cached_source) {
            DecodeAdpcmBlock(byte, address, channels, block);
            cached = address, cached_source = byte;
        }
        left = block[0][index % 64] / 32768.0f;
        right = stereo ? block[1][index % 64] / 32768.0f : left;
        return;
    }
    uint32_t bytes = container == 0 ? 1 : container == 1 ? 2 : 4;
    uint32_t address = base + index * bytes * channels;
    auto one = [&](uint32_t a) -> float {
        if (bytes == 1) return (byte(a)[0] - 128) / 128.0f;
        if (bytes == 2) return int16_t(byte(a)[0] | (byte(a + 1)[0] << 8)) / 32768.0f;
        int32_t s = int32_t(byte(a)[0] | (byte(a + 1)[0] << 8) | (byte(a + 2)[0] << 16) |
                            (uint32_t(byte(a + 3)[0]) << 24));
        return size == 2 ? float(s << 8) / 2147483648.0f : float(s) / 2147483648.0f;
    };
    left = one(address);
    right = stereo ? one(address + bytes) : left;
}

// 12 bit attenuation in 1/64 dB
float Gain(uint32_t attenuation) {
    return attenuation >= 0xFFF ? 0.0f : std::pow(10.0f, -float(attenuation) / (64.0f * 20.0f));
}

// a voice's notifier slot (0 and 1 for the stream segment lists) reports done, and the voice interrupt fires
void Notify(uint32_t v, int slot) {
    if (uint32_t base = Reg(kFenaddr)) {
        // the first two notifiers belong to the front end itself, then four per voice
        uint8_t* n = mem::Device(base + (2 + v * 4 + slot) * 16);
        uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() - g_start).count());
        std::memcpy(n, &now, 8);
        n[0x0E] = 0;
        n[0x0F] = 0x01;  // done; DirectSound set 0x80 (in progress) when it queued the list
    }
    Reg(kIsts) |= kIstsVoice | kIstsGlobal;
    g_irq_wake.notify_one();
    if (g_trace) std::fprintf(g_trace, "%u notify voice %02X slot %d\n", GetTickCount(), v, slot);
}

void MixVoice(uint32_t v, int list, float bins[kBins][kChunk]) {
    uint32_t fmt = Field(v, FMT);
    bool stream = fmt & kFmtStream;
    // a one frame (32 sample) loop is fed by the GP every frame (the effects return); with the GP standing
    // in it holds no sound, only whatever DC was left there
    if (!stream && (fmt & kFmtLoop) && (Field(v, PAR_NEXT) & 0xFFFFFF) < 32) return;
    double step = std::exp2(int16_t(Field(v, TAR_PITCH_LINK) >> 16) / 4096.0);
    uint32_t vbin = Field(v, VBIN), vola = Field(v, TAR_VOLA), volb = Field(v, TAR_VOLB), volc = Field(v, TAR_VOLC);
    int bin[8] = {int(vbin & 31),         int((vbin >> 5) & 31),  int((vbin >> 10) & 31), int((vbin >> 16) & 31),
                  int((vbin >> 21) & 31), int((vbin >> 26) & 31), int(fmt & 31),          int((fmt >> 5) & 31)};
    float target_gain[8] = {
        Gain((vola >> 4) & 0xFFF), Gain(vola >> 20), Gain((volb >> 4) & 0xFFF), Gain(volb >> 20),
        Gain((volc >> 4) & 0xFFF), Gain(volc >> 20),
        Gain((vola & 0xF) | ((volb & 0xF) << 4) | ((volc & 0xF) << 8)),
        Gain(((vola >> 16) & 0xF) | (((volb >> 16) & 0xF) << 4) | (((volc >> 16) & 0xF) << 8))};
    const bool releasing = g_releasing[v];
    if (releasing) {
        std::fill(std::begin(target_gain), std::end(target_gain), 0.0f);
        g_releasing[v] = false;
        Field(v, PAR_STATE) &= ~kStateActive;  // after this chunk; the mix loop unlinks it
    }
    float gain[8], gain_step[8];
    for (int k = 0; k < 8; k++) {
        float from = g_gain_primed[v] ? g_gain[v][k] : target_gain[k];
        gain[k] = from;
        gain_step[k] = (target_gain[k] - from) / kChunk;
        g_gain[v][k] = target_gain[k];
    }
    g_gain_primed[v] = true;
    bool stereo = fmt & kFmtStereo;
    uint32_t target = Field(v, HRTF_TARGET) & 0xFFFF;
    HrtfVoice* hrtf = list == 2 && !stereo && target < kHrtfEntries ? &g_hrtf_voices[v] : nullptr;
    float ear_gain[2] = {0, 0}, ear_step[2] = {0, 0};
    if (hrtf) {
        const HrtfEntry& e = g_hrtf[target];
        if (!hrtf->primed) {
            std::fill(std::begin(hrtf->history), std::end(hrtf->history), 0.0f);
            hrtf->gain[0] = e.gain[0], hrtf->gain[1] = e.gain[1];
            hrtf->primed = true;
        }
        for (int ear = 0; ear < 2; ear++) {
            ear_gain[ear] = hrtf->gain[ear];
            ear_step[ear] = (e.gain[ear] - hrtf->gain[ear]) / kChunk;
            hrtf->gain[ear] = e.gain[ear];
        }
    }
    auto mix = [&](int i, const float s[2]) {
        if (hrtf) {
            const HrtfEntry& e = g_hrtf[target];
            hrtf->history[hrtf->position++ % kHrtfHistory] = s[0];
            float ear[2];
            for (int k = 0; k < 2; k++) {
                ear[k] = (ear_gain[k] + ear_step[k] * i) *
                         hrtf->history[(hrtf->position - 1 - e.delay[k]) % kHrtfHistory];
            }
            for (int k = 0; k < 8; k++)
                if (float g = gain[k] + gain_step[k] * i; g > 0) bins[bin[k]][i] += g * (k < 4 ? ear[k >> 1] : s[0]);
            return;
        }
        for (int k = 0; k < 8; k++)
            if (float g = gain[k] + gain_step[k] * i; g > 0) bins[bin[k]][i] += g * s[stereo ? (k & 1) : 0];
    };
    double fraction = g_fraction[v];

    if (stream) {
        // segment lists: entries of [device address, frame count] at VPSSLADDR
        Stream& st = g_streams[v];
        for (int i = 0; i < kChunk;) {
            uint32_t ssl = st.ssl[st.current], count = ssl & 0xFF;
            if (!count || !Reg(kVpssladdr)) break;  // starved until DirectSound queues more
            const uint32_t* entry =
                reinterpret_cast<const uint32_t*>(mem::Device(Reg(kVpssladdr) + ((ssl >> 8) + st.segment) * 8));
            uint32_t address = entry[0], frames = entry[1] & 0xFFFF;
            if (st.frame >= frames) {
                st.frame = 0;
                if (++st.segment >= count) {
                    st.ssl[st.current] = 0;  // played; the notifier tells DirectSound to refill it
                    Notify(v, st.current);
                    st.current ^= 1;
                    st.segment = 0;
                }
                continue;
            }
            float s[2];
            Interpolate(v, [&](float f[2]) { Fetch(DeviceByte, fmt, address, st.frame, f[0], f[1]); }, fraction, s);
            mix(i, s);
            fraction += step;
            uint32_t whole = uint32_t(fraction);
            st.frame += whole;
            g_interp[v].consumed += whole;
            fraction -= whole;
            i++;
        }
        g_fraction[v] = fraction;
        return;
    }

    uint32_t base = Field(v, PSL_START) & 0xFFFFFF, lbo = Field(v, PSH_SAMPLE) & 0xFFFFFF;
    uint32_t cbo = Field(v, PAR_OFFSET) & 0xFFFFFF, ebo = Field(v, PAR_NEXT) & 0xFFFFFF;
    bool loop = fmt & kFmtLoop;
    for (int i = 0; i < kChunk; i++) {
        if (cbo > ebo) {
            if (!loop) {
                Field(v, PAR_STATE) &= ~kStateActive;
                break;
            }
            cbo = lbo + (cbo - ebo - 1) % (ebo - lbo + 1);
        }
        float s[2];
        Interpolate(v, [&](float f[2]) { Fetch(SgeByte, fmt, base, cbo, f[0], f[1]); }, fraction, s);
        mix(i, s);
        fraction += step;
        uint32_t whole = uint32_t(fraction);
        cbo += whole;
        g_interp[v].consumed += whole;
        fraction -= whole;
    }
    g_fraction[v] = fraction;
    SetLow24(v, PAR_OFFSET, cbo);
}

int g_mixed = 0;  // voices mixed in the last chunk

void Mix(float bins[kBins][kChunk]) {
    std::lock_guard lock(g_lock);
    g_mixed = 0;
    if (!Reg(kVpvaddr) || !Reg(kVpsgeaddr)) return;
    static ULONGLONG last_trace = 0;
    const bool trace = g_trace_voices && GetTickCount64() - last_trace > 5000;
    if (trace) last_trace = GetTickCount64();
    for (int list = 1; list <= 3; list++) {
        uint32_t guard = 0;
        for (uint32_t v = Reg(kTop[list]) & 0xFFFF; v != kNoVoice && Valid(v) && guard < kVoices; guard++) {
            uint32_t next = Next(v), state = Field(v, PAR_STATE);
            if (trace && (state & kStateActive)) {
                rt::Log("active voice %02X list %d fmt %08X pitch %04X vol %08X %08X %08X state %08X psl %08X psh %08X",
                        v, list, Field(v, FMT), Field(v, TAR_PITCH_LINK) >> 16, Field(v, TAR_VOLA), Field(v, TAR_VOLB),
                        Field(v, TAR_VOLC), state, Field(v, PSL_START), Field(v, PSH_SAMPLE));
            }
            if ((state & kStateActive) && !(state & kStatePaused)) MixVoice(v, list, bins), g_mixed++;
            if (!(Field(v, PAR_STATE) & kStateActive)) Unlink(v);  // finished voices leave the list
            v = next;
        }
    }
}

// Stand-in for the GP's I3DL2 reverb (dsstdfx runs it on the DSP, which is not emulated): DirectSound sends
// every 3D voice to mixbin 10 as well, at its reverb level. The game does set a listener environment (the
// effect's parameters change when a level starts), but its DSP parameter layout is not decoded, so this is a
// fixed, moderate outdoor-like room: early reflections and a four line feedback delay network with damping.
// FCI_NO_REVERB=1 turns it off.
class Reverb {
public:
    Reverb() {
        constexpr double kDecay = 1.4;  // seconds to -60 dB
        for (int n = 0; n < kLines; n++) {
            lines_[n].assign(kLength[n], 0.0f);
            feedback_[n] = float(std::pow(10.0, -3.0 * kLength[n] / (kRate * kDecay)));
        }
        early_.assign(kEarlyLength, 0.0f);
    }

    void Process(float in, float out[2]) {
        // early reflections from a tapped delay line, alternating sides
        early_[early_at_] = in;
        float er[2] = {0, 0};
        for (int t = 0; t < kTaps; t++) {
            int at = early_at_ - kTap[t].delay;
            if (at < 0) at += kEarlyLength;
            er[t & 1] += early_[at] * kTap[t].gain;
        }
        early_at_ = (early_at_ + 1) % kEarlyLength;
        int pre = early_at_ - kPredelay - 1;
        if (pre < 0) pre += kEarlyLength;
        float late_in = early_[pre] * 0.35f;
        // late reverb: Householder mixing of the delay outputs, a one pole low pass per line for the duller tail
        float y[kLines], sum = 0;
        for (int n = 0; n < kLines; n++) {
            y[n] = lines_[n][at_[n]];
            sum += y[n];
        }
        for (int n = 0; n < kLines; n++) {
            float v = (y[n] - 0.5f * sum) * feedback_[n];
            damp_[n] += kDamping * (v - damp_[n]);
            lines_[n][at_[n]] = damp_[n] + late_in;
            at_[n] = (at_[n] + 1) % kLength[n];
        }
        out[0] = kLevel * (er[0] + y[0] + 0.6f * y[2]);
        out[1] = kLevel * (er[1] + y[1] + 0.6f * y[3]);
    }

private:
    static constexpr int kLines = 4;
    static constexpr int kLength[kLines] = {1687, 2053, 2389, 2777};
    static constexpr float kDamping = 0.45f;  // 1: no damping
    static constexpr float kLevel = 0.4f;
    struct Tap {
        int delay;
        float gain;
    };
    static constexpr int kTaps = 6;
    static constexpr Tap kTap[kTaps] = {{960, 0.32f}, {1296, 0.28f}, {1776, 0.22f}, {2208, 0.18f},
                                        {2880, 0.14f}, {3552, 0.10f}};
    static constexpr int kEarlyLength = 4096, kPredelay = 1200;
    std::vector<float> lines_[kLines];
    std::vector<float> early_;
    float feedback_[kLines] = {}, damp_[kLines] = {};
    int at_[kLines] = {}, early_at_ = 0;
};

void MixerThread() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IXAudio2* xaudio = nullptr;
    IXAudio2MasteringVoice* master = nullptr;
    IXAudio2SourceVoice* source = nullptr;
    WAVEFORMATEX format{WAVE_FORMAT_IEEE_FLOAT, 2, kRate, kRate * 8, 8, 32, 0};
    if (FAILED(XAudio2Create(&xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR)) ||
        FAILED(xaudio->CreateMasteringVoice(&master)) || FAILED(xaudio->CreateSourceVoice(&source, &format))) {
        rt::Log("apu: XAudio2 unavailable, no sound");
        return;
    }
    source->Start();
    rt::Log("apu: XAudio2 output, %d Hz stereo", kRate);
    constexpr int kQueue = 4;
    static float output[kQueue][kChunk * 2];
    static float bins[kBins][kChunk];
    int slot = 0;
    uint64_t chunks = 0;
    float peak = 0;
    int most_voices = 0;
    for (;;) {
        XAUDIO2_VOICE_STATE state;
        source->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if (state.BuffersQueued >= kQueue - 1) {
            Sleep(2);
            continue;
        }
        std::memset(bins, 0, sizeof bins);
        Mix(bins);
        // with FCI_TRACE_VOICES: the level of every mixbin, logged with the statistics below
        static double bin_energy[kBins];
        if (g_trace_voices)
            for (int k = 0; k < kBins; k++)
                for (int i = 0; i < kChunk; i++) bin_energy[k] += double(bins[k][i]) * bins[k][i];
        // DirectSound's speaker mixbins: front left/right, center, LFE, back left/right; the GP's crosstalk
        // effect would send the 3D bins (XTLK front left/right, back left/right: 6 to 9) to the same speakers
        float* out = output[slot];
        // one pole DC blocker per channel, so no stray offset can push the mix into clipping
        static float dc_in[2], dc_out[2];
        static Reverb reverb;
        static const bool no_reverb = GetEnvironmentVariableA("FCI_NO_REVERB", nullptr, 0) > 0;
        for (int i = 0; i < kChunk; i++) {
            float b[11];
            for (int k = 0; k < 11; k++) b[k] = bins[k][i] * g_headroom[k];
            float c = 0.7071f * b[2] + 0.5f * b[3];
            float mixed[2] = {b[0] + c + b[4] + b[6] + b[8], b[1] + c + b[5] + b[7] + b[9]};
            if (!no_reverb) {
                float wet[2];
                reverb.Process(b[10], wet);
                mixed[0] += wet[0], mixed[1] += wet[1];
            }
            float y[2];
            for (int ch = 0; ch < 2; ch++) {
                y[ch] = mixed[ch] - dc_in[ch] + 0.995f * dc_out[ch];
                dc_in[ch] = mixed[ch], dc_out[ch] = y[ch];
            }
            // limiter instead of clipping: the gain follows the peak at once and recovers over ~150 ms, so loud
            // scenes get quieter without distorting
            constexpr float kCeiling = 0.9f;
            static const float release = std::exp(-1.0f / (0.15f * kRate));
            static float envelope = 0;
            envelope = std::max({std::fabs(y[0]), std::fabs(y[1]), envelope * release});
            float g = envelope > kCeiling ? kCeiling / envelope : 1.0f;
            for (int ch = 0; ch < 2; ch++) out[2 * i + ch] = y[ch] * g;
            peak = std::max({peak, std::fabs(out[2 * i]), std::fabs(out[2 * i + 1])});
        }
        most_voices = std::max(most_voices, g_mixed);
        if (++chunks % 500 == 0) {
            int active = 0;
            {
                std::lock_guard lock(g_lock);
                if (Reg(kVpvaddr))
                    for (uint32_t v = 0; v < kVoices; v++) active += (Field(v, PAR_STATE) & kStateActive) != 0;
            }
            rt::Log("apu: %llu s, up to %d voices, peak %.3f, lists %04X %04X %04X, %d active descriptors",
                    (unsigned long long)(chunks / 100), most_voices, peak, Reg(kTop[1]) & 0xFFFF, Reg(kTop[2]) & 0xFFFF,
                    Reg(kTop[3]) & 0xFFFF, active);
            peak = 0, most_voices = 0;
            if (g_trace_voices) {
                std::string levels;
                for (int k = 0; k < kBins; k++)
                    if (double rms = std::sqrt(bin_energy[k] / (500.0 * kChunk)); rms > 1e-4) {
                        char part[32];
                        snprintf(part, sizeof part, " %d:%.4f", k, rms);
                        levels += part;
                    }
                rt::Log("apu: mixbin rms%s", levels.c_str());
                std::fill(std::begin(bin_energy), std::end(bin_energy), 0.0);
            }
        }
        // FCI_RECORD_AUDIO=1: the output as raw 48 kHz stereo floats in audio.raw
        static FILE* record = GetEnvironmentVariableA("FCI_RECORD_AUDIO", nullptr, 0) ? std::fopen("audio.raw", "wb") : nullptr;
        if (record) std::fwrite(out, sizeof(float), kChunk * 2, record);
        XAUDIO2_BUFFER buffer{};
        buffer.AudioBytes = kChunk * 8;
        buffer.pAudioData = reinterpret_cast<const BYTE*>(out);
        source->SubmitSourceBuffer(&buffer);
        slot = (slot + 1) % kQueue;
    }
}

// FCI_TRACE_GP=1: what DirectSound changed in the GP scratch pages before each mailbox command
const bool g_trace_gp = GetEnvironmentVariableA("FCI_TRACE_GP", nullptr, 0) > 0;

void TraceScratch(uint32_t sges, uint32_t command) {
    constexpr int kPages = 32;
    static std::vector<uint32_t> last(kPages * 1024);
    static bool primed = false;
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    int logged = 0;
    for (int p = 0; p < kPages; p++) {
        uint32_t page = *reinterpret_cast<uint32_t*>(mem::Device(sges + 8 * p)) & ~0xFFFu;
        if (!page) break;
        const uint32_t* now = reinterpret_cast<const uint32_t*>(mem::Device(page));
        for (int i = 0; i < 1024; i++) {
            uint32_t& old = last[p * 1024 + i];
            if (old != now[i] && primed && logged++ < 64)
                rt::Log("apu: %.2f GP command %u: scratch %05X %08X -> %08X", t, command, p * 4096 + i * 4, old, now[i]);
            old = now[i];
        }
    }
    // and the GP's own memories (X, Y, P), which DirectSound writes through the register window
    static std::vector<uint32_t> last_mmio(0x10000 / 4);
    const uint32_t* mmio = reinterpret_cast<const uint32_t*>(rt::Host(kMmioBase + 0x30000));
    for (int i = 0; i < 0x10000 / 4; i++) {
        if (last_mmio[i] != mmio[i] && primed && logged++ < 64)
            rt::Log("apu: %.2f GP command %u: gp %05X %08X -> %08X", t, command, 0x30000 + i * 4, last_mmio[i], mmio[i]);
        last_mmio[i] = mmio[i];
    }
    if (!primed) rt::Log("apu: %.2f GP command %u: scratch snapshot taken", t, command);
    else if (logged > 64) rt::Log("apu: GP command %u: %d more changes", command, logged - 64);
    primed = true;
}

void GpThread() {
    uint32_t commands = 0;
    for (;;) {
        Sleep(1);
        uint32_t sges = Reg(kGpsaddr);
        if (!sges || !Reg(kGpReset)) continue;
        uint32_t page = *reinterpret_cast<uint32_t*>(mem::Device(sges)) & ~0xFFFu;
        auto* mailbox = reinterpret_cast<volatile uint32_t*>(mem::Device(page + kMailbox));
        if (uint32_t command = *mailbox) {
            if (commands++ < 16) rt::Log("apu: GP command %u acknowledged", command);
            if (g_trace_gp) TraceScratch(sges, command);
            *mailbox = 0;
        }
    }
}

// DirectSound's service routine runs on the APU's vector whenever an enabled status bit is pending
void IrqThread() {
    rt::GuestThread* ctx = rt::CreateServiceContext("apu interrupt");
    rt::BindServiceContext(ctx);
    rt::W8(ctx->kpcr + rt::kKpcrIrql, 0x1A);
    for (;;) {
        {
            std::unique_lock lock(g_lock);
            g_irq_wake.wait_for(lock, std::chrono::milliseconds(5),
                                [] { return (Reg(kIsts) & Reg(kIen) & ~kIstsGlobal) != 0; });
            if (!(Reg(kIsts) & Reg(kIen) & ~kIstsGlobal)) continue;
        }
        uint32_t object, routine, context;
        if (!kernel::GetInterrupt(kApuVector, &object, &routine, &context)) continue;
        rt::AcquireCpu(true);
        rt::GuestCall(ctx->cpu, routine, {object, context});
        rt::ReleaseCpu();
    }
}

}  // namespace

void Init() {
    g_start = std::chrono::steady_clock::now();
    std::fill(std::begin(g_headroom), std::end(g_headroom), 1.0f);
    mem::Commit(kMmioBase, kMmioSize);  // APU registers
    mem::Commit(0xFEC00000, 0x1000);    // AC'97 controller
    if (GetEnvironmentVariableA("FCI_TRACE_APU", nullptr, 0)) g_trace = std::fopen("apu.log", "w");
    // DirectSound on the host (src/xdk/dsound.cpp) mixes and outputs itself and never touches the APU: no mixer,
    // no output, no GP or interrupt service here. The registers stay mapped.
    if (xdk::HleAudio()) {
        rt::Log("apu: idle, DirectSound runs on the host (FCI_LLE_AUDIO=1 for the emulated APU)");
        return;
    }
    std::thread(GpThread).detach();
    std::thread(MixerThread).detach();
    std::thread(IrqThread).detach();
}

uint32_t Read(uint32_t offset, unsigned size) {
    if (offset == kFe + PIO_FREE) return 0x80;  // the method FIFO always has room
    if (offset == kEpAlive && size == 4 && (Reg(kEpReset) & 3) == 3) return kEpAliveValue;
    if (offset == kXgscnt)
        return uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - g_start)
                            .count() * 48 / 1000);
    uint32_t v = 0;
    std::memcpy(&v, rt::Host(kMmioBase + offset), size);
    return v;
}

void Write(uint32_t offset, uint32_t value, unsigned size) {
    if (g_trace) {
        static uint64_t traced = 0;
        if (traced++ < 1000000) std::fprintf(g_trace, "%u %05X = %08X\n", GetTickCount(), offset, value);
        if (traced % 256 == 0) std::fflush(g_trace);
    }
    if (offset >= kFe && offset < kFe + 0x2000 && size == 4) return Method(offset - kFe, value);
    if (offset == kIsts && size == 4) {
        std::lock_guard lock(g_lock);
        Reg(kIsts) &= ~value;
        if (!(Reg(kIsts) & ~kIstsGlobal)) Reg(kIsts) &= ~kIstsGlobal;
        return;
    }
    if (offset == kFectl && size == 4) {
        std::lock_guard lock(g_lock);
        Reg(kFectl) = value;
        TrapIdleVoice();  // running again: the next finished voice, if any
        return;
    }
    std::memcpy(rt::Host(kMmioBase + offset), &value, size);
    // the front end has executed every method before this write, so the fence lands at once
    if (offset == kFememdata && size == 4)
        if (uint32_t address = Reg(kFememaddr))
            if (uint8_t* p = mem::Device(address)) std::memcpy(p, &value, 4);
}

}  // namespace apu
