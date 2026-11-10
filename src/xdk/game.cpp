// Game patches for the PC display features (app/pcmode.h): Hor+ widescreen and the field of view.
//
// FCI's camera (CryEngine's CCamera) keeps its horizontal field of view at +0x38 (radians) and its aspect ratio
// (width / height) at +0x384. Its update, 0x002A84E0 (camera in ebx), builds the projection with
// D3DXMatrixPerspectiveFovRH(fov / aspect, aspect, near, far) and the culling frustum from tan(fov / 2) and
// tan(fov / aspect / 2). Both come from the same two values, so widening them here widens what is drawn, what is
// culled and every pass derived from the camera (water reflections, shadow frusta) consistently. The widened values
// stay in the camera afterwards (the water culls later from them), and the next update starts again from the
// game's own values (see Widened), so nothing compounds when the game animates them (zoom, cutscenes).
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "app/pcmode.h"
#include "gpu/ao.h"
#include "hle/mouse_look.h"  // direct mouse look
#include "runtime/memory.h"
#include "xdk/hle.h"

namespace {

bool HleOff() { return !xdk::HleGraphics(); }

using rt::R32;

// FCI_TRACE_VIEW=1: every 5 s; FCI_TRACE_VIEW=<ms> (> 1): at that interval
const ULONGLONG trace_view = [] {
    char v[16] = {};
    if (!GetEnvironmentVariableA("FCI_TRACE_VIEW", v, sizeof v)) return 0ull;
    const ULONGLONG ms = strtoull(v, nullptr, 10);
    return ms > 1 ? ms : 5000ull;
}();

constexpr uint32_t kCamFov = 0x38, kCamOrtho = 0x364, kCamAspect = 0x384;

void SetF(uint32_t va, float v) { std::memcpy(rt::Host(va), &v, 4); }

// The widened values stay in the camera after its update: the game culls its water patches later from the
// camera's own field of view and aspect (put back to 4:3, the patches beside the 4:3 middle were dropped and the
// sea floor showed through). Per camera the console's values and the widened ones written: a camera that still
// holds what was written is still at the console's values; anything else is the game's new value (zoom).
struct Widened {
    uint32_t cam;
    float fov, aspect;            // the game's own
    float wide_fov, wide_aspect;  // what was written
};
std::vector<Widened> g_widened;

// FCI_TEST_CRT_MATH=1: runs the game's x87 tanh (0x005001F5) and exp (0x004ED12C) once, on the game thread at the
// first camera update, for an argument of each class (normal, zero, infinite, NaN, denormal) and logs them beside
// the host's. The CRT dispatches each class to its own handler through a byte-packed table (fci-recomp,
// PackedStructPointers); tanh of a normal number is the call of GitHub #7 (0x00500275).
void TestCrtMath(rt::Cpu& c) {
    const rt::Cpu saved = c;
    struct Fn { const char* name; uint32_t va; double (*host)(double); } fns[] = {
        {"tanh", 0x005001F5u, [](double x) { return std::tanh(x); }},
        {"exp", 0x004ED12Cu, [](double x) { return std::exp(x); }},
    };
    const double inf = INFINITY, args[] = {0.5, -0.25, 3.0, -20.0, 0.0, -0.0, 1e-310, inf, -inf, NAN};
    int bad = 0;
    for (const auto& fn : fns)
        for (double x : args) {
            rt::FPush(c, x);
            rt::GuestCall(c, fn.va, {});
            const double y = rt::ST(c, 0), want = fn.host(x);
            rt::FPop(c);
            const bool ok = (std::isnan(y) && std::isnan(want)) || y == want || std::fabs(y - want) <= 1e-12 * std::fabs(want);
            bad += !ok;
            rt::Log("crt math: %s(%g) = %.17g, host %.17g%s", fn.name, x, y, want, ok ? "" : "  MISMATCH");
        }
    rt::Log("crt math: %d mismatches", bad);
    c = saved;
}

// FCI_TRACE_FRAME_TIME=1: the game's frame timer (global at 0x00644488, updated by 0x001FF7C0 once per frame:
// +0 frames, +0x28 game time, +0x30 frame time, both double seconds; the time source is XAPI's
// QueryPerformanceCounter = rdtsc), summed up once per second at the main camera's update
const bool trace_frame_time = GetEnvironmentVariableA("FCI_TRACE_FRAME_TIME", nullptr, 0) > 0;

void TraceFrameTime() {
    constexpr uint32_t kTimer = 0x00644488;
    static ULONGLONG last = GetTickCount64();
    static uint32_t frames0 = R32(kTimer), n = 0, tiny = 0;
    static double lo = 1e9, hi = 0, sum = 0, game0 = 0;
    double dt, game;
    std::memcpy(&dt, rt::Host(kTimer + 0x30), 8);
    std::memcpy(&game, rt::Host(kTimer + 0x28), 8);
    lo = std::min(lo, dt), hi = std::max(hi, dt), sum += dt, n++;
    tiny += dt < 0.0005;
    if (GetTickCount64() - last < 1000) return;
    rt::Log("game: frame time: %u timer frames, %u camera updates, dt %.3f..%.3f ms (mean %.3f), %u below 0.5 ms, "
            "game time %.3f s (+%.3f)", R32(kTimer) - frames0, n, lo * 1e3, hi * 1e3, sum / n * 1e3, tiny, game,
            game - game0);
    last = GetTickCount64(), frames0 = R32(kTimer), n = tiny = 0, lo = 1e9, hi = sum = 0, game0 = game;
}

// ---- frame-rate audit (README, frame rate and game logic): game state over game time, all diagnostics off by default ----
constexpr uint32_t kFrameTimer = 0x00644488;  // +0 frames, +0x28 game time, +0x30 frame time (double seconds)

double GameTime() {
    double t;
    std::memcpy(&t, rt::Host(kFrameTimer + 0x28), 8);
    return t;
}

double g_level_start = -1;  // game time at the level's first main camera frame

// MSVC 7 std::string (CEntity +0xA8 name, +0xC4 class): +4 inline buffer (capacity < 16) or pointer, +0x14 size,
// +0x18 capacity
std::string GuestString(uint32_t s) {
    const uint32_t size = R32(s + 0x14), cap = R32(s + 0x18);
    if (size > cap || cap > 4096 || cap < 15) return "?";
    const uint32_t p = cap < 16 ? s + 4 : R32(s + 4);
    if (!p) return "?";
    std::string out(reinterpret_cast<const char*>(rt::Host(p)), size);
    for (char& ch : out)
        if (ch <= ' ' || ch > '~') ch = '_';
    return out;
}

// FCI_TRACE_STATE=<ms> (diagnostics): every <ms> of the game's own time (frame timer +0x28; not wall time, so runs at
// different frame rates line up) a block in state.log next to the EXE: the time, frames and frame time; the player
// (entity +0x34C position, +0x358 angles, living physics +0x238 -> +0x118/+0x124 velocity; CPlayer = entity +0x23C:
// +0x160 health, +0x168 armor, +0x170 adrenaline, +0x178 breath, the offsets the script binding 0x000BC798
// registers); every entity of the entity system ([0x006B38EC] +0x24: list, node +0xC entity) that appeared, went or
// moved (> 2 cm, > 0.5 degrees) or changed its flags +0x18 since the block before, with name, class, position, yaw
// and for rigid bodies (physics vtables 0x00625AF0 rigid, 0x00625D40 wheeled) the speed (+0x2C4). Runs on the game
// thread from the main camera's update, between two frames, so the lists are consistent.
const double trace_state = [] {
    char v[16] = {};
    return GetEnvironmentVariableA("FCI_TRACE_STATE", v, sizeof v) ? std::max(atof(v), 10.0) * 0.001 : 0.0;
}();

void TraceState() {
    struct Seen {
        float pos[3], yaw;
        uint32_t flags;
        bool alive;
        std::string name;
    };
    static FILE* out = std::fopen("state.log", "w");
    static std::unordered_map<uint32_t, Seen> seen;  // by entity address
    static double next = 0;
    if (!out) return;
    const double now = GameTime() - g_level_start;
    if (now < next) return;
    next = (std::floor(now / trace_state) + 1) * trace_state;
    double dt;
    std::memcpy(&dt, rt::Host(kFrameTimer + 0x30), 8);
    std::fprintf(out, "t %.3f frames %u dt %.1f\n", now, R32(kFrameTimer), dt * 1000);
    const uint32_t system = R32(0x006B38EC);
    if (!system) return;
    const uint32_t head = R32(system + 0x24);
    for (auto& s : seen) s.second.alive = false;
    uint32_t n = head ? R32(head) : 0, count = 0;
    for (; n && n != head && count < 8192; n = R32(n), count++) {
        const uint32_t e = R32(n + 0xC);
        if (!e) continue;
        const float pos[3] = {xdk::GF(e + 0x34C), xdk::GF(e + 0x350), xdk::GF(e + 0x354)};
        const float yaw = xdk::GF(e + 0x360);
        const uint32_t flags = R32(e + 0x18);
        auto it = seen.find(e);
        const bool fresh = it == seen.end();
        const std::string cls = GuestString(e + 0xC4);
        if (cls == "Player") {
            const uint32_t phys = R32(e + 0x238), player = R32(e + 0x23C);
            const bool living = phys && R32(phys) == 0x00625C40u;
            std::fprintf(out, "player %.3f %.3f %.3f ang %.2f %.2f %.2f v %.3f %.3f %.3f", pos[0], pos[1], pos[2],
                         xdk::GF(e + 0x358), xdk::GF(e + 0x35C), yaw, living ? xdk::GF(phys + 0x124) : 0.0f,
                         living ? xdk::GF(phys + 0x128) : 0.0f, living ? xdk::GF(phys + 0x12C) : 0.0f);
            if (player && R32(player + 0x254) == kFrameTimer)
                std::fprintf(out, " health %.2f armor %.2f adrenaline %.2f breath %.2f", xdk::GF(player + 0x160),
                             xdk::GF(player + 0x168), xdk::GF(player + 0x170), xdk::GF(player + 0x178));
            std::fprintf(out, "\n");
        }
        if (!fresh) {
            const Seen& s = it->second;
            const float d = std::fabs(pos[0] - s.pos[0]) + std::fabs(pos[1] - s.pos[1]) + std::fabs(pos[2] - s.pos[2]);
            if (d < 0.02f && std::fabs(yaw - s.yaw) < 0.5f && flags == s.flags) {
                it->second.alive = true;
                continue;
            }
        }
        const uint32_t phys = R32(e + 0x238), vt = phys ? R32(phys) : 0;
        float speed = -1;
        if (vt == 0x00625AF0u || vt == 0x00625D40u)
            speed = std::sqrt(xdk::GF(phys + 0x2C4) * xdk::GF(phys + 0x2C4) + xdk::GF(phys + 0x2C8) * xdk::GF(phys + 0x2C8) +
                              xdk::GF(phys + 0x2CC) * xdk::GF(phys + 0x2CC));
        const std::string name = GuestString(e + 0xA8);
        std::fprintf(out, "%c %s %s %.3f %.3f %.3f yaw %.1f flags %08X", fresh ? '+' : 'e', name.c_str(), cls.c_str(),
                     pos[0], pos[1], pos[2], yaw, flags);
        if (speed >= 0) std::fprintf(out, " speed %.3f", speed);
        std::fprintf(out, "\n");
        seen[e] = Seen{{pos[0], pos[1], pos[2]}, yaw, flags, true, name};
    }
    for (auto s = seen.begin(); s != seen.end();)
        if (!s->second.alive) {
            std::fprintf(out, "- %s\n", s->second.name.c_str());
            s = seen.erase(s);
        } else {
            ++s;
        }
    std::fprintf(out, "entities %u\n", count);
    std::fflush(out);
}
// ---- end frame-rate audit ----
// FCI_TRACE_ENTITY_FRAMES=<x>,<y>,<z> (diagnostics, GitHub #22): the entity placed there (CEntity: vtable
// 0x005F6AA0, position +0x34C, flags +0x18) at the start of every game frame (from the frame timer's time source),
// with the frame time of the frame before: the first 300 frames of the level, after that whenever it moved.
const float* trace_frames = [] {
    static float at[3];
    char v[64] = {};
    if (!GetEnvironmentVariableA("FCI_TRACE_ENTITY_FRAMES", v, sizeof v) ||
        std::sscanf(v, "%f,%f,%f", &at[0], &at[1], &at[2]) != 3)
        return static_cast<const float*>(nullptr);
    return static_cast<const float*>(at);
}();

// first guest dword address in committed read/write memory for which match() holds (4-byte steps)
template <class F>
uint32_t ScanGuest(F match) {
    for (uint64_t a = mem::kVirtualBegin; a < mem::kVirtualEnd;) {
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery(rt::Host(uint32_t(a)), &info, sizeof info)) break;
        const uint64_t end = std::min<uint64_t>(
            uint64_t(static_cast<uint8_t*>(info.BaseAddress) - rt::Host(0)) + info.RegionSize, mem::kVirtualEnd);
        if (info.State == MEM_COMMIT && info.Protect == PAGE_READWRITE)
            for (uint64_t p = a; p + 0x500 <= end; p += 4)
                if (match(uint32_t(p))) return uint32_t(p);
        a = end;
    }
    return 0;
}

std::atomic<uint32_t> g_trace_entity{0};

void TraceEntityFrames() {
    static std::once_flag once;
    std::call_once(once, [] {  // the level's entities exist before its first frame: found from a thread of its own
        std::thread([] {
            uint32_t entity = 0;
            while (!entity) {
                Sleep(100);
                entity = ScanGuest([](uint32_t p) {
                    return R32(p) == 0x005F6AA0u && std::fabs(xdk::GF(p + 0x34C) - trace_frames[0]) < 0.01f &&
                           std::fabs(xdk::GF(p + 0x350) - trace_frames[1]) < 0.01f &&
                           std::fabs(xdk::GF(p + 0x354) - trace_frames[2]) < 0.01f;
                });
            }
            rt::Log("game: FCI_TRACE_ENTITY_FRAMES entity %08X", entity);
            g_trace_entity = entity;
        }).detach();
    });
    const uint32_t entity = g_trace_entity;
    if (!entity) return;
    static uint32_t frames = 0, lines = 0;
    static float last[3] = {};
    const float pos[3] = {xdk::GF(entity + 0x34C), xdk::GF(entity + 0x350), xdk::GF(entity + 0x354)};
    const bool moved = std::memcmp(pos, last, sizeof pos) != 0;
    std::memcpy(last, pos, sizeof pos);
    if (frames++ >= 300 && (!moved || lines >= 3000)) return;
    lines++;
    double dt, game;
    std::memcpy(&dt, rt::Host(0x00644488 + 0x30), 8);
    std::memcpy(&game, rt::Host(0x00644488 + 0x28), 8);
    rt::Log("game: entity frame %u dt %.1f ms game time %.3f pos %.2f %.2f %.2f flags %08X", R32(0x00644488), dt * 1e3,
            game, pos[0], pos[1], pos[2], R32(entity + 0x18));
}

}  // namespace

double xdk::LevelSeconds() { return g_level_start < 0 ? -1.0 : GameTime() - g_level_start; }

// ---- precise frame timer (high frame rates) ----
// The game's frame timer (0x00644488, see TraceFrameTime) reads its time from 0x001FF8F0 (thiscall, the timer in
// ecx, the time in seconds in st(0)): the game's millisecond clock 0x002856F0, a stopwatch at 0x006E2390 over XAPI's
// QueryPerformanceCounter (rdtsc) that rounds to whole milliseconds (0x00286440: trunc(ms + 0.555)), times 0.001.
// At the console's 30 fps a frame is 33 or 34 ms (±1.5 %); at 120 fps 8 or 9 ms, at 400 fps 2 or 3 ms (±25 %).
// FCI_PRECISE_FRAME_TIMER=1 (experiment): the same clock without the rounding (the stopwatch's start, frequency and
// offset, the same rdtsc). Measured: it evens out the frame times, but the vehicle and swimming problems at high
// frame rates stay, so the game's own clock stays the default.
const bool game_frame_timer = GetEnvironmentVariableA("FCI_PRECISE_FRAME_TIMER", nullptr, 0) == 0;

// FCI_MIN_FRAME_TIME=<ms> (experiment, GitHub #22): every game frame takes at least this long in real time (waits
// before the frame timer's update reads the clock; return address 0x001FF7D2 = the per-frame update 0x001FF7C0)
const double min_frame_ms = [] {
    char v[16] = {};
    return GetEnvironmentVariableA("FCI_MIN_FRAME_TIME", v, sizeof v) ? atof(v) : 0.0;
}();

HLE_THISCALL(Game_FrameTimeSource, 0) {
    const bool frame_update = R32(c.esp) == 0x001FF7D2u;
    if (trace_frames && frame_update) TraceEntityFrames();
    if (min_frame_ms > 0 && frame_update) {
        static LARGE_INTEGER last{};
        LARGE_INTEGER f, now;
        QueryPerformanceFrequency(&f);
        for (;;) {
            QueryPerformanceCounter(&now);
            if (!last.QuadPart || double(now.QuadPart - last.QuadPart) * 1000.0 / double(f.QuadPart) >= min_frame_ms) break;
            Sleep(0);
        }
        last = now;
    }
    constexpr uint32_t kWatch = 0x006E2390, kGuard = 0x006E23D0;  // stopwatch, its static-init guard (bit 0)
    if (game_frame_timer || !(R32(kGuard) & 1)) {  // the game's own; its first call builds the stopwatch
        xdk::CallOriginal(c, GUEST_ORIG(Game_FrameTimeSource));
        if (game_frame_timer) return c.eax;
        rt::FPop(c);
    }
    uint64_t start, freq;
    std::memcpy(&start, rt::Host(kWatch + 0x10), 8);
    std::memcpy(&freq, rt::Host(kWatch + 0x18), 8);
    const int32_t offset_ms = int32_t(R32(kWatch));  // the stopwatch's own correction (0 in play)
    const double ms = double(int64_t(rt::ReadTsc() - start)) / double(freq ? freq : 733333333) * 1000.0 + offset_ms;
    rt::FPush(c, ms * 0.001);
    return c.eax;
}
// ---- end precise frame timer ----

// ---- bound AI does not drown (GitHub #22) ----
// The script binding IsUnderWater of a player or AI (CPlayer; 0x000C7710, thiscall: the script object in ecx with the
// CPlayer at [ecx+0x10], the script function handler in [esp+4]; ret 4) answers with the flag [CPlayer+0x43C], which
// the player update (0x00094374) sets each frame from the eye height +0x22C below the water's height. An AI's script
// drowns it on one such answer: Merc:CheckDrowning deals max_health as "drowning" damage. The binding returns its
// value through the handler's return stream: 5 bytes at [[handler+4]] - 5 after the call, tag 2 with the float 1.0 =
// under water, tag 1 (nil) = not.
// The opening of 13_Black_Forest binds the stand-ins Carver_start and Doyle_start (Mercs, not invulnerable unlike
// every other stand-in of the level) to bones of the helicopter, whose animation starts in the same frame and blends
// in from the rest pose, which lies under the water (the bone "Car" at z 11.55, the water at 16.01; the animation's
// pose is at ~24 m). The blend lifts the bones out of the water 35-50 ms of game time later, depending on the frame
// times. On the console (33 ms or more per frame) the first check after the bind sees Carver dry; here the first
// game frames of the level are not shown and so not paced (fps_limit 30: 5, 1, 1, 13, 3 ms; with shadow_cascades
// 5, 0, 9, 3, 15 ms) and at high frame rates every frame is short, so a check lands in the blend: Carver drowns, the
// eyes stay bound to its body and the level never starts (the camera rests in the wreck), at every frame rate.
// So an AI bound to another entity (CEntity +0x140: the entity id of its parent, cleared by the unbind 0x0001CF4D)
// is not under water for its scripts: it is carried by its parent and cannot swim. The local players (entity id at
// CEntity +0xA4 against [[game+0x28+4i]+0x30], game = [0x006B3984], as Game:IsLocalPlayer 0x000B8C60 does) keep the
// game's answer, so a player bound to a vehicle sunk in water still drowns as before. FCI_BOUND_DROWNING=1: the
// game's own answer for everyone; FCI_TRACE_DROWNING=1 logs every check.
namespace {
const bool bound_drowning = GetEnvironmentVariableA("FCI_BOUND_DROWNING", nullptr, 0) > 0;
const bool trace_drowning = GetEnvironmentVariableA("FCI_TRACE_DROWNING", nullptr, 0) > 0;

bool IsLocalPlayer(uint32_t entity) {
    const uint32_t game = R32(0x006B3984);
    if (!game) return false;
    for (uint32_t i = 0; i < 4; i++)
        if (const uint32_t p = R32(game + 0x28 + 4 * i); p && R32(p + 0x30) == R32(entity + 0xA4)) return true;
    return false;
}
}  // namespace

HLE_THISCALL(Game_PlayerIsUnderWater, 1) {
    const uint32_t player = R32(c.ecx + 0x10), handler = R32(c.esp + 4);
    const uint32_t stream = R32(handler + 4), before = R32(stream);
    const uint32_t r = xdk::CallOriginal(c, GUEST_ORIG(Game_PlayerIsUnderWater));
    if (!player || R32(stream) != before + 5) return r;  // nothing returned (wrong argument count) or no player
    uint8_t* tag = rt::Host(before);
    const uint32_t entity = R32(player + 4), parent = entity ? R32(entity + 0x140) : 0;
    const bool under = *tag == 2;
    const bool carried = under && parent && !bound_drowning && !IsLocalPlayer(entity);
    if (carried) *tag = 1;  // nil: not under water
    if (trace_drowning) {
        double now;
        std::memcpy(&now, rt::Host(0x00644488 + 0x28), 8);
        rt::Log("game: player %08X under water check at %.3f (frame %u): %s; entity %08X id %u at z %.2f, bound to %u%s",
                player, now, R32(0x00644488), under ? (carried ? "under water, bound: not reported" : "under water")
                : "dry", entity, entity ? R32(entity + 0xA4) : 0u, entity ? xdk::GF(entity + 0x354) : 0.0f, parent,
                entity && IsLocalPlayer(entity) ? " (local player)" : "");
    } else if (carried) {
        LOG_FIRST(8, "game: AI %08X under water while bound to entity %u: not drowning", player, parent);
    }
    return r;
}
// ---- end bound AI does not drown ----

// ---- water bob at any frame rate ----
// The player's view angle update 0x00098AE0 (thiscall, player in ecx, ret 4; a virtual method, vtable entry
// 0x005FC3F0) sways the view while swimming: with the depth factor d = clamp(depth +0x444 - p_waterbob_mindepth,
// 0, 1) and phases advanced by p_waterbob_pitchspeed/rollspeed x frame time (+0xB4C/+0xB50), it adds
// p_waterbob_pitch x d x sin(2 pi phase) to the view pitch each frame (and the roll likewise), to the angles that
// already hold the frames before: the sway is a sum over frames, its size grows with the frame rate. At the
// console's 30 fps p_waterbob_pitch = 0.1 gives 4.8 degrees from top to bottom; at 120 fps 19, at 450 fps 72 (measured
// 82 at unlimited). Swimming goes where the view points, so the swimmer sank or rose, and whether forward on a ladder
// in the water climbs up or down depends on it. Each frame's step is scaled to frame time x 30 here, so the sum is the
// console's at any rate: the amplitudes (cvar block [player +0x258] +0xDC pitch, +0xE4 roll, value at [cvar +0x14])
// for the call, then back. FCI_FRAME_BOB=1: the game's own (per frame).
const bool frame_bob = GetEnvironmentVariableA("FCI_FRAME_BOB", nullptr, 0) > 0;

HLE_THISCALL(Game_PlayerAngles, 1) {
    const uint32_t player = c.ecx, cvars = R32(player + 0x258), timer = R32(player + 0x254);
    uint32_t amp[2] = {};
    float keep[2] = {};
    if (!frame_bob && cvars && timer) {
        double dt;
        std::memcpy(&dt, rt::Host(timer + 0x68), 8);  // the frame time 0x001FF840 hands this function
        const float scale = float(std::clamp(dt, 0.0, 0.1) * 30.0);
        for (int i = 0; i < 2; i++) {
            const uint32_t cvar = R32(cvars + (i ? 0xE4 : 0xDC));
            amp[i] = cvar ? R32(cvar + 0x14) : 0;
            if (amp[i]) keep[i] = xdk::GF(amp[i]), SetF(amp[i], keep[i] * scale);
        }
    }
    const uint32_t r = xdk::CallOriginal(c, GUEST_ORIG(Game_PlayerAngles));
    for (int i = 0; i < 2; i++)
        if (amp[i]) SetF(amp[i], keep[i]);
    return r;
}
// ---- end water bob at any frame rate ----

// ---- physics step (high frame rates) ----
// The physics world's time step 0x003741E0 (stdcall: world, frame time in seconds; ret 8), called once per frame
// by 0x001FCF40 with the frame timer's time (times the game's time scale). FCI_PHYSICS_MIN_STEP=<ms> (experiment):
// frames shorter than that collect their time until it is reached, then one step over all of it.
const float physics_min_step = [] {
    char v[16] = {};
    return GetEnvironmentVariableA("FCI_PHYSICS_MIN_STEP", v, sizeof v) ? float(atof(v)) * 0.001f : 0.0f;
}();

static void CheckStaticBody();  // FCI_TRACE_CONTACT_AREAS (contact areas, below)

HLE_STDCALL(Game_PhysicsStep, 2) {
    static float pending = 0;
    float dt;
    std::memcpy(&dt, rt::Host(c.esp + 8), 4);
    if (physics_min_step > 0 && dt >= 0 && dt <= 0.2f) {
        pending += dt;
        if (pending < physics_min_step) return c.eax;
        dt = std::min(pending, 0.2f);
        pending = 0;
        std::memcpy(rt::Host(c.esp + 8), &dt, 4);
    }
    xdk::StartLevelTick(c);  // tests: FCI_TEST_COMMAND_DELAY
    const uint32_t r = xdk::CallOriginal(c, GUEST_ORIG(Game_PhysicsStep));
    CheckStaticBody();
    return r;
}
// ---- end physics step ----

// ---- contact areas within their buffer (06_Cliffs crash, 2026-11-09) ----
// The trimesh contact code 0x00393C60 (CTriMesh::Intersect: a rigid body against a mesh, the terrain too) can make
// contact areas: the points of an area go into the world's shared buffers for one collision pass, 256 points
// (0x006CD338, Vec3; prim/feature ids 0x0068F040..0x00690040), areas of 0x30 bytes at 0x006CCD38 (+4 npt, +8 nmaxpt =
// the room left in the buffers, +0x20 pt; counters 0x0068F038 points, 0x00690040 areas). The area's outline comes from
// the 2D polygon intersection 0x00441800 (two triangles of the bodies, projected onto the contact plane). The copy
// loop at 0x003970E2 stores min(n, nmaxpt) of its n points, but then sets npt = n (0x00397293) and moves the point
// counter on by n: an area claims more points than were stored and than fit. The code that walks the area's npt
// points afterwards (in the same rigid body step) reads and writes past the buffers' end into the next
// globals, first the world's static rigid body 0x006CDF40 (vtable 0x00626374, +0xD0 its contact handler = itself),
// the body of every contact with a static entity (terrain, buildings). With its vtable overwritten by point data,
// the next RegisterContact 0x0040AA20 of any rigid body touching the ground calls [[body +0xD0]] +8: read of
// 00000008, the crash. Normally n is small (3..9); near-touching triangles (one corner a fraction of a millimetre
// beyond the other triangle's edge) make the polygon routine repeat a corner until its own limit, 172 points; two
// such areas in one pass (172 + 172 > 256) and the buffer overflows. Seen at 06_Cliffs' start with the test patch
// "mercenaries -> fish" at 120 fps and unlimited, ~1 in 10 level starts; original game code (the console runs
// the same, it only needs the same near-touching geometry within one pass).
// The fix: the polygon's point count is limited to the room of its area (nmaxpt), so npt is what was stored and the
// counter stays within the buffer. Both calls of 0x00441800 (0x00396DDD, 0x003970DA) are in 0x00393C60 and lead to
// that copy loop; edi holds the geom_contact there (its area at +0x68). FCI_CONTACT_AREA_OVERFLOW=1: the original.
// FCI_TRACE_CONTACT_AREAS=1: every limited polygon, and the static body checked after each physics step.
namespace {
const bool contact_area_overflow = GetEnvironmentVariableA("FCI_CONTACT_AREA_OVERFLOW", nullptr, 0) > 0;
const bool trace_contact_areas = GetEnvironmentVariableA("FCI_TRACE_CONTACT_AREAS", nullptr, 0) > 0;
constexpr uint32_t kStaticBody = 0x006CDF40, kRigidBodyVtable = 0x00626374;
}  // namespace

static void CheckStaticBody() {
    static bool reported = false;
    if (!trace_contact_areas || reported || R32(kStaticBody) == kRigidBodyVtable) return;
    reported = true;
    rt::Log("game: contact areas: the static rigid body %08X is overwritten (vtable %08X; points used %d, areas %d)",
            kStaticBody, R32(kStaticBody), int(R32(0x0068F038)), int(R32(0x00690040)));
}

// cdecl (n1, poly1, closed, ids out; eax = slot for the points' address, ecx = n2, edx = poly2): the number of
// points of the intersection of the two polygons
HLE_CDECL(Game_ContactPolygon) {
    const uint32_t contact = c.edi, n1 = R32(c.esp + 4), n2 = c.ecx;
    const uint32_t n = xdk::CallOriginal(c, GUEST_ORIG(Game_ContactPolygon));
    const uint32_t area = contact ? R32(contact + 0x68) : 0;
    const uint32_t room = area ? R32(area + 8) : 0;
    if (!area || n <= room || room > 256) return n;
    static std::atomic<uint32_t> count{0};
    const uint32_t k = ++count;
    if (k <= 8 || trace_contact_areas)
        rt::Log("game: contact areas: polygon of %u points (from %u + %u corners), room for %u%s (%u so far)", n, n1,
                n2, room, contact_area_overflow ? ", kept (FCI_CONTACT_AREA_OVERFLOW)" : ", limited", k);
    return contact_area_overflow ? n : room;
}
// ---- end contact areas within their buffer ----

// ---- vehicle forces at the console's step ----
// CWheeledVehicleEntity (vtable 0x00625D40): its per-step function 0x003CC9A0 (thiscall, step time; ret 4; called by
// the rigid body step 0x003B2FE0 through vtable +0xD8) applies all wheel forces as impulses (suspension, engine,
// tyre friction; measured: between its calls only gravity and hull collisions act). Wheel i at +0x6A0 + i x 0xEC:
// +0x68 angular velocity, +0x70 radius, +0xA4 contact flag, +0xB0 velocity of the contact point. The drive limit of
// a wheel (0x003CF122) follows the direction of its slip, the contact velocity minus radius x angular velocity
// along the rolling direction: mu N x |slip direction . rolling direction| when |slip|^2 > 0.0025, else mu N. While
// a wheel grips, the step sets its angular velocity to the rolling speed (0x003CF316), so the next step's rolling
// slip is just the speed the car gained in one step (acceleration x step time), while the sideways slip is a
// velocity. At the console's 1/60 s vehicle steps (30 fps in two substeps) the rolling part dominates; at 300
// steps/s it is a fifth as large, the slip points sideways and the drive limit drops. Measured on the straight
// before the curve of Humvee_Beginning (02_Native): at ~300 fps 65-80 % of the engine's force reached the road
// (at 60 fps all of it), the corners bled speed differently and the truck left its path above ~150 fps.
// So the function runs at most 60 times per second of simulated time, over the time since its last call: steps
// shorter than that only collect their time (the body still moves every step with its velocity, gravity and
// collisions; the wheel impulses come once per 1/60 s, as on the console). The impulses change the velocity by
// at most a 60th of a second's worth, so between calls positions differ from a smooth path by about a millimetre.
// FCI_VEHICLE_EVERY_STEP=1: every step (the game's own).
namespace {
const bool vehicle_every_step = GetEnvironmentVariableA("FCI_VEHICLE_EVERY_STEP", nullptr, 0) > 0;
constexpr float kVehicleStep = 1.0f / 60.0f;

// the time each vehicle has collected since its last impulse step, for the vehicles stepped recently
struct VehicleTime {
    uint32_t veh;
    float pending;
};
VehicleTime g_vehicle_time[32];
uint32_t g_vehicle_time_next = 0;

float& PendingTime(uint32_t veh) {
    for (VehicleTime& v : g_vehicle_time)
        if (v.veh == veh) return v.pending;
    VehicleTime& v = g_vehicle_time[g_vehicle_time_next++ % 32];
    v = {veh, 0.0f};
    return v.pending;
}

// false: this step only collects its time; true: run the function, over the step time now at esp + 4
bool VehicleStepDue(rt::Cpu& c, uint32_t veh) {
    float dt = xdk::GF(c.esp + 4);
    if (vehicle_every_step || !(dt > 0.0f) || dt >= kVehicleStep) return true;
    float& pending = PendingTime(veh);
    pending += dt;
    // the step that comes closest to 1/60 s (half of it may still follow), at most 0.1 s
    if (pending + 0.5f * dt < kVehicleStep) return false;
    dt = std::min(pending, 0.1f);
    pending = 0.0f;
    SetF(c.esp + 4, dt);
    return true;
}
}  // namespace

// FCI_TRACE_VEHICLE=<x>,<y> (diagnostics): the wheeled vehicle first seen within 3 m of (x, y), logged every 0.1 s
// of simulated time from its per-step function. Body (CRigidEntity): +0x118 position, +0x2A0 momentum, +0x2AC
// angular momentum, +0x2B8 angular velocity, +0x2C4 velocity; +0xE0C pedal, +0xE10 steering, +0xE74 gear, +0xE94
// engine speed. Momentum changes are summed inside the calls ("imp": all wheel forces) and between them ("rest":
// contact solver, gravity, damping), as force/torque per second; "calls": impulse steps in the line.
const float* trace_vehicle = [] {
    static float at[2];
    char v[64] = {};
    if (!GetEnvironmentVariableA("FCI_TRACE_VEHICLE", v, sizeof v) || std::sscanf(v, "%f,%f", &at[0], &at[1]) != 2)
        return static_cast<const float*>(nullptr);
    return static_cast<const float*>(at);
}();

void TraceVehicle(uint32_t veh, float dt, bool called, const float* before, float call_dt) {
    static uint32_t locked = 0;
    static float t = 0, span = 0, steps = 0, calls = 0, last[6], imp[6], rest[6];
    if (!locked && std::fabs(xdk::GF(veh + 0x118) - trace_vehicle[0]) < 3 &&
        std::fabs(xdk::GF(veh + 0x11C) - trace_vehicle[1]) < 3) {
        locked = veh;
        for (int i = 0; i < 6; i++) last[i] = before[i], imp[i] = rest[i] = 0;
        rt::Log("game: vehicle %08X traced (vtable %08X)", veh, R32(veh));
    }
    if (veh != locked) return;
    for (int i = 0; i < 6; i++) {
        const float now = xdk::GF(veh + 0x2A0 + 4 * i);
        rest[i] += before[i] - last[i], imp[i] += now - before[i], last[i] = now;
    }
    t += dt, span += dt, steps++, calls += called;
    static const float from = [] {  // FCI_TRACE_VEHICLE_FROM=<s>: every impulse step for 0.6 s from then
        char v[32] = {};
        return GetEnvironmentVariableA("FCI_TRACE_VEHICLE_FROM", v, sizeof v) ? float(atof(v)) : 1e9f;
    }();
    if (called && t >= from && t < from + 0.6f) {
        const float vb = std::sqrt(before[0] * before[0] + before[1] * before[1]) * xdk::GF(veh + 0x2D4);
        rt::Log("game: vehicle call t %.4f dt %.4f |v| %.3f -> %.3f vz %.3f wz %.3f steer %.3f pedal %.2f w %.1f %.1f "
                "%.1f %.1f contacts %u%u%u%u", t, call_dt, vb,
                std::hypot(xdk::GF(veh + 0x2C4), xdk::GF(veh + 0x2C8)), xdk::GF(veh + 0x2CC), xdk::GF(veh + 0x2C0),
                xdk::GF(veh + 0xE10), xdk::GF(veh + 0xE0C), xdk::GF(veh + 0x708), xdk::GF(veh + 0x708 + 0xEC),
                xdk::GF(veh + 0x708 + 0x1D8), xdk::GF(veh + 0x708 + 0x2C4), R32(veh + 0x744) != 0,
                R32(veh + 0x744 + 0xEC) != 0, R32(veh + 0x744 + 0x1D8) != 0, R32(veh + 0x744 + 0x2C4) != 0);
    }
    if (span < 0.1f) return;
    const uint32_t n = std::min<uint32_t>(R32(veh + 0x104) - R32(veh + 0xE24), 8);
    uint32_t contacts = 0;
    for (uint32_t w = 0; w < n; w++) contacts = contacts * 10 + (R32(veh + 0x744 + w * 0xEC) != 0);
    auto f = [veh](uint32_t o) { return xdk::GF(veh + o); };
    rt::Log("game: vehicle t %.2f steps %.0f pos %.2f %.2f %.2f v %.2f %.2f %.2f w %.3f %.3f %.3f steer %.3f "
            "pedal %.2f wheels %u | imp F %.0f %.0f %.0f T %.0f %.0f %.0f | rest F %.0f %.0f %.0f T %.0f %.0f %.0f "
            "| gear %u engine %.1f calls %.0f",
            t, steps, f(0x118), f(0x11C), f(0x120), f(0x2C4), f(0x2C8), f(0x2CC), f(0x2B8), f(0x2BC), f(0x2C0),
            f(0xE10), f(0xE0C), contacts, imp[0] / span, imp[1] / span, imp[2] / span, imp[3] / span,
            imp[4] / span, imp[5] / span, rest[0] / span, rest[1] / span, rest[2] / span, rest[3] / span,
            rest[4] / span, rest[5] / span, R32(veh + 0xE74), f(0xE94), calls);
    span = steps = calls = 0;
    for (int i = 0; i < 6; i++) imp[i] = rest[i] = 0;
}

HLE_THISCALL(Game_VehicleImpulses, 1) {
    const uint32_t veh = c.ecx;
    const float dt = xdk::GF(c.esp + 4);
    float before[6];
    if (trace_vehicle)
        for (int i = 0; i < 6; i++) before[i] = xdk::GF(veh + 0x2A0 + 4 * i);
    const bool due = VehicleStepDue(c, veh);
    const float call_dt = xdk::GF(c.esp + 4);
    const uint32_t r = due ? xdk::CallOriginal(c, GUEST_ORIG(Game_VehicleImpulses)) : c.eax;
    if (due) SetF(c.esp + 4, dt);  // the caller's argument as it was
    if (trace_vehicle) TraceVehicle(veh, dt, due, before, call_dt);
    return r;
}
// ---- end vehicle forces at the console's step ----

// ---- boat water forces at the console's step (GitHub #26) ----
// The physics world steps a rigid body in steps of at most its max_time_step (+0x1D0: 0.02 s by default, the setter
// 0x003AB21B keeps it at most 0.02 s; GetMaxTimeStep 0x003B56C0 carries the rest of a frame into the next one). So on
// the console (30 fps and below) every boat was stepped exactly 50 times per second of game time; above 50 fps it is
// stepped once per frame. After each step the rigid body step 0x003B2FE0 calls the post-step method of the entity's
// container (0x003B565E, physics flag 0x800); for vehicles that is CVehicle (vtable 0x0060E248) +0x3C = 0x0019C730,
// which for boats (mode +0xA8 = 1, 2) runs CBoat's own water model 0x0019C830 (the rigid body's buoyancy, 0x003B9200,
// is switched off there: water density 0). Buoyancy (as the body's gravity, SetParams), righting and wave torque, the
// damping of the rotation and of the vertical speed all scale with the step time. The horizontal drag does not: it is
// an impulse of -(Dampv x forward speed + Dampvs x sideways speed) x mass per call (0x0019CFFB..0x0019D05E; boat_params
// Dampv 0.005 / AI 0.0025, Dampvs 0.067 jet ski, 0.042 Zodiac, 1 patrol boat). With more physics steps per second the
// boats lost more speed to it: at 60 fps 1.2x the console's drag, at 120 fps 2.2x, at 270 fps 5.4x (measured: jet ski
// at full throttle ~9.6 m/s at 120 fps instead of 15, AI patrol boats ~1/4 of their speed at unlimited).
// The fix keeps the console's cadence: the method runs at most 50 times per second of simulated time, over the time
// since its last call (the step that comes closest to 1/50 s, at most 0.1 s); shorter steps only collect their time.
// The body still moves every step, with the impulses, the gravity and the thrust gate (engine under water, +0x14) of
// the last call, as between two steps on the console. Where the collected time is not exactly 1/50 s (60 fps: a step
// is 1/60 s; 120 fps: two steps), the per-call factors are converted to it for the call: drag 1 - (1 - c)^(dt x 50),
// and the threshold of the nose-up tilt when accelerating (TiltSpdA, a speed change per call over the speed) x dt x 50.
// At 50 fps and below every call is a 1/50 s step and nothing changes. Left per call: the tilt's decay (a constant 0.1
// per call above TiltSpdMinV), between 45 and 60 calls per second instead of 50 (pitch by a fraction of a degree).
// FCI_FRAME_WATER_FORCES=1: every step with the game's own factors (the original).
namespace {
const bool frame_water_forces = GetEnvironmentVariableA("FCI_FRAME_WATER_FORCES", nullptr, 0) > 0;
constexpr float kBoatStep = 0.02f;  // the rigid body's max_time_step: the console's step
// CVehicle: +0x04 entity, +0x10 speed, +0x14 engine under water, +0xA8 mode (1, 2 boat; 3 wreck; 4 hovercraft),
// +0xB8 boat_params (+0xBC Dampv, +0xC0 Dampvs, +0xF4 TiltSpdA, +0x100 JumpStrength)
constexpr uint32_t kBoatMode = 0xA8, kDampv = 0xBC, kDampvs = 0xC0, kTiltSpdA = 0xF4, kJumpStrength = 0x100;

struct BoatTime {
    uint32_t boat;
    float pending;
};
BoatTime g_boat_time[32];
uint32_t g_boat_time_next = 0;

float& BoatPending(uint32_t boat) {
    for (BoatTime& b : g_boat_time)
        if (b.boat == boat) return b.pending;
    BoatTime& b = g_boat_time[g_boat_time_next++ % 32];
    b = {boat, 0.0f};
    LOG_FIRST(32, "game: boat %08X: water model at 50 Hz (mode %u, Dampv %g, Dampvs %g)", boat, R32(boat + 0xA8),
              xdk::GF(boat + 0xBC), xdk::GF(boat + 0xC0));
    return b.pending;
}

// a factor applied once per 1/50 s step (a share of the speed taken per call), for a call over `steps` x 1/50 s
float PerCall(float c, float steps) {
    if (!(c > 0.0f) || c >= 1.0f) return c;  // nothing, or all of it each call
    return float(1.0 - std::pow(1.0 - double(c), double(steps)));
}

// FCI_TRACE_BOAT=<x>,<y> (diagnostics): the boat first seen within 5 m of (x, y), every 0.1 s of simulated time:
// physics steps and calls of its water model (with the step times seen), position, velocity (body +0x118, +0x2C4,
// +0x2B8), speed along its heading, engine under water, the drag factors as called and the rigid body's own water
// density (+0x658, 0 for boats)
const float* trace_boat = [] {
    static float at[2];
    char v[64] = {};
    if (!GetEnvironmentVariableA("FCI_TRACE_BOAT", v, sizeof v) || std::sscanf(v, "%f,%f", &at[0], &at[1]) != 2)
        return static_cast<const float*>(nullptr);
    return static_cast<const float*>(at);
}();

void TraceBoat(uint32_t veh, float dt, bool called, float call_dt, float dampv, float dampvs) {
    static uint32_t locked = 0;
    static float t = 0, span = 0, steps = 0, calls = 0, lo = 1e9f, hi = 0, call_lo = 1e9f, call_hi = 0;
    const uint32_t entity = R32(veh + 4), body = entity ? R32(entity + 0x238) : 0;
    if (!body || R32(body) != 0x00625AF0u) return;  // rigid body
    if (!locked && std::fabs(xdk::GF(body + 0x118) - trace_boat[0]) < 5 &&
        std::fabs(xdk::GF(body + 0x11C) - trace_boat[1]) < 5) {
        locked = veh;
        rt::Log("game: boat %08X traced: mode %u Dampv %g Dampvs %g TiltSpdA %g JumpStrength %g, body %08X max step %g "
                "water density %g damping %g", veh, R32(veh + kBoatMode), xdk::GF(veh + kDampv), xdk::GF(veh + kDampvs),
                xdk::GF(veh + kTiltSpdA), xdk::GF(veh + kJumpStrength), body, xdk::GF(body + 0x1D0),
                xdk::GF(body + 0x658), xdk::GF(body + 0x270));
    }
    if (veh != locked) return;
    t += dt, span += dt, steps++, lo = std::min(lo, dt), hi = std::max(hi, dt);
    if (called) calls++, call_lo = std::min(call_lo, call_dt), call_hi = std::max(call_hi, call_dt);
    if (span < 0.1f) return;
    auto f = [body](uint32_t o) { return xdk::GF(body + o); };
    rt::Log("game: boat t %.2f steps %.0f (%.4f..%.4f) calls %.0f (%.4f..%.4f) pos %.2f %.2f %.2f v %.2f %.2f %.2f "
            "|v| %.2f w %.2f %.2f %.2f speed %.2f engine in water %u drag %.5f %.5f",
            t, steps, lo, hi, calls, calls ? call_lo : 0.0f, call_hi, f(0x118), f(0x11C), f(0x120), f(0x2C4), f(0x2C8),
            f(0x2CC), std::sqrt(f(0x2C4) * f(0x2C4) + f(0x2C8) * f(0x2C8) + f(0x2CC) * f(0x2CC)), f(0x2B8), f(0x2BC),
            f(0x2C0), xdk::GF(veh + 0x10), uint32_t(*static_cast<uint8_t*>(rt::Host(veh + 0x14))), dampv, dampvs);
    span = steps = calls = 0, lo = call_lo = 1e9f, hi = call_hi = 0;
}
}  // namespace

HLE_THISCALL(Game_BoatStep, 1) {
    const uint32_t veh = c.ecx, mode = R32(veh + kBoatMode);
    const float dt = xdk::GF(c.esp + 4);
    if (frame_water_forces || mode < 1 || mode > 4 || !(dt > 0.0f)) {
        if (trace_boat && mode >= 1 && mode <= 4)
            TraceBoat(veh, dt, true, dt, xdk::GF(veh + kDampv), xdk::GF(veh + kDampvs));
        return xdk::CallOriginal(c, GUEST_ORIG(Game_BoatStep));
    }
    float& pending = BoatPending(veh);
    pending += dt;
    if (pending + 0.5f * dt < kBoatStep) {  // the step that comes closest to 1/50 s is still to come
        if (trace_boat) TraceBoat(veh, dt, false, 0, 0, 0);
        return c.eax;
    }
    const float step = std::min(pending, 0.1f);
    pending = 0.0f;
    const bool scale = step != kBoatStep && (mode == 1 || mode == 2);
    const float keep[3] = {xdk::GF(veh + kDampv), xdk::GF(veh + kDampvs), xdk::GF(veh + kTiltSpdA)};
    if (scale) {
        const float steps = step / kBoatStep;
        SetF(veh + kDampv, PerCall(keep[0], steps));
        SetF(veh + kDampvs, PerCall(keep[1], steps));
        SetF(veh + kTiltSpdA, keep[2] * steps);
    }
    if (trace_boat) TraceBoat(veh, dt, true, step, xdk::GF(veh + kDampv), xdk::GF(veh + kDampvs));
    SetF(c.esp + 4, step);
    const uint32_t r = xdk::CallOriginal(c, GUEST_ORIG(Game_BoatStep));
    SetF(c.esp + 4, dt);  // the caller's argument as it was
    if (scale) SetF(veh + kDampv, keep[0]), SetF(veh + kDampvs, keep[1]), SetF(veh + kTiltSpdA, keep[2]);
    return r;
}
// ---- end boat water forces at the console's step ----

// ---- swimming at the console's rate ----
// A swimmer is a living entity (CLivingEntity, vtable 0x00625C40: the player and the AI, they are all CPlayer) in the
// game's swimming mode: the player's dynamics update 0x0008EB50 sets pe_player_dynamics bSwimming (+0x1B4), air control
// 1 (+0x164), inertia 1 (+0x160) and as gravity swimming_gravity (+0x13C: (0, 0, 1), buoyancy upwards). Three things
// there run per frame or per step instead of per second:
// - Every frame the movement (0x0008CF8B .. 0x0008DF30) sends pe_action_move with the swim direction. With air control
//   exactly 1 the living entity's Action 0x003BE8B0 sets the velocity and the requested velocity to that direction
//   plus +0x730, the velocity change that pe_action_set_velocity (type 0x0A, 0x003BEECA: +0x724 = new - old, moved to
//   +0x730 at the next step's start, 0x003C162B) made in the frame before. So whatever a frame added to the velocity
//   (WaterCurrent areas: SetPhysSpeed(speed + fAcceleration x frame time), 0x00212EA5) counts as a velocity for one
//   frame only, and the buoyancy (velocity += gravity x step time, 0x003C5B00) likewise lasts one step: both drift
//   speeds are proportional to the frame time. 13_Black_Forest has an upward current (WaterCurrent0, 5 m/s^2) where the
//   player lands after the opening jump: on the console 0.37 m/s up to the surface, at 120 fps 0.09 m/s.
// - The living move 0x003C4890 skips the sweep (0x003C2570) when the step's move is at most 1 mm (|move|^2 <= 1e-6 at
//   0x003C5003). At 30 fps that drops only speeds below 0.03 m/s; at 120 fps below 0.12 m/s. The idle float (buoyancy
//   only, 0.033 m/s on the console) and the Black Forest current at 120 fps (0.09 m/s) stay below it: the swimmer hangs
//   under the surface, the breath runs out and the drowning damage starts.
// The fix gives a swimmer the console's 30 fps (only for frames shorter than 30 ms: at 30 fps and below nothing
// changes): the velocity change of the frame before scaled to 1/30 s for the pe_action_move call (Game_LivingAction),
// the buoyancy scaled to 1/30 s for the step when the action reset the velocity this frame (Game_LivingStep), and a
// move below 1 mm whose speed the console would have moved (above 1 mm per 1/30 s) collected until it reaches 1 mm
// (Game_LivingMove). Swimming with the stick (a real velocity) is unchanged. FCI_FRAME_SWIM=1: the original.
namespace {
const bool frame_swim = GetEnvironmentVariableA("FCI_FRAME_SWIM", nullptr, 0) > 0;
constexpr float kConsoleStep = 1.0f / 30.0f;
constexpr float kShortFrame = 0.03f;  // frames from here on (30 fps and below) stay the game's own

// living entity: +0x124 velocity, +0x130 requested velocity, +0x13C gravity, +0x164 air control, +0x1A8 flying,
// +0x1B4 swimming, +0x1E0 (Action ignored when set), +0x730 the last step's velocity change by pe_action_set_velocity
bool Swimming(uint32_t living) {
    return R32(living) == 0x00625C40u && R32(living + 0x1B4) != 0 && R32(living + 0x1A8) != 0 &&
           xdk::GF(living + 0x164) == 1.0f;
}

struct SwimState {
    uint32_t living;
    uint32_t set_frame[2];  // the last two frames with a pe_action_set_velocity (frame timer +0), newest first
    float set_dt[2];        // and their frame times: +0x730 is the change of the frame before the movement's
    float pending[3];  // moves below 1 mm, collected
    uint32_t frame;    // the frame (frame timer +0) they were collected in
};
SwimState g_swim[64];
uint32_t g_swim_next = 0;

SwimState& SwimOf(uint32_t living) {
    for (SwimState& s : g_swim)
        if (s.living == living) return s;
    SwimState& s = g_swim[g_swim_next++ % 64];
    s = {living, {0, 0}, {0, 0}, {0, 0, 0}, 0};
    return s;
}

// FCI_TRACE_SWIM=<from>,<to> (diagnostics; level seconds): every step of the local player's living entity in that
// window to swim.log next to the EXE (position, velocity before/after, requested velocity, velocity change +0x730,
// gravity, the move collected below 1 mm; depth, breath, health of the CPlayer; after the step position, velocity,
// ground normal, flags, the edge/landing rules, the entity under the feet and its ground point's velocity)
const float* trace_swim = [] {
    static float at[2];
    char v[64] = {};
    if (!GetEnvironmentVariableA("FCI_TRACE_SWIM", v, sizeof v) || std::sscanf(v, "%f,%f", &at[0], &at[1]) != 2)
        return static_cast<const float*>(nullptr);
    return static_cast<const float*>(at);
}();
float g_edge_trace = 1.0f;  // the traced entity's last step-down factor (Game_LivingMove)
bool g_hold_trace = false;  // and whether its landing was held
uint32_t g_swim_living = 0, g_swim_player = 0;  // the local player's living entity and CPlayer (camera update)

// FCI_TRACE_SWIM_NAME=<entity name>: trace that entity (e.g. an AI) instead of the local player
const std::string trace_swim_name = [] {
    char v[64] = {};
    return GetEnvironmentVariableA("FCI_TRACE_SWIM_NAME", v, sizeof v) ? std::string(v) : std::string();
}();

void FindSwimPlayer() {  // the entity of class "Player" (single player: the local one), or the named one
    const uint32_t system = R32(0x006B38EC);
    if (!system) return;
    const uint32_t head = R32(system + 0x24);
    uint32_t n = head ? R32(head) : 0, count = 0;
    for (; n && n != head && count < 8192; n = R32(n), count++) {
        const uint32_t e = R32(n + 0xC);
        if (!e) continue;
        if (trace_swim_name.empty() ? GuestString(e + 0xC4) != "Player" : GuestString(e + 0xA8) != trace_swim_name)
            continue;
        const uint32_t phys = R32(e + 0x238);
        g_swim_living = phys && R32(phys) == 0x00625C40u ? phys : 0;
        g_swim_player = R32(e + 0x23C);
        return;
    }
}

void TraceSwim(uint32_t living, float dt, float z0, float vz0, float scale) {
    static FILE* out = std::fopen("swim.log", "w");
    if (!out) return;
    auto f = [living](uint32_t o) { return xdk::GF(living + o); };
    const SwimState& s = SwimOf(living);
    std::fprintf(out,
                 "t %.4f dt %.4f z %.4f -> %.4f vz %.4f -> %.4f req %.3f %.3f %.3f dv %.4f g %.3f x%.2f swim %u fly %u "
                 "pending %.5f |",
                 xdk::LevelSeconds(), dt, z0, f(0x120), vz0, f(0x12C), f(0x130), f(0x134), f(0x138), f(0x738), f(0x144),
                 scale, R32(living + 0x1B4), R32(living + 0x1A8),
                 std::sqrt(s.pending[0] * s.pending[0] + s.pending[1] * s.pending[1] + s.pending[2] * s.pending[2]));
    if (const uint32_t p = g_swim_player)
        std::fprintf(out, " swimming %u under %u depth %.4f breath %.2f health %.2f",
                     *static_cast<uint8_t*>(rt::Host(p + 0x43F)), *static_cast<uint8_t*>(rt::Host(p + 0x43C)),
                     xdk::GF(p + 0x444), xdk::GF(p + 0x178), xdk::GF(p + 0x160));
    // after the step: position, velocity, ground normal (+0x148), the flags +0x1AC/+0x1B0, +0x18C
    std::fprintf(out, " | pos %.4f %.4f %.4f v %.4f %.4f %.4f n %.3f %.3f %.3f f %u %u %.3f", f(0x118), f(0x11C), f(0x120),
                 f(0x124), f(0x128), f(0x12C), f(0x148), f(0x14C), f(0x150), R32(living + 0x1AC), R32(living + 0x1B0),
                 f(0x18C));
    // the edge and landing rules of Game_LivingMove, then +0x184 (step distance unit), +0x16C, +0x178, +0x1D4 (walkable
    // normal z), +0x1DC (slide normal z), +0x1C4 (flight time)
    std::fprintf(out, " hold %u edge x%.2f par %.3f %.3f %.3f %.3f %.3f %.3f", unsigned(g_hold_trace), g_edge_trace,
                 f(0x184), f(0x16C), f(0x178), f(0x1D4), f(0x1DC), f(0x1C4));
    // the entity under the feet (+0x200, its vtable) and the velocity of that ground point (+0x214)
    const uint32_t ground = R32(living + 0x200);
    std::fprintf(out, " gnd %08X %08X gv %.3f %.3f %.3f", ground, ground ? R32(ground) : 0u, f(0x214), f(0x218),
                 f(0x21C));
    std::fprintf(out, "\n");
}
}  // namespace

// thiscall (living entity, pe_action*; ret 4): for pe_action_move of a swimmer, the velocity change of the frame
// before (+0x730) as the console's 1/30 s would have made it
HLE_THISCALL(Game_LivingAction, 1) {
    const uint32_t living = c.ecx, act = R32(c.esp + 4);
    float keep[3];
    bool scaled = false;
    if (!frame_swim && R32(act) == 0x0A) {  // pe_action_set_velocity: its change (+0x724) and this frame's time
        double frame_dt;
        std::memcpy(&frame_dt, rt::Host(kFrameTimer + 0x68), 8);
        SwimState& s = SwimOf(living);
        s.set_frame[1] = s.set_frame[0], s.set_dt[1] = s.set_dt[0];
        s.set_frame[0] = R32(kFrameTimer), s.set_dt[0] = float(frame_dt);
    } else if (!frame_swim && R32(act) == 1 && R32(living + 0x1E0) == 0 && Swimming(living)) {
        // the steps come first in a frame, then the scripts (set velocity), then the movement: +0x730 is the change
        // the scripts made in the frame before
        const SwimState& s = SwimOf(living);
        const uint32_t before = R32(kFrameTimer) - 1;
        const float last = s.set_frame[0] == before ? s.set_dt[0] : s.set_frame[1] == before ? s.set_dt[1] : 0.0f;
        if (last > 0.0f && last < kShortFrame) {
            for (int i = 0; i < 3; i++) {
                keep[i] = xdk::GF(living + 0x730 + 4 * i);
                SetF(living + 0x730 + 4 * i, keep[i] * (kConsoleStep / last));
            }
            scaled = true;
        }
    }
    const uint32_t r = xdk::CallOriginal(c, GUEST_ORIG(Game_LivingAction));
    if (scaled)
        for (int i = 0; i < 3; i++) SetF(living + 0x730 + 4 * i, keep[i]);
    return r;
}

float g_living_step_dt = 0.0f;  // the step time of the living step running now (Game_PathFollow)

// thiscall (living entity, step time; ret 4): a swimmer whose velocity the movement reset this frame (velocity ==
// requested velocity) gets the buoyancy of a 1/30 s step
HLE_THISCALL(Game_LivingStep, 1) {
    const uint32_t living = c.ecx;
    const float dt = xdk::GF(c.esp + 4);
    g_living_step_dt = dt;
    const bool swimming = R32(living) == 0x00625C40u && Swimming(living);
    float scale = 1.0f, keep[3];
    if (swimming) {
        if (!frame_swim && dt > 0.0f && dt < kShortFrame &&
            std::memcmp(rt::Host(living + 0x124), rt::Host(living + 0x130), 12) == 0) {
            scale = kConsoleStep / dt;
            for (int i = 0; i < 3; i++) {
                keep[i] = xdk::GF(living + 0x13C + 4 * i);
                SetF(living + 0x13C + 4 * i, keep[i] * scale);
            }
        }
    }
    const bool trace = trace_swim && living == g_swim_living && xdk::LevelSeconds() >= trace_swim[0] &&
                       xdk::LevelSeconds() <= trace_swim[1];
    const float z0 = trace ? xdk::GF(living + 0x120) : 0, vz0 = trace ? xdk::GF(living + 0x12C) : 0;
    const uint32_t r = xdk::CallOriginal(c, GUEST_ORIG(Game_LivingStep));
    if (scale != 1.0f)
        for (int i = 0; i < 3; i++) SetF(living + 0x13C + 4 * i, keep[i]);
    if (trace) TraceSwim(living, dt, z0, vz0, scale);
    return r;
}

// Step-down at the console's move length (catalogue #16, GitHub #31): before its sweep the living move 0x003C4890
// probes the ground at the destination and, for an entity on the ground, follows it down (0x003C4EF5) when the drop is
// below +0x184 x 0.06 or "gentle": drop / horizontal length of this step's move below tan(0.7156 rad = 41 deg)
// (0x003C4E48..0x003C4E7A; the double at 0x0062AFD8 is read only there, two direct loads, no other reference in the
// code or the generated sources). The length is the step's: a 16 cm step-down at 6 m/s is a 20 cm move on the console
// (0.8, gentle: the entity steps down and walks on) but 5 cm at 120 fps (3.2: it walks off the edge and flies, with air
// control faster than walking). 05_Mines: ShaftMan1 flew down the stairs at 60-120 fps, was on the landing 1 s
// earlier, saw the player and stayed up instead of coming down as at 30 fps. For steps shorter than 30 ms the angle is
// set for the call so that the test reads drop / (length x (1/30 s) / dt), the console's move at the same speed (from
// 1/240 s down the factor stays 8), and put back right after the call. The same constant also gates that test on the
// ground normal at the destination (cos of the angle): that gate widens by the same amount. FCI_FRAME_EDGE=1: the
// game's own per-step test. FCI_TRACE_SWIM shows the factor per step ("edge x").
//
// The first landing check after a take-off at the console's step: after its sweep the move lets a flying entity land
// when its feet are within +0x184 x 0.01 (player 7.5 mm, AI 6 mm) of the probed ground (0x003C50CF; the flight time
// +0x1C4 grows by the step before). On the console the first check after a take-off comes one 1/30 s step later, 15-19
// cm on; at 120 fps 3 cm on, where the corner of the edge it left is still under it, and it lands at once (05_Mines,
// top of the stairs: the player flies 0.6 s down the ramp at 30 fps, at 120 fps he walks down). So for the first 1/30 s
// of a flight (the step that comes closest to it) the entity keeps flying: +0x73C, read in the move only by that
// decision, is set for the call. Later checks run every step as before (a check at every 1/30 s boundary instead held a
// falling player in "flight" on the ground for up to 3 steps and lost the fall damage). FCI_FRAME_LANDING=1: the first
// check at the first step as in the game.
namespace {
const bool frame_edge = GetEnvironmentVariableA("FCI_FRAME_EDGE", nullptr, 0) > 0;
const bool frame_landing = GetEnvironmentVariableA("FCI_FRAME_LANDING", nullptr, 0) > 0;
constexpr uint32_t kEdgeAngleAt = 0x0062AFD8;  // double, 0.7156 rad
}  // namespace

uint32_t LivingMoveSwim(rt::Cpu& c, uint32_t living, uint32_t pdt, uint32_t pmove);

// stdcall (living entity, &step time, &move; ret 0xC)
HLE_STDCALL(Game_LivingMove, 3) {
    const uint32_t living = R32(c.esp + 4), pdt = R32(c.esp + 8), pmove = R32(c.esp + 0xC);
    const float dt = xdk::GF(pdt);
    static double game_angle = 0;
    if (game_angle == 0) std::memcpy(&game_angle, rt::Host(kEdgeAngleAt), 8);
    const bool edge = !frame_edge && dt > 0.0f && dt < kShortFrame && R32(living) == 0x00625C40u && game_angle > 0.1 &&
                      game_angle < 1.4;
    double steps = 1.0;
    if (edge) {
        steps = double(kConsoleStep) / std::max(double(dt), 1.0 / 240.0);  // the console's move over this one
        const double angle = std::atan(std::tan(game_angle) * steps);
        std::memcpy(rt::Host(kEdgeAngleAt), &angle, 8);
    }
    if (living == g_swim_living) g_edge_trace = float(steps);
    // the first 1/30 s of a flight: no landing yet (swimmers and ladders, +0x1B4, as before)
    const bool hold = !frame_landing && dt > 0.0f && dt < kShortFrame && R32(living) == 0x00625C40u &&
                      R32(living + 0x1A8) != 0 && R32(living + 0x1B4) == 0 && R32(living + 0x73C) == 0 &&
                      double(xdk::GF(living + 0x1C4)) + 1.5 * dt < double(kConsoleStep);
    if (living == g_swim_living) g_hold_trace = hold;
    if (hold) rt::W32(living + 0x73C, 1);
    const uint32_t r = LivingMoveSwim(c, living, pdt, pmove);
    if (hold) rt::W32(living + 0x73C, 0);
    if (edge) std::memcpy(rt::Host(kEdgeAngleAt), &game_angle, 8);
    return r;
}

// a swimmer's move below 1 mm that the console would have made (at least 1 mm per 1/30 s) is collected and made once
// it reaches 1 mm; a larger move takes what was collected along
uint32_t LivingMoveSwim(rt::Cpu& c, uint32_t living, uint32_t pdt, uint32_t pmove) {
    const float dt = xdk::GF(pdt);
    if (frame_swim || !(dt > 0.0f) || dt >= kShortFrame || !Swimming(living))
        return xdk::CallOriginal(c, GUEST_ORIG(Game_LivingMove));
    SwimState& s = SwimOf(living);
    const uint32_t frame = R32(0x00644488);
    if (frame - s.frame > 2)  // left the water or a pause in between: what was collected is gone
        for (float& p : s.pending) p = 0.0f;
    s.frame = frame;
    float m[3] = {xdk::GF(pmove), xdk::GF(pmove + 4), xdk::GF(pmove + 8)};
    const float len2 = m[0] * m[0] + m[1] * m[1] + m[2] * m[2];
    const float console2 = len2 * (kConsoleStep / dt) * (kConsoleStep / dt);  // the same speed over 1/30 s
    const float* keep = nullptr;
    float sum[3];
    if (len2 > 1e-6f || console2 > 1e-6f) {
        for (int i = 0; i < 3; i++) sum[i] = s.pending[i] + m[i];
        if (len2 > 1e-6f || sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2] > 1e-6f) {
            for (int i = 0; i < 3; i++) SetF(pmove + 4 * i, sum[i]), s.pending[i] = 0.0f;
            keep = m;
        } else {
            for (int i = 0; i < 3; i++) s.pending[i] = sum[i];
        }
    } else {
        for (float& p : s.pending) p = 0.0f;  // slower than the console moves: stays, as there
    }
    const uint32_t r = xdk::CallOriginal(c, GUEST_ORIG(Game_LivingMove));
    if (keep)
        for (int i = 0; i < 3; i++) SetF(pmove + 4 * i, keep[i]);
    return r;
}
// ---- end swimming at the console's rate ----

// ---- AI path following with the console's look-ahead ----
// An AI walking a path is steered in its physics step: the living step's move 0x003C6560 calls 0x001E50B0 (stdcall
// (&position, &move; ret 8), path in ebx: points of 16 bytes at [+0x14, +0x18), index +0x20 (word)) before the move
// (only caller 0x003C6859). It is a pursuit with the step's move length L as look-ahead: it skips the path points
// within L of the entity, takes the crossing of the circle of radius L around it with the next path segment
// (0x001BEDB0), and turns the move toward that point with the length L (the path index moves on with it). On the
// console L is the move of one 1/30 s frame (20 cm at 6 m/s), at 120 fps 5 cm: the agent keeps closer to the path and
// its corners. 05_Mines: ShaftMan1 followed the path into the corner of the landing at x 45.8 (the target point across
// the wall, +y; the sweep then moves nothing) and stayed there, 1.3 m before the console's corner.
// So for steps shorter than 30 ms the look-ahead is the console's: the length of the entity's move over the last
// complete console frame (the game time cut into 1/30 s cells; the sum of the steps in it), the move is scaled to it
// for the call and the move returned scaled back to this step's length. (The AI's requested speed is its animation's
// root motion / step time and jumps from step to step at high frame rates, 0 / 11 m/s at 120 fps; a step's length
// times (1/30 s) / dt gave look-aheads up to 37 cm.)
// The console's release from a corner: the path at that landing runs into a wall corner, and at 30 fps the agent stands
// there too, 2.3 / 2.4 / 3.6 / 7.2 s in four runs, until one frame's move is long enough to reach the next segment.
// That frame comes from 30 Hz aliasing of the animation's root motion: per frame the run cycle gives 5.88 / 6.06 m/s,
// and from time to time (as the animation's phase drifts against the frames) 7.35 / 4.55 / 6.06; one 7.35 frame is a
// 24.5 cm look-ahead. Summed over console-frame cells at higher rates the root motion stays at 5.8..6.2 m/s, so the
// agent would stay in the corner for good (8 of 8 runs at 60..120 fps). At 30 fps these windows of long frames last
// 0.5..0.67 s (a long frame every third frame) and come every 8.3 s, at a phase that differs from run to run; an agent
// in a corner walks on in the next window, through the following corners of that landing too. So, only below 30 ms
// frames and only while an agent on a path stands still (path index the same and within 5 mm for two console frames,
// move requested), it gets the same windows: per entity a phase (RNG seeded with it), every 8.3 s a 0.6 s window, in it
// every third console frame a look-ahead 7.35 / 6.06 times the last console frame's; once released in a window, also
// while walking until the window ends (one stand in a series of corners, as at 30 fps). Other walking is not touched.
// FCI_FRAME_PATH=1: the step's own look-ahead (the whole fix). FCI_FRAME_PATH_RELEASE=1: no release from a corner.
// FCI_TRACE_PATH=1 logs each call for the entity of FCI_TRACE_SWIM(_NAME) (index, move before and after, look-ahead)
// and each corner release of any other agent (living entity, position, path index).
namespace {
const bool frame_path = GetEnvironmentVariableA("FCI_FRAME_PATH", nullptr, 0) > 0;
const bool trace_path = GetEnvironmentVariableA("FCI_TRACE_PATH", nullptr, 0) > 0;
const bool frame_path_release = GetEnvironmentVariableA("FCI_FRAME_PATH_RELEASE", nullptr, 0) > 0;
// the console's release from a corner (below), measured at 30 fps (05_Mines, ShaftMan1, four runs): the run cycle's long
// frame over its mean (7.35 / 6.06 m/s), the period of its aliasing windows (8.17..8.38 s) and their length (0.5..0.67 s)
constexpr double kAliasLongFrame = 7.35 / 6.06, kAliasPeriod = 8.3, kAliasWindow = 0.6;
}  // namespace

// an entity's moves per console frame: the game time cut into 1/30 s cells; the last complete cell's move is the move
// a console frame would have had (with the console's jitter of the animated speed from frame to frame)
struct ConsoleFrameMoves {
    int64_t cell = INT64_MIN;
    double x = 0, y = 0, last = 0;  // the move so far in this cell, the length of the last complete one (0: none)
    double last_call = -1;
    // release from a corner (below): where the entity stands still, since when, after how long it is released
    uint32_t index = 0xFFFFFFFFu;
    float px = 0, py = 0;
    double still_since = -1, window_end = -1;  // ... and until when a window it was released in lasts
};

HLE_STDCALL(Game_PathFollow, 2) {
    const uint32_t ppos = R32(c.esp + 4), pmove = R32(c.esp + 8), path = c.ebx;
    const uint32_t living = ppos - 0x118;
    const double dt = g_living_step_dt;
    const float in[2] = {xdk::GF(pmove), xdk::GF(pmove + 4)};
    const double len = std::sqrt(double(in[0]) * in[0] + double(in[1]) * in[1]);
    static std::unordered_map<uint32_t, ConsoleFrameMoves> moves;
    ConsoleFrameMoves& m = moves[living];
    const double now = GameTime();
    const int64_t cell = int64_t(std::floor(now * 30.0 + 1e-6));
    if (now - m.last_call > 0.1 || now < m.last_call) m = ConsoleFrameMoves{};  // a new walk: no old steps
    m.last_call = now;
    if (cell != m.cell) {
        m.last = m.cell == cell - 1 ? std::sqrt(m.x * m.x + m.y * m.y) : 0.0;
        m.cell = cell, m.x = m.y = 0;
    }
    m.x += in[0], m.y += in[1];
    double look = len;  // the console's move length here: the move of the last console frame
    const uint32_t index0 = R32(path + 0x20) & 0xFFFFu;
    bool released = false;
    if (!frame_path && dt > 0.0 && dt < kShortFrame && R32(living) == 0x00625C40u) {
        look = m.last;
        if (look <= 0) look = len * kConsoleStep / std::max(dt, 1.0 / 240.0);  // no complete console frame yet
        if (!frame_path_release && len > 1e-4) {
            const float px = xdk::GF(ppos), py = xdk::GF(ppos + 4);
            if (index0 != m.index || std::fabs(px - m.px) > 0.005f || std::fabs(py - m.py) > 0.005f ||
                m.still_since < 0)  // moving on: a new place to watch
                m.index = index0, m.px = px, m.py = py, m.still_since = now;
            // the run cycle's aliasing phase of this entity (seeded with it): a window of kAliasWindow s every
            // kAliasPeriod s, in it every third console frame the long one
            static std::unordered_map<uint32_t, double> phases;
            auto ph = phases.find(living);
            if (ph == phases.end()) {
                std::minstd_rand rng(living * 2654435761u + 1);
                ph = phases.emplace(living, std::uniform_real_distribution<double>(0, kAliasPeriod)(rng)).first;
            }
            const double in_cycle = std::fmod(now - ph->second + 100 * kAliasPeriod, kAliasPeriod);
            // released once in a window, the agent keeps the window's long frames until it ends (as at 30 fps, where
            // they come while walking too and carry it through the next corners of the same landing)
            const bool in_window = in_cycle < kAliasWindow;
            if (in_window && cell % 3 == 0 && (now - m.still_since >= 2 * kConsoleStep || now < m.window_end)) {
                look *= kAliasLongFrame, released = true;
                if (now >= m.window_end) m.window_end = now + (kAliasWindow - in_cycle);
            }
        }
    }
    const bool scale = look != len && len > 1e-6;
    if (scale) SetF(pmove, float(in[0] * look / len)), SetF(pmove + 4, float(in[1] * look / len));
    const uint32_t r = xdk::CallOriginal(c, GUEST_ORIG(Game_PathFollow));
    if (scale) SetF(pmove, float(xdk::GF(pmove) * len / look)), SetF(pmove + 4, float(xdk::GF(pmove + 4) * len / look));
    if (trace_path && living == g_swim_living)
        rt::Log("game: path at %.3f: pos %.3f %.3f index %u -> %u, move %.4f %.4f -> %.4f %.4f (look-ahead %.3f m, "
                "step %.3f m)%s",
                xdk::LevelSeconds(), xdk::GF(ppos), xdk::GF(ppos + 4), index0, unsigned(R32(path + 0x20) & 0xFFFFu),
                in[0], in[1], xdk::GF(pmove), xdk::GF(pmove + 4), look, len, released ? ", released" : "");
    else if (trace_path && released)  // the corner releases of every other agent
        rt::Log("game: path release of %08X at %.3f: pos %.3f %.3f %.3f index %u", living, xdk::LevelSeconds(),
                xdk::GF(ppos), xdk::GF(ppos + 4), xdk::GF(ppos + 8), index0);
    return r;
}
// ---- end AI path following with the console's look-ahead ----

// ---- fall damage at the console's sampling ----
// The landing event of a player or AI, 0x00090460 (thiscall CPlayer, no stack arguments; virtual, CPlayer vtable entry
// at 0x005FC448), sends the script event Land (0x18) with fEntitySpeed = -[CPlayer+0xB34]; Player.lua's fall damage
// comes from it and rises steeply with it. +0xB34 is the vertical speed the player update stores each frame
// (0x000987E3) from the living entity, and at the landing it is the speed of about 1.5 frames before the impact
// (measured with FCI_TRACE_LAND: impact speed - B34 = 1.5 x frame time x g at 30 and at 120 fps). So at 120 fps the
// event carries ~0.37 m/s more than on the console for the same fall: a 9 m fall leaves 9.7-12.1 health at 30 fps and
// kills at 120 fps. For frames shorter than 30 ms the speed is set for the call to the console's sample, B34 + 1.5 x
// (1/30 s - frame time) x g (g from the living entity, +0x144), then put back: only after a real fall (speed above
// 1.5 x 1/30 s x g) and only ever towards a smaller speed, never past 0. FCI_FRAME_FALL=1: the
// game's own value. FCI_TRACE_LAND=1 logs each landing event (time, frame time, B34 as the game had it and as sent,
// the living entity's vertical speed now).
namespace {
const bool frame_fall = GetEnvironmentVariableA("FCI_FRAME_FALL", nullptr, 0) > 0;
const bool trace_land = GetEnvironmentVariableA("FCI_TRACE_LAND", nullptr, 0) > 0;
}  // namespace

HLE_THISCALL(Game_PlayerLand, 0) {
    const uint32_t player = c.ecx;
    double dt;
    std::memcpy(&dt, rt::Host(kFrameTimer + 0x30), 8);
    const uint32_t entity = R32(player + 4), phys = entity ? R32(entity + 0x238) : 0;
    const bool living = phys && R32(phys) == 0x00625C40u;
    const float keep = xdk::GF(player + 0xB34);
    float sent = keep;
    const float g = living ? std::fabs(xdk::GF(phys + 0x144)) : 0.0f;
    // only after a real fall (falling for more than 1.5 console frames: speed above 1.5 x 1/30 s x g) and only ever
    // towards a smaller speed, never past 0: a slide or a short hop keeps the game's value
    if (!frame_fall && living && dt > 0.0 && dt < kShortFrame && keep < -1.5f * kConsoleStep * g) {
        sent = std::min(0.0f, keep + 1.5f * (kConsoleStep - float(dt)) * g);
        SetF(player + 0xB34, sent);
    }
    if (trace_land)
        rt::Log("game: land %08X at %.3f (frame %.1f ms): B34 %.3f, sent %.3f, living vz %.3f", player,
                GameTime() - g_level_start, dt * 1000, keep, sent, living ? xdk::GF(phys + 0x12C) : 0.0f);
    const uint32_t r = xdk::CallOriginal(c, GUEST_ORIG(Game_PlayerLand));
    if (sent != keep) SetF(player + 0xB34, keep);
    return r;
}
// ---- end fall damage at the console's sampling ----

// ---- entity timers see the console's frame time ----
// An entity's timer (Lua self:SetTimer, binding 0x0020FE20: CEntity +0x3E0 period, +0x3E4 start, in ms) fires in the
// entity system update (0x00035162) through the virtual CEntity::OnTimer 0x0001DD40 (thiscall, period; ret 4;
// CEntity vtable 0x005F6AA0 +0x124), which calls the script state's OnTimer. The script runs with the Lua global
// _frametime of the frame, set each frame by 0x00215C10 (stdcall: frame time, AI ticks; SetGlobalValue 0x0028B3B0
// with the script system in eax, the name in ebx, the value in xmm0). BasicPlayer:Server_OnTimer, the drowning of
// players and AI (a timer every UpdateTime while under water without breath), deals max_health / drown_time x
// _frametime per call - the frame time, not the timer's: on the console 1/30 s per call, at 120 fps a quarter of it.
// Measured (02_Native, scenario swim): health fell 7.2/s at 30 fps and 2.6/s at 120 fps once the breath was gone (dead
// 13.8 s against 38.8 s after it). In lua.dat only that callback reads _frametime inside a timer; the per-frame users
// (GameRules:OnUpdate's regeneration, Client_DeadOnUpdate, the vehicles' UpdateClient water particles) run outside.
// A timer fires once the game time has passed its period, so the period rounds up to whole frames: the 50 ms drowning
// timer fires every 66.7 ms at 30 fps and every 50 ms at 120 fps (1/30 s per call alone gave 9.6/s at 120 fps). So for
// frames shorter than 30 ms _frametime is set during an entity's OnTimer to 1/30 s x (the timer's interval at this
// rate: the game time since the entity's last OnTimer, or else the period rounded up to whole frames of this one) /
// (its interval at 30 fps), the console's damage per second, and set back to the frame's value right after (with the
// frame time of the call alone the drowning ran 8 % fast at 120 fps: frame times jitter around the mean).
// FCI_FRAME_TIMERS=1: the frame's value for the timers too. FCI_TRACE_TIMERS=1 logs each call (period, entity name,
// game time, frame time, the _frametime the callback sees).
namespace {
const bool frame_timers = GetEnvironmentVariableA("FCI_FRAME_TIMERS", nullptr, 0) > 0;
const bool trace_timers = GetEnvironmentVariableA("FCI_TRACE_TIMERS", nullptr, 0) > 0;
constexpr uint32_t kScriptSystem = 0x006B3EBC, kSetGlobalFloat = 0x0028B3B0, kFrameTimeName = 0x00616278;  // "_frametime"
float g_lua_frametime = 0.0f;  // what 0x00215C10 set last

void SetLuaFrameTime(rt::Cpu& c, float value) {
    const uint32_t eax = c.eax, ebx = c.ebx;
    const auto x0 = c.xmm[0];
    c.eax = R32(kScriptSystem);
    c.ebx = kFrameTimeName;
    c.xmm[0].f[0] = value;
    if (c.eax) rt::GuestCall(c, kSetGlobalFloat, {});
    c.eax = eax, c.ebx = ebx, c.xmm[0] = x0;
}
}  // namespace

HLE_STDCALL(Game_ScriptGlobals, 2) {
    g_lua_frametime = xdk::GF(c.esp + 4);
    return xdk::CallOriginal(c, GUEST_ORIG(Game_ScriptGlobals));
}

HLE_THISCALL(Game_EntityTimer, 1) {
    const uint32_t entity = c.ecx, period_ms = R32(c.esp + 4);
    const double dt = g_lua_frametime;
    const bool scope = !frame_timers && dt > 0.0 && dt < kShortFrame && R32(kScriptSystem);
    float value = g_lua_frametime;
    static std::unordered_map<uint32_t, double> last_fired;  // entity -> game time of its last OnTimer
    const double now = GameTime();
    double& last = last_fired[entity];
    const double since = now - last;
    last = now;
    if (scope) {  // the timer's interval here (measured, else its period rounded up to whole frames) and at 30 fps
        const double period = period_ms / 1000.0, step = kConsoleStep;
        const double here = since >= period - 0.002 && since <= period + step
                                ? since
                                : std::max(1.0, std::ceil(period / dt - 1e-3)) * dt;
        const double console = std::max(1.0, std::ceil(period / step - 1e-3)) * step;
        value = float(step * here / console);
    }
    if (trace_timers)
        rt::Log("game: timer %u of '%s' at %.3f (frame %.1f ms), _frametime %.4f", period_ms,
                GuestString(entity + 0xA8).c_str(), GameTime() - g_level_start, dt * 1000, value);
    if (scope) SetLuaFrameTime(c, value);
    const uint32_t r = xdk::CallOriginal(c, GUEST_ORIG(Game_EntityTimer));
    if (scope) SetLuaFrameTime(c, g_lua_frametime);
    return r;
}
// ---- end entity timers see the console's frame time ----

HLE_STDCALL(Game_CameraUpdate, 0) {
    static bool test_math = GetEnvironmentVariableA("FCI_TEST_CRT_MATH", nullptr, 0) > 0;
    if (test_math) {
        test_math = false;
        TestCrtMath(c);
    }
    const uint32_t cam = c.ebx;
    const float widen = pcmode::Widen(), fov_scale = pcmode::FovScale();
    float fov = xdk::GF(cam + kCamFov), aspect = xdk::GF(cam + kCamAspect);
    auto known = std::find_if(g_widened.begin(), g_widened.end(), [cam](const Widened& w) { return w.cam == cam; });
    if (known != g_widened.end() && fov == known->wide_fov && aspect == known->wide_aspect)
        fov = known->fov, aspect = known->aspect;  // still widened from the last update: the game's values
    // the screen's cameras (4:3, the split screen's views); not square render to texture views or ortho ones
    const bool screen = R32(cam + kCamOrtho) == 0 && aspect > 1.2f && aspect < 3.6f && fov > 0.05f && fov < 3.0f;
    const bool patch = (widen != 1.0f || fov_scale != 1.0f) && screen;
    // ---- direct mouse look (hle/mouse_look.cpp): the client's view (built by 0x000F0500) shows this frame's
    // mouse turn already; the game's own fov (before the widening) gives the zoom ----
    if (R32(c.esp) == 0x000F0C55u) hle::PredictView(cam, fov);
    // ---- end direct mouse look ----
    if (screen && trace_view) {  // FCI_TRACE_VIEW: where the screen's camera is and its angles (compare runs)
        static ULONGLONG last = 0;
        if (GetTickCount64() - last >= trace_view) {
            last = GetTickCount64();
            rt::Log("game: view at %.2f %.2f %.2f angles %.2f %.2f %.2f", xdk::GF(cam), xdk::GF(cam + 4), xdk::GF(cam + 8),
                    xdk::GF(cam + 0xC), xdk::GF(cam + 0x10), xdk::GF(cam + 0x14));
        }
    }
    if (trace_frame_time && R32(c.esp) == 0x000F0C55u) TraceFrameTime();
    // ---- frame-rate audit: the level's clock and FCI_TRACE_STATE ----
    if (R32(c.esp) == 0x000F0C55u) {
        if (g_level_start < 0) g_level_start = GameTime();
        if (trace_state > 0) TraceState();
        if (trace_swim) FindSwimPlayer();
    }
    // ---- end frame-rate audit ----
    {
        static std::vector<uint64_t> seen;  // each camera shape and caller once
        uint32_t from = R32(c.esp);
        uint64_t key = (uint64_t(from) << 32) ^ (uint32_t(aspect * 1000) << 12) ^ uint32_t(fov * 1000);
        if (seen.size() < 32 && std::find(seen.begin(), seen.end(), key) == seen.end()) {
            seen.push_back(key);
            rt::Log("game: camera %08X from %08X: fov %.4f aspect %.4f near %g far %g ortho %u%s", cam, from, fov,
                    aspect, xdk::GF(cam + 0x30), xdk::GF(cam + 0x34), R32(cam + kCamOrtho), patch ? " (widened)" : "");
        }
    }
    // ---- fov beyond the engine's limit ----
    // The vertical field of view (fov / aspect) stays, times fov_scale; the horizontal one grows with the screen.
    // The engine's angle (vertical x aspect) gives the frustum's sides at tan(fov / 2), so it has to stay below pi:
    // in play (game fov pi / 2) that held fov only up to ~1.45x at 16:9 and ~1.08x at 21:9, and on 32:9 below 1.
    // Beyond it the engine gets a culling angle that stays valid and at least as wide as the view (the true
    // horizontal angle, not less than the limit) with the aspect that keeps the vertical angle exact, and after
    // the update the projection's x scale is set to the real aspect (column 0 of the projection matrices; the
    // planes come from the angles, not from these). proj_fovy / proj_aspect: the picture's projection, for AO.
    float proj_fovy = fov / aspect, proj_aspect = aspect, x_fix = 1.0f;
    if (patch) {
        constexpr float kEngineLimit = 3.05f;
        const float want = fov * widen * fov_scale, wide_aspect = aspect * widen;
        proj_aspect = wide_aspect;
        if (want <= kEngineLimit) {
            proj_fovy = want / wide_aspect;
            SetF(cam + kCamFov, want);
            SetF(cam + kCamAspect, wide_aspect);
        } else {
            proj_fovy = std::min(fov / aspect * fov_scale, 2.8f);  // 160 degrees vertically at most
            const float horizontal = 2.0f * std::atan(wide_aspect * std::tan(proj_fovy * 0.5f));
            const float cull = std::clamp(horizontal, kEngineLimit, 3.1f);
            SetF(cam + kCamFov, cull);
            SetF(cam + kCamAspect, cull / proj_fovy);
            x_fix = cull / proj_fovy / wide_aspect;
            static float logged = 0;  // once per requested value: a tester's log shows it
            if (logged != fov * fov_scale) {
                logged = fov * fov_scale;
                rt::Log("game: fov x%.2f at aspect %.3f beyond the engine's angle (game fov %.4f): projection %.1f x "
                        "%.1f degrees, culling %.1f degrees wide", fov_scale, wide_aspect, fov,
                        horizontal * 57.29578f, proj_fovy * 57.29578f, cull * 57.29578f);
            }
        }
    }
    // ---- end fov beyond the engine's limit ----
    // ---- ambient occlusion (gpu/ao.h): the screen camera's projection, PerspectiveFovRH(fov / aspect, aspect) ----
    if (screen) {
        const float f = proj_fovy, a = proj_aspect;
        const float y_scale = 1.0f / std::tan(f * 0.5f);
        // (performance: in push buffer order, the render thread may still draw the frame before)
        const float x_scale = y_scale / a, far_plane = xdk::GF(cam + 0x34);
        xdk::RenderPost([=] { gpu::ao::SetProjection(x_scale, y_scale, far_plane); });
    }
    // ---- end ambient occlusion ----
    uint32_t r = xdk::CallOriginal(c, GUEST_ORIG(Game_CameraUpdate));
    // ---- fov beyond the engine's limit: projection +0x234, with depth scale +0x274, view x projection +0x2B4/+0x2F4
    // (row vectors: x_clip = column 0; the depth scale matrix of +0x274 is diagonal, so the same factor holds) ----
    if (x_fix != 1.0f)
        for (uint32_t m : {0x234u, 0x274u, 0x2B4u, 0x2F4u})
            for (uint32_t row = 0; row < 4; row++) SetF(cam + m + row * 16, xdk::GF(cam + m + row * 16) * x_fix);
    // ---- end fov beyond the engine's limit ----
    if (patch) {
        Widened w{cam, fov, aspect, xdk::GF(cam + kCamFov), xdk::GF(cam + kCamAspect)};
        if (known != g_widened.end()) *known = w;
        else if (g_widened.size() < 64) g_widened.push_back(w);
    } else if (known != g_widened.end() && xdk::GF(cam + kCamFov) == known->wide_fov) {
        SetF(cam + kCamFov, fov), SetF(cam + kCamAspect, aspect);  // no longer widened (setting off, not a screen camera)
    }
    return r;
}

// The renderer's second projection, 0x002469E0 (renderer in eax), for draw items with flag bit 0 (the first
// person view): PerspectiveFovRH(fov +0x698, width +0x6B8 / height +0x6BC x pixel aspect +0x6AC, 1e-8, far +0x6C8),
// independent of the camera. Its aspect widens the same way; 0x00246BC0 restores the camera's matrices after.
HLE_STDCALL(Game_FirstPersonProjection, 0) {
    const uint32_t renderer = c.eax;
    const float widen = pcmode::Widen();
    const float pixel_aspect = xdk::GF(renderer + 0x6AC);
    LOG_FIRST(4, "game: first person projection, fov %.4f viewport %ux%u pixel aspect %.4f far %g%s",
              xdk::GF(renderer + 0x698), R32(renderer + 0x6B8), R32(renderer + 0x6BC), pixel_aspect,
              xdk::GF(renderer + 0x6C8), widen != 1.0f ? " (widened)" : "");
    if (widen != 1.0f) SetF(renderer + 0x6AC, pixel_aspect * widen);
    // ---- direct mouse look (hle/mouse_look.cpp): the hands and the weapon with the view they were placed for ----
    float view[16];
    const bool fp_view = hle::FirstPersonView(renderer, view);
    // ---- end direct mouse look ----
    uint32_t r = xdk::CallOriginal(c, GUEST_ORIG(Game_FirstPersonProjection));
    if (fp_view) hle::FirstPersonViewDone(renderer, view);
    if (widen != 1.0f) SetF(renderer + 0x6AC, pixel_aspect);
    return r;
}
