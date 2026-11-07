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
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "app/pcmode.h"
#include "gpu/ao.h"
#include "hle/mouse_look.h"  // direct mouse look
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

}  // namespace

// ---- precise frame timer (high frame rates) ----
// The game's frame timer (0x00644488, see TraceFrameTime) reads its time from 0x001FF8F0 (thiscall, the timer in
// ecx, the time in seconds in st(0)): the game's millisecond clock 0x002856F0, a stopwatch at 0x006E2390 over XAPI's
// QueryPerformanceCounter (rdtsc) that rounds to whole milliseconds (0x00286440: trunc(ms + 0.555)), times 0.001.
// At the console's 30 fps a frame is 33 or 34 ms (±1.5 %); at 120 fps 8 or 9 ms, at 400 fps 2 or 3 ms (±25 %).
// FCI_PRECISE_FRAME_TIMER=1 (experiment): the same clock without the rounding (the stopwatch's start, frequency and
// offset, the same rdtsc). Measured: it evens out the frame times, but the vehicle and swimming problems at high
// frame rates stay (HANDOFF), so the game's own clock stays the default.
const bool game_frame_timer = GetEnvironmentVariableA("FCI_PRECISE_FRAME_TIMER", nullptr, 0) == 0;

HLE_THISCALL(Game_FrameTimeSource, 0) {
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
    return xdk::CallOriginal(c, GUEST_ORIG(Game_PhysicsStep));
}
// ---- end physics step ----

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
