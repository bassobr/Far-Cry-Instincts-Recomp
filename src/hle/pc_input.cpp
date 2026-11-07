// Keyboard and mouse for player 1: the pad buttons and stick directions bound to keys, mouse buttons and the
// wheel (FarCryInstincts.ini, bind_<button> = <key>[, <key>...]), and mouse look on the right stick.
// The wheel as two bindable keys follows SCDA-XBERecomp (MIT, Copyright (c) 2026 TGP482, Joshhhuaaa),
// src/core/input.cpp: codes Windows leaves unassigned, a notch is a short press.
//
// Mouse look, mouse_mode = direct (the default): the motion becomes degrees (mouse_sensitivity per 10 counts, with
// the optional acceleration, smoothing and zoom scale) that hle/mouse_look.cpp adds to the view angles in the game's
// look input, once a frame (TakeLookDegrees). Where that does not run, and with mouse_mode = stick, the old way:
// the game turns the view at a rate given by the right stick's deflection. The motion is turned into an angle, and
// each poll deflects the stick so that the game's turn rate, measured for this game (kYaw/kPitch), times the
// frame's time turns that angle. What the stick cannot turn at once (the game's top rate) carries over to the next
// polls, so a given amount of motion always turns the same angle, however fast it is made.
#include "hle/pc_input.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "app/settings.h"
#include "runtime/guest_time.h"
#include "runtime/log.h"
#include "video/display.h"

namespace hle::pc {

namespace {

constexpr int kWheelUp = 0x0E, kWheelDown = 0x0F;  // as SCDA: codes Windows leaves unassigned
constexpr int kMaxCode = 0x100;

// ---- key names ----

struct KeyName {
    const char* name;
    int vk;
};
constexpr KeyName kKeyNames[] = {
    {"space", VK_SPACE},        {"enter", VK_RETURN},      {"return", VK_RETURN},     {"escape", VK_ESCAPE},
    {"esc", VK_ESCAPE},         {"tab", VK_TAB},           {"backspace", VK_BACK},    {"capslock", VK_CAPITAL},
    {"shift", VK_SHIFT},        {"lshift", VK_LSHIFT},     {"rshift", VK_RSHIFT},     {"ctrl", VK_CONTROL},
    {"lctrl", VK_LCONTROL},     {"rctrl", VK_RCONTROL},    {"alt", VK_MENU},          {"lalt", VK_LMENU},
    {"ralt", VK_RMENU},         {"up", VK_UP},             {"down", VK_DOWN},         {"left", VK_LEFT},
    {"right", VK_RIGHT},        {"insert", VK_INSERT},     {"delete", VK_DELETE},     {"home", VK_HOME},
    {"end", VK_END},            {"pageup", VK_PRIOR},      {"pagedown", VK_NEXT},     {"minus", VK_OEM_MINUS},
    {"equals", VK_OEM_PLUS},    {"comma", VK_OEM_COMMA},   {"period", VK_OEM_PERIOD}, {"slash", VK_OEM_2},
    {"semicolon", VK_OEM_1},    {"apostrophe", VK_OEM_7},  {"lbracket", VK_OEM_4},    {"rbracket", VK_OEM_6},
    {"backslash", VK_OEM_5},    {"grave", VK_OEM_3},       {"multiply", VK_MULTIPLY}, {"add", VK_ADD},
    {"subtract", VK_SUBTRACT},  {"decimal", VK_DECIMAL},   {"divide", VK_DIVIDE},     {"mouse1", VK_LBUTTON},
    {"lmb", VK_LBUTTON},        {"mouse2", VK_RBUTTON},    {"rmb", VK_RBUTTON},       {"mouse3", VK_MBUTTON},
    {"mmb", VK_MBUTTON},        {"mouse4", VK_XBUTTON1},   {"mouse5", VK_XBUTTON2},   {"wheelup", kWheelUp},
    {"wheeldown", kWheelDown},
};

std::string Lower(std::string s) {
    for (char& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t"), e = s.find_last_not_of(" \t");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

// a key's virtual key code (or a wheel direction), -1 if the name is unknown
int ParseKey(const std::string& text) {
    std::string name = Lower(Trim(text));
    if (name.size() == 1 && std::isalnum(static_cast<unsigned char>(name[0])))
        return std::toupper(static_cast<unsigned char>(name[0]));
    if (name.size() >= 2 && name[0] == 'f' && std::isdigit(static_cast<unsigned char>(name[1]))) {
        int n = std::atoi(name.c_str() + 1);
        if (n >= 1 && n <= 24) return VK_F1 + n - 1;
    }
    if (name.size() == 4 && name.compare(0, 3, "num") == 0 && std::isdigit(static_cast<unsigned char>(name[3])))
        return VK_NUMPAD0 + (name[3] - '0');
    if (name.size() > 2 && name.compare(0, 2, "0x") == 0) {
        int vk = int(std::strtol(name.c_str() + 2, nullptr, 16));
        return vk > 0 && vk < kMaxCode ? vk : -1;
    }
    for (const KeyName& k : kKeyNames)
        if (name == k.name) return k.vk;
    return -1;
}

const char* KeyLabel(int code) {
    static char buf[8][16];
    static int next = 0;
    for (const KeyName& k : kKeyNames)
        if (k.vk == code) return k.name;
    char* b = buf[next++ & 7];
    if ((code >= 'A' && code <= 'Z') || (code >= '0' && code <= '9')) snprintf(b, 16, "%c", char(std::tolower(code)));
    else if (code >= VK_F1 && code <= VK_F24) snprintf(b, 16, "f%d", code - VK_F1 + 1);
    else if (code >= VK_NUMPAD0 && code <= VK_NUMPAD9) snprintf(b, 16, "num%d", code - VK_NUMPAD0);
    else snprintf(b, 16, "0x%02x", code);
    return b;
}

// ---- what the bindings press: the pad's buttons and the sticks' directions, the names of FCI_INPUT_SCRIPT ----

struct Target {
    const char* name;
    uint16_t button;  // digital button bit
    int analog;       // analog button index, -1 for none
    int axis, sign;   // stick direction: thumb index (LX LY RX RY) and sign, -1 for none
    const char* keys;  // the default binding (the fixed map of earlier versions)
};
constexpr Target kTargets[] = {
    {"up", kDpadUp, -1, -1, 0, "up"},           {"down", kDpadDown, -1, -1, 0, "down"},
    {"left", kDpadLeft, -1, -1, 0, "left"},     {"right", kDpadRight, -1, -1, 0, "right"},
    {"start", kStart, -1, -1, 0, "enter"},      {"back", kBack, -1, -1, 0, "escape"},
    {"lthumb", kLeftThumb, -1, -1, 0, "lshift"}, {"rthumb", kRightThumb, -1, -1, 0, "v"},
    {"a", 0, kA, -1, 0, "space"},               {"b", 0, kB, -1, 0, "c"},
    {"x", 0, kX, -1, 0, "e"},                   {"y", 0, kY, -1, 0, "q"},
    {"white", 0, kWhite, -1, 0, "f"},           {"black", 0, kBlack, -1, 0, "g"},
    {"lt", 0, kLeftTrigger, -1, 0, "mouse2"},   {"rt", 0, kRightTrigger, -1, 0, "mouse1"},
    {"lup", 0, -1, 1, 1, "w"},                  {"ldown", 0, -1, 1, -1, "s"},
    {"lleft", 0, -1, 0, -1, "a"},               {"lright", 0, -1, 0, 1, "d"},
    {"rup", 0, -1, 3, 1, "i"},                  {"rdown", 0, -1, 3, -1, "k"},
    {"rleft", 0, -1, 2, -1, "j"},               {"rright", 0, -1, 2, 1, "l"},
};
constexpr int kTargetCount = int(sizeof kTargets / sizeof kTargets[0]);

std::vector<int> ParseKeys(const std::string& list, const char* target) {
    std::vector<int> keys;
    size_t at = 0;
    while (at <= list.size()) {
        size_t end = list.find_first_of(", ", at);
        if (end == std::string::npos) end = list.size();
        std::string name = Trim(list.substr(at, end - at));
        at = end + 1;
        if (name.empty() || Lower(name) == "none") continue;
        int vk = ParseKey(name);
        if (vk < 0) rt::Log("input: bind_%s: unknown key '%s'", target, name.c_str());
        else keys.push_back(vk);
    }
    return keys;
}

// settings menu: read again after a change (ReloadBindings), while the guest is held
std::vector<std::vector<int>> g_binds;
bool g_binds_read = false;

const std::vector<std::vector<int>>& Bindings() {
    if (!g_binds_read) {
        g_binds_read = true;
        g_binds = [] {
        std::vector<std::vector<int>> b(kTargetCount);
        std::vector<bool> set(kTargetCount);
        std::vector<int> claimed;  // keys the ini binds: they leave the default bindings of other buttons
        for (int i = 0; i < kTargetCount; i++) {
            std::string key = std::string("bind_") + kTargets[i].name;
            std::string value = settings::String(key.c_str(), kTargets[i].keys);
            b[i] = ParseKeys(value, kTargets[i].name);
            set[i] = value != kTargets[i].keys;
            if (set[i]) claimed.insert(claimed.end(), b[i].begin(), b[i].end());
        }
        std::string changed;
        for (int i = 0; i < kTargetCount; i++) {
            if (!set[i])
                std::erase_if(b[i], [&](int vk) { return std::find(claimed.begin(), claimed.end(), vk) != claimed.end(); });
            if (b[i] != ParseKeys(kTargets[i].keys, kTargets[i].name)) {
                changed += std::string(" ") + kTargets[i].name + "=";
                for (size_t k = 0; k < b[i].size(); k++) changed += std::string(k ? "+" : "") + KeyLabel(b[i][k]);
                if (b[i].empty()) changed += "none";
            }
        }
        rt::Log("input: keyboard/mouse bindings%s", changed.empty() ? ": defaults" : (":" + changed).c_str());
        return b;
        }();
    }
    return g_binds;
}

// ---- injected host input, for unattended tests ----
// FCI_KEY_SCRIPT="30:space,31.5:mouse1:0.5,40:wheelup:2,200:mx:900:1,205:my:-200": at that many seconds (the
// clock of FCI_INPUT_SCRIPT) a key or mouse button is held (0.2 s or the given time), the wheel turns notches
// (1 or the given number), the mouse moves counts in x or y (spread evenly over the given seconds, else at
// once). Works without the window's focus and without a captured mouse; goes through the bindings and the
// mouse look like the real devices.

struct Injected {
    double at, duration;
    int code;    // key, or -1 for mouse motion
    int axis;    // mouse: 0 = x, 1 = y
    double amount;  // mouse counts, wheel notches
};

const std::vector<Injected>& KeyScript() {
    static const std::vector<Injected> script = [] {
        std::vector<Injected> out;
        char text[4096] = {};
        if (!GetEnvironmentVariableA("FCI_KEY_SCRIPT", text, sizeof text)) return out;
        for (char* item = std::strtok(text, ","); item; item = std::strtok(nullptr, ",")) {
            char* name = std::strchr(item, ':');
            if (!name) continue;
            *name++ = 0;
            char* arg = std::strchr(name, ':');
            if (arg) *arg++ = 0;
            char* arg2 = arg ? std::strchr(arg, ':') : nullptr;
            if (arg2) *arg2++ = 0;
            double at = std::atof(item);
            std::string n = Lower(name);
            if (n == "mx" || n == "my") {
                out.push_back({at, arg2 ? std::atof(arg2) : 0.0, -1, n == "my", arg ? std::atof(arg) : 0.0});
                continue;
            }
            int vk = ParseKey(n);
            if (vk < 0) {
                rt::Log("input: unknown FCI_KEY_SCRIPT key '%s'", name);
                continue;
            }
            bool wheel = vk == kWheelUp || vk == kWheelDown;
            out.push_back({at, wheel ? 0.0 : (arg ? std::atof(arg) : 0.2), vk, 0, wheel ? (arg ? std::atof(arg) : 1) : 0});
        }
        rt::Log("input: FCI_KEY_SCRIPT with %zu events", out.size());
        return out;
    }();
    return script;
}

bool g_script_mouse = false;  // the script moves the mouse: mouse look works without a captured mouse

// ---- the wheel: a notch is a press of kTapSeconds, then a release of kGapSeconds ----

constexpr double kTapSeconds = 0.1, kGapSeconds = 0.06;
struct Wheel {
    int pending = 0;
    double down_until = -1, free_at = -1;
} g_wheel[2];  // up, down

void UpdateWheel(double now, int real_notches, double script_from, double script_to) {
    int notches[2] = {real_notches > 0 ? real_notches : 0, real_notches < 0 ? -real_notches : 0};
    for (const Injected& e : KeyScript())
        if ((e.code == kWheelUp || e.code == kWheelDown) && e.at >= script_from && e.at < script_to)
            notches[e.code == kWheelDown] += int(e.amount);
    for (int d = 0; d < 2; d++) {
        Wheel& w = g_wheel[d];
        w.pending = std::min(w.pending + notches[d], 10);
        if (w.pending && now >= w.free_at) {
            w.pending--;
            w.down_until = now + kTapSeconds;
            w.free_at = w.down_until + kGapSeconds;
        }
    }
}

// ---- mouse look ----

// The game's turn rate (degrees per second) over the right stick's deflection, measured in the first level with
// FCI_INPUT_SCRIPT holding the stick at fixed deflections for 1-1.5 s and the camera's angles traced from guest
// memory (FCI_TRACE_FLOATS, camera at 0x0072FCD4: position, then pitch, roll, yaw in degrees). Linear between
// points. Nothing turns below ~0.37; from 0.95 on the game accelerates the turn the longer the stick is held
// (125 deg/s rising past 240 deg/s within 0.6 s), so the mouse stays below that, at up to 0.94 (~124 deg/s).
// Pitch turns at ~0.55 times the yaw rate (measured at 0.65, 0.8 and 0.9), ~78 deg/s at 0.94. The top points
// were corrected with the scripted mouse test (FCI_KEY_SCRIPT): fast and slow motion turn the same angle.
struct Point {
    double deflection, rate;
};
constexpr double kFull = 32767;
const Point kYaw[] = {{0.37 * kFull, 0},    {0.40 * kFull, 0.75}, {0.45 * kFull, 1.72},  {0.50 * kFull, 3.1},
                      {0.55 * kFull, 4.4},  {0.65 * kFull, 8.7},  {0.70 * kFull, 12.0},  {0.75 * kFull, 16.8},
                      {0.80 * kFull, 26.8}, {0.85 * kFull, 43.0}, {0.88 * kFull, 61.3},  {0.90 * kFull, 84.0},
                      {0.92 * kFull, 108.5}, {0.94 * kFull, 124.5}};
const Point kPitch[] = {{0.37 * kFull, 0},    {0.40 * kFull, 0.41}, {0.45 * kFull, 0.95},  {0.50 * kFull, 1.7},
                        {0.55 * kFull, 2.4},  {0.65 * kFull, 4.8},  {0.70 * kFull, 6.6},   {0.75 * kFull, 9.2},
                        {0.80 * kFull, 13.4}, {0.85 * kFull, 23.7}, {0.88 * kFull, 33.7},  {0.90 * kFull, 46.5},
                        {0.92 * kFull, 62.0}, {0.94 * kFull, 78.0}};

double Rate(const Point* curve, int n, double d) {
    if (d <= curve[0].deflection) return 0;
    for (int i = 1; i < n; i++)
        if (d <= curve[i].deflection)
            return curve[i - 1].rate + (curve[i].rate - curve[i - 1].rate) * (d - curve[i - 1].deflection) /
                                           (curve[i].deflection - curve[i - 1].deflection);
    return curve[n - 1].rate;
}

// the smallest deflection that turns at least this rate (the top deflection if none does)
double Deflection(const Point* curve, int n, double rate) {
    if (rate <= 0) return 0;
    for (int i = 1; i < n; i++)
        if (rate <= curve[i].rate)
            return curve[i - 1].deflection + (curve[i].deflection - curve[i - 1].deflection) * (rate - curve[i - 1].rate) /
                                                 (curve[i].rate - curve[i - 1].rate);
    return curve[n - 1].deflection;
}

struct Axis {
    const Point* curve;
    int points;
    double pending = 0;  // degrees still to turn (signed)
};
Axis g_yaw{kYaw, int(sizeof kYaw / sizeof kYaw[0])}, g_pitch{kPitch, int(sizeof kPitch / sizeof kPitch[0])};

enum class ZoomScale { kFov, kTan, kOff };

struct MouseSettings {
    bool direct;                  // mouse_mode: direct (onto the view angles) or stick (the old right stick)
    double degrees_x, degrees_y;  // per count
    bool invert_y;
    double acceleration;          // extra gain per count/ms of speed, 0 for none
    double acceleration_cap;      // the gain's ceiling (a factor, >= 1)
    double smoothing;             // seconds the motion is spread over, 0 for none
    ZoomScale zoom_scale;         // while zoomed: the game's fov ratio, the ratio of tan(fov / 2), or none
    double zoom_sensitivity;      // times this while zoomed
};

MouseSettings g_mouse;
bool g_mouse_read = false;  // settings menu: read again after a change (ReloadMouse)

const MouseSettings& Mouse() {
    if (g_mouse_read) return g_mouse;
    g_mouse_read = true;
    g_mouse = [] {
        MouseSettings m;
        m.direct = Lower(Trim(settings::String("mouse_mode", "direct"))) != "stick";
        double base = std::clamp(settings::Float("mouse_sensitivity", 1.0f), 0.01f, 100.0f);
        m.degrees_x = std::clamp(settings::Float("mouse_sensitivity_x", float(base)), 0.01f, 100.0f) * 0.1;
        m.degrees_y = std::clamp(settings::Float("mouse_sensitivity_y", float(base)), 0.01f, 100.0f) * 0.1;
        m.invert_y = settings::Bool("mouse_invert_y", false);
        m.acceleration = std::max(0.0f, settings::Float("mouse_acceleration", 0.0f));
        m.acceleration_cap = std::max(1.0f, settings::Float("mouse_acceleration_cap", 2.0f));
        m.smoothing = std::clamp(settings::Float("mouse_smoothing", 0.0f), 0.0f, 200.0f) / 1000.0;
        std::string zoom = Lower(Trim(settings::String("mouse_zoom_scale", "fov")));
        m.zoom_scale = zoom == "tan" ? ZoomScale::kTan : zoom == "off" ? ZoomScale::kOff : ZoomScale::kFov;
        m.zoom_sensitivity = std::clamp(settings::Float("mouse_zoom_sensitivity", 1.0f), 0.05f, 10.0f);
        rt::Log("input: mouse look %s, %.3f/%.3f degrees per count%s, acceleration %.2f (cap %.2f), smoothing %.0f ms, "
                "zoom %s x %.2f",
                m.direct ? "direct" : "through the right stick", m.degrees_x, m.degrees_y,
                m.invert_y ? ", inverted y" : "", m.acceleration, m.acceleration_cap, m.smoothing * 1000,
                m.zoom_scale == ZoomScale::kTan ? "tan" : m.zoom_scale == ZoomScale::kOff ? "off" : "fov",
                m.zoom_sensitivity);
        return m;
    }();
    return g_mouse;
}

// mouse_acceleration: the gain for this much motion in this time (counts per ms, so the same at any frame rate)
double AccelerationGain(const MouseSettings& m, double counts, double dt) {
    if (m.acceleration <= 0 || dt <= 0) return 1;
    return std::min(1 + m.acceleration * std::fabs(counts) / (dt * 1000), m.acceleration_cap);
}

// ---- direct mouse look (hle/mouse_look.cpp): the motion goes onto the view angles in the game's look input,
// once a frame, after the game's stick handling; the poll only hands it the scripted counts ----
struct Direct {
    double script_x = 0, script_y = 0;  // FCI_KEY_SCRIPT counts since the last look input
    double last = -1;                   // script time of the last look input
    bool focused = false;               // as the last poll saw it
    // mouse_smoothing: each frame's degrees spread evenly over [from, from + smoothing]
    struct Sample {
        double from, yaw, pitch;
    };
    std::vector<Sample> samples;
} g_direct;
// the look input ran this recently: the mouse is direct (else the right stick takes it)
constexpr double kDirectTimeout = 0.25;
// ---- end direct mouse look ----

// ---- settings menu: after the menu closes, keys and buttons still held reach the game only once let go ----
bool g_settle_pending = false;
bool g_stale[kMaxCode] = {};
// ---- end settings menu ----

constexpr double kMaxBacklog = 0.5;  // seconds at the top rate that may wait; more is dropped

// The stick value for one axis this poll. The game polls once a frame and turns by the rate of what it read times
// the time since its previous frame (a 100 ms hitch turns three times as far), so the angle due is asked for as
// a rate over dt, the time since our previous poll; what the top rate cannot turn waits for the next frames.
int16_t Look(Axis& a, double degrees, double dt, bool active) {
    if (!active) {
        a.pending = 0;
        return 0;
    }
    a.pending += degrees;
    double top = a.curve[a.points - 1].rate;
    a.pending = std::clamp(a.pending, -top * kMaxBacklog, top * kMaxBacklog);
    if (std::fabs(a.pending) < 1e-3) a.pending = 0;  // what the deflection's resolution leaves over
    if (dt <= 0 || !a.pending) return 0;
    double want = std::min(std::fabs(a.pending) / dt, top);
    double d = std::floor(Deflection(a.curve, a.points, want));  // the remainder goes with the next frame
    double turned = Rate(a.curve, a.points, d) * dt;
    if (turned <= 0) return 0;
    double sign = a.pending < 0 ? -1 : 1;
    a.pending = sign * std::max(0.0, std::fabs(a.pending) - turned);
    int v = std::min(int(d), 32767);
    return int16_t(sign < 0 ? -v : v);
}

// ---- FCI_TRACE_PAD ----

bool TraceOn() {
    static const bool on = GetEnvironmentVariableA("FCI_TRACE_PAD", nullptr, 0) > 0;
    return on;
}

}  // namespace

double ScriptSeconds() {
    static const int64_t start = rt::GuestQpc();
    static const double frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return double(f.QuadPart);
    }();
    return double(rt::GuestQpc() - start) / frequency;
}

void ApplyKeyboardMouse(Pad& p, bool focused) {
    const auto& binds = Bindings();
    const MouseSettings& mouse = Mouse();
    double now = ScriptSeconds();
    static double last = now;
    double dt = std::clamp(now - last, 0.0, 0.25);

    // the script's keys held now, and its mouse motion since the last poll
    bool held[kMaxCode] = {};
    double script_dx = 0, script_dy = 0;
    for (const Injected& e : KeyScript()) {
        if (e.code >= 0) {
            if (e.code != kWheelUp && e.code != kWheelDown && now >= e.at && now < e.at + e.duration) held[e.code] = true;
            continue;
        }
        double counts = 0;
        if (e.duration <= 0) counts = e.at >= last && e.at < now ? e.amount : 0;
        else counts = e.amount * (std::clamp(now, e.at, e.at + e.duration) - std::clamp(last, e.at, e.at + e.duration)) / e.duration;
        if (counts) g_script_mouse = true;
        (e.axis ? script_dy : script_dx) += counts;
    }
    // ---- settings menu: what is held as the menu closes (the key that closed it) waits for its release; the
    // motion and the wheel while it was open are dropped ----
    if (g_settle_pending && focused) {
        g_settle_pending = false;
        for (int code = 1; code < kMaxCode; code++) g_stale[code] = (GetAsyncKeyState(code) & 0x8000) != 0;
        int ignore_x, ignore_y;
        display::TakeMouseMotion(&ignore_x, &ignore_y);
        display::TakeWheelNotches();
        g_yaw.pending = g_pitch.pending = 0;
        g_direct.script_x = g_direct.script_y = 0, g_direct.samples.clear();
    }
    // ---- end settings menu ----
    UpdateWheel(now, focused ? display::TakeWheelNotches() : 0, last, now);

    auto down = [&](int code) {
        if (code == kWheelUp || code == kWheelDown) return now < g_wheel[code == kWheelDown].down_until;
        if (held[code]) return true;
        if (!focused) return false;
        bool is_down = (GetAsyncKeyState(code) & 0x8000) != 0;
        if (g_stale[code]) {  // settings menu
            if (!is_down) g_stale[code] = false;
            return false;
        }
        return is_down;
    };

    int axis_neg[4] = {}, axis_pos[4] = {};
    for (int i = 0; i < kTargetCount; i++) {
        bool on = false;
        for (int code : binds[i]) on = on || down(code);
        if (!on) continue;
        const Target& t = kTargets[i];
        p.buttons |= t.button;
        if (t.analog >= 0) p.analog[t.analog] = 255;
        if (t.axis >= 0) (t.sign > 0 ? axis_pos : axis_neg)[t.axis] = 1;
    }
    int16_t keys[4];
    for (int i = 0; i < 4; i++) {
        keys[i] = int16_t((axis_pos[i] ? 32767 : 0) - (axis_neg[i] ? 32767 : 0));
        if (keys[i]) p.thumb[i] = keys[i];
    }

    // direct mouse look: the game's look input took the motion since the previous poll (hle/mouse_look.cpp), so
    // the scripted counts wait there for the next one. Where it does not run (menus, no player in control), the
    // motion falls back to the right stick below, as with mouse_mode = stick
    g_direct.focused = focused;
    const bool direct = mouse.direct && g_direct.last >= 0 && now - g_direct.last < kDirectTimeout;
    if (direct) {
        g_direct.script_x += script_dx, g_direct.script_y += script_dy;
        g_yaw.pending = g_pitch.pending = 0;
        last = now;
        return;
    }
    g_direct.script_x = g_direct.script_y = 0, g_direct.samples.clear();

    // mouse look through the right stick, unless keys push it
    int dx = 0, dy = 0;
    bool captured = focused && display::TakeMouseMotion(&dx, &dy);
    double mx = dx + script_dx, my = dy + script_dy;
    double gain = AccelerationGain(mouse, std::sqrt(mx * mx + my * my), dt);
    mx *= gain, my *= gain;
    bool active = (captured || g_script_mouse) && !keys[2] && !keys[3];
    int16_t rx = Look(g_yaw, mx * mouse.degrees_x, dt, active);
    int16_t ry = Look(g_pitch, -my * mouse.degrees_y * (mouse.invert_y ? -1 : 1), dt, active);
    if (rx || ry) p.thumb[2] = rx, p.thumb[3] = ry;  // a pad's stick when the mouse rests
    if (TraceOn() && (mx || my || rx || ry))
        rt::Log("input: %.3f s mouse %.0f,%.0f dt %.1f ms -> stick %d,%d pending %.2f,%.2f deg", now, mx, my, dt * 1000,
                rx, ry, g_yaw.pending, g_pitch.pending);
    last = now;
}

// ---- direct mouse look (hle/mouse_look.cpp) ----
double ZoomFactor(double fov) {
    const MouseSettings& m = Mouse();
    constexpr double kNormal = 1.5707963;  // the game's own reference: it scales the stick by fov / 90 degrees
    if (!(fov > 0.01 && fov < kNormal - 1e-3)) return 1;  // not zoomed (or a wider view: no faster mouse)
    double f = m.zoom_scale == ZoomScale::kFov ? fov / kNormal
               : m.zoom_scale == ZoomScale::kTan ? std::tan(fov / 2) / std::tan(kNormal / 2)
                                                 : 1.0;
    return f * m.zoom_sensitivity;
}

bool TakeLookDegrees(double fov, double* yaw, double* pitch) {
    const MouseSettings& m = Mouse();
    *yaw = *pitch = 0;
    const double now = ScriptSeconds();
    const double dt = g_direct.last >= 0 ? std::clamp(now - g_direct.last, 0.0, 0.25) : 0.0;
    const double from = g_direct.last >= 0 ? g_direct.last : now;
    g_direct.last = now;
    if (!m.direct) return false;
    int dx = 0, dy = 0;
    if (g_direct.focused) display::TakeMouseMotion(&dx, &dy);
    // FCI_TEST_MOUSE_UNEVEN=<f> (tests, 0-1): the scripted counts reach the look input unevenly, like a real mouse
    // read out of step with the frames: every other look input takes (1 - f) of its counts, the next one the rest
    // on top of its own (an even motion of c a frame becomes c (1 - f), c (1 + f), ...; the sum stays exact)
    static const double uneven = [] {
        char v[32] = {};
        return GetEnvironmentVariableA("FCI_TEST_MOUSE_UNEVEN", v, sizeof v) ? std::clamp(std::atof(v), 0.0, 1.0) : 0.0;
    }();
    if (uneven > 0) {
        static bool hold = false;
        static double held_x = 0, held_y = 0;
        double sx = g_direct.script_x + held_x, sy = g_direct.script_y + held_y;
        held_x = held_y = 0;
        if (g_direct.script_x || g_direct.script_y) {
            hold = !hold;
            if (hold) held_x = g_direct.script_x * uneven, held_y = g_direct.script_y * uneven, sx -= held_x, sy -= held_y;
        }
        g_direct.script_x = sx, g_direct.script_y = sy;
    }
    double mx = dx + g_direct.script_x, my = dy + g_direct.script_y;
    g_direct.script_x = g_direct.script_y = 0;
    const double gain = AccelerationGain(m, std::sqrt(mx * mx + my * my), dt);
    const double zoom = ZoomFactor(fov);
    double dyaw = mx * m.degrees_x * gain * zoom;
    double dpitch = -my * m.degrees_y * gain * zoom * (m.invert_y ? -1 : 1);  // mouse forward (dy < 0) looks up
    if (m.smoothing > 0) {
        // the motion of [from, now] spread over [from, from + smoothing]: what falls before now goes this frame
        if (dyaw || dpitch) g_direct.samples.push_back({from, dyaw, dpitch});
        dyaw = dpitch = 0;
        for (auto& s : g_direct.samples) {
            double share = (std::min(now, s.from + m.smoothing) - std::max(from, s.from)) / m.smoothing;
            if (share > 0) dyaw += s.yaw * share, dpitch += s.pitch * share;
        }
        std::erase_if(g_direct.samples, [&](const Direct::Sample& s) { return s.from + m.smoothing <= now; });
    } else {
        g_direct.samples.clear();
    }
    *yaw = dyaw, *pitch = dpitch;
    if (TraceOn() && (mx || my || dyaw || dpitch))
        rt::Log("input: %.3f s mouse %.0f,%.0f dt %.1f ms gain %.3f zoom %.3f (fov %.4f) -> yaw %+.4f pitch %+.4f deg",
                now, mx, my, dt * 1000, gain, zoom, fov, dyaw, dpitch);
    return true;
}

bool DirectLook() { return Mouse().direct; }
// ---- end direct mouse look ----

// ---- settings menu (app/menu.cpp) ----
int BindingCount() { return kTargetCount; }
const char* BindingName(int target) { return target >= 0 && target < kTargetCount ? kTargets[target].name : ""; }
std::vector<int> BindingKeys(int target) {
    return target >= 0 && target < kTargetCount ? Bindings()[size_t(target)] : std::vector<int>{};
}
std::string KeyName(int code) { return KeyLabel(code); }
int WheelCode(bool up) { return up ? kWheelUp : kWheelDown; }

// keys as the ini writes them: "space, wheelup", "none" for no key
std::string KeyList(const std::vector<int>& keys) {
    std::string out;
    for (int vk : keys) out += (out.empty() ? "" : ", ") + std::string(KeyLabel(vk));
    return out.empty() ? "none" : out;
}

void SetBinding(int target, int slot, int code) {
    if (target < 0 || target >= kTargetCount) return;
    std::vector<std::vector<int>> b = Bindings();
    std::vector<int>& keys = b[size_t(target)];
    if (code > 0) {
        // a key is bound once: it leaves the other buttons (their lines change too)
        for (int i = 0; i < kTargetCount; i++)
            if (i != target && std::erase(b[size_t(i)], code))
                settings::Set((std::string("bind_") + kTargets[i].name).c_str(), KeyList(b[size_t(i)]));
        std::erase(keys, code);
        if (slot < int(keys.size())) keys[size_t(slot)] = code;
        else keys.push_back(code);
    } else if (slot < int(keys.size())) {
        keys.erase(keys.begin() + slot);
    }
    settings::Set((std::string("bind_") + kTargets[target].name).c_str(), KeyList(keys));
    ReloadBindings();
}

void ResetBindings() {
    for (int i = 0; i < kTargetCount; i++)
        settings::Set((std::string("bind_") + kTargets[i].name).c_str(), kTargets[i].keys);
    ReloadBindings();
}

void ReloadBindings() { g_binds_read = false, Bindings(); }
void ReloadMouse() { g_mouse_read = false, Mouse(); }
void SettleAfterMenu() { g_settle_pending = true; }
// ---- end settings menu ----

void TracePad(const Pad& p) {
    if (!TraceOn()) return;
    static Pad previous;
    static int polls = 0;
    static double second = 0;
    double now = ScriptSeconds();
    polls++;
    if (now - second >= 1) {
        if (second > 0) rt::Log("input: %.3f s, %d polls of player 1 in %.2f s", now, polls, now - second);
        polls = 0, second = now;
    }
    if (p == previous) return;
    previous = p;
    rt::Log("input: %.3f s pad 1 buttons %02X analog %u %u %u %u %u %u %u %u sticks %d %d %d %d", now, p.buttons,
            p.analog[0], p.analog[1], p.analog[2], p.analog[3], p.analog[4], p.analog[5], p.analog[6], p.analog[7],
            p.thumb[0], p.thumb[1], p.thumb[2], p.thumb[3]);
}

}  // namespace hle::pc
