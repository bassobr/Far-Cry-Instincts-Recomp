// Direct mouse look (mouse_mode = direct, the default): the mouse turns the player's view by a fixed angle per
// count, without going through the right stick (GitHub #2, #3).
//
// How the game turns the view: once a frame the client update (0x000F0500, client in ebp) calls the look input
// 0x0003D450 (stdcall: aim controller [client+0x24], processing command client+0x60C, TURNLR [client+0x5DC],
// TURNUD [client+0x5E0]; the actions 7 and 8 the right stick drives through the game's own dead zone of 0.35). It
// snaps nearly straight deflections to one axis, curves the values (the weapon's ControllerSensitivities), turns
// them into rates that ramp up the longer the stick is held (ControllerSpeeds, AccelerationThresholdX/Y), lets the
// aim assist (StickyAiming, AimingRotation) slow or bend them, multiplies by the frame time and by the view's
// field of view / 90 degrees (zoom), and writes the frame's yaw and pitch change in degrees to the command:
// +0xBC yaw (> 0 right), +0xC0 pitch (> 0 down; the camera's pitch +0xC grows downwards too, its yaw +0x14 to
// the left). The player applies the command right after (vtable +0xCC of the player at 0x000F08BC: it wraps the
// pitch, then holds it within +-85 degrees on foot), the command is cleared (0x000F08FE).
//
// The hook runs the game's look input unchanged (the stick, the aim assist, the pad of another player) and adds
// the mouse's degrees for this frame on top, for the first local player only (split screen: the mouse is player
// 1's). The motion is read here, as the frame's view is computed.
//
// The player's angles take the command at once, but the client builds this frame's camera from the player's
// camera of the frame before (0x000F097B copies [entity+0x104]); the turn would show one frame later. So the
// camera this frame shows (Game_CameraUpdate from 0x000F0C55, xdk/game.cpp) gets the turn added already
// (PredictView); the next frame's copy carries it for real. Where the game holds the view back, the turn the
// player took last frame falls short of what was asked. Pitch: the shown angle stays within the limits the game
// was seen to hold (85 degrees up and down on foot, measured; narrowed where a turn fell short, e.g. 45 on the
// ATV, back to 85 once the view went well past it, up to 90), so looking up or down never shows past the limit
// for more than the first frame at a new, narrower limit. Yaw (a vehicle's or a mounted gun's arc or a cutscene
// hold it): a direction whose turn fell short is not added ahead until a turn in it, or one away from it, goes
// through again (in a cutscene: the first one or two frames of motion show a turn of that frame's size, then
// nothing).
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include "hle/mouse_look.h"
#include "hle/pc_input.h"
#include "xdk/hle.h"

namespace {

bool HleOff() { return false; }

constexpr uint32_t kCmdOffset = 0x60C;          // processing command in the client
constexpr uint32_t kCmdYaw = 0xBC, kCmdPitch = 0xC0;
constexpr uint32_t kClientView = 0x58;          // the client's camera (CCamera: fov +0x38), last frame's
constexpr uint32_t kClientLocalPlayer = 0x34;   // word: local player index (0 = the first)

constexpr uint32_t kCamPitch = 0xC, kCamYaw = 0x14;  // degrees; pitch grows downwards, yaw to the left

// In a vehicle and on a mounted gun the yaw is held within a limit relative to the vehicle or the gun
// (fLimitLRAngles 100-150, angHLimit 15-130 degrees in the levels' data), and the game takes the relative angle
// modulo 360 before it clamps it: a frame's turn that carries it past 180 lands at the opposite limit (measured on
// the ATV, limit 110: 200 degrees right in one frame ended 110 degrees left). A stick never turns that far in a
// frame. The mouse's yaw goes in steps of at most kMaxYawStep a frame, the rest in the next frames: less than
// 180 minus the widest limit, so a fast flick into a limit stays there. On foot (no limit) only flicks faster than
// kMaxYawStep a frame (3000 degrees/s at 120 fps, 1500 at 60) take a frame longer.
constexpr double kMaxYawStep = 25.0;

const bool trace_look = GetEnvironmentVariableA("FCI_TRACE_LOOK", nullptr, 0) > 0;
const bool no_ahead = GetEnvironmentVariableA("FCI_NO_LOOK_AHEAD", nullptr, 0) > 0;  // diagnosis: the camera a frame late

// what the look input asked this frame and what the camera did, per axis (0 yaw, 1 pitch), in camera degrees
struct Ahead {
    uint32_t camera = 0;
    float fov = 0;               // the game's own field of view of the camera last frame
    bool pending = false;        // the look input ran since the last view
    float asked[2] = {};         // this frame's mouse turn
    float asked_last[2] = {};    // the last frame's
    float real_last[2] = {};     // the camera's angles last frame, before the prediction
    bool have_last = false;
    bool yaw_blocked[2] = {};    // [direction]: the game held a yaw turn this way back
    float pitch_limit[2] = {-85.0f, 85.0f};  // up, down: as far as the game was seen to let the view go
    double yaw_left = 0;         // the mouse's yaw beyond kMaxYawStep, for the next frames
} g_ahead;

void AddF(uint32_t va, float v) {
    float f;
    std::memcpy(&f, rt::Host(va), 4);
    f += v;
    std::memcpy(rt::Host(va), &f, 4);
}

}  // namespace

// Game_LookInput(aim, cmd, turn_lr, turn_ud), stdcall
HLE_STDCALL(Game_LookInput, 4) {
    const uint32_t cmd = a[1];
    const uint32_t r = xdk::CallOriginal(c, GUEST_ORIG(Game_LookInput));
    const uint32_t client = cmd - kCmdOffset;
    if (const uint16_t local = rt::R16(client + kClientLocalPlayer)) {  // split screen: players 2-4 keep their pads
        if (trace_look) LOG_FIRST(4, "look: local player %u (client %08X): pad only", local + 1u, client);
        return r;
    }
    double yaw = 0, pitch = 0;
    const float fov = g_ahead.camera == client + kClientView ? g_ahead.fov : 0.0f;  // last frame's view
    if (!hle::pc::TakeLookDegrees(fov, &yaw, &pitch)) yaw = pitch = 0;
    yaw += g_ahead.yaw_left;
    const double left = yaw - std::clamp(yaw, -kMaxYawStep, kMaxYawStep);
    g_ahead.yaw_left = left, yaw -= left;
    if (yaw || pitch) {
        // the player wraps its pitch before it clamps it (-20 - 200 ends at the lower limit, 85): the mouse's part
        // stops at straight up or down, from the view's pitch of last frame plus the stick's turn of this one
        const float now = xdk::GF(client + kClientView + kCamPitch) + xdk::GF(cmd + kCmdPitch);
        pitch = std::clamp(pitch, double(now - 90.0f), double(now + 90.0f));
        AddF(cmd + kCmdYaw, float(yaw));
        AddF(cmd + kCmdPitch, float(-pitch));  // pitch > 0 is up
    }
    g_ahead.camera = client + kClientView;
    g_ahead.pending = true;
    g_ahead.asked[0] = float(-yaw), g_ahead.asked[1] = float(-pitch);
    if (trace_look)
        rt::Log("look: input stick %.3f %.3f -> yaw %+.4f pitch %+.4f (mouse %+.4f %+.4f) fov %.4f actions %08X %08X%s",
                xdk::ArgF(a, 2), xdk::ArgF(a, 3), xdk::GF(cmd + kCmdYaw), xdk::GF(cmd + kCmdPitch), yaw, pitch, fov,
                rt::R32(cmd + 4), rt::R32(cmd + 8),
                left ? (" (yaw " + std::to_string(left) + " next frames)").c_str() : "");
    return r;
}

void hle::PredictView(uint32_t camera, float game_fov) {
    Ahead& g = g_ahead;
    if (camera != g.camera) return;
    g.fov = game_fov;
    const uint32_t at[2] = {camera + kCamYaw, camera + kCamPitch};
    float real[2] = {xdk::GF(at[0]), xdk::GF(at[1])};
    if (no_ahead) {
        if (trace_look) rt::Log("look: camera yaw %.4f pitch %.4f", real[0], real[1]);
        g.pending = false;
        return;
    }
    // the pitch limits: where the view went, and where last frame's turn fell short (other turns, the stick's, the
    // bob, add noise: only clear cases count)
    float* limit = g.pitch_limit;
    // (a cutscene's camera may point anywhere: the player's view never goes past straight up or down). A view well
    // past a limit learned before means it no longer holds (the player got off a vehicle or a gun): back to the
    // limits on foot, so the turn beyond it is shown at once and not a frame late; a little past it (a vehicle
    // that tilts) moves it there
    constexpr float kGone = 3.0f;
    if (real[1] < limit[0]) limit[0] = std::max(real[1] < limit[0] - kGone ? std::min(real[1], -85.0f) : real[1], -90.0f);
    if (real[1] > limit[1]) limit[1] = std::min(real[1] > limit[1] + kGone ? std::max(real[1], 85.0f) : real[1], 90.0f);
    if (g.have_last)
        for (int i = 0; i < 2; i++) {
            const float asked = g.asked_last[i];
            if (std::fabs(asked) < 0.05f) continue;
            float done = real[i] - g.real_last[i];
            if (i == 0) done = std::remainder(done, 360.0f);
            const bool short_of = done / asked < 0.5f;
            // (a yaw turn that goes through leaves the limit on the other side: ahead again when turning back to it)
            if (i == 0) g.yaw_blocked[asked > 0] = short_of, g.yaw_blocked[asked <= 0] &= short_of;
            else if (short_of) limit[asked > 0] = real[1];
        }
    float shown[2] = {real[0], real[1]};
    const float yaw = g.pending ? g.asked[0] : 0.0f, pitch = g.pending ? g.asked[1] : 0.0f;
    // (the client copies the player's camera over this one every frame: real is never last frame's shown)
    if (yaw && !g.yaw_blocked[yaw > 0]) shown[0] = real[0] + yaw;
    if (pitch) shown[1] = pitch > 0 ? std::max(real[1], std::min(real[1] + pitch, limit[1]))
                                    : std::min(real[1], std::max(real[1] + pitch, limit[0]));
    for (int i = 0; i < 2; i++) {
        if (shown[i] != real[i]) std::memcpy(rt::Host(at[i]), &shown[i], 4);
        g.real_last[i] = real[i];
    }
    g.asked_last[0] = yaw, g.asked_last[1] = pitch;
    g.have_last = true;
    g.pending = false;
    if (trace_look)  // what the frame shows (the camera), what the player's angles are (real)
        rt::Log("look: camera yaw %.4f pitch %.4f (real %.4f %.4f, pitch limits %.2f %.2f)%s", shown[0], shown[1],
                real[0], real[1], limit[0], limit[1], g.yaw_blocked[0] || g.yaw_blocked[1] ? " (yaw held)" : "");
}
