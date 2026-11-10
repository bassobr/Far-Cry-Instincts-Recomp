// Adapted from SCDA-XBERecomp (MIT, Copyright (c) 2026 TGP482, Joshhhuaaa): src/xdk/dsound.cpp.
// DirectSound (XDK 5849) on a host software mixer. Buffers and streams are read from guest memory as they play;
// the XDK's I3DL2 reverb as its DSP program computes it and the full HRTF tables of the library place 3D voices.
// FCI_LLE_AUDIO=1 sends every call back to the recompiled library on the emulated APU (src/audio/apu.cpp).
//
// FCI differences to SCDA: the recompiler copies tail jumps into the caller, so the IDirectSoundStream_* thunks
// the game calls carry their own copy of the CDirectSoundStream_* body and are replaced too; no voice link; the
// effects image comes from the disc (sounds_xb/dsstdfx.bin); the stream callback pump runs on a guest service
// context; FCI_TRACE_VOICES and FCI_NO_REVERB work as in the LLE path.
//
// FCI additions: the MCPX voice filter of SetFilter (DLS2 low pass, the I3DL2 one poles of 3D voices), Doppler, 3D
// settings deferred until CommitDeferredSettings, and the crosstalk crossfeed for an effects image whose decrypted
// crosstalk code holds the coefficients.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "app/settings.h"  // speech volume
#include "audio/hle_output.h"
#include "runtime/memory.h"
#include "runtime/scheduler.h"
#include "runtime/thread.h"
#include "xdk/hle.h"

using namespace kernel;
using xdk::HostPtr;

namespace {

bool HleOff() { return !xdk::HleAudio(); }

constexpr uint32_t kStreamVtable = SYM(DSound_StreamVtable);  // CDirectSoundStream XMediaObject vtable
constexpr uint32_t kStreamVtable2 = SYM(DSound_StreamVtable2);
constexpr uint32_t kPacketPending = 0x8000000A, kFlushed = 0x80004004;

// mix bins (DSMIXBIN) the host mixes by name
constexpr uint32_t kBinCentre = 2;
constexpr uint32_t kBinSub = 3;
constexpr uint32_t kBinCrosstalkFrontLeft = 6;
constexpr uint32_t kBinCrosstalkFrontRight = 7;
constexpr uint32_t kBinCrosstalkBackLeft = 8;
constexpr uint32_t kBinCrosstalkBackRight = 9;
constexpr uint32_t kBinFirstSend = 10;  // effect sends from here up
constexpr uint32_t kBinI3dl2 = 10;
constexpr uint32_t kSends = 1u << kBinI3dl2;  // the effects image mixes these into its reverb (state X:0x1540)
constexpr uint32_t kLanes = kBinFirstSend;     // the mixer's lanes are bins 0 to 9, speakers then crosstalk

constexpr uint32_t kEarLength = 64;  // past the longest interaural delay, 31 samples

// DSFILTERDESC.dwMode, the voice processor's filter mode (CFG_MISC bits 16-17)
constexpr uint32_t kFilterBypass = 0;
constexpr uint32_t kFilterDls2 = 1;
constexpr uint32_t kFilterParamEq = 2;
constexpr uint32_t kFilterMulti = 3;

const bool g_trace = GetEnvironmentVariableA("FCI_TRACE_VOICES", nullptr, 0) > 0;
const bool g_no_reverb = GetEnvironmentVariableA("FCI_NO_REVERB", nullptr, 0) > 0;
const bool g_no_cross_feed = GetEnvironmentVariableA("FCI_NO_CROSSFEED", nullptr, 0) > 0;
// FCI_TRACE_VOICE_LEVELS=1: every second, each voice heard with its settings and the level it adds to the stereo fold
const bool g_trace_levels = GetEnvironmentVariableA("FCI_TRACE_VOICE_LEVELS", nullptr, 0) > 0;
// FCI_TEST_SOLO=1: the 3D voices alone in the mix, the loudest one's Doppler shift logged every 0.25 s; "first":
// the first 3D stream alone (in FCI the opening cutscene's helicopter). Measurements only.
const int g_test_solo = [] {
    char text[16] = {};
    if (!GetEnvironmentVariableA("FCI_TEST_SOLO", text, sizeof text)) return 0;
    return std::strcmp(text, "first") == 0 ? 2 : 1;
}();
uint32_t g_solo = 0;  // FCI_TEST_SOLO=first: that stream

// ---- speech volume (GitHub #25): speech_volume (dB, -12 to +12, 0 = the console's mix) raises every voice whose data
// the game read from a language sound bank (sounds_xb\<language>\*.LS2: the player, the radio, characters, cutscenes)
// as a volume would, direct path and reverb send alike; music and effects (*.SS2) stay. The limiter catches the peaks.
std::atomic<float> g_speech_gain{1.0f};
float SpeechGain(float db) { return std::pow(10.0f, std::clamp(db, -12.0f, 12.0f) / 20.0f); }
// ---- end speech volume ----

struct Packet {
    uint32_t data, size, completed_ptr, status_ptr, context;
    uint32_t used = 0;
    bool speech = false;  // speech volume: its data came from a language sound bank (kernel::IsSpeechData)
};

struct Voice {
    uint32_t obj = 0;
    bool stream = false;
    int refs = 1;
    // format
    uint16_t tag = 1, channels = 1, bits = 16, align = 2;
    uint32_t rate = 48000;
    // buffer
    uint32_t data = 0, bytes = 0, loop_start = 0, loop_len = 0;
    uint32_t owned = 0;  // the buffer's own memory, made with it: freed by SetBufferData and Release
    bool playing = false, looping = false, paused = false;
    bool active = false, ending = false;  // stream: took a packet since it went quiet, quiet again once they run out
    double pos = 0;  // frames
    uint32_t freq = 0;
    // stream
    uint32_t max_packets = 1, callback = 0, context = 0;
    std::deque<Packet> packets;
    // levels
    float volume = 1, headroom = 1;
    uint8_t nbins = 0, bin[8] = {};
    int32_t bin_mb[32] = {};  // by mix bin
    bool is3d = false;
    bool speech = false;  // speech volume: a buffer's data, or a stream's last packet, came from a language bank
    uint32_t flags = 0;  // DSBUFFERDESC / DSSTREAMDESC dwFlags
    // 3D settings as the title sets them; a commit (CommitDeferredSettings, or the call itself with DS3D_IMMEDIATE)
    // applies them, as the library's settings and its 3D pass do
    struct {
        float p[3] = {}, vel[3] = {}, min_dist = 1, max_dist = 1e9f;
        uint32_t i3dl2[9] = {};  // DSI3DL2BUFFER, all zero until set
    } next;
    float p[3] = {}, vel[3] = {}, min_dist = 1, max_dist = 1e9f;
    float lf = 1;          // I3DL2 direct path level of a 3D voice (lows)
    float room = 1;        // I3DL2 reverb send of a 3D voice
    uint16_t i3dl2_fc[2] = {};  // its high-frequency one poles, direct and room (FC2, FC3 of the library's 3D pass)
    float doppler = 1;          // pitch ratio from the 3D pass
    // the MCPX voice filter (SetFilter): mode and Q, the title's coefficients, and what they come to
    uint32_t fmode = 0, fq = 0;
    uint16_t fc[4] = {};
    float svf_f = 1, svf_q = 1;           // DLS2: state variable low pass
    float pole_direct = 0, pole_send = 0; // I3DL2: one poles on the direct path and on the reverb send
    float svf_low[2] = {}, svf_band[2] = {}, direct_lp[2] = {}, send_lp[2] = {};
    float last_p[3] = {};  // FCI_TEST_DSOUND=doppler: where the voice was at the last commit
    int64_t last_commit = 0;
    float gain[2][kLanes] = {};  // the last block's levels, the next block ramps from them
    bool has_gain = false;
    // a mono 3D voice's interaural delay in samples and its recent samples
    float itd = 0;
    float ear[kEarLength] = {};
    uint32_t ear_pos = 0;
    // FCI_TRACE_VOICE_LEVELS: energy this voice added to the stereo fold since the last report
    double fold_energy = 0;
    uint32_t fold_frames = 0;
    // ADPCM block cache
    int64_t block = -1;
    int16_t decoded[65 * 8];
};

// a 3D voice as the library's 3D pass sees it (0x4CE757 into the slot builder 0x4D45CB in SCDA's image), as gains
struct Place {
    bool on = false;
    float att = 1;      // every bin
    float front = 1;    // crosstalk front pair
    float back = 0;     // crosstalk back pair, silent on a stereo console
    float centre = 1;   // centre bin
    float azimuth = 0;
    float elevation = 0;
};

struct Done {
    uint32_t voice, callback, context, packet_context, status;
};

std::recursive_mutex g_lock;
std::map<uint32_t, Voice> g_voices;
// the listener as the title sets it and as the last commit applied it (DS3DLISTENER defaults)
struct Listener {
    float p[3] = {}, vel[3] = {}, front[3] = {0, 0, 1}, top[3] = {0, 1, 0};
    float doppler = 1, rolloff = 1;
};
Listener g_listener, g_listener_next;
float g_i3dl2_next[12] = {};  // a deferred SetI3DL2Listener
bool g_i3dl2_pending = false;
float g_hf_reference = 5000;  // the applied I3DL2 listener's flHFReference, the I3DL2 one poles' reference
// Completed packets' callbacks, run as the XDK runs them: in DirectSoundDoWork on the title's thread, so they never
// overlap the title's own stream code. A stream flushed or released runs its own at once, before it is gone.
std::deque<Done> g_done;
std::condition_variable_any g_done_cv;
thread_local std::vector<Done> t_flushed;  // run by the STREAM call that flushed them, once g_lock is let go
std::atomic<int64_t> g_pumped{0};          // the last DirectSoundDoWork, steady ms
bool g_started = false;
uint32_t g_ds = 0;
bool g_headphones = false;  // IDirectSound_EnableHeadphones
// IDirectSound_SetMixBinHeadroom, a step halves the bin. The library starts bins 0..30 at 1 and 31 at 0
uint8_t g_bin_headroom[32] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                              1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0};

float Db(int32_t hundredths) { return hundredths <= -10000 ? 0.0f : std::pow(10.0f, hundredths / 2000.0f); }

float F32(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

// Xbox ADPCM plays 64 frames per 36 byte block (header sample + 63 nibbles, the last nibble is dropped), not
// MS IMA's 65: counting 65 stretches every ADPCM sound by 1/64.
constexpr uint32_t kAdpcmFrames = 64;

uint32_t FramesOf(const Voice& v, uint32_t bytes) {
    if (v.tag == 0x69) return bytes / (36u * v.channels) * kAdpcmFrames;
    return bytes / std::max<uint32_t>(v.align, 1);
}
uint32_t BytesOf(const Voice& v, uint32_t frames) {
    if (v.tag == 0x69) return frames / kAdpcmFrames * 36 * v.channels;
    return frames * v.align;
}

const int kIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
const int kStep[89] = {7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
                       31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
                       130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
                       544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
                       2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
                       9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

// one 36 byte per channel IMA block, 65 frames decoded, kAdpcmFrames played
void DecodeBlock(const uint8_t* b, int ch, int16_t* out) {
    for (int c = 0; c < ch; c++) {
        int pred = int16_t(b[c * 4] | b[c * 4 + 1] << 8), idx = std::min<int>(b[c * 4 + 2], 88);
        out[c] = int16_t(pred);
        const uint8_t* d = b + ch * 4;
        for (int k = 0; k < 8; k++)
            for (int j = 0; j < 8; j++) {
                int n = (d[(k * ch + c) * 4 + j / 2] >> ((j & 1) * 4)) & 15;
                int step = kStep[idx], diff = step >> 3;
                if (n & 1) diff += step >> 2;
                if (n & 2) diff += step >> 1;
                if (n & 4) diff += step;
                pred = std::clamp(n & 8 ? pred - diff : pred + diff, -32768, 32767);
                idx = std::clamp(idx + kIndex[n], 0, 88);
                out[(1 + k * 8 + j) * ch + c] = int16_t(pred);
            }
    }
}

// sample of channel c at frame f of memory starting at data
float Sample(Voice& v, uint32_t data, uint32_t f, int c) {
    c = std::min<int>(c, v.channels - 1);
    const uint8_t* p = static_cast<const uint8_t*>(HostPtr(data));
    if (v.tag == 0x69) {
        int64_t at = int64_t(data) + int64_t(f / kAdpcmFrames) * 36 * v.channels;
        if (at != v.block) {
            DecodeBlock(static_cast<const uint8_t*>(HostPtr(uint32_t(at))), std::min<int>(v.channels, 8), v.decoded);
            v.block = at;
        }
        return v.decoded[(f % kAdpcmFrames) * v.channels + c] / 32768.0f;
    }
    if (v.bits == 8) return (p[f * v.align + c] - 128) / 128.0f;
    int16_t s;
    std::memcpy(&s, p + f * v.align + c * 2, 2);
    return s / 32768.0f;
}

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// a guest event handle (XMEDIAPACKET.hCompletionEvent)
void SignalHandle(uint32_t handle) {
    if (auto o = kernel::FindObject(handle))
        if (void* w = o->Waitable()) SetEvent(w);
}

void Complete(Voice& v, Packet& pk, uint32_t status) {
    if (pk.completed_ptr) W32(pk.completed_ptr, status == 0 ? pk.size : pk.used);
    if (pk.status_ptr) W32(pk.status_ptr, status);
    if (v.callback) {
        g_done.push_back({v.obj, v.callback, v.context, pk.context, status});
        g_done_cv.notify_one();
    } else if (pk.context) {
        SignalHandle(pk.context);
    }
}

// The XDK's I3DL2 reverb as its DSP program runs it: a 24 kHz core, delays in samples at that rate
struct Reverb {
    struct Pole {
        float a = 0, b = 0, z = 0;
        float Run(float x) { return z = a * z + b * x + 1e-20f; }  // the offset keeps silence out of denormals
    };
    struct Line {
        std::vector<float> s;
        uint32_t w = 0;
        // a delay of the whole line reads its oldest sample
        float At(uint32_t delay) const { return s[(w + s.size() - delay % s.size()) % s.size()]; }
        void Put(float x) {
            s[w] = x;
            w = (w + 1) % uint32_t(s.size());
        }
    };
    struct Params {
        uint32_t refl, tap[4], in[4], chain[8], comb[4], echo;
        float early[4], late, diff[8], rest, cross;
        Pole in_lp, chain_lp[3], loop_lp[4], echo_lp[2];
    };
    static constexpr int kLen[14] = {6000, 6000, 6000, 6000, 680, 612, 640, 720, 1392, 1684, 2044, 2476, 24000, 24000};
    Line line[14];
    Params now, next;
    bool on = false, pending = false;
    float fade = 1, held[4] = {};
    uint32_t tick = 0;
    float last[12] = {};  // FCI: the listener last set, a repeat changes nothing
    double wet = 0;       // FCI_TRACE_VOICES: energy of the front pair's reverb output

    Reverb() {
        for (int i = 0; i < 14; i++) line[i].s.assign(kLen[i], 0.0f);
    }
    static float Lin(float mb) { return std::pow(10.0f, mb / 2000.0f); }
    // y = a * y1 + b * x: mb millibels at 0 Hz, hf more at ref Hz
    static Pole Design(int mb, int hf, float ref, float rate) {
        Pole p;
        if (mb <= -10000) return p;
        // 1.0 is past the DSP's fractions and comes out as -1
        p.b = mb ? Lin(float(mb)) : -1;
        if (hf >= 0) return p;
        p.b = Lin(float(mb));
        float g = Lin(2.0f * hf), c = std::cos(6.2831853f * ref / rate);
        p.a = (1 - g * c - std::sqrt(2 * g * (1 - c) - g * g * (1 - c * c))) / (1 - g);
        p.b *= 1 - p.a;
        return p;
    }
    // DSI3DL2LISTENER as 12 floats, the library's own conversion
    bool Set(const float* p) {
        if (on && std::memcmp(p, last, sizeof last) == 0) return false;
        std::memcpy(last, p, sizeof last);
        static const float kGain[4] = {0.57f, 0.52f, 0.54f, 0.6f}, kTap[4] = {0.1052f, 0.0673f, 0.1737f, 0.2404f};
        static const float kComb[4] = {0.2469f, 0.2209f, 0.2311f, 0.2608f};
        static const float kCombBase[4] = {0.003651f, 0.003515f, 0.003673f, 0.003946f};
        static const float kChain[8] = {0.040023f, 0.288186f, 0.603764f, 0.851905f,
                                        0.021224f, 0.37542f,  0.623583f, 0.759229f};
        static const float kSign[8] = {1, -1, -1, 1, 1, 1, -1, -1};
        static const int kInput[4] = {0, 58, 134, 232};
        auto al = [](int n) { return (n + 1) & ~3; };
        // taps are read in blocks of 4 samples, 16 back at least
        auto tap = [](int n) { return uint32_t(std::max((n + 3) & ~3, 16)); };
        float decay = std::max(p[3], 0.1f), ratio = std::max(p[4], 0.1f), d = std::clamp(p[9] * 0.01f, 0.0f, 1.0f);
        float sc = std::clamp((2 - p[10] * 0.01f) * (decay - 1) * 5, 1.0f, 60.0f);
        float f = sc >= 2.5f ? 1.0f : (sc - 0.8f) * 0.5882353f;
        // a loop of n samples loses 60 dB in the decay time, high frequencies sooner by the ratio
        auto loop = [&](int n) {
            float x = n / 24000.0f / decay * -6000.0f;
            int lf = int(x), hf = int(ratio < 1 ? x / ratio : x);
            return Design(lf, hf - lf, p[11], 24000);
        };
        Params q;
        q.in_lp = Design(0, int(p[1]), p[11], 48000);
        int refl = al(int(p[6] * 24000)), rev = int(p[8] * 24000), echo = int(f * 24000), prev = 0;
        q.refl = tap(refl);
        q.late = Lin(p[7] + p[0]);
        for (int k = 0; k < 4; k++) {
            q.early[k] = Lin(p[5] + p[0]) * kGain[k];
            q.tap[k] = tap(refl + al(int(rev * kTap[k])));
            q.in[k] = tap(refl + al(rev) + kInput[k]);
            q.comb[k] = tap(std::min(al(int((p[8] * kComb[k] + kCombBase[k]) * 24000)), kLen[4 + k] - 1));
            q.loop_lp[k] = loop(kLen[8 + k]);
            // echo times count from the input, a stage of the chain holds 250 ms
            int a = al(int(f * kChain[k] * 24000)), b = al(int(f * kChain[4 + k] * 24000)), m = std::max(a, b) - prev;
            float r = m > 5999 ? 5999.0f / m : 0;
            q.chain[k * 2] = tap(r ? al(int((a - prev) * r)) : a - prev);
            q.chain[k * 2 + 1] = tap(r ? al(int((b - prev) * r)) : b - prev);
            prev = b;
        }
        q.echo = tap(std::min(al(echo), 23999));
        for (int k = 0; k < 3; k++) q.chain_lp[k] = loop(echo >> 2);
        q.echo_lp[0] = q.echo_lp[1] = loop(echo);
        for (int k = 0; k < 8; k++) q.diff[k] = d * kSign[k];
        q.rest = d > 0 ? 1 - d : -1;
        q.cross = 0.2f * d;
        // a change fades out, swaps, fades in
        (on ? next : now) = q;
        pending = on;
        on = true;
        return true;
    }
    // one 24 kHz sample in, front left, front right, back left, back right out
    void Frame(float x, float o[4]) {
        static const float kFeed[4] = {-0.4f, 0.4f, -0.4f, -0.4f};
        Params& q = now;
        Line* l = line;
        float t = l[0].At(q.refl), e[4], in[4], c[8], u[4], sum = 0;
        for (int k = 0; k < 4; k++) {
            e[k] = t + l[0].At(q.tap[k]) + kFeed[k] * l[4 + k].At(q.comb[k]);
            in[k] = l[0].At(q.in[k]);
            c[k * 2] = l[k].At(q.chain[k * 2]);
            c[k * 2 + 1] = l[k].At(q.chain[k * 2 + 1]);
        }
        // the echoes, front and back
        float sa = c[1] + c[2] + c[4] + c[7], sb = c[0] + c[3] + c[5] + c[6];
        float ea = q.rest * sa + l[12].At(q.echo), eb = q.rest * sb + l[13].At(q.echo);
        for (int j = 0; j < 4; j++)
            sum += u[j] = l[8 + j].At(0) + q.diff[j * 2] * in[j & 1 ? 2 : 0] + q.diff[j * 2 + 1] * in[j & 1 ? 3 : 1];
        for (int j = 0; j < 4; j++) {
            o[j] = q.early[j] * e[j] + q.late * u[j] + q.late * 0.5f * (j < 2 ? ea : eb);
            l[4 + j].Put(e[j]);
            l[8 + j].Put(q.loop_lp[j].Run(u[j] - 0.5f * sum + q.cross * (sa + sb)));
        }
        l[0].Put(x);
        for (int k = 0; k < 3; k++) l[k + 1].Put(q.chain_lp[k].Run(c[k * 2 + 1]));
        l[12].Put(q.echo_lp[0].Run(ea));
        l[13].Put(q.echo_lp[1].Run(eb));
    }
    // adds the reverb of the 48 kHz send to the crosstalk bins, as the image's reverb writes them
    void Run(const float* send, float* out, uint32_t frames) {
        static const uint32_t kOut[4] = {kBinCrosstalkFrontLeft, kBinCrosstalkFrontRight, kBinCrosstalkBackLeft,
                                         kBinCrosstalkBackRight};
        for (uint32_t i = 0; on && i < frames; i++) {
            float x = now.in_lp.Run(send[i] * 0.5f), o[4];
            if (!(tick++ & 1)) {
                if (pending && (fade -= 1 / 128.0f) <= 0) {
                    now = next;
                    pending = false;
                    fade = 0;
                } else if (!pending) {
                    fade = std::min(fade + 1 / 128.0f, 1.0f);
                }
                Frame(x, o);
                for (int k = 0; k < 4; k++) held[k] = o[k] * fade;
            }
            for (int k = 0; k < 4; k++) out[i * kLanes + kOut[k]] += held[k];
            wet += double(held[0]) * held[0] + double(held[1]) * held[1];
        }
    }
} g_reverb;

// dsstdfx effect 4 on a pair of crosstalk bins: each side adds the other side's output of 11 samples back through a
// one pole filter (image coefficients 0x7645A1 and 0xF88313). Headphones turn it off.
struct CrossFeed {
    static constexpr uint32_t kDelay = 11;
    static constexpr float kPole = 0.924f;
    static constexpr float kFeed = -0.0585f;
    float s[2] = {};
    float past[2][kDelay] = {};
    uint32_t at = 0;
    void Run(float& left, float& right) {
        // the offset keeps silence out of denormals
        s[0] = kPole * s[0] + kFeed * past[1][at] + 1e-20f;
        s[1] = kPole * s[1] + kFeed * past[0][at] + 1e-20f;
        left += s[0];
        right += s[1];
        past[0][at] = left;
        past[1][at] = right;
        at = (at + 1) % kDelay;
    }
};
CrossFeed g_front_feed;
CrossFeed g_back_feed;
bool g_cross_feed = false;  // the title's effects image holds that crosstalk effect (HasCrossTalk)

// The library's full HRTF tables (DirectSoundUseFullHRTF): a filter number per azimuth step by elevation step, and
// the filters, 31 taps and the interaural delay in the last byte, the right ear's filter after the left's. In FCI's
// image the library's filter pick at 0x00580144 reads both (index 0x00598F60, filters 0x005879A0/0x005879C0).
constexpr uint32_t kHrtfIndex = SYM(DSound_HrtfIndex);
constexpr uint32_t kHrtfFilters = SYM(DSound_HrtfFilters);
constexpr uint32_t kHrtfFilterBytes = 32;
constexpr uint32_t kHrtfDelayByte = 31;
constexpr int kHrtfAzimuths = 61;     // 0 to 180 degrees in 3
constexpr int kHrtfElevations = 31;   // -90 to 90 degrees in 6
constexpr int kHrtfAzimuthStep = 3;
constexpr int kHrtfElevationStep = 6;
// the azimuth steps widen towards the poles: 3 up to 30 degrees of elevation, 6 up to 60, 12 beyond, none at 90
constexpr int kHrtfMidStep = 6;
constexpr int kHrtfPoleStep = 12;
constexpr int kHrtfLowBand = 30;
constexpr int kHrtfMidBand = 60;
constexpr int kHrtfPole = 90;

float Dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// Degrees from straight ahead, plus to the right. The library's octant estimate, not atan2: it picks the HRTF
// entry and the interaural delay.
float Azimuth(const float d[3], const float front[3], const float right[3]) {
    float ahead = Dot(d, front);
    float side = Dot(d, right);
    float a = std::fabs(ahead);
    float s = std::fabs(side);
    float deg = 0;
    if (a > s) deg = 45 * s / a;
    else if (s > 0) deg = 90 - 45 * a / s;
    if (ahead < 0) deg = 180 - deg;
    return side < 0 ? -deg : deg;
}

// degrees above the horizontal, the same estimate
float Elevation(const float d[3], const float front[3], const float right[3], const float top[3]) {
    float flat = std::hypot(Dot(d, front), Dot(d, right));
    float up = Dot(d, top);
    float u = std::fabs(up);
    float deg = 0;
    if (flat > u) deg = 45 * u / flat;
    else if (u > 0) deg = 90 - 45 * flat / u;
    return up < 0 ? -deg : deg;
}

// The library's filter pick. With speakers behind the listener a sound behind takes the entry of its mirror in front.
uint32_t HrtfEntry(float azimuth, float elevation, bool surround) {
    int half = kHrtfElevationStep / 2;
    int el = int(elevation < 0 ? elevation - half : elevation + half) / kHrtfElevationStep * kHrtfElevationStep;
    float az = std::fabs(azimuth);
    int step = kHrtfAzimuthStep;
    if (std::abs(el) > kHrtfLowBand) step = kHrtfMidStep;
    if (std::abs(el) > kHrtfMidBand) step = kHrtfPoleStep;
    int deg = int(az + step / 2.0f) / step * step;
    if (std::abs(el) == kHrtfPole) deg = 0;
    if (surround && deg > 90) deg = 180 - deg;
    int a = deg / kHrtfAzimuthStep;
    int e = (el + kHrtfPole) / kHrtfElevationStep;
    if (a < 0 || a >= kHrtfAzimuths) a = 0;
    if (e < 0 || e >= kHrtfElevations) e = 0;
    return static_cast<const uint16_t*>(HostPtr(kHrtfIndex))[a * kHrtfElevations + e];
}

// Interaural delay in samples, above 0 the left ear hears later: the last byte of the far ear's filter
float Itd(float azimuth, float elevation, bool surround) {
    if (!(std::fabs(azimuth) <= 180) || !(std::fabs(elevation) <= 90)) return 0;
    const uint8_t* filters = static_cast<const uint8_t*>(HostPtr(kHrtfFilters));
    float delay = filters[HrtfEntry(azimuth, elevation, surround) * kHrtfFilterBytes + kHrtfDelayByte];
    return azimuth < 0 ? -delay : delay;
}

// A voice whose fifth bin is the centre shares the front with it by whole degrees below 45: the library's tables
// (0x00587830 and 0x005878E8 in FCI) are 0.7071 cos^2(2a) and 0.8409 cos(2a)
void ShareCentre(Place& at) {
    constexpr float kRadiansPerDegree = 0.017453293f;
    constexpr int kCentreLimit = 45;
    int az = int(std::fabs(at.azimuth));
    int el = int(std::fabs(at.elevation));
    if (az >= kCentreLimit || el >= kCentreLimit) {
        at.centre = 0;
        return;
    }
    float both = std::cos(2 * az * kRadiansPerDegree) * std::cos(2 * el * kRadiansPerDegree);
    at.centre *= 0.7071f * both;
    at.front *= 1 - 0.5f * both * both;
}

// Share of a 3D voice for the crosstalk pair behind, by power: none to 45 degrees, half at 90, all from 135, drawn
// towards half and half within half a unit of the head
float BackShare(float azimuth, float elevation, float dist) {
    float a = std::fabs(azimuth) / 90;
    float e = std::fabs(elevation) / 90;
    float back = std::clamp(0.5f - (1 - a) * (1 - e), 0.0f, 1.0f);
    if (dist < 0.5f) back = ((back - 0.5f) * dist + 1) * 0.5f;
    return back;
}

// Where a 3D voice sits for the library's 3D pass: distance attenuation and direction
Place Locate(const Voice& v) {
    Place at;
    if (!v.is3d) return at;
    at.on = true;
    const Listener& l = g_listener;
    float d[3] = {v.p[0] - l.p[0], v.p[1] - l.p[1], v.p[2] - l.p[2]};
    float dist = std::sqrt(Dot(d, d));
    // the library's rolloff (0x0057FDB9): min / (min + rolloff (distance - min)), the distance held at the maximum
    if (dist > v.min_dist && v.min_dist > 0)
        at.att = 1 / (1 + l.rolloff * (std::min(dist, v.max_dist) / v.min_dist - 1));
    const float* front = l.front;
    const float* top = l.top;
    float right[3] = {top[1] * front[2] - top[2] * front[1], top[2] * front[0] - top[0] * front[2],
                      top[0] * front[1] - top[1] * front[0]};
    at.azimuth = Azimuth(d, front, right);
    at.elevation = Elevation(d, front, right, top);
    // surround without headphones splits the crosstalk pairs, a stereo console plays the front alone
    if (audio::IsSurround() && !g_headphones) {
        float back = BackShare(at.azimuth, at.elevation, dist);
        at.front = std::sqrt(1 - back);
        at.back = std::sqrt(back);
    }
    // the centre takes the front's share too
    at.centre = at.front;
    if (v.nbins >= 5 && v.bin[4] == kBinCentre) ShareCentre(at);
    return at;
}

// The library's level of a 3D voice in a bin: the crosstalk pairs and the centre take their shares, every bin takes
// the distance attenuation. Left and right come from the HRTF, both bins of a pair get the same.
float BinScale(const Place& at, uint32_t bin) {
    if (!at.on) return 1.0f;
    if (bin == kBinCentre) return at.att * at.centre;
    if (bin == kBinCrosstalkFrontLeft || bin == kBinCrosstalkFrontRight) return at.att * at.front;
    if (bin == kBinCrosstalkBackLeft || bin == kBinCrosstalkBackRight) return at.att * at.back;
    return at.att;
}

// bins 0 to 5 are the speakers, the image's crosstalk effect adds 6 to 9 into the front and back pairs (AddCrosstalk)
static_assert(kBinSub == audio::Sub && kBinCentre == audio::Centre && audio::BackRight == 5);
static_assert(audio::kChannels == kBinCrosstalkFrontLeft);

// a slot onto the speakers, effect sends on none. A voice heard through effect sends alone plays on the front pair.
void AddToSpeakers(float g[kLanes], uint32_t b, float x, bool speakers) {
    if (!speakers) {
        g[audio::FrontLeft] = std::max(g[audio::FrontLeft], x);
        g[audio::FrontRight] = std::max(g[audio::FrontRight], x);
        return;
    }
    if (b >= kBinFirstSend) return;
    g[b] = std::max(g[b], x);
}

// gain of voice channel c on each speaker, w its share for the reverb. A hardware voice holds two channels and owns
// a run of slots, which alternate between them.
void Levels(const Voice& v, const Place& at, float g[2][kLanes], float w[2]) {
    std::memset(g, 0, 2 * kLanes * sizeof(float));
    w[0] = w[1] = 0;
    static const uint8_t kFront[2] = {0, 1};
    const uint8_t* bin = v.nbins ? v.bin : kFront;
    uint32_t n = v.nbins ? v.nbins : 2, voices = (std::max<uint32_t>(v.channels, 1) - 1) / 2 + 1;
    uint32_t per = std::max<uint32_t>(n / voices, 2), step = v.channels == 1 ? 1 : 2;
    bool speakers = false;
    // muted speaker bins do not count: such a voice is heard through its effect send
    for (uint32_t s = 0; s < n; s++)
        speakers |= bin[s] < kBinFirstSend && bin[s] != kBinSub && v.bin_mb[bin[s]] > -10000;
    for (uint32_t c = 0; c < 2; c++)
        for (uint32_t s = c; c < v.channels && s < per && s < n; s += step) {
            uint32_t b = bin[s];
            float x = Db(v.bin_mb[b]) * v.volume * v.headroom * BinScale(at, b);
            if (kSends >> b & 1) {
                if (at.on) x *= v.room;
                w[c] += std::ldexp(x, -g_bin_headroom[b]);
                continue;
            }
            if (at.on) x *= v.lf;
            if (speakers && b >= kBinFirstSend) continue;  // other effect sends
            AddToSpeakers(g[c], b, x, speakers);
        }
    // the game can place a sound at NaN: that voice alone goes silent, where its NaN gain would turn the whole mix
    // to NaN, which plays as silence
    for (int c = 0; c < 2; c++) {
        for (uint32_t k = 0; k < kLanes; k++)
            if (!std::isfinite(g[c][k])) g[c][k] = 0;
        if (!std::isfinite(w[c])) w[c] = 0;
    }
}

// 3D levels follow the listener once a game frame: stepped per block, a fast turn clicks
void UpdateRamp(Voice& v, const float g[2][kLanes], uint32_t frames, float dg[2][kLanes]) {
    if (!v.has_gain) std::memcpy(v.gain, g, sizeof(v.gain));
    v.has_gain = true;
    for (int c = 0; c < 2; c++)
        for (uint32_t k = 0; k < kLanes; k++) dg[c][k] = (g[c][k] - v.gain[c][k]) / float(frames);
}

// the 3D voice's recent samples, the ear further from the sound reads them later
void PushEar(Voice& v, float x) {
    v.ear[v.ear_pos % kEarLength] = x;
    v.ear_pos++;
}
float EarAt(const Voice& v, float delay) {
    float back = std::max(delay, 0.0f);
    uint32_t whole = uint32_t(back);
    float part = back - float(whole);
    float now = v.ear[(v.ear_pos - 1 - whole) % kEarLength];
    float before = v.ear[(v.ear_pos - 2 - whole) % kEarLength];
    return now + (before - now) * part;
}

// a stream goes quiet: a started one loses its pause
void Deactivate(Voice& v) {
    if (v.active) v.active = v.paused = v.ending = false;
}

// where a frame and the one after it are read from, for interpolation
struct Cursor {
    uint32_t data;
    uint32_t f;
    uint32_t next_data;
    uint32_t next_f;
    bool speech = false;  // speech volume: the frame is speech
};

// FCI: guest memory below 64 KB is the null page, which the fault handler does not commit
bool Readable(uint32_t va) { return va >= 0x10000 && va < rt::kMmioBase; }

// the stream's frame at its position, none once its packets ran out
std::optional<Cursor> StreamFrame(Voice& v) {
    uint32_t f = uint32_t(v.pos);
    while (!v.packets.empty() && (f >= FramesOf(v, v.packets.front().size) || !Readable(v.packets.front().data))) {
        Packet pk = v.packets.front();
        v.packets.pop_front();
        pk.used = pk.size;
        Complete(v, pk, 0);
        v.pos = std::max(0.0, v.pos - FramesOf(v, pk.size));
        f = uint32_t(v.pos);
    }
    if (v.packets.empty()) {
        v.pos = 0;
        if (v.ending) Deactivate(v);
        return std::nullopt;
    }
    Packet& pk = v.packets.front();
    Cursor at = {pk.data, f, pk.data, f, pk.speech};
    if (f + 1 < FramesOf(v, pk.size)) {
        at.next_f = f + 1;
    } else if (v.packets.size() > 1 && Readable(v.packets[1].data)) {
        at.next_data = v.packets[1].data;
        at.next_f = 0;
    }
    return at;
}

// the buffer's frame at its position, none once it stopped
std::optional<Cursor> BufferFrame(Voice& v) {
    uint32_t f = uint32_t(v.pos);
    uint32_t end = FramesOf(v, v.bytes);
    if (v.looping && v.loop_len) end = FramesOf(v, v.loop_start + v.loop_len);
    uint32_t start = 0;
    if (v.loop_len) start = FramesOf(v, v.loop_start);
    if (f >= end) {
        if (!v.looping || !end || start >= end) {
            v.playing = false;
            v.pos = 0;
            return std::nullopt;
        }
        v.pos -= end - start;
        if (v.pos >= end) v.pos = start;
        f = uint32_t(v.pos);
    }
    if (!Readable(v.data)) return std::nullopt;
    Cursor at = {v.data, f, v.data, f, v.speech};
    if (f + 1 < end) {
        at.next_f = f + 1;
    } else if (v.looping && end) {
        at.next_f = start;
    }
    return at;
}

// the voice's channels at its position, interpolated
void ReadFrame(Voice& v, const Cursor& at, float x[2]) {
    float t = float(v.pos - at.f);
    for (int c = 0; c < 2; c++) {
        float now = Sample(v, at.data, at.f, c);
        x[c] = now + (Sample(v, at.next_data, at.next_f, c) - now) * t;
    }
}

// The MCPX voice filter on the voice's 48 kHz samples: what the speaker and crosstalk bins (direct) and the reverb
// send take. DLS2 is a state variable low pass, both take it. MULTI runs the I3DL2 one poles, FC2 on the direct path
// and FC3 on the send; their FCA half (a parametric EQ) and PARAMEQ are not reproduced, FCI sets neither.
void VoiceFilter(Voice& v, const float x[2], float direct[2], float send[2]) {
    int n = v.channels > 1 ? 2 : 1;
    for (int c = 0; c < 2; c++) direct[c] = send[c] = x[c];
    if (v.fmode == kFilterDls2) {
        for (int c = 0; c < n; c++) {
            // Chamberlin: F = 2 sin(pi fc / 48000), damping q = 1 / Q; the offset keeps silence out of denormals
            float low = v.svf_low[c] + v.svf_f * v.svf_band[c];
            float high = x[c] - low - v.svf_q * v.svf_band[c];
            v.svf_band[c] += v.svf_f * high + 1e-20f;
            v.svf_low[c] = low;
            direct[c] = send[c] = low;
        }
    } else if (v.fmode == kFilterMulti) {
        // a pole of 0 passes the samples, the state follows them as the hardware's would
        for (int c = 0; c < n; c++) {
            direct[c] = v.direct_lp[c] = v.pole_direct * v.direct_lp[c] + (1 - v.pole_direct) * x[c] + 1e-20f;
            send[c] = v.send_lp[c] = v.pole_send * v.send_lp[c] + (1 - v.pole_send) * x[c] + 1e-20f;
        }
    }
    if (n == 1) {
        direct[1] = direct[0];
        send[1] = send[0];
    }
}

// the voice's playback rate on the 48 kHz mix: its frequency, the Doppler shift on top, the pitch held to the
// voice processor's range (0x00582DEB: -32767 to 8191 in 1/4096 octaves from 48 kHz)
double Rate(const Voice& v) {
    double rate = double(v.freq ? v.freq : v.rate) / audio::kRate;
    if (v.doppler == 1) return rate;
    constexpr double kMax = 3.99966;  // 2^(8191 / 4096)
    return std::min(rate * v.doppler, kMax);
}

// A mono 3D voice leaves the HRTF as two ears on the left and right speakers, the far one delayed. The centre and
// sub are not HRTF slots and take it as it is.
void EarFrame(Voice& v, float x, float frame[kLanes]) {
    PushEar(v, x);
    float left = EarAt(v, v.itd);
    float right = EarAt(v, -v.itd);
    frame[audio::FrontLeft] = left;
    frame[audio::BackLeft] = left;
    frame[audio::FrontRight] = right;
    frame[audio::BackRight] = right;
    frame[kBinCrosstalkFrontLeft] = left;
    frame[kBinCrosstalkBackLeft] = left;
    frame[kBinCrosstalkFrontRight] = right;
    frame[kBinCrosstalkBackRight] = right;
    frame[audio::Centre] = x;
    frame[audio::Sub] = x;
}

// adds the voice to the lane frames and its reverb share to the send
void MixVoice(Voice& v, float* out, float* send, uint32_t frames) {
    Place at = Locate(v);
    float g[2][kLanes];
    float w[2];
    Levels(v, at, g, w);
    bool ears = at.on && v.channels == 1;
    float itd = Itd(at.azimuth, at.elevation, audio::IsSurround());
    if (!v.has_gain) v.itd = itd;
    float itd_step = (itd - v.itd) / float(frames);
    float dg[2][kLanes];
    UpdateRamp(v, g, frames, dg);
    double rate = Rate(v);
    const float speech = g_speech_gain.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < frames; i++) {
        std::optional<Cursor> cursor = v.stream ? StreamFrame(v) : BufferFrame(v);
        if (!cursor) break;
        if (cursor->data - 0x80000000u < 0x100000u)
            LOG_FIRST(8, "dsound: %s %08X plays from %08X frame %u (%u bytes)", v.stream ? "stream" : "buffer", v.obj,
                      cursor->data, cursor->f, v.stream ? v.packets.front().size : v.bytes);
        float x[2], d[2], r[2];
        ReadFrame(v, *cursor, x);
        if (cursor->speech) x[0] *= speech, x[1] *= speech;  // speech volume
        VoiceFilter(v, x, d, r);
        send[i] += r[0] * w[0] + r[1] * w[1];  // the send leaves before the HRTF
        float ear[kLanes];
        if (ears) {
            v.itd += itd_step;
            EarFrame(v, d[0], ear);
        }
        float* o = out + i * kLanes;
        float added[kLanes];
        for (uint32_t k = 0; k < kLanes; k++) {
            v.gain[0][k] += dg[0][k];
            v.gain[1][k] += dg[1][k];
            added[k] = ears ? ear[k] * v.gain[0][k] : d[0] * v.gain[0][k] + d[1] * v.gain[1][k];
            o[k] += added[k];
        }
        if (g_trace_levels) {
            // the stereo fold of hle_output.cpp, the crosstalk bins on their speakers
            float left = added[0] + added[6] + 0.707f * (added[2] + added[4] + added[8]);
            float right = added[1] + added[7] + 0.707f * (added[2] + added[5] + added[9]);
            v.fold_energy += double(left) * left + double(right) * right;
            v.fold_frames++;
        }
        v.pos += rate;
    }
    std::memcpy(v.gain, g, sizeof(g));
    v.itd = itd;
}

// the speaker frames from the lane frames: the crosstalk bins through the image's crosstalk effect onto the front
// and back pairs
void AddCrosstalk(const float* lanes, float* out, uint32_t frames) {
    for (uint32_t i = 0; i < frames; i++) {
        const float* m = lanes + i * kLanes;
        float* o = out + i * audio::kChannels;
        float front_left = m[kBinCrosstalkFrontLeft];
        float front_right = m[kBinCrosstalkFrontRight];
        float back_left = m[kBinCrosstalkBackLeft];
        float back_right = m[kBinCrosstalkBackRight];
        if (g_cross_feed && !g_headphones) {
            g_front_feed.Run(front_left, front_right);
            g_back_feed.Run(back_left, back_right);
        }
        std::memcpy(o, m, audio::kChannels * sizeof(float));
        o[audio::FrontLeft] += front_left;
        o[audio::FrontRight] += front_right;
        o[audio::BackLeft] += back_left;
        o[audio::BackRight] += back_right;
    }
}

// FCI_TRACE_VOICES: the 3D and filter calls as the title makes them
struct CallStats {
    uint32_t deferred = 0, immediate = 0;
};
std::map<std::string, CallStats> g_call_stats;
void CountApply(const char* what, uint32_t apply) {
    if (!g_trace) return;
    CallStats& s = g_call_stats[what];
    (apply & 1 ? s.deferred : s.immediate)++;
}
void TraceCalls() {
    for (auto& [name, s] : g_call_stats)
        rt::Log("dsound: %s: %u deferred, %u immediate", name.c_str(), s.deferred, s.immediate);
    g_call_stats.clear();
}

// FCI_TRACE_VOICES: what plays, every 5 s
// The reverb tail: its output in blocks whose send is silent, after a block that was not
void TraceMix(uint32_t frames, const float* send, double wet) {
    static uint64_t mixed = 0;
    static double send_energy = 0, tail_energy = 0;
    static uint32_t tail_blocks = 0, since_send = ~0u;
    double block = 0;
    for (uint32_t i = 0; i < frames; i++) block += double(send[i]) * send[i];
    send_energy += block;
    since_send = block > 1e-12 ? 0 : since_send + 1;
    if (since_send > 0 && since_send <= 100) {  // up to a second after the send went quiet
        tail_energy += wet;
        tail_blocks++;
    }
    mixed += frames;
    if (mixed < 5 * audio::kRate) return;
    int buffers = 0, streams = 0, three_d = 0, packets = 0, modes[4] = {}, high_cut = 0, shifted = 0;
    float lowest = 1, highest = 1;
    const Voice* strongest = nullptr;
    for (auto& [obj, v] : g_voices) {
        bool on = v.stream ? !v.packets.empty() && !v.paused : v.playing && !v.paused;
        if (!on) continue;
        (v.stream ? streams : buffers)++;
        three_d += v.is3d;
        packets += int(v.packets.size());
        modes[v.fmode & 3]++;
        high_cut += v.fmode == kFilterMulti && (v.pole_direct > 0 || v.pole_send > 0);
        shifted += v.doppler != 1;
        lowest = std::min(lowest, v.doppler);
        highest = std::max(highest, v.doppler);
        if (v.doppler != 1 && (!strongest || std::fabs(std::log(v.doppler)) > std::fabs(std::log(strongest->doppler))))
            strongest = &v;
    }
    if (strongest) {
        const Voice& v = *strongest;
        float d[3] = {v.p[0] - g_listener.p[0], v.p[1] - g_listener.p[1], v.p[2] - g_listener.p[2]};
        float dist = std::sqrt(Dot(d, d));
        float rel[3] = {v.vel[0] - g_listener.vel[0], v.vel[1] - g_listener.vel[1], v.vel[2] - g_listener.vel[2]};
        rt::Log("dsound: strongest Doppler now: %08X at %.1f m moving %.1f m/s away (doppler factor %.2f): pitch x%.4f",
                v.obj, dist, dist > 0 ? Dot(rel, d) / dist : 0.0f, g_listener.doppler, v.doppler);
    }
    rt::Log("dsound: filters playing: %d bypass, %d DLS2, %d parametric EQ, %d MULTI (%d with an I3DL2 high cut); "
            "%d Doppler shifted (%.4f to %.4f)", modes[0], modes[1], modes[2], modes[3], high_cut, shifted, lowest,
            highest);
    rt::Log("dsound: %d buffers and %d streams playing (%d 3D, %d packets queued), %zu voices, reverb send rms %.4f, "
            "reverb out rms %.4f%s",
            buffers, streams, three_d, packets, g_voices.size(), std::sqrt(send_energy / double(mixed)),
            std::sqrt(g_reverb.wet / double(2 * mixed)), g_reverb.on ? "" : " (no listener yet)");
    if (tail_blocks)
        rt::Log("dsound: reverb tail rms %.5f over %u blocks of silent send", std::sqrt(tail_energy / (2.0 * tail_blocks * frames)),
                tail_blocks);
    TraceCalls();
    g_reverb.wet = 0;
    tail_energy = 0;
    tail_blocks = 0;
    mixed = 0;
    send_energy = 0;
}

std::string Bins(const Voice& v);

// FCI_TRACE_VOICE_LEVELS: every second the voices heard, their volume, headroom, bins (mB), distance gain and I3DL2
// direct level, and the rms they added to the stereo fold (dBFS over the frames they played), loudest first
void TraceLevels(uint32_t frames) {
    static uint64_t mixed = 0;
    mixed += frames;
    if (mixed < audio::kRate) return;
    mixed = 0;
    std::vector<std::pair<double, const Voice*>> heard;
    for (auto& [obj, v] : g_voices)
        if (v.fold_frames) heard.push_back({v.fold_energy / (2.0 * v.fold_frames), &v});
    std::sort(heard.begin(), heard.end(), [](auto& a, auto& b) { return a.first > b.first; });
    for (auto& [ms, vp] : heard) {
        const Voice& v = *vp;
        Place at = Locate(v);
        uint32_t data = v.stream ? (v.packets.empty() ? 0 : v.packets.front().data) : v.data;
        auto db = [](float x) { return 20 * std::log10(std::max(x, 1e-6f)); };
        rt::Log("dsound: level %08X %s %u ch %u Hz %s%s: %.1f dBFS over %.2f s, volume %.1f dB headroom %.1f dB, bins%s, "
                "distance %.1f dB lf %.1f dB, data %08X",
                v.obj, v.stream ? "stream" : "buffer", v.channels, v.freq ? v.freq : v.rate, v.is3d ? "3D" : "2D",
                v.speech ? " speech" : "", 10 * std::log10(std::max(ms, 1e-12)), v.fold_frames / double(audio::kRate), db(v.volume), db(v.headroom),
                Bins(v).c_str(), db(at.att), db(v.lf), data);
    }
    for (auto& [obj, v] : g_voices) v.fold_energy = 0, v.fold_frames = 0;
}

// the device clips once, after the fold to stereo sums the speakers
void Render(float* out, uint32_t frames) {
    // the output thread alone renders
    static std::vector<float> lanes;
    static std::vector<float> send;
    lanes.assign(frames * kLanes, 0.0f);
    send.assign(frames, 0.0f);
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    for (auto& [obj, v] : g_voices) {
        if (v.paused || (!v.stream && !v.playing)) continue;
        if (g_test_solo && (!v.is3d || (g_test_solo == 2 && obj != g_solo))) {
            // played on, unheard: its packets still complete
            static std::vector<float> unheard_lanes, unheard_send;
            unheard_lanes.assign(frames * kLanes, 0.0f);
            unheard_send.assign(frames, 0.0f);
            MixVoice(v, unheard_lanes.data(), unheard_send.data(), frames);
            continue;
        }
        MixVoice(v, lanes.data(), send.data(), frames);
    }
    if (g_test_solo) {
        static int blocks = 0;
        const Voice* loudest = nullptr;
        float most = 0;
        for (auto& [obj, v] : g_voices) {
            if (!v.is3d || v.paused || (!v.stream && !v.playing) || (v.stream && v.packets.empty())) continue;
            if (g_test_solo == 2 && obj != g_solo) continue;
            Place at = Locate(v);
            float level = v.volume * v.lf * at.att;
            if (level > most) {
                most = level;
                loudest = &v;
            }
        }
        if (loudest && blocks++ % 25 == 0)
            rt::Log("dsound: loudest 3D voice %08X level %.3f, pitch x%.4f", loudest->obj, most, loudest->doppler);
    }
    if (g_trace_levels) TraceLevels(frames);
    double wet_before = g_reverb.wet;
    if (!g_no_reverb) g_reverb.Run(send.data(), lanes.data(), frames);
    if (g_trace) TraceMix(frames, send.data(), g_reverb.wet - wet_before);
    AddCrosstalk(lanes.data(), out, frames);
}

void Run(rt::Cpu& c, const std::vector<Done>& done) {
    for (const Done& d : done) rt::GuestCall(c, d.callback, {d.context, d.packet_context, d.status});
}

void Start() {
    if (g_started) return;
    g_started = true;
    xdk::SetSpeechVolume(settings::Float("speech_volume", 0));  // speech volume
    audio::StartOutput(Render);
    // for a title that does not call DirectSoundDoWork for a while (loading): its callbacks come from here instead,
    // on a guest context of their own that takes the CPU like any guest thread
    std::thread([] {
        rt::GuestThread* t = rt::CreateServiceContext("dsound callbacks");
        rt::BindServiceContext(t);
        constexpr int64_t kPumpMs = 250;
        for (;;) {
            std::vector<Done> done;
            {
                std::unique_lock<std::recursive_mutex> lock(g_lock);
                g_done_cv.wait_for(lock, std::chrono::milliseconds(50));
                if (NowMs() - g_pumped < kPumpMs || g_done.empty()) continue;
                done.assign(g_done.begin(), g_done.end());
                g_done.clear();
            }
            rt::AcquireCpu();
            Run(t->cpu, done);
            rt::ReleaseCpu();
        }
    }).detach();
}

void ReadFormat(Voice& v, uint32_t wfx) {
    if (!wfx) return;
    v.tag = R16(wfx);
    v.channels = std::max<uint16_t>(R16(wfx + 2), 1);
    v.rate = R32(wfx + 4);
    v.align = std::max<uint16_t>(R16(wfx + 12), 1);
    v.bits = R16(wfx + 14);
    if (v.tag != 1 && v.tag != 0x69) LOG_FIRST(8, "dsound: format tag %X not supported", v.tag);
}

// DSMIXBINS: the voice's bins and their volumes
void SetBins(Voice& v, uint32_t mixbins) {
    v.nbins = 0;  // none listed: front left and right
    if (!mixbins) return;
    uint32_t n = std::min<uint32_t>(R32(mixbins), 8), pairs = R32(mixbins + 4);
    for (uint32_t i = 0; i < n; i++) {
        v.bin[i] = R32(pairs + i * 8) & 31;
        v.bin_mb[v.bin[i]] = int32_t(R32(pairs + i * 8 + 4));
    }
    v.nbins = uint8_t(n);
}
// volumes only, the bins stay
void SetBinVolumes(Voice& v, uint32_t mixbins) {
    if (!mixbins) return;
    uint32_t n = std::min<uint32_t>(R32(mixbins), 32), pairs = R32(mixbins + 4);
    for (uint32_t i = 0; i < n; i++) v.bin_mb[R32(pairs + i * 8) & 31] = int32_t(R32(pairs + i * 8 + 4));
}
// 600 mB unless 3D or a mix in or effect in buffer
float DefaultHeadroom(uint32_t flags) { return flags & 0x382010 ? 1.0f : Db(-600); }

Voice* Find(uint32_t obj) {
    auto it = g_voices.find(obj);
    return it == g_voices.end() ? nullptr : &it->second;
}

std::string Bins(const Voice& v) {
    std::string s;
    for (uint32_t i = 0; i < v.nbins; i++) {
        char part[24];
        snprintf(part, sizeof part, " %u:%d", v.bin[i], v.bin_mb[v.bin[i]]);
        s += part;
    }
    return s.empty() ? " none" : s;
}

void TraceVoice(const char* what, const Voice& v) {
    if (!g_trace) return;
    rt::Log("dsound: %s %08X %s tag %X %u ch %u Hz %s, bins%s, volume %.3f headroom %.3f, data %08X+%X", what, v.obj,
            v.stream ? "stream" : "buffer", v.tag, v.channels, v.freq ? v.freq : v.rate, v.is3d ? "3D" : "2D",
            Bins(v).c_str(), v.volume, v.headroom, v.stream && !v.packets.empty() ? v.packets.back().data : v.data,
            v.stream && !v.packets.empty() ? v.packets.back().size : v.bytes);
}

// FCI_TRACE_VOICES: a filter as the title sets it, with the title's caller (the wrapper at 0x00310E80 keeps it 0x50
// bytes up the stack)
void TraceFilter(const Voice& v, uint32_t desc, uint32_t esp) {
    if (!g_trace) return;
    uint32_t ret = R32(esp), outer = 0;
    if (ret == 0x00310F37 || ret == 0x00310F61) outer = R32(esp + 0x50);
    uint32_t d[6] = {};
    if (desc)
        for (int i = 0; i < 6; i++) d[i] = R32(desc + 4 * i);
    char key[160];
    snprintf(key, sizeof key, "filter %s %s from %08X/%08X: mode %u q %u coefficients %04X %04X %04X %04X",
             v.stream ? "stream" : "buffer", v.is3d ? "3D" : "2D", ret, outer, d[0], d[1], d[2], d[3], d[4], d[5]);
    if (g_call_stats[key].immediate++ == 0) rt::Log("dsound: %s (first, %08X)", key, v.obj);
}

// FCI_TEST_DSOUND, measurements only: "dls2:<cutoff Hz>:<resonance dB>" puts every voice's filter (at creation and
// at each SetFilter) into DLS2 as the title's own conversion (0x00310F90) would set it, "obstruction:<mB>" obstructs every I3DL2 source,
// "doppler[:<factor>]" gives the listener and 3D voices the velocities of their moves between commits (FCI sets the
// doppler factor 0 and every velocity 0) and the factor (default 1)
struct TestOverride {
    enum Kind { kNone, kDls2, kObstruction, kDoppler } kind = kNone;
    float a = 0, b = 0;
    char text[64] = {};
    TestOverride() {
        if (!GetEnvironmentVariableA("FCI_TEST_DSOUND", text, sizeof text)) return;
        if (std::sscanf(text, "dls2:%f:%f", &a, &b) == 2) kind = kDls2;
        else if (std::sscanf(text, "obstruction:%f", &a) == 1) kind = kObstruction;
        else if (std::strncmp(text, "doppler", 7) == 0) {
            kind = kDoppler;
            a = 1;
            std::sscanf(text, "doppler:%f", &a);
        }
    }
} const g_test;

// the filter's coefficients as the voice processor uses them
void UpdateFilter(Voice& v) {
    v.svf_f = std::clamp(std::exp2(int16_t(v.fc[0]) / 4096.0f), 1 / 256.0f, 1.0f);
    v.svf_q = std::clamp(v.fc[1] / 32768.0f, 0.05f, 1.0f);
    uint16_t direct = v.is3d ? v.i3dl2_fc[0] : v.fc[2];
    uint16_t room = v.is3d ? v.i3dl2_fc[1] : v.fc[3];
    v.pole_direct = direct / 65536.0f;
    v.pole_send = room / 65536.0f;
}

// CMcpxVoiceClient_SetFilter (0x0058344F): mode and Q into the voice's CFG_MISC, coefficients 0 and 1 into FCA, 2 and
// 3 into FCB unless the voice is 3D, whose FCB the 3D pass fills. A 3D voice left in bypass runs MULTI for I3DL2.
// FC0 is the DLS2 cutoff as 4096 log2(2 sin(pi fc / 48000)), FC1 its damping 1/Q in 1/32768 (the title's conversion
// at 0x00311020 reproduces both); FC2/FC3 are one pole coefficients in 1/65536.
void SetFilter(Voice& v, uint32_t desc) {
    if (desc) {
        v.fmode = R32(desc) & 3;
        v.fq = R32(desc + 4) & 7;
        for (int i = 0; i < 4; i++) v.fc[i] = uint16_t(R32(desc + 8 + i * 4));
    }
    if (g_test.kind == TestOverride::kDls2) {
        double f = 2 * std::sin(3.141592653589793 * std::clamp(g_test.a, 30.0f, 8000.0f) / audio::kRate);
        v.fmode = kFilterDls2;
        v.fc[0] = uint16_t(int16_t(std::lround(4096 * std::log2(f))));
        v.fc[1] = uint16_t(std::min(std::lround(32768 * std::pow(10.0, -std::clamp(g_test.b, 0.0f, 22.5f) / 20)), 0xFFFFL));
    }
    if (v.flags & 0x200010 && v.fmode == kFilterBypass) v.fmode = kFilterMulti;
    if (v.fmode == kFilterParamEq || (v.fmode == kFilterMulti && (v.fc[0] || v.fc[1])))
        LOG_FIRST(8, "dsound: %08X: parametric EQ (mode %u, %04X %04X) not reproduced", v.obj, v.fmode, v.fc[0], v.fc[1]);
    UpdateFilter(v);
}

void ApplyI3DL2(Voice& v);
float DopplerRatio(const Voice& v, const Listener& l);

// a voice's 3D settings applied, its 3D pass run against the applied listener
void CommitVoice(Voice& v) {
    if (g_test.kind == TestOverride::kDoppler) {
        int64_t now = NowMs();
        float dt = (now - v.last_commit) / 1000.0f;
        if (v.last_commit && dt > 0.005f && dt < 0.5f) {
            for (int i = 0; i < 3; i++) v.next.vel[i] = (v.next.p[i] - v.last_p[i]) / dt;
            if (Dot(v.next.vel, v.next.vel) > 100 * 100) std::memset(v.next.vel, 0, sizeof v.next.vel);  // moved
        }
        if (!v.last_commit || dt > 0.005f) {
            std::memcpy(v.last_p, v.next.p, sizeof v.last_p);
            v.last_commit = now;
        }
    }
    std::memcpy(v.p, v.next.p, sizeof v.p);
    std::memcpy(v.vel, v.next.vel, sizeof v.vel);
    v.min_dist = v.next.min_dist;
    v.max_dist = v.next.max_dist;
    if (!v.is3d) return;
    ApplyI3DL2(v);
    v.doppler = DopplerRatio(v, g_listener);

    UpdateFilter(v);
}

// DSI3DL2LISTENER as 12 floats into the reverb; its HF reference sets the I3DL2 one poles of the voices
void ApplyListenerI3DL2(const float* p) {
    g_hf_reference = p[11] > 0 ? p[11] : 5000;
    if (g_reverb.Set(p))
        LOG_FIRST(200, "dsound: listener room %.0f hf %.0f, reflections %.0f after %.3f s, reverb %.0f after %.3f s, "
                       "decay %.2f s (hf ratio %.2f), diffusion %.0f density %.0f, hf reference %.0f Hz",
                  p[0], p[1], p[5], p[6], p[7], p[8], p[3], p[4], p[9], p[10], p[11]);
}

// CommitDeferredSettings (0x0057E93B): the listener, then every voice. An immediate listener call commits too.
void CommitAll() {
    if (g_test.kind == TestOverride::kDoppler) {
        static float last[3];
        static int64_t at = 0;
        int64_t now = NowMs();
        float dt = (now - at) / 1000.0f;
        if (at && dt > 0.005f && dt < 0.5f) {
            for (int i = 0; i < 3; i++) g_listener_next.vel[i] = (g_listener_next.p[i] - last[i]) / dt;
            if (Dot(g_listener_next.vel, g_listener_next.vel) > 100 * 100) std::memset(g_listener_next.vel, 0, 12);
        }
        if (!at || dt > 0.005f) {
            std::memcpy(last, g_listener_next.p, sizeof last);
            at = now;
        }
        g_listener_next.doppler = g_test.a;
    }
    g_listener = g_listener_next;
    if (g_i3dl2_pending) ApplyListenerI3DL2(g_i3dl2_next);
    g_i3dl2_pending = false;
    for (auto& [obj, v] : g_voices) CommitVoice(v);
}
// dwApply: DS3D_DEFERRED waits for CommitDeferredSettings
constexpr uint32_t kDeferred = 1;

uint32_t NewBuffer(uint32_t desc, uint32_t out) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Start();
    uint32_t obj = mem::AllocSystem(0x40);
    std::memset(HostPtr(obj), 0, 0x40);
    Voice& v = g_voices[obj];
    v.obj = obj;
    uint32_t flags = R32(desc + 4), bytes = R32(desc + 8);
    ReadFormat(v, R32(desc + 12));
    SetBins(v, R32(desc + 16));
    v.is3d = flags & 0x10;
    v.flags = flags;
    SetFilter(v, 0);
    v.headroom = DefaultHeadroom(flags);
    if (bytes) {
        v.data = v.owned = mem::Contiguous().Alloc(bytes, 4);
        v.bytes = v.loop_len = bytes;
    }
    TraceVoice("new", v);
    W32(out, obj);
    return 0;
}

// DSI3DL2BUFFER as the title passes it
void I3DL2Source(Voice& v, uint32_t src) {
    if (!src) return;
    for (int i = 0; i < 9; i++) v.next.i3dl2[i] = R32(src + i * 4);
    if (g_test.kind == TestOverride::kObstruction) {
        v.next.i3dl2[5] = uint32_t(int32_t(g_test.a));
        v.next.i3dl2[6] = 0;  // flLFRatio 0, as FCI sets it: the highs alone
    }
    const uint32_t* q = v.next.i3dl2;
    if (g_trace && (q[1] || q[5] || q[7]))
        LOG_FIRST(60, "dsound: i3dl2 source %08X direct %d hf %d room %d hf %d rolloff %.2f obstruction %d (%.2f) "
                      "occlusion %d (%.2f)", v.obj, int32_t(q[0]), int32_t(q[1]), int32_t(q[2]), int32_t(q[3]),
                  F32(q[4]), int32_t(q[5]), F32(q[6]), int32_t(q[7]), F32(q[8]));
}

// The library's I3DL2 high-frequency coefficient (0x00584F7F): the pole of a one pole low pass, DC gain 1, whose
// level at the listener's HF reference is mb millibels, in 1/65536. 0 bypasses.
uint16_t I3dl2Coefficient(int32_t mb, float reference) {
    if (mb == 0) return 0;
    double g = std::pow(10.0, 2.0 * mb / 2000.0);  // power at the reference
    double c = std::cos(6.283185307179586 * reference / audio::kRate);
    double a = (1 - g * c - std::sqrt(std::max(0.0, 2 * g * (1 - c) - g * g * (1 - c * c)))) / (1 - g);
    if (!(a < 1)) return 0xFFFF;
    return uint16_t(std::clamp(std::lround(a * 65536), 0L, 0xFFFFL));
}

// The library's I3DL2 source pass (0x00585117), mB: the direct path takes obstruction and occlusion times their
// LF ratio off its lows and the rest off its highs; the room takes occlusion times its LF ratio, its highs (sic) the
// rest of the occlusion on lRoom. FCI's room rolloff is 0, the distance term is left out.
void ApplyI3DL2(Voice& v) {
    const uint32_t* q = v.next.i3dl2;
    int32_t direct = int32_t(q[0]), direct_hf = int32_t(q[1]), room = int32_t(q[2]);
    int32_t obstruction = int32_t(q[5]), occlusion = int32_t(q[7]);
    float obstruction_lf = F32(q[6]), occlusion_lf = F32(q[8]);
    auto round = [](float x) { return int32_t(std::lrint(x)); };
    auto clamp = [](int32_t mb) { return std::clamp(mb, -10000, 0); };
    int32_t lf = round(obstruction * obstruction_lf + occlusion * occlusion_lf) + direct;
    int32_t hf = clamp(round((1 - obstruction_lf) * obstruction + (1 - occlusion_lf) * occlusion) + direct_hf);
    int32_t room_lf = round(occlusion * occlusion_lf) + room;
    int32_t room_hf = clamp(round((1 - occlusion_lf) * occlusion) + room);
    v.lf = Db(lf);
    v.room = Db(room_lf);
    v.i3dl2_fc[0] = I3dl2Coefficient(hf, g_hf_reference);
    v.i3dl2_fc[1] = I3dl2Coefficient(room_hf, g_hf_reference);
}

// The library's Doppler shift (0x0057FD6E, 0x005800CB): the source's speed away from the listener, relative to the
// listener's, times the doppler factor, against 342 m/s; 4096 * log2(1 - v / 342) in 1/4096 octaves, an octave up
// at most, silent from 342 m/s away
float DopplerRatio(const Voice& v, const Listener& l) {
    float d[3] = {v.p[0] - l.p[0], v.p[1] - l.p[1], v.p[2] - l.p[2]};
    float len = std::sqrt(Dot(d, d));
    if (len > 0)
        for (float& x : d) x /= len;
    float rel[3] = {v.vel[0] - l.vel[0], v.vel[1] - l.vel[1], v.vel[2] - l.vel[2]};
    float speed = Dot(rel, d) * l.doppler;
    constexpr float kSound = 342;
    if (!(speed != 0)) return 1;
    int32_t pitch;
    if (speed >= kSound) pitch = -32767;
    else if (speed <= -kSound) pitch = 4096;
    else pitch = int32_t(std::lrint(4096 * std::log2(1 - speed / kSound)));
    return std::exp2(pitch / 4096.0f);
}

// an XBE section by name, from the section headers in guest memory
bool FindSection(const std::string& name, uint32_t* va, uint32_t* size) {
    constexpr uint32_t kBase = 0x10000;
    uint32_t count = R32(kBase + 0x11C), headers = R32(kBase + 0x120);
    for (uint32_t i = 0; i < count && i < 64; i++) {
        uint32_t h = headers + i * 56;
        if (kernel::ReadCString(R32(h + 20)) != name) continue;
        *va = R32(h + 4);
        *size = R32(h + 8);
        return true;
    }
    return false;
}

// The effects image's DSP code is encrypted with a key per effect, the keys too; the library decrypts it as it
// downloads the image (0x00580363, cipher 0x00585ECD on the 64 bit shift register 0x00585E27)
struct FxCipher {
    uint64_t s = 0;
    void Step() {
        uint32_t lo = uint32_t(s), hi = uint32_t(s >> 32);
        uint64_t y = (s ^ (s << 2)) << 1;
        uint32_t bit = ((uint32_t(y) ^ lo) << 28 ^ hi) & 0x80000000u;
        s = s >> 1 | uint64_t(bit) << 32;
    }
    // 0x00585EA0: the state after 81 steps from the library's seed
    static uint64_t Seed() {
        FxCipher r;
        r.s = 0x7FA49BCA49DE12BAull;
        for (int i = 0; i < 0x51; i++) r.Step();
        return r.s;
    }
    static void Decrypt(const uint8_t key[8], uint8_t* data, size_t n) {
        uint64_t seed = Seed();
        uint8_t st[8];
        std::memcpy(st, &seed, 8);
        FxCipher r;
        r.s = seed;
        r.Step();
        for (size_t i = 0; i < n; i++) {
            uint8_t product = uint8_t(int8_t(st[i & 7]) * int8_t(key[i & 7]));
            uint8_t c = uint8_t(data[i] - product);
            r.Step();
            data[i] = uint8_t(st[i & 7] ^ uint8_t(r.s) ^ c);
        }
    }
};

// The image's crosstalk effect is the one CrossFeed reproduces: its decrypted code loads both coefficients
// (move #0x7645A1,x0 and move #0xF88313,x0). FCI's disc image holds it as effect 4, its delay 11.
bool HasCrossTalk(const std::vector<uint8_t>& img) {
    auto u32 = [&](size_t o) {
        uint32_t v = 0;
        if (o + 4 <= img.size()) std::memcpy(&v, &img[o], 4);
        return v;
    };
    size_t table = 0x818 + size_t(u32(0x804) + u32(0x80C)) * 4;
    uint32_t count = u32(table);
    if (!count || count > 64) return false;
    size_t keys_at = table + (count - 1) * 32 + 0x28;
    if (keys_at + count * 8 > img.size()) return false;
    std::vector<uint8_t> keys(img.begin() + keys_at, img.begin() + keys_at + count * 8);
    uint8_t seed[8];
    uint64_t s = FxCipher::Seed();
    std::memcpy(seed, &s, 8);
    FxCipher::Decrypt(seed, keys.data(), keys.size());
    size_t code = 0x818;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t size = u32(table + 8 + i * 32 + 4);
        if (code + size > img.size()) return false;
        std::vector<uint8_t> c(img.begin() + code, img.begin() + code + size);
        FxCipher::Decrypt(&keys[i * 8], c.data(), c.size());
        code += size;
        bool pole = false, feed = false;
        for (size_t w = 0; w + 8 <= c.size(); w += 4) {
            uint32_t op, imm;
            std::memcpy(&op, &c[w], 4);
            std::memcpy(&imm, &c[w + 4], 4);
            if (op != 0x44F400) continue;
            pole |= imm == 0x7645A1;
            feed |= imm == 0xF88313;
        }
        if (pole && feed) {
            rt::Log("dsound: effect %u is the crosstalk", i);
            return true;
        }
    }
    return false;
}

}  // namespace

// ---- speech volume ----
void xdk::SetSpeechVolume(float db) {
    float clamped = std::isfinite(db) ? std::clamp(db, -12.0f, 12.0f) : 0.0f;
    g_speech_gain = SpeechGain(clamped);
    rt::Log("dsound: speech volume %+.1f dB%s", clamped, clamped == 0 ? " (the console's mix)" : "");
}
// ---- end speech volume ----

// device

HLE_STDCALL(DirectSoundCreate, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Start();
    if (!g_ds) {
        g_ds = mem::AllocSystem(0x40);
        std::memset(HostPtr(g_ds), 0, 0x40);
        rt::Log("dsound: DirectSound on the host mixer (HLE), FCI_LLE_AUDIO=1 for the emulated APU");
        if (g_test.kind != TestOverride::kNone)
            rt::Log("dsound: FCI_TEST_DSOUND=%s (a measurement, not the game's sound)", g_test.text);
    }
    W32(a[1], g_ds);
    return 0;
}
HLE_STDCALL(IDirectSound_Release, 1) { return 0; }
HLE_STDCALL(DirectSoundUseFullHRTF, 0) { return 0; }
// DSSPEAKER as the library keeps it: the dashboard's XC_AUDIO_FLAGS
HLE_STDCALL(IDirectSound_GetSpeakerConfig, 2) {
    constexpr uint32_t kStereo = 0;
    constexpr uint32_t kMono = 1;                     // DSSPEAKER_MONO
    constexpr uint32_t kSurroundAc3 = 0x2 | 0x10000;  // DSSPEAKER_SURROUND | DSSPEAKER_ENABLE_AC3
    uint32_t config = kStereo;
    if (audio::IsMono()) config = kMono;
    if (audio::IsSurround()) config = kSurroundAc3;
    W32(a[1], config);
    return 0;
}
HLE_STDCALL(IDirectSound_SetEffectData, 6) {
    if (g_trace) {
        uint32_t d[4] = {};
        for (uint32_t i = 0; i < 4 && i * 4 < a[4]; i++) d[i] = R32(a[3] + i * 4);
        LOG_FIRST(40, "dsound: effect data from %08X: effect %u offset %X, %u bytes (%08X %08X %08X %08X), apply %u",
                  R32(c.esp), a[1], a[2], a[4], d[0], d[1], d[2], d[3], a[5]);
    }
    return 0;
}
HLE_STDCALL(IDirectSound_EnableHeadphones, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_headphones = a[1] != 0;
    LOG_FIRST(4, "dsound: headphones %s", g_headphones ? "on" : "off");
    return 0;
}
HLE_STDCALL(IDirectSound_SetMixBinHeadroom, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    LOG_FIRST(40, "dsound: mix bin %u headroom %u (from %08X)", a[1], a[2], R32(c.esp));
    if (a[1] < 32) g_bin_headroom[a[1]] = a[2] & 7;
    return 0;
}
HLE_STDCALL(IDirectSound_CommitDeferredSettings, 1) {
    CountApply("commit", 0);
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    CommitAll();
    return 0;
}
HLE_STDCALL(IDirectSound_SetDopplerFactor, 3) {
    CountApply("doppler factor", a[2]);
    LOG_FIRST(8, "dsound: doppler factor %.3f", F32(a[1]));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_listener_next.doppler = F32(a[1]);
    if (!(a[2] & kDeferred)) CommitAll();
    return 0;
}
HLE_STDCALL(IDirectSound_SetRolloffFactor, 3) {
    CountApply("rolloff factor", a[2]);
    LOG_FIRST(8, "dsound: rolloff factor %.3f", F32(a[1]));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_listener_next.rolloff = F32(a[1]);
    if (!(a[2] & kDeferred)) CommitAll();
    return 0;
}
HLE_STDCALL(IDirectSound_SetVelocity, 5) {
    CountApply("listener velocity", a[4]);
    if (g_trace && (a[1] | a[2] | a[3]) << 1)
        LOG_FIRST(40, "dsound: listener velocity %.2f %.2f %.2f", F32(a[1]), F32(a[2]), F32(a[3]));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    for (int i = 0; i < 3; i++) g_listener_next.vel[i] = F32(a[1 + i]);
    if (!(a[4] & kDeferred)) CommitAll();
    return 0;
}
HLE_STDCALL(IDirectSound_SetI3DL2Listener, 3) {
    CountApply("i3dl2 listener", a[2]);
    if (!a[1]) return 0;
    float p[12];
    for (int i = 0; i < 12; i++) {
        uint32_t v = R32(a[1] + i * 4);
        p[i] = i < 2 || i == 5 || i == 7 ? float(int32_t(v)) : F32(v);
    }
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    std::memcpy(g_i3dl2_next, p, sizeof p);
    g_i3dl2_pending = true;
    if (!(a[2] & kDeferred)) CommitAll();
    return 0;
}
HLE_STDCALL(IDirectSound_SetPosition, 5) {
    CountApply("listener position", a[4]);
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    for (int i = 0; i < 3; i++) g_listener_next.p[i] = F32(a[1 + i]);
    if (!(a[4] & kDeferred)) CommitAll();
    return 0;
}
HLE_STDCALL(IDirectSound_SetOrientation, 8) {
    CountApply("listener orientation", a[7]);
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    for (int i = 0; i < 3; i++) {
        g_listener_next.front[i] = F32(a[1 + i]);
        g_listener_next.top[i] = F32(a[4 + i]);
    }
    if (!(a[7] & kDeferred)) CommitAll();
    return 0;
}
HLE_STDCALL(DirectSoundDoWork, 0) {
    g_pumped = NowMs();
    std::vector<Done> done;
    {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        done.assign(g_done.begin(), g_done.end());
        g_done.clear();
    }
    Run(c, done);
    return 0;
}

// Effects run on the console's DSP, the I3DL2 reverb and the crosstalk are reproduced (Reverb, AddCrosstalk). The
// descriptor keeps the image's effect list so the title finds its indices, every segment points at zeroed memory.
// FCI loads sounds_xb\dsstdfx.bin from the disc (flags 0), with the reverb at effect 3 and the crosstalk at 4.
HLE_STDCALL(XAudioDownloadEffectsImage, 4) {
    std::string name = kernel::ReadCString(a[0]);
    std::vector<uint8_t> img;
    if (a[2] & 1) {
        uint32_t va, size;
        if (FindSection(name, &va, &size))
            img.assign(static_cast<uint8_t*>(HostPtr(va)), static_cast<uint8_t*>(HostPtr(va)) + size);
    } else {
        if (!kernel::ReadTitleFile(name, &img)) img.clear();
    }
    auto u32 = [&](size_t o) {
        uint32_t v = 0;
        if (o + 4 <= img.size()) std::memcpy(&v, &img[o], 4);
        return v;
    };
    size_t t = 0x818 + size_t(u32(0x804) + u32(0x80C)) * 4;
    uint32_t count = std::min<uint32_t>(u32(t), 64);
    rt::Log("dsound: effects image %s, %zu bytes, %u effects", name.c_str(), img.size(), count);
    {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        g_cross_feed = HasCrossTalk(img) && !g_no_cross_feed;
        rt::Log("dsound: crosstalk crossfeed %s", g_cross_feed ? "on" : "off");
    }
    uint32_t desc = mem::AllocSystem(8 + std::max(count, 1u) * 32);
    W32(desc, count);
    W32(desc + 4, u32(t + 4));
    for (uint32_t i = 0; i < count; i++) {
        size_t m = t + 8 + i * 32;
        uint32_t d = desc + 8 + i * 32;
        for (int k = 0; k < 4; k++) {
            uint32_t size = u32(m + k * 8 + 4);
            uint32_t seg = mem::AllocSystem(std::max(size, 0x40u));
            W32(d + k * 8, seg);
            W32(d + k * 8 + 4, size);
        }
    }
    if (a[3]) W32(a[3], desc);
    return 0;
}

// buffers

HLE_STDCALL(DirectSoundCreateBuffer, 2) { return NewBuffer(a[0], a[1]); }
HLE_STDCALL(IDirectSound_CreateSoundBuffer, 4) { return NewBuffer(a[1], a[2]); }

#define BUFFER(name, hook, nargs)                           \
    static uint32_t buf_##name(Voice& v, const Args& a);    \
    HLE_STDCALL(hook, nargs) {                              \
        std::lock_guard<std::recursive_mutex> lock(g_lock); \
        Voice* v = Find(a[0]);                              \
        return v ? buf_##name(*v, a) : 0x80004005u;         \
    }                                                       \
    static uint32_t buf_##name([[maybe_unused]] Voice& v, [[maybe_unused]] const Args& a)

BUFFER(Release, IDirectSoundBuffer_Release, 1) {
    if (--v.refs > 0) return uint32_t(v.refs);
    if (v.owned) mem::Contiguous().Free(v.owned);
    uint32_t obj = v.obj;
    g_voices.erase(obj);
    mem::System().Free(obj);
    return 0;
}
BUFFER(Unlock, IDirectSoundBuffer_Unlock, 5) { return 0; }
BUFFER(SetBufferData, IDirectSoundBuffer_SetBufferData, 3) {
    if (a[1] == v.data && a[2] == v.bytes) return 0;
    // a buffer made at a sound's size can be handed the sound's own data
    if (v.owned != a[1]) {
        if (v.owned) mem::Contiguous().Free(v.owned);
        v.owned = 0;
    }
    v.data = a[1];
    if (!a[1] && a[2]) v.data = v.owned = mem::Contiguous().Alloc(a[2], 4);  // a size with no data
    v.bytes = v.loop_len = a[2];
    v.loop_start = 0;
    v.pos = 0;
    v.block = -1;
    return 0;
}
BUFFER(Play, IDirectSoundBuffer_Play, 4) {
    v.playing = true;
    v.speech = kernel::IsSpeechData(v.data);  // speech volume
    v.looping = a[3] & 1;
    if (a[3] & 2) v.pos = 0;
    TraceVoice(v.looping ? "play looping" : "play", v);
    return 0;
}
BUFFER(Stop, IDirectSoundBuffer_Stop, 1) {
    v.playing = false;
    return 0;
}
BUFFER(SetLoopRegion, IDirectSoundBuffer_SetLoopRegion, 3) {
    if (a[1] > v.bytes || a[2] > v.bytes - a[1]) return 0x88780032u;  // DSERR_INVALIDCALL
    v.loop_start = a[1];
    v.loop_len = a[2] ? a[2] : v.bytes - a[1];
    return 0;
}
BUFFER(GetStatus, IDirectSoundBuffer_GetStatus, 2) {
    W32(a[1], (v.playing ? 1u : 0u) | (v.playing && v.looping ? 4u : 0u));
    return 0;
}
BUFFER(GetCurrentPosition, IDirectSoundBuffer_GetCurrentPosition, 3) {
    uint32_t p = BytesOf(v, uint32_t(v.pos));
    if (a[1]) W32(a[1], p);
    if (a[2]) W32(a[2], v.bytes ? (p + BytesOf(v, kAdpcmFrames * 4)) % v.bytes : 0);
    return 0;
}
BUFFER(SetCurrentPosition, IDirectSoundBuffer_SetCurrentPosition, 2) {
    v.pos = FramesOf(v, a[1]);
    return 0;
}
BUFFER(Lock, IDirectSoundBuffer_Lock, 8) {
    uint32_t off = a[1], n = a[2];
    if (a[7] & 1) n = v.bytes;  // DSBLOCK_ENTIREBUFFER
    if (!v.bytes) return 0x80004005u;  // E_FAIL
    off %= v.bytes;
    n = std::min(n, v.bytes);
    uint32_t first = std::min(n, v.bytes - off);
    W32(a[3], v.data + off);
    W32(a[4], first);
    if (a[5]) W32(a[5], n > first ? v.data : 0);
    if (a[6]) W32(a[6], n > first ? n - first : 0);
    return 0;
}
BUFFER(SetVolume, IDirectSoundBuffer_SetVolume, 2) {
    v.volume = Db(int32_t(a[1]));
    return 0;
}
BUFFER(SetFrequency, IDirectSoundBuffer_SetFrequency, 2) {
    v.freq = a[1];
    return 0;
}
BUFFER(SetFilter, IDirectSoundBuffer_SetFilter, 2) {
    TraceFilter(v, a[1], a.c.esp);
    SetFilter(v, a[1]);
    return 0;
}
BUFFER(SetHeadroom, IDirectSoundBuffer_SetHeadroom, 2) {
    if (g_trace_levels) LOG_FIRST(60, "dsound: buffer %08X headroom %u from %08X", v.obj, a[1], R32(a.c.esp));
    v.headroom = Db(-int32_t(a[1]));
    return 0;
}
BUFFER(SetMixBinVolumes, IDirectSoundBuffer_SetMixBinVolumes_8, 2) {
    SetBinVolumes(v, a[1]);
    return 0;
}
BUFFER(SetMixBins, IDirectSoundBuffer_SetMixBins, 2) {
    SetBins(v, a[1]);
    return 0;
}
BUFFER(SetMaxDistance, IDirectSoundBuffer_SetMaxDistance, 3) {
    CountApply("buffer max distance", a[2]);
    v.next.max_dist = F32(a[1]);
    if (!(a[2] & kDeferred)) CommitVoice(v);
    return 0;
}
BUFFER(SetMinDistance, IDirectSoundBuffer_SetMinDistance, 3) {
    CountApply("buffer min distance", a[2]);
    v.next.min_dist = F32(a[1]);
    if (!(a[2] & kDeferred)) CommitVoice(v);
    return 0;
}
BUFFER(SetPosition, IDirectSoundBuffer_SetPosition, 5) {
    CountApply("buffer position", a[4]);
    for (int i = 0; i < 3; i++) v.next.p[i] = F32(a[1 + i]);
    if (!(a[4] & kDeferred)) CommitVoice(v);
    return 0;
}
BUFFER(SetVelocity, IDirectSoundBuffer_SetVelocity, 5) {
    CountApply("buffer velocity", a[4]);
    if (g_trace && (a[1] | a[2] | a[3]) << 1)
        LOG_FIRST(40, "dsound: buffer %08X velocity %.2f %.2f %.2f", v.obj, F32(a[1]), F32(a[2]), F32(a[3]));
    for (int i = 0; i < 3; i++) v.next.vel[i] = F32(a[1 + i]);
    if (!(a[4] & kDeferred)) CommitVoice(v);
    return 0;
}
BUFFER(SetI3DL2Source, IDirectSoundBuffer_SetI3DL2Source, 3) {
    CountApply("buffer i3dl2 source", a[2]);
    I3DL2Source(v, a[1]);
    if (!(a[2] & kDeferred)) CommitVoice(v);
    return 0;
}

// streams, the object carries the XDK's own vtables so the title's XMediaObject calls reach the functions below

HLE_STDCALL(DirectSoundCreateStream, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Start();
    uint32_t desc = a[0];
    uint32_t obj = mem::AllocSystem(0x80);
    std::memset(HostPtr(obj), 0, 0x80);
    W32(obj, kStreamVtable);
    W32(obj + 4, kStreamVtable2);
    Voice& v = g_voices[obj];
    v.obj = obj;
    v.stream = true;
    uint32_t flags = R32(desc);
    v.max_packets = std::max<uint32_t>(R32(desc + 4), 1);
    ReadFormat(v, R32(desc + 8));
    v.callback = R32(desc + 12);
    v.context = R32(desc + 16);
    SetBins(v, R32(desc + 20));
    v.is3d = flags & 0x10;
    v.flags = flags;
    SetFilter(v, 0);
    v.headroom = DefaultHeadroom(flags);
    if (g_test_solo == 2 && v.is3d && !g_solo) g_solo = obj;
    TraceVoice("new", v);
    W32(a[1], obj);
    return 0;
}

namespace {

// the stream's callbacks still queued and those of its flushed packets, in order, go to the calling thread
void FlushStream(Voice& v) {
    for (auto& pk : v.packets) Complete(v, pk, kFlushed);
    v.packets.clear();
    v.pos = 0;
    for (auto it = g_done.begin(); it != g_done.end();) {
        if (it->voice != v.obj) {
            ++it;
            continue;
        }
        t_flushed.push_back(*it);
        it = g_done.erase(it);
    }
    Deactivate(v);
}

}  // namespace

// The stream methods, once each; STREAM_HOOK binds one to a replaced function. The game reaches the XMediaObject
// methods through the vtable (CDirectSoundStream_*) and the rest through the IDirectSoundStream_* thunks, which in
// the recompiled code hold their own copy of the CDirectSoundStream_* body, so both are replaced.
#define STREAM(name) static uint32_t str_##name([[maybe_unused]] Voice& v, [[maybe_unused]] const Args& a)
#define STREAM_HOOK(hook, name, nargs)                           \
    HLE_STDCALL(hook, nargs) {                                   \
        uint32_t r;                                              \
        std::vector<Done> flushed;                               \
        {                                                        \
            std::lock_guard<std::recursive_mutex> lock(g_lock);  \
            Voice* v = Find(a[0]);                               \
            r = v ? str_##name(*v, a) : 0x80004005u;             \
            flushed.swap(t_flushed);                             \
        }                                                        \
        Run(c, flushed);                                         \
        return r;                                                \
    }

STREAM(AddRef) { return uint32_t(++v.refs); }
STREAM(Release) {
    if (--v.refs > 0) return uint32_t(v.refs);
    FlushStream(v);
    uint32_t obj = v.obj;
    g_voices.erase(obj);
    mem::System().Free(obj);
    return 0;
}
STREAM(GetInfo) {
    uint32_t info = a[1];
    W32(info, 0x3);  // fixed sample size, fixed packet alignment
    W32(info + 4, v.tag == 0x69 ? 36u * v.channels : v.align);
    W32(info + 8, 0);
    W32(info + 12, 0);
    return 0;
}
STREAM(GetStatus) {
    W32(a[1], v.packets.size() < v.max_packets ? 1u : 0u);
    return 0;
}
STREAM(Process) {
    uint32_t in = a[1];
    if (!in) return 0;
    Packet pk{R32(in), R32(in + 4), R32(in + 8), R32(in + 12), R32(in + 16)};
    pk.speech = v.speech = kernel::IsSpeechData(pk.data);  // speech volume
    if (pk.completed_ptr) W32(pk.completed_ptr, 0);
    if (pk.status_ptr) W32(pk.status_ptr, kPacketPending);
    bool was_active = v.active;
    v.packets.push_back(pk);
    if (g_trace && !was_active) TraceVoice("start", v);
    v.active = true;
    v.ending = false;
    return 0;
}
STREAM(Discontinuity) {
    if (!v.packets.empty()) v.ending = true;
    else if (v.active) FlushStream(v);
    return 0;
}
STREAM(Flush) {
    FlushStream(v);
    return 0;
}
STREAM(FlushEx) {
    if (a[3] && !v.active) return 0;
    FlushStream(v);
    return 0;
}
STREAM(Pause) {
    v.paused = a[1] == 1;
    return 0;
}
STREAM(SetVolume) {
    v.volume = Db(int32_t(a[1]));
    return 0;
}
STREAM(SetFrequency) {
    v.freq = a[1];
    return 0;
}
STREAM(SetFilter) {
    TraceFilter(v, a[1], a.c.esp);
    SetFilter(v, a[1]);
    return 0;
}
STREAM(SetHeadroom) {
    if (g_trace_levels) LOG_FIRST(60, "dsound: stream %08X headroom %u from %08X", v.obj, a[1], R32(a.c.esp));
    v.headroom = Db(-int32_t(a[1]));
    return 0;
}
STREAM(SetMixBinVolumes) {
    SetBinVolumes(v, a[1]);
    return 0;
}
STREAM(SetMixBins) {
    SetBins(v, a[1]);
    return 0;
}
STREAM(SetMaxDistance) {
    CountApply("stream max distance", a[2]);
    v.next.max_dist = F32(a[1]);
    if (!(a[2] & kDeferred)) CommitVoice(v);
    return 0;
}
STREAM(SetMinDistance) {
    CountApply("stream min distance", a[2]);
    v.next.min_dist = F32(a[1]);
    if (!(a[2] & kDeferred)) CommitVoice(v);
    return 0;
}
STREAM(SetPosition) {
    CountApply("stream position", a[4]);
    for (int i = 0; i < 3; i++) v.next.p[i] = F32(a[1 + i]);
    if (!(a[4] & kDeferred)) CommitVoice(v);
    return 0;
}
STREAM(SetVelocity) {
    CountApply("stream velocity", a[4]);
    if (g_trace && (a[1] | a[2] | a[3]) << 1)
        LOG_FIRST(40, "dsound: stream %08X velocity %.2f %.2f %.2f", v.obj, F32(a[1]), F32(a[2]), F32(a[3]));
    for (int i = 0; i < 3; i++) v.next.vel[i] = F32(a[1 + i]);
    if (!(a[4] & kDeferred)) CommitVoice(v);
    return 0;
}
STREAM(SetI3DL2Source) {
    CountApply("stream i3dl2 source", a[2]);
    I3DL2Source(v, a[1]);
    if (!(a[2] & kDeferred)) CommitVoice(v);
    return 0;
}

STREAM_HOOK(CDirectSoundStream_AddRef, AddRef, 1)
STREAM_HOOK(CDirectSoundStream_Release, Release, 1)
STREAM_HOOK(CDirectSoundStream_GetInfo, GetInfo, 2)
STREAM_HOOK(CDirectSoundStream_GetStatus__r2, GetStatus, 2)
STREAM_HOOK(CDirectSoundStream_Process, Process, 3)
STREAM_HOOK(CDirectSoundStream_Discontinuity, Discontinuity, 1)
STREAM_HOOK(CDirectSoundStream_Flush, Flush, 1)
STREAM_HOOK(CDirectSoundStream_FlushEx, FlushEx, 4)
STREAM_HOOK(CDirectSoundStream_Pause, Pause, 2)
STREAM_HOOK(CDirectSoundStream_SetVolume, SetVolume, 2)
STREAM_HOOK(CDirectSoundStream_SetFrequency, SetFrequency, 2)
STREAM_HOOK(CDirectSoundStream_SetFilter, SetFilter, 2)
STREAM_HOOK(CDirectSoundStream_SetHeadroom, SetHeadroom, 2)
STREAM_HOOK(CDirectSoundStream_SetMixBinVolumes_8, SetMixBinVolumes, 2)
STREAM_HOOK(CDirectSoundStream_SetMixBins, SetMixBins, 2)
STREAM_HOOK(CDirectSoundStream_SetMaxDistance, SetMaxDistance, 3)
STREAM_HOOK(CDirectSoundStream_SetMinDistance, SetMinDistance, 3)
STREAM_HOOK(CDirectSoundStream_SetPosition, SetPosition, 5)
STREAM_HOOK(CDirectSoundStream_SetVelocity, SetVelocity, 5)
STREAM_HOOK(CDirectSoundStream_SetI3DL2Source, SetI3DL2Source, 3)
STREAM_HOOK(IDirectSoundStream_FlushEx, FlushEx, 4)
STREAM_HOOK(IDirectSoundStream_Pause, Pause, 2)
STREAM_HOOK(IDirectSoundStream_SetVolume, SetVolume, 2)
STREAM_HOOK(IDirectSoundStream_SetFrequency, SetFrequency, 2)
STREAM_HOOK(IDirectSoundStream_SetFilter, SetFilter, 2)
STREAM_HOOK(IDirectSoundStream_SetHeadroom, SetHeadroom, 2)
STREAM_HOOK(IDirectSoundStream_SetMixBinVolumes_8, SetMixBinVolumes, 2)
STREAM_HOOK(IDirectSoundStream_SetMixBins, SetMixBins, 2)
STREAM_HOOK(IDirectSoundStream_SetMaxDistance, SetMaxDistance, 3)
STREAM_HOOK(IDirectSoundStream_SetMinDistance, SetMinDistance, 3)
STREAM_HOOK(IDirectSoundStream_SetPosition, SetPosition, 5)
STREAM_HOOK(IDirectSoundStream_SetVelocity, SetVelocity, 5)
STREAM_HOOK(IDirectSoundStream_SetI3DL2Source, SetI3DL2Source, 3)
