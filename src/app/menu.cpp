// The settings menu: F10 (or Back + Start on a controller), every setting of FarCryInstincts.ini on two pages.
// Adapted from SCDA-XBERecomp (MIT, Copyright (c) 2026 TGP482, Joshhhuaaa): src/app/menu.cpp (the options panel on
// F10: rows of a label and a stepped value with an arrow each side, key boxes with capture, tabs stepped with LB/RB,
// the footer's hint and buttons, the panel's layout) and src/app/main.cpp (F10, Escape, ImGui's window messages).
// Here on Direct3D 11 (gpu/present.cpp) instead of Direct3D 12, with FCI's settings, and the game held while it is
// open (app/focus_pause.cpp draws the frames). English text only.
//
// Threads: the window procedure (display.cpp) hands ImGui its messages; the focus pause's thread, which holds the
// guest CPU while the menu is open, makes the frames. ImGui's state is shared under g_ui. Settings change while the
// guest is held, so the game's readers (input, camera, renderer) never see a change mid-poll.
#include "app/menu.h"

#include <xinput.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>
#include <imgui_impl_win32.h>

#include "app/focus_pause.h"
#include "app/panel_colors.h"
#include "app/pcmode.h"
#include "app/settings.h"
#include "gpu/ao.h"
#include "gpu/fps_overlay.h"
#include "gpu/present.h"
#include "gpu/water.h"  // PC water
#include "hle/host_pad.h"  // controllers (SDL3)
#include "hle/pc_input.h"
#include "runtime/log.h"
#include "video/display.h"
#include "xdk/hle.h"  // performance: frame_queue

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace hle {
void SettlePads();  // hle/xinput.cpp
}  // namespace hle

namespace app {

namespace {

std::atomic<bool> g_open{false};
std::recursive_mutex g_ui;    // ImGui's state, between the window thread and the menu's frames
bool g_context = false;       // ImGui's context and Win32 backend exist (under g_ui)
std::atomic<bool> g_pad_armed{false};  // the pad's buttons were all let go since the menu opened
int g_pad_step = 0;                    // the controller's left / right this frame (FeedPads)
ImGuiID g_nav_row = 0, g_nav_row_prev = 0;  // the option row focused this frame and the last
std::atomic<bool> g_save_failed{false};     // the last save could not write FarCryInstincts.ini (shown in the panel)

// every change goes into the file at once; a failure (a folder without write access) is shown, not only logged
void SaveSettings() { g_save_failed = !settings::Save(); }

std::string Lower(std::string s) {
    for (char& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

std::string Upper(std::string s) {
    for (char& ch : s) ch = char(std::toupper(static_cast<unsigned char>(ch)));
    return s;
}

std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t"), e = s.find_last_not_of(" \t");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

std::string Num(double v, int decimals) {
    char text[32];
    std::snprintf(text, sizeof text, "%.*f", decimals, v);
    return text;
}

// ---- the settings: values as the ini writes them, labels as the menu shows them ----

struct Choice {
    std::string value, label;
};
using Choices = std::vector<Choice>;

enum class When { Live, Restart, NextStart };

struct Setting {
    const char* key;
    const char* label;
    const char* fallback;  // what the readers use without a value
    Choices (*choices)();
    When when;
    const char* help;
};

Choices OnOff() { return {{"0", "OFF"}, {"1", "ON"}}; }

Choices Range(double lo, double hi, double step, double def, int decimals, const char* suffix = "") {
    Choices out;
    for (int i = 0;; i++) {
        double v = lo + i * step;
        if (v > hi + step * 0.01) break;
        std::string text = Num(v, decimals);
        std::string label = text + suffix;
        if (std::fabs(v - def) < step * 0.01) label += " (DEFAULT)";
        out.push_back({text, label});
    }
    return out;
}

struct Monitor {
    std::wstring device;
    int number, w, h;
    bool primary;
};

std::vector<Monitor> Monitors() {
    std::vector<Monitor> out;
    auto add = [](HMONITOR mon, HDC, LPRECT, LPARAM list) -> BOOL {
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof mi;
        if (!GetMonitorInfoW(mon, &mi)) return TRUE;
        std::wstring dev = mi.szDevice;
        size_t digits = dev.find_last_not_of(L"0123456789") + 1;
        int n = digits < dev.size() ? _wtoi(dev.c_str() + digits) : 0;
        reinterpret_cast<std::vector<Monitor>*>(list)->push_back(
            {dev, n, int(mi.rcMonitor.right - mi.rcMonitor.left), int(mi.rcMonitor.bottom - mi.rcMonitor.top),
             (mi.dwFlags & MONITORINFOF_PRIMARY) != 0});
        return TRUE;
    };
    EnumDisplayMonitors(nullptr, nullptr, add, LPARAM(&out));
    std::sort(out.begin(), out.end(), [](const Monitor& a, const Monitor& b) { return a.number < b.number; });
    return out;
}

// the monitor setting's display (as app/output.cpp reads it: 0 the primary, n the n-th, or a device name)
std::wstring ChosenDevice() {
    std::vector<Monitor> list = Monitors();
    std::string want = Lower(settings::String("monitor", ""));
    for (size_t i = 0; i < list.size(); i++) {
        std::string dev;
        for (wchar_t c : list[i].device) dev += char(std::tolower(int(c)));
        if (std::isdigit(static_cast<unsigned char>(want.empty() ? 'x' : want[0])) && std::atoi(want.c_str()) == int(i + 1))
            return list[i].device;
        if (!want.empty() && (dev == want || dev == "\\\\.\\" + want)) return list[i].device;
    }
    for (const Monitor& m : list)
        if (m.primary) return m.device;
    return {};
}

Choices MonitorChoices() {
    Choices out;
    std::vector<Monitor> list = Monitors();
    for (const Monitor& m : list)
        if (m.primary) out.push_back({"0", "PRIMARY (" + std::to_string(m.w) + "x" + std::to_string(m.h) + ")"});
    if (out.empty()) out.push_back({"0", "PRIMARY"});
    std::string set = Trim(settings::String("monitor", "0"));
    for (size_t i = 0; i < list.size(); i++) {
        const Monitor& m = list[i];
        if (m.primary && set != std::to_string(i + 1)) continue;  // the primary is the first choice already
        std::string label = std::to_string(i + 1) + ": " + std::to_string(m.w) + "x" + std::to_string(m.h);
        out.push_back({std::to_string(i + 1), label + (m.primary ? " PRIMARY" : "")});
    }
    return out;
}

// the chosen monitor's modes, smallest first
Choices ResolutionChoices() {
    Choices out = {{"", "DEFAULT"}};
    std::wstring device = ChosenDevice();
    std::vector<std::pair<int, int>> modes;
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    for (DWORD i = 0; EnumDisplaySettingsW(device.empty() ? nullptr : device.c_str(), i, &dm); i++) {
        if (dm.dmBitsPerPel < 32 || dm.dmPelsWidth < 640) continue;
        std::pair<int, int> m{int(dm.dmPelsWidth), int(dm.dmPelsHeight)};
        if (std::find(modes.begin(), modes.end(), m) == modes.end()) modes.push_back(m);
    }
    std::sort(modes.begin(), modes.end());
    for (auto [w, h] : modes) {
        std::string v = std::to_string(w) + "x" + std::to_string(h);
        out.push_back({v, v});
    }
    return out;
}

Choices ScaleChoices() {
    Choices out = {{"auto", "AUTO (OUTPUT HEIGHT)"}};
    // performance: fractional render scales, a share of the output's height (as SCDA's 20-200 %)
    for (int p : {50, 67, 75, 85, 125, 150, 200})
        out.push_back({std::to_string(p) + "%", std::to_string(p) + "% OF THE OUTPUT"});
    for (int s = 1; s <= 6; s++)
        out.push_back({std::to_string(s), std::to_string(s) + "x (" + std::to_string(s * 480) + " LINES)"});
    return out;
}

Choices FpsChoices() {
    Choices out = {{"auto", "AUTO (DISPLAY'S REFRESH)"}};
    for (int f : {30, 60, 75, 90, 100, 120, 144, 165, 240})
        out.push_back({std::to_string(f), std::to_string(f) + (f == 30 ? " (CONSOLE)" : "")});
    out.push_back({"0", "UNLIMITED"});
    return out;
}

Choices PeakChoices() {
    Choices out = {{"0", "DISPLAY'S"}};
    for (int n : {400, 600, 800, 1000, 1200, 1500, 2000, 4000, 10000})
        out.push_back({std::to_string(n), std::to_string(n) + " NITS"});
    return out;
}

Choices PaperChoices() {
    Choices out = {{"auto", "WINDOWS SDR BRIGHTNESS"}};
    for (int n = 80; n <= 1000; n += 10)
        out.push_back({std::to_string(n), std::to_string(n) + " NITS" + (n == 200 ? " (DEFAULT)" : "")});
    return out;
}

// PC level of detail: multiples of the console's distances
Choices DistanceChoices(double def) {
    Choices out;
    for (double v : {0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 2.5, 3.0, 4.0, 5.0, 6.0, 8.0}) {
        std::string text = Num(v, 2);
        out.push_back({text, text + "x" + (v == 1.0 ? " (CONSOLE)" : v == def ? " (DEFAULT)" : "")});
    }
    return out;
}

const Setting kSettings[] = {
    // display
    {"display_mode", "DISPLAY MODE", "windowed",
     [] { return Choices{{"windowed", "WINDOWED"}, {"borderless", "BORDERLESS"}, {"fullscreen", "EXCLUSIVE FULLSCREEN"}}; },
     When::Restart, "Window, borderless over the monitor or exclusive fullscreen. F11 / Alt+Enter switch at once."},
    {"monitor", "MONITOR", "0", MonitorChoices, When::Restart, "The monitor the game starts on."},
    {"resolution", "RESOLUTION", "", ResolutionChoices, When::Restart,
     "Windowed: the client area; exclusive fullscreen: the display mode. Default: as before."},
    {"aspect", "ASPECT RATIO", "auto",
     [] { return Choices{{"auto", "AUTO (OUTPUT)"}, {"4:3", "4:3 (CONSOLE)"}, {"16:10", "16:10"}, {"16:9", "16:9"},
                         {"21:9", "21:9"}, {"32:9", "32:9"}}; },
     When::Restart, "Shape of the 3D view (Hor+: wider, same height)."},
    {"scale", "RENDER RESOLUTION", "auto", ScaleChoices, When::Restart,
     "Times the console's 640x480, or a share of the output's height; auto = the output's height."},
    {"fov", "FIELD OF VIEW", "1", [] { return Range(0.5, 2.0, 0.05, 1.0, 2, "x"); }, When::Live,
     "Scale of the game's vertical field of view."},
    {"fps_limit", "MAX FRAME RATE", "auto", FpsChoices, When::Live,
     "Highest frame rate; auto = the display's refresh rate, 30 = the console's."},
    {"vsync", "VERTICAL SYNC", "0", OnOff, When::Restart, "Show frames with the monitor's refresh."},
    {"fps_counter", "FPS COUNTER", "0", OnOff, When::Live, "Frame rate and frame times, top left."},
    {"frame_queue", "FRAMES AHEAD", "1",
     [] { return Choices{{"1", "1 (DEFAULT)"}, {"2", "2"}, {"3", "3"}}; }, When::Live,
     "Frames the game may finish before they are shown: 2 hides single slow game frames (CPU-bound handhelds) for "
     "up to one frame more latency."},
    {"fmv_scale", "FMV SCALE", "fill",
     [] { return Choices{{"fill", "FILL (DEFAULT)"}, {"4:3", "4:3 (XBOX)"}}; }, When::Restart,
     "Videos (intro, title, menus): fill the wide screen without cropping the picture, or 4:3 as on the Xbox."},
    {"colour_blind", "COLOUR BLIND MODE", "0",
     [] { return Choices{{"0", "OFF"}, {"1", "PROTANOPIA"}, {"2", "DEUTERANOPIA"}, {"3", "TRITANOPIA"}}; },
     When::Restart, "Adjusts the colours for red, green or blue colour blindness (SDR and HDR10)."},
    {"hdr", "HDR10 OUTPUT", "auto", [] { return Choices{{"auto", "AUTO (WINDOWS HDR)"}, {"on", "ON"}, {"off", "OFF"}}; },
     When::Restart, "HDR10 output; auto when Windows HDR is on for the monitor."},
    {"hdr_paper_white", "HDR PAPER WHITE", "200", PaperChoices, When::Live, "Brightness of the game's white in HDR."},
    {"hdr_peak", "HDR PEAK BRIGHTNESS", "0", PeakChoices, When::Live, "The display's peak brightness."},
    {"hdr_boost", "HDR HIGHLIGHT BOOST", "0", [] { return Range(0, 1, 0.05, 0, 2); }, When::Live,
     "Stretches the highlights towards the peak brightness; 0 is off."},
    // graphics
    {"msaa", "MSAA", "4", [] { return Choices{{"1", "OFF"}, {"2", "2x"}, {"4", "4x (DEFAULT)"}, {"8", "8x"}}; },
     When::Restart, "Multisampling of the screen sized targets."},
    {"smaa", "SMAA", "1", OnOff, When::Live, "SMAA 1x over the finished picture."},
    {"aniso", "ANISOTROPIC FILTERING", "16",
     [] { return Choices{{"1", "OFF"}, {"2", "2x"}, {"4", "4x"}, {"8", "8x"}, {"16", "16x (DEFAULT)"}}; }, When::Restart,
     "Texture filtering at grazing angles."},
    {"shadow_scale", "SHADOW RESOLUTION", "2",
     [] { return Choices{{"1", "1x (1024, CONSOLE)"}, {"2", "2x (2048, DEFAULT)"}, {"3", "3x (3072)"}, {"4", "4x (4096)"}, {"6", "6x (6144)"}, {"8", "8x (8192)"}}; },
     When::Restart, "Resolution of the shadow maps."},
    {"shadow_distance", "SHADOW DISTANCE", "1", [] { return Range(1, 8, 1, 1, 0, "x"); }, When::Restart,
     "Reach of the sun's shadows, times the console's (~15 m)."},
    {"shadow_filter", "SHADOW FILTER", "soft",
     [] { return Choices{{"soft", "SOFT (DEFAULT)"}, {"sharp", "SHARP (CONSOLE)"}}; }, When::Restart,
     "Soft shadow edges, or the NV2A's 2x2 filter."},
    // PC water (gpu/water.h)
    {"water_quality", "WATER", "high",
     [] { return Choices{{"console", "CONSOLE"}, {"medium", "MEDIUM"}, {"high", "HIGH (DEFAULT)"}}; }, When::Live,
     "The sea's surface: console = as on the Xbox; medium adds Fresnel, depth colour, a soft shore and sun glints; "
     "high also refraction. From console when ambient occlusion was off at start needs a restart."},
    // PC level of detail (xdk/lod.cpp)
    {"lod_distance", "DETAIL DISTANCE", "4", [] { return DistanceChoices(pcmode::kDefaultLodDistance); }, When::Live,
     "Distance at which trees, plants and objects switch to simpler models and to sprites, times the console's."},
    {"view_distance", "OBJECT VIEW DISTANCE", "2", [] { return DistanceChoices(pcmode::kDefaultViewDistance); }, When::Live,
     "How far trees, plants and objects are drawn, times the console's (the fog still ends the view)."},
    {"ao", "AMBIENT OCCLUSION", "ssao", [] { return Choices{{"off", "OFF"}, {"ssao", "SSAO (DEFAULT)"}, {"gtao", "GTAO"}}; },
     When::Live, "Screen space ambient occlusion. Switching on when it was off at start needs a restart."},
    {"ao_strength", "AO STRENGTH", "1", [] { return Range(0, 4, 0.1, 1, 1); }, When::Live, "Strength of the occlusion."},
    {"ao_resolution", "AO RESOLUTION", "auto",
     [] { return Choices{{"auto", "AUTO"}, {"full", "FULL"}, {"half", "HALF"}}; }, When::Restart,
     "Auto: half resolution above 720 lines."},
    // general
    {"skip_intro", "SKIP STARTUP VIDEOS", "0", OnOff, When::Live,
     "Skip the Ubisoft, Crytek and Far Cry Instincts logo videos at start. The story's cutscenes (the opening on the "
     "boat) are part of the levels and can't be skipped, as on the Xbox."},
    {"pause_on_focus_loss", "PAUSE ON FOCUS LOSS", "0", OnOff, When::Live,
     "Hold the game while the window is in the background."},
    {"texture_dump", "TEXTURE DUMPING", "0", OnOff, When::Restart, "Write every game texture to textures\\dump."},
    {"texture_replace", "TEXTURE REPLACEMENT", "0", OnOff, When::Restart, "Use the images in textures\\replace."},
};
const char* const kSections[] = {"DISPLAY", "GRAPHICS", "GENERAL"};
constexpr int kSectionStart[] = {0, 14, 26};  // first setting of each section in kSettings

const Setting kMouseSettings[] = {
    {"mouse_sensitivity_x", "HORIZONTAL SENSITIVITY", "", [] { return Range(0.1, 10, 0.1, 1, 1); }, When::Live,
     "Mouse look: 1.0 turns 0.1 degrees per count."},
    {"mouse_sensitivity_y", "VERTICAL SENSITIVITY", "", [] { return Range(0.1, 10, 0.1, 1, 1); }, When::Live,
     "Mouse look up and down: 1.0 turns 0.1 degrees per count."},
    {"mouse_invert_y", "INVERT Y AXIS", "0", OnOff, When::Live, "Mouse forward looks down."},
    {"mouse_acceleration", "MOUSE ACCELERATION", "0", [] { return Range(0, 2, 0.05, 0, 2); }, When::Live,
     "Extra gain with speed; 0 is raw input without acceleration."},
};

// ---- CONTROLLER page (hle/host_pad.h) ----
const Setting kPadSettings[] = {
    {"pad_backend", "CONTROLLER INPUT", "sdl",
     [] { return Choices{{"sdl", "SDL3 (DEFAULT)"}, {"xinput", "XINPUT"}, {"off", "OFF"}}; }, When::Restart,
     "SDL3: Xbox, PlayStation, Switch and generic pads; XInput: Xbox pads only; off: keyboard only."},
    {"pad_dead_zone", "STICK DEAD ZONE", "0",
     [] {
         Choices c = Range(0, 0.9, 0.05, 0, 2);
         c[0].label = "0.00 (GAME'S OWN, DEFAULT)";
         return c;
     },
     When::Live, "Share of full deflection a stick counts as centred, on top of the game's own."},
    {"pad_rumble", "VIBRATION STRENGTH", "100", [] { return Range(0, 100, 10, 100, 0, "%"); }, When::Live,
     "Strength of the controller's motors."},
};

const char* const kPadBindLabels[][2] = {
    {"a", "A"}, {"b", "B"}, {"x", "X"}, {"y", "Y"}, {"white", "WHITE"}, {"black", "BLACK"},
    {"lt", "LEFT TRIGGER"}, {"rt", "RIGHT TRIGGER"}, {"start", "START"}, {"back", "BACK"},
    {"lthumb", "LEFT STICK CLICK"}, {"rthumb", "RIGHT STICK CLICK"}, {"up", "D-PAD UP"}, {"down", "D-PAD DOWN"},
    {"left", "D-PAD LEFT"}, {"right", "D-PAD RIGHT"},
};

std::string PadBindLabel(int target) {
    const char* name = hle::host::BindingName(target);
    for (const auto& l : kPadBindLabels)
        if (!std::strcmp(l[0], name)) return l[1];
    return Upper(name);
}
// ---- end CONTROLLER page ----

const char* const kBindLabels[][2] = {
    {"up", "D-PAD UP"}, {"down", "D-PAD DOWN"}, {"left", "D-PAD LEFT"}, {"right", "D-PAD RIGHT"},
    {"start", "START"}, {"back", "BACK"}, {"lthumb", "LEFT STICK CLICK"}, {"rthumb", "RIGHT STICK CLICK"},
    {"a", "A"}, {"b", "B"}, {"x", "X"}, {"y", "Y"}, {"white", "WHITE"}, {"black", "BLACK"},
    {"lt", "LEFT TRIGGER"}, {"rt", "RIGHT TRIGGER"},
    {"lup", "MOVE FORWARD"}, {"ldown", "MOVE BACK"}, {"lleft", "STRAFE LEFT"}, {"lright", "STRAFE RIGHT"},
    {"rup", "LOOK UP"}, {"rdown", "LOOK DOWN"}, {"rleft", "LOOK LEFT"}, {"rright", "LOOK RIGHT"},
};

std::string BindLabel(int target) {
    const char* name = hle::pc::BindingName(target);
    for (const auto& l : kBindLabels)
        if (!std::strcmp(l[0], name)) return l[1];
    return Upper(name);
}

std::string Fallback(const Setting& s) {
    // the two mouse axes follow mouse_sensitivity
    if (!std::strncmp(s.key, "mouse_sensitivity_", 18)) return settings::String("mouse_sensitivity", "1");
    return settings::Default(s.key, s.fallback);  // device presets: the Steam Deck's defaults
}

bool Truthy(const std::string& v) { return v == "1" || v == "true" || v == "yes" || v == "on"; }

// which choice the value is: as written, else the nearest number, else the default's
int IndexOf(const Setting& s, const Choices& list, std::string v) {
    v = Lower(Trim(v));
    bool boolean = list.size() == 2 && list[0].value == "0" && list[1].value == "1";
    if (boolean) return Truthy(v) ? 1 : (v == "0" || v == "false" || v == "no" || v == "off") ? 0 : IndexOf(s, list, Fallback(s));
    for (size_t i = 0; i < list.size(); i++)
        if (Lower(list[i].value) == v) return int(i);
    char* end = nullptr;
    double n = std::strtod(v.c_str(), &end);
    if (!v.empty() && end != v.c_str()) {
        int best = -1;
        double best_d = 1e300;
        for (size_t i = 0; i < list.size(); i++) {
            char* e = nullptr;
            double c = std::strtod(list[i].value.c_str(), &e);
            if (e == list[i].value.c_str()) continue;
            if (std::fabs(c - n) < best_d) best_d = std::fabs(c - n), best = int(i);
        }
        if (best >= 0) return best;
    }
    std::string def = Lower(Fallback(s));
    for (size_t i = 0; i < list.size(); i++)
        if (Lower(list[i].value) == def) return int(i);
    return 0;
}

int Current(const Setting& s, const Choices& list) { return IndexOf(s, list, settings::String(s.key, Fallback(s).c_str())); }
int AtBoot(const Setting& s, const Choices& list) { return IndexOf(s, list, settings::Boot(s.key, Fallback(s).c_str())); }

// a change waits for the next start: what the row says, or the AO pass that was off at start
bool NeedsRestart(const Setting& s) {
    if (s.when != When::Live) return true;
    // PC water: from console it reads the scene depth, which the depth targets only offer when made for AO or water
    if (!std::strcmp(s.key, "water_quality"))
        return !gpu::water::BootQuality() && !gpu::ao::BootMode() &&
               Lower(settings::String("water_quality", "high")) != "console";
    return !std::strcmp(s.key, "ao") && !gpu::ao::BootMode();
}

// a live setting into effect (the guest is held)
void ApplyLive(const Setting& s) {
    std::string key = s.key;
    if (key == "fov") pcmode::SetFovScale(settings::Float("fov", 1.0f));
    else if (key == "fps_limit") {
        const std::string v = settings::String("fps_limit", "auto");
        pcmode::SetFpsLimit(v == "auto" ? pcmode::kFpsAuto : std::atoi(v.c_str()));
    }
    else if (key == "fps_counter") gpu::fps::SetEnabled(settings::Bool("fps_counter", false));
    else if (key == "frame_queue") xdk::SetFrameQueue(settings::Int("frame_queue", 1));  // performance
    else if (key == "smaa") gpu::present::SetSmaa(settings::Int("smaa", 1) != 0);
    else if (key == "lod_distance") pcmode::SetLodDistance(settings::Float("lod_distance", pcmode::kDefaultLodDistance));  // PC level of detail
    else if (key == "view_distance") pcmode::SetViewDistance(settings::Float("view_distance", pcmode::kDefaultViewDistance));
    else if (key == "hdr_paper_white" || key == "hdr_peak" || key == "hdr_boost") {
        std::string paper = Lower(settings::String("hdr_paper_white", "200"));
        gpu::present::SetHdrLevels(paper == "auto" ? 0.0f : std::clamp(float(std::atof(paper.c_str())), 80.0f, 1000.0f),
                                   std::max(settings::Float("hdr_peak", 0), 0.0f), settings::Float("hdr_boost", 0));
    } else if (key == "ao") {
        std::string v = Lower(settings::String("ao", "ssao"));
        gpu::ao::SetMode(v == "gtao" ? 2 : v == "ssao" ? 1 : 0);
    } else if (key == "water_quality") {  // PC water
        std::string v = Lower(settings::String("water_quality", "high"));
        gpu::water::SetQuality(v == "console" ? gpu::water::kConsole : v == "medium" ? gpu::water::kMedium : gpu::water::kHigh);
    } else if (key == "ao_strength") gpu::ao::SetStrength(settings::Float("ao_strength", 1.0f));
    else if (!key.compare(0, 6, "mouse_")) hle::pc::ReloadMouse();
    else if (key == "pause_on_focus_loss") SetPauseOnFocusLoss(settings::Bool("pause_on_focus_loss", false));
    else if (key == "pad_dead_zone" || key == "pad_rumble") hle::host::ReloadOptions();
}

void Change(const Setting& s, const std::string& value) {
    settings::Set(s.key, value);
    bool restart = NeedsRestart(s);
    if (!restart) ApplyLive(s);
    rt::Log("settings menu: %s = %s (%s)", s.key, value.c_str(),
            restart ? (s.when == When::NextStart ? "next start" : "after a restart") : "applied");
    SaveSettings();
}

// ---- the panel (after SCDA's) ----

int g_pick_frame = 0;  // frame of the last key bind: the key that bound must not start another

int StepIndex(int index, int step, int count) {
    if (count <= 0) return 0;
    index = (index + step) % count;
    return index < 0 ? index + count : index;
}

void Section(const char* title) {
    float em = ImGui::GetFontSize();
    ImGui::Dummy(ImVec2(0, em * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Text, kHeading);
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, em * 0.2f));
}

constexpr float kValueEms = 15.0f;  // the value column's width

// a row's place and size: the label left, the value block (an arrow each side of the value) right
struct RowBox {
    float em, row_w, row_h, value_w, block_w, row_x, row_y;
};

RowBox MeasureRow() {
    const ImGuiStyle& style = ImGui::GetStyle();
    RowBox box;
    box.em = ImGui::GetFontSize();
    box.row_w = ImGui::GetContentRegionAvail().x;
    box.row_h = ImGui::GetFrameHeight();
    box.value_w = box.em * kValueEms;
    box.block_w = box.row_h * 2 + box.value_w + style.ItemSpacing.x * 2;
    box.row_x = ImGui::GetCursorPosX();
    box.row_y = ImGui::GetCursorPosY();
    return box;
}

void RowLabel(const RowBox& box, const char* label) {
    ImGui::SetCursorPos(ImVec2(box.row_x + ImGui::GetStyle().ItemInnerSpacing.x, box.row_y));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
}

// the cursor back to the row's start, then past the whole row
void EndRow(const RowBox& box) {
    ImGui::SetCursorPos(ImVec2(box.row_x, box.row_y));
    ImGui::Dummy(ImVec2(box.row_w, box.row_h));
}

// label left, arrows and value right, the row is the only nav stop; Enter / A / a click on it steps on as Right
// does. note: shown beside the value (RESTART)
int OptionRow(const char* label, const char* value, const char* note, bool* focused) {
    const ImGuiStyle& style = ImGui::GetStyle();
    RowBox box = MeasureRow();
    ImGui::PushID(label);
    int step = 0;
    if (ImGui::Selectable("##row", false, ImGuiSelectableFlags_AllowOverlap, ImVec2(box.row_w, box.row_h))) step = 1;
    *focused = ImGui::IsItemFocused() || ImGui::IsItemHovered();
    if (ImGui::IsItemFocused()) {
        ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                                  ImGui::GetColorU32(ImGuiCol_Header));
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) step = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) step = 1;
    }
    // the controller's left / right (FeedPads) to the row focused last frame: the frame of a pad press finds no
    // focused item (ImGui's nav takes the direction too)
    if (ImGui::IsItemFocused()) g_nav_row = ImGui::GetItemID();
    if (g_pad_step && ImGui::GetItemID() == g_nav_row_prev) step = g_pad_step;
    RowLabel(box, label);
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav | ImGuiItemFlags_ButtonRepeat, true);  // held arrows keep stepping
    float block_x = box.row_x + box.row_w - box.block_w;
    if (note && *note) {
        float w = ImGui::CalcTextSize(note).x;
        ImGui::SetCursorPos(ImVec2(block_x - w - style.ItemSpacing.x * 2, box.row_y));
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, kRestart);
        ImGui::TextUnformatted(note);
        ImGui::PopStyleColor();
    }
    ImGui::SetCursorPos(ImVec2(block_x, box.row_y));
    if (ImGui::ArrowButton("##dec", ImGuiDir_Left)) step = -1;
    float value_x = block_x + box.row_h + style.ItemSpacing.x;
    float pad = (box.value_w - ImGui::CalcTextSize(value).x) * 0.5f;
    ImGui::SetCursorPos(ImVec2(value_x + std::max(pad, 0.0f), box.row_y));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(value);
    ImGui::SetCursorPos(ImVec2(value_x + box.value_w + style.ItemSpacing.x, box.row_y));
    if (ImGui::ArrowButton("##inc", ImGuiDir_Right)) step = 1;
    ImGui::PopItemFlag();
    ImGui::PopID();
    EndRow(box);
    return step;
}

constexpr float kKeyBoxEms = 10.0f;  // a key box's width when the row has two

// label left, a box per key right, ending where the other rows' values do; Enter or a click on a box starts its
// capture, a right click or Delete on it sets cleared. The box pressed, -1 for none.
int BindRow(const char* label, const std::vector<std::string>& keys, int capturing, int& cleared, bool* focused,
            const char* prompt = "PRESS A KEY") {
    const ImGuiStyle& style = ImGui::GetStyle();
    RowBox row = MeasureRow();
    float value_end = row.row_x + row.row_w - row.block_w + row.row_h + style.ItemSpacing.x + row.value_w;
    int n = int(keys.size());
    int pressed = -1;
    float box_w = row.em * kKeyBoxEms;
    ImGui::PushID(label);
    RowLabel(row, label);
    for (int i = 0; i < n; i++) {
        float x = value_end - float(n - i) * box_w - float(n - 1 - i) * style.ItemSpacing.x;
        ImGui::SetCursorPos(ImVec2(x, row.row_y));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##key", ImVec2(box_w, row.row_h))) pressed = i;
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right) ||
            (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) ||
            (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false)))
            cleared = i;
        if (ImGui::IsItemHovered() || ImGui::IsItemFocused()) *focused = true;
        bool lit = capturing == i || ImGui::IsItemFocused() || ImGui::IsItemHovered();
        ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                                  ImGui::GetColorU32(lit ? ImGuiCol_Header : ImGuiCol_FrameBg));
        std::string value = capturing == i ? prompt : keys[i].empty() ? "-" : keys[i];
        float pad = (box_w - ImGui::CalcTextSize(value.c_str()).x) * 0.5f;
        ImGui::SetCursorPos(ImVec2(x + std::max(pad, 0.0f), row.row_y));
        ImGui::AlignTextToFramePadding();
        if (capturing == i) ImGui::PushStyleColor(ImGuiCol_Text, kHeading);
        ImGui::TextUnformatted(value.c_str());
        if (capturing == i) ImGui::PopStyleColor();
        ImGui::PopID();
    }
    ImGui::PopID();
    EndRow(row);
    return pressed;
}

const char* const kPages[] = {"SETTINGS", "MOUSE AND KEYBOARD", "CONTROLLER"};
constexpr int kSettingsPage = 0, kKeyboardPage = 1, kControllerPage = 2;
int g_page = kSettingsPage;

// the binding being captured (-1 when idle), its box, and the key the window procedure caught for it
std::atomic<int> g_capture{-1};
int g_capture_slot = 0;
std::atomic<int> g_caught{0};
std::atomic<bool> g_capture_pad{false};  // the capture is a controller button's (CONTROLLER page)
bool g_capture_armed = false;            // its buttons were all let go since it began
const char* g_hint = "";            // the focused row's help, this frame
bool g_hint_restart = false;        // the focused row's change waits for a restart
bool g_hint_env = false;            // the focused row's value comes from the environment
std::atomic<bool> g_close{false};  // closing was asked for this frame
// the page's first row takes the keyboard / controller focus in that many frames (after a page switch the tab bar
// settles first, a focus asked for at once was lost)
int g_focus_first = 0;
bool g_focus_now = false;  // this frame (DrawPanel counts down once a frame)

void FocusFirst() {
    if (!g_focus_now) return;
    g_focus_now = false;
    ImGui::SetKeyboardFocusHere();
}

void SettingRow(const Setting& s) {
    Choices list = s.choices();
    if (list.empty()) return;
    // device presets: a default of the device moves the "(DEFAULT)" mark to its choice
    if (settings::Default(s.key, s.fallback) != s.fallback) {
        const std::string def = Lower(Fallback(s));
        for (Choice& c : list) {
            size_t at = c.label.find(" (DEFAULT)");
            if (at != std::string::npos) c.label.erase(at, 10);
            if (Lower(c.value) == def) c.label += " (" + Upper(settings::Device()) + " DEFAULT)";
        }
    }
    int i = Current(s, list);
    bool restart = NeedsRestart(s);
    const char* note = "";
    if (restart && i != AtBoot(s, list)) note = s.when == When::NextStart ? "NEXT START" : "RESTART";
    bool focused = false;
    int step = OptionRow(s.label, list[size_t(i)].label.c_str(), note, &focused);
    if (focused) g_hint = s.help, g_hint_restart = restart, g_hint_env = settings::FromEnvironment(s.key);
    if (step) Change(s, list[size_t(StepIndex(i, step, int(list.size())))].value);
}

void DrawSettings() {
    for (size_t i = 0; i < std::size(kSettings); i++) {
        for (int k = 0; k < int(std::size(kSections)); k++)
            if (kSectionStart[k] == int(i)) Section(kSections[k]);
        FocusFirst();
        SettingRow(kSettings[i]);
    }
}

std::string KeyText(int code) {
    std::string name = Upper(hle::pc::KeyName(code));
    return name;
}

void DrawKeyboard() {
    Section("MOUSE");
    for (size_t i = 0; i < std::size(kMouseSettings); i++) {
        FocusFirst();
        SettingRow(kMouseSettings[i]);
    }
    Section("KEYBOARD AND MOUSE BUTTONS");
    for (int t = 0; t < hle::pc::BindingCount(); t++) {
        std::vector<int> keys = hle::pc::BindingKeys(t);
        std::vector<std::string> boxes(2);
        for (size_t k = 0; k < keys.size() && k < 2; k++) boxes[k] = KeyText(keys[k]);
        if (keys.size() > 2) boxes[1] += " +" + std::to_string(keys.size() - 2);
        int cleared = -1;
        bool focused = false;
        std::string label = BindLabel(t);
        int box = BindRow(label.c_str(), boxes, g_capture == t ? g_capture_slot : -1, cleared, &focused);
        if (focused)
            g_hint = "Enter, A or a click binds a key; Delete, X or a right click clears it.", g_hint_restart = false,
            g_hint_env = settings::FromEnvironment((std::string("bind_") + hle::pc::BindingName(t)).c_str());
        bool idle = g_capture < 0 && ImGui::GetFrameCount() > g_pick_frame + 1;
        if (box >= 0 && idle) {
            g_capture_slot = std::min(box, int(keys.size()));
            g_caught = 0;
            g_capture = t;
        }
        if (cleared >= 0 && idle && cleared < int(keys.size())) {
            hle::pc::SetBinding(t, cleared, 0);
            rt::Log("settings menu: bind_%s cleared key %d", hle::pc::BindingName(t), cleared + 1);
            SaveSettings();
        }
    }
}

// ---- CONTROLLER page: the controllers, the pad settings, a controller button per Xbox button ----
// a row of text: the label left, the value where the other rows have theirs (no nav stop)
void InfoRow(const char* label, const std::string& value) {
    const ImGuiStyle& style = ImGui::GetStyle();
    RowBox box = MeasureRow();
    RowLabel(box, label);
    float value_x = box.row_x + box.row_w - box.block_w + box.row_h + style.ItemSpacing.x;
    float w = ImGui::CalcTextSize(value.c_str()).x;
    ImGui::SetCursorPos(ImVec2(value_x + std::max((box.value_w - w) * 0.5f, 0.0f), box.row_y));
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, value.empty() || value[0] == '-' ? kDim : kText);
    ImGui::TextUnformatted(value.c_str());
    ImGui::PopStyleColor();
    EndRow(box);
}

void DrawController() {
    Section("CONTROLLERS");
    for (int slot = 0; slot < hle::host::kSlots; slot++) {
        std::string name = hle::host::Name(slot);
        std::string label = "PORT " + std::to_string(slot + 1) + (slot == 0 ? " (WITH KEYBOARD)" : "");
        InfoRow(label.c_str(), name.empty() ? std::string("- (NONE)") : Upper(name));
    }
    Section("GENERAL");
    for (size_t i = 0; i < std::size(kPadSettings); i++) {
        FocusFirst();
        SettingRow(kPadSettings[i]);
    }
    Section("BUTTONS");
    for (int t = 0; t < hle::host::BindingCount(); t++) {
        std::vector<int> buttons = hle::host::BindingButtons(t);
        std::vector<std::string> boxes(2);
        for (size_t k = 0; k < buttons.size() && k < 2; k++) boxes[k] = Upper(hle::host::ButtonName(buttons[k]));
        if (buttons.size() > 2) boxes[1] += " +" + std::to_string(buttons.size() - 2);
        int cleared = -1;
        bool focused = false;
        std::string label = PadBindLabel(t);
        int box = BindRow(label.c_str(), boxes, g_capture == t ? g_capture_slot : -1, cleared, &focused, "PRESS A BUTTON");
        if (focused)
            g_hint = "Enter, A or a click waits for a controller button; Delete, X or a right click clears it.",
            g_hint_restart = false,
            g_hint_env = settings::FromEnvironment((std::string("pad_bind_") + hle::host::BindingName(t)).c_str());
        bool idle = g_capture < 0 && ImGui::GetFrameCount() > g_pick_frame + 1;
        if (box >= 0 && idle) {
            g_capture_slot = std::min(box, int(buttons.size()));
            g_caught = 0;
            g_capture_pad = true;
            g_capture_armed = false;
            g_capture = t;
        }
        if (cleared >= 0 && idle && cleared < int(buttons.size())) {
            hle::host::SetBinding(t, cleared, -1);
            rt::Log("settings menu: pad_bind_%s cleared button %d", hle::host::BindingName(t), cleared + 1);
            SaveSettings();
        }
    }
}

// a controller button binds once every button was let go since the capture began; Escape cancels. Read raw
// through host_pad (the game is held, nothing else polls the controllers)
void PollPadCapture(int t) {
    int code = g_caught.exchange(0);
    int button = hle::host::PressedButton();
    if (code != VK_ESCAPE) {
        if (!g_capture_armed) {
            g_capture_armed = button < 0;
            return;
        }
        if (button < 0) return;
        hle::host::SetBinding(t, g_capture_slot, button);
        rt::Log("settings menu: pad_bind_%s button %d = %s", hle::host::BindingName(t), g_capture_slot + 1,
                hle::host::ButtonName(button));
        SaveSettings();
    }
    g_capture = -1;
    g_capture_pad = false;
    g_pad_armed = false;  // the button that bound acts in the menu only once let go
    g_pick_frame = ImGui::GetFrameCount();
}
// ---- end CONTROLLER page ----

// a key the window procedure caught binds (Escape cancels)
void PollCapture() {
    int t = g_capture;
    if (t < 0) return;
    if (g_capture_pad) return PollPadCapture(t);
    int code = g_caught.exchange(0);
    if (!code) return;
    if (code != VK_ESCAPE) {
        hle::pc::SetBinding(t, g_capture_slot, code);
        rt::Log("settings menu: bind_%s key %d = %s", hle::pc::BindingName(t), g_capture_slot + 1,
                hle::pc::KeyName(code).c_str());
        SaveSettings();
    }
    g_capture = -1;
    g_pick_frame = ImGui::GetFrameCount();
}

void Defaults() {
    g_capture = -1;
    if (g_page == kKeyboardPage) {
        for (const Setting& s : kMouseSettings) settings::Set(s.key, std::strstr(s.key, "sensitivity") ? "1.0" : "0");
        hle::pc::ReloadMouse();
        hle::pc::ResetBindings();
    } else if (g_page == kControllerPage) {
        for (const Setting& s : kPadSettings) settings::Set(s.key, s.fallback);
        hle::host::ReloadOptions();
        hle::host::ResetBindings();
    } else {
        for (const Setting& s : kSettings) {
            settings::Set(s.key, Fallback(s));  // device presets: the device's defaults
            if (!NeedsRestart(s)) ApplyLive(s);
        }
    }
    rt::Log("settings menu: defaults for the %s page", kPages[g_page]);
    SaveSettings();
}

// one tab a page, as wide as they share the line; LB / RB step them as SCDA's do
void DrawTabs() {
    int step = ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) - ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false);
    bool set = step && g_capture < 0;
    if (set) g_page = StepIndex(g_page, step, int(std::size(kPages))), g_focus_first = 3;
    int shown = int(std::size(kPages));
    ImGui::PushStyleColor(ImGuiCol_Tab, kTab);
    ImGui::PushStyleColor(ImGuiCol_TabHovered, kTabHovered);
    ImGui::PushStyleColor(ImGuiCol_TabSelected, kHighlight);
    ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline, kHeading);
    ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, 0.0f);
    if (ImGui::BeginTabBar("##pages")) {
        float gaps = ImGui::GetStyle().ItemInnerSpacing.x * float(shown - 1);
        float w = (ImGui::GetContentRegionAvail().x - gaps) / float(shown);
        for (int i = 0; i < shown; i++) {
            ImGui::SetNextItemWidth(w);
            if (ImGui::BeginTabItem(kPages[i], nullptr, set && i == g_page ? ImGuiTabItemFlags_SetSelected : 0)) {
                if (!set && i != g_page) {
                    g_page = i;
                    g_focus_first = 3;
                    g_capture = -1;
                    g_capture_pad = false;
                }
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
}

void DrawPanel() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float em = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(em * 52, vp->Size.x - 16), std::min(em * 34, vp->Size.y - 16)),
                             ImGuiCond_Always);
    for (const auto& [col, color] : kPanelColors) ImGui::PushStyleColor(col, color);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em * 1.6f, em * 1.2f));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar |
                                        ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##settings", nullptr, kFlags)) {
        const char* title = "FAR CRY INSTINCTS - SETTINGS";
        float title_pad = (ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(title).x) * 0.5f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(title_pad, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, kHeading);
        ImGui::TextUnformatted(title);
        ImGui::PopStyleColor();
        DrawTabs();
        float footer = ImGui::GetTextLineHeightWithSpacing() * 2 + ImGui::GetFrameHeightWithSpacing() + em;
        ImGui::BeginChild("##rows", ImVec2(0, -footer), ImGuiChildFlags_NavFlattened);
        g_hint = "", g_hint_restart = false, g_hint_env = false;
        g_focus_now = g_focus_first > 0 && --g_focus_first == 0;
        g_nav_row_prev = g_nav_row, g_nav_row = 0;
        if (g_page == kKeyboardPage) DrawKeyboard();
        else if (g_page == kControllerPage) DrawController();
        else DrawSettings();
        ImGui::EndChild();
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        std::string hint = g_hint;
        if (g_capture >= 0 && g_capture_pad) hint = "Press a controller button to bind it, Escape cancels.";
        else if (g_capture >= 0) hint = "Press a key or mouse button to bind it, Escape cancels.";
        else if (!*g_hint) hint = "Up/Down select, Left/Right change, LB/RB switch pages, Escape or B closes.";
        ImGui::TextUnformatted(hint.c_str());
        std::string note;
        if (g_capture < 0 && *g_hint) note = g_hint_restart ? "Saved; takes effect after a restart." : "Applies at once and is saved.";
        if (g_hint_env) note += "  FCI_ environment variable set: it wins over the saved value at start.";
        if (g_save_failed) note = "Could not write FarCryInstincts.ini beside the game (no write access?): changes are lost "
                                  "when the game closes.";
        ImGui::TextUnformatted(note.c_str());
        ImGui::PopStyleColor();
        float bw = em * 8;
        float buttons_pad = ImGui::GetContentRegionAvail().x - bw * 2 - ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(buttons_pad, 0.0f));
        if (ImGui::Button("Back", ImVec2(bw, 0))) g_close = true;
        ImGui::SameLine();
        if (ImGui::Button("Defaults", ImVec2(bw, 0))) Defaults();
    }
    ImGui::End();
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(int(std::size(kPanelColors)));
}

// ---- the controller: every connected pad (ImGui's Win32 backend would read only the first XInput one), through
// hle/host_pad (SDL3: Xbox, PlayStation, Switch, generic; the pad_bind_ bindings apply), Back + Start ----

std::atomic<uint16_t> g_script_pad{0};  // FCI_MENU_SCRIPT: pad buttons held

// the d-pad's or left stick's left / right this frame (-1, 0, 1): pressed, then held 0.4 s ten times a second. ImGui's
// own IsKeyPressed missed the first frame of a gamepad press here (its nav takes the same direction)
int PadStep(int dir) {
    static int held = 0;
    static ULONGLONG since = 0, last = 0;
    ULONGLONG now = GetTickCount64();
    if (dir != held) {
        held = dir, since = last = now;
        return dir;
    }
    if (dir && now - since >= 400 && now - last >= 100) {
        last = now;
        return dir;
    }
    return 0;
}

void FeedPads(ImGuiIO& io) {
    XINPUT_GAMEPAD g{};
    for (int i = 0; i < hle::host::kSlots; i++) {
        hle::pc::Pad p;
        if (!hle::host::Read(i, p)) continue;
        // d-pad, start, back and the thumb clicks share their bits with XInput; the face and shoulder buttons are
        // analog on the Xbox pad (white = LB, black = RB as in the bindings)
        WORD buttons = p.buttons & 0xFF;
        auto on = [&](int k, WORD bit) { if (p.analog[k] > 30) buttons |= bit; };
        on(hle::pc::kA, XINPUT_GAMEPAD_A), on(hle::pc::kB, XINPUT_GAMEPAD_B);
        on(hle::pc::kX, XINPUT_GAMEPAD_X), on(hle::pc::kY, XINPUT_GAMEPAD_Y);
        on(hle::pc::kWhite, XINPUT_GAMEPAD_LEFT_SHOULDER), on(hle::pc::kBlack, XINPUT_GAMEPAD_RIGHT_SHOULDER);
        g.wButtons |= buttons;
        g.bLeftTrigger = std::max(g.bLeftTrigger, p.analog[hle::pc::kLeftTrigger]);
        g.bRightTrigger = std::max(g.bRightTrigger, p.analog[hle::pc::kRightTrigger]);
        auto bigger = [](SHORT a, SHORT b) { return std::abs(int(b)) > std::abs(int(a)) ? b : a; };
        g.sThumbLX = bigger(g.sThumbLX, p.thumb[0]), g.sThumbLY = bigger(g.sThumbLY, p.thumb[1]);
    }
    g.wButtons |= g_script_pad.load();
    // the buttons that opened the menu (or were held as it opened) count once let go
    bool held = g.wButtons || g.bLeftTrigger > 30 || g.bRightTrigger > 30;
    if (!g_pad_armed) {
        if (held) g = XINPUT_GAMEPAD{};
        else g_pad_armed = true;
    }
    if (g_capture >= 0) g = XINPUT_GAMEPAD{};  // a binding waits for a key
    int dir = (g.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) || g.sThumbLX < -16384  ? -1
              : (g.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) || g.sThumbLX > 16384 ? 1
                                                                                : 0;
    g_pad_step = PadStep(dir);
    static uint16_t previous = 0;
    constexpr uint16_t kCombo = XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START;
    if ((g.wButtons & kCombo) == kCombo && (previous & kCombo) != kCombo) g_close = true;
    previous = g.wButtons;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    auto button = [&](ImGuiKey key, WORD bit) { io.AddKeyEvent(key, (g.wButtons & bit) != 0); };
    button(ImGuiKey_GamepadStart, XINPUT_GAMEPAD_START);
    button(ImGuiKey_GamepadBack, XINPUT_GAMEPAD_BACK);
    button(ImGuiKey_GamepadFaceLeft, XINPUT_GAMEPAD_X);
    button(ImGuiKey_GamepadFaceRight, XINPUT_GAMEPAD_B);
    button(ImGuiKey_GamepadFaceUp, XINPUT_GAMEPAD_Y);
    button(ImGuiKey_GamepadFaceDown, XINPUT_GAMEPAD_A);
    button(ImGuiKey_GamepadDpadLeft, XINPUT_GAMEPAD_DPAD_LEFT);
    button(ImGuiKey_GamepadDpadRight, XINPUT_GAMEPAD_DPAD_RIGHT);
    button(ImGuiKey_GamepadDpadUp, XINPUT_GAMEPAD_DPAD_UP);
    button(ImGuiKey_GamepadDpadDown, XINPUT_GAMEPAD_DPAD_DOWN);
    button(ImGuiKey_GamepadL1, XINPUT_GAMEPAD_LEFT_SHOULDER);
    button(ImGuiKey_GamepadR1, XINPUT_GAMEPAD_RIGHT_SHOULDER);
    auto analog = [&](ImGuiKey key, int v, int v0, int v1) {
        float n = float(v - v0) / float(v1 - v0);
        io.AddKeyAnalogEvent(key, n > 0.10f, std::clamp(n, 0.0f, 1.0f));
    };
    analog(ImGuiKey_GamepadL2, g.bLeftTrigger, XINPUT_GAMEPAD_TRIGGER_THRESHOLD, 255);
    analog(ImGuiKey_GamepadR2, g.bRightTrigger, XINPUT_GAMEPAD_TRIGGER_THRESHOLD, 255);
    constexpr int kDead = XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE;
    analog(ImGuiKey_GamepadLStickLeft, g.sThumbLX, -kDead, -32768);
    analog(ImGuiKey_GamepadLStickRight, g.sThumbLX, kDead, 32767);
    analog(ImGuiKey_GamepadLStickUp, g.sThumbLY, kDead, 32767);
    analog(ImGuiKey_GamepadLStickDown, g.sThumbLY, -kDead, -32768);
}

// ---- ImGui's context: on the first frame, the font at the window's size ----

bool EnsureContext() {
    if (g_context) return true;
    HWND hwnd = static_cast<HWND>(display::Window());
    if (!hwnd) return false;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad |
                      ImGuiConfigFlags_NoMouseCursorChange;  // the window procedure sets the pointer
    ImGui_ImplWin32_Init(hwnd);
    io.KeyRepeatRate = 0.1f;  // held arrows step ten times a second (as SCDA)
    RECT rc{};
    GetClientRect(hwnd, &rc);
    float px = std::clamp(float(rc.bottom) / 38.0f, 15.0f, 44.0f);
    char windir[MAX_PATH] = "C:\\Windows";
    GetEnvironmentVariableA("WINDIR", windir, sizeof windir);
    ImFont* font = nullptr;
    for (const char* name : {"seguisb.ttf", "segoeui.ttf", "arial.ttf"}) {
        std::string path = std::string(windir) + "\\Fonts\\" + name;
        if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            font = io.Fonts->AddFontFromFileTTF(path.c_str(), px);
            if (font) break;
        }
    }
    if (!font) {
        io.Fonts->AddFontDefault();
        io.FontGlobalScale = px / 13.0f;
    }
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(px / 16.0f);
    g_context = true;
    rt::Log("settings menu: ImGui %s, font %.0f px (%s)", IMGUI_VERSION, px, font ? "Segoe UI" : "built in");
    return true;
}

ImDrawData* Build() {
    std::lock_guard lock(g_ui);
    ImGui_ImplWin32_NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    FeedPads(io);
    ImGui::NewFrame();
    PollCapture();
    DrawPanel();
    // B closes, as Escape does, unless it answers a capture
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) && g_capture < 0) g_close = true;
    ImGui::Render();
    return ImGui::GetDrawData();
}

// ---- FCI_MENU_SCRIPT: host input for unattended tests, by host seconds since the window opened ----
// "62:f10,63:down,63.5:right,64:pad_a,65:!click_640_360,70:escape": keys go to the window as messages (held 0.1 s or
// the time after a second colon), pad_<a|b|x|y|lb|rb|back|start|up|down|left|right|combo> to the menu's controller
// input. With '!' through SendInput as real input (!l, !escape, !click_<x>_<y> at client pixels; only while the
// window is in front, else skipped): GetAsyncKeyState sees those, so they test that the game gets nothing. Posted
// clicks (click_/rclick_) reach ImGui only while the real pointer is over the window. activate / deactivate send
// WM_ACTIVATE (focus pause).

void ScriptThread(std::string text, HWND hwnd) {
    struct Item {
        double at, hold;
        std::string what;
    };
    std::vector<Item> items;
    for (size_t pos = 0; pos < text.size();) {
        size_t end = text.find(',', pos);
        std::string item = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = end == std::string::npos ? text.size() : end + 1;
        size_t c1 = item.find(':');
        if (c1 == std::string::npos) continue;
        size_t c2 = item.find(':', c1 + 1);
        items.push_back({std::atof(item.c_str()), c2 == std::string::npos ? 0.1 : std::atof(item.c_str() + c2 + 1),
                         Lower(item.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1))});
    }
    rt::Log("settings menu: FCI_MENU_SCRIPT with %zu events", items.size());
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.at < b.at; });
    ULONGLONG start = GetTickCount64();
    static const struct {
        const char* name;
        int vk;
    } kKeys[] = {{"f10", VK_F10},       {"f11", VK_F11},    {"escape", VK_ESCAPE}, {"up", VK_UP},         {"down", VK_DOWN},
                 {"left", VK_LEFT},     {"right", VK_RIGHT}, {"enter", VK_RETURN},  {"space", VK_SPACE},   {"tab", VK_TAB},
                 {"delete", VK_DELETE}, {"pageup", VK_PRIOR}, {"pagedown", VK_NEXT}, {"lshift", VK_SHIFT}, {"home", VK_HOME},
                 {"end", VK_END}};
    static const struct {
        const char* name;
        uint16_t bits;
    } kPad[] = {{"a", XINPUT_GAMEPAD_A},     {"b", XINPUT_GAMEPAD_B},          {"x", XINPUT_GAMEPAD_X},
                {"y", XINPUT_GAMEPAD_Y},     {"lb", XINPUT_GAMEPAD_LEFT_SHOULDER}, {"rb", XINPUT_GAMEPAD_RIGHT_SHOULDER},
                {"back", XINPUT_GAMEPAD_BACK}, {"start", XINPUT_GAMEPAD_START}, {"up", XINPUT_GAMEPAD_DPAD_UP},
                {"down", XINPUT_GAMEPAD_DPAD_DOWN}, {"left", XINPUT_GAMEPAD_DPAD_LEFT}, {"right", XINPUT_GAMEPAD_DPAD_RIGHT},
                {"combo", XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START}};
    for (const Item& it : items) {
        while (GetTickCount64() - start < ULONGLONG(it.at * 1000)) Sleep(10);
        DWORD hold = DWORD(it.hold * 1000);
        rt::Log("settings menu: script %.1f s %s", it.at, it.what.c_str());
        if (!it.what.compare(0, 4, "pad_")) {
            for (const auto& p : kPad)
                if (it.what.substr(4) == p.name) {
                    g_script_pad |= p.bits;
                    Sleep(hold);
                    g_script_pad &= uint16_t(~p.bits);
                }
            continue;
        }
        // vpad_<button>: pressed on virtual controller 1 (FCI_VIRTUAL_PADS), read back through SDL like a real one
        // (the game's input path, which feeds it otherwise, stands still while the menu is open)
        if (!it.what.compare(0, 5, "vpad_")) {
            using namespace hle::pc;
            std::string b = it.what.substr(5);
            Pad p;
            static const struct {
                const char* name;
                int analog;
                uint16_t bit;
            } kVirtual[] = {{"a", kA, 0},           {"b", kB, 0},           {"x", kX, 0},
                            {"y", kY, 0},           {"lb", kWhite, 0},      {"rb", kBlack, 0},
                            {"lt", kLeftTrigger, 0}, {"rt", kRightTrigger, 0}, {"start", -1, kStart},
                            {"back", -1, kBack},     {"ls", -1, kLeftThumb}, {"rs", -1, kRightThumb},
                            {"up", -1, kDpadUp},     {"down", -1, kDpadDown}, {"left", -1, kDpadLeft},
                            {"right", -1, kDpadRight}};
            bool known = false;
            for (const auto& v : kVirtual)
                if (b == v.name) {
                    known = true;
                    if (v.analog >= 0) p.analog[v.analog] = 255;
                    p.buttons |= v.bit;
                }
            if (!known || !hle::host::Virtual(0)) {
                rt::Log("settings menu: script %s: %s", it.what.c_str(), known ? "no virtual controller in slot 1" : "unknown");
                continue;
            }
            hle::host::FeedVirtual(0, p);
            Sleep(hold);
            hle::host::FeedVirtual(0, Pad{});
            continue;
        }
        if (it.what == "deactivate" || it.what == "activate") {  // as Windows tells the window (focus pause tests)
            SendMessageW(hwnd, WM_ACTIVATE, it.what == "activate" ? WA_ACTIVE : WA_INACTIVE, 0);
            continue;
        }
        int x = 0, y = 0;
        if (!it.what.compare(0, 7, "!click_")) {  // a real click there: the pointer moves, the window must be in front
            std::sscanf(it.what.c_str() + 7, "%d_%d", &x, &y);
            POINT p{x, y};
            ClientToScreen(hwnd, &p);
            if (GetForegroundWindow() != hwnd) SetForegroundWindow(hwnd), Sleep(100);
            if (GetForegroundWindow() != hwnd) {  // never into another window
                rt::Log("settings menu: script real click skipped, the window is not in front");
                continue;
            }
            SetCursorPos(p.x, p.y);
            Sleep(100);
            INPUT in[2] = {};
            in[0].type = in[1].type = INPUT_MOUSE;
            in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN, in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
            SendInput(1, &in[0], sizeof(INPUT));
            Sleep(hold);
            SendInput(1, &in[1], sizeof(INPUT));
            rt::Log("settings menu: script real click at %d,%d (window %s)", x, y,
                    GetForegroundWindow() == hwnd ? "in front" : "NOT in front");
            continue;
        }
        bool right = !it.what.compare(0, 7, "rclick_");
        if (!it.what.compare(0, 6, "click_") || right) {
            std::sscanf(it.what.c_str() + (right ? 7 : 6), "%d_%d", &x, &y);
            LPARAM at = MAKELPARAM(x, y);
            PostMessageW(hwnd, WM_MOUSEMOVE, 0, at);
            Sleep(50);
            PostMessageW(hwnd, right ? WM_RBUTTONDOWN : WM_LBUTTONDOWN, right ? MK_RBUTTON : MK_LBUTTON, at);
            Sleep(hold);
            PostMessageW(hwnd, right ? WM_RBUTTONUP : WM_LBUTTONUP, 0, at);
            continue;
        }
        int vk = 0;
        for (const auto& k : kKeys)
            if (it.what == k.name) vk = k.vk;
        if (!vk && it.what.size() == 1 && std::isalnum(static_cast<unsigned char>(it.what[0])))
            vk = std::toupper(static_cast<unsigned char>(it.what[0]));
        bool real = !vk && it.what.size() > 1 && it.what[0] == '!';  // "!l": through SendInput, as a real key
        if (real) {
            std::string name = it.what.substr(1);
            for (const auto& k : kKeys)
                if (name == k.name) vk = k.vk;
            if (!vk && name.size() == 1 && std::isalnum(static_cast<unsigned char>(name[0])))
                vk = std::toupper(static_cast<unsigned char>(name[0]));
        }
        if (!vk) {
            rt::Log("settings menu: script key '%s' unknown", it.what.c_str());
            continue;
        }
        UINT scan = MapVirtualKeyW(UINT(vk), MAPVK_VK_TO_VSC);
        if (real) {
            if (GetForegroundWindow() != hwnd) SetForegroundWindow(hwnd), Sleep(100);
            if (GetForegroundWindow() != hwnd) {  // never into another window
                rt::Log("settings menu: script real key skipped, the window is not in front");
                continue;
            }
            INPUT in[2] = {};
            in[0].type = in[1].type = INPUT_KEYBOARD;
            in[0].ki.wVk = in[1].ki.wVk = WORD(vk);
            in[0].ki.wScan = in[1].ki.wScan = WORD(scan);
            in[1].ki.dwFlags = KEYEVENTF_KEYUP;
            UINT sent = SendInput(1, &in[0], sizeof(INPUT));
            rt::Log("settings menu: script real key down (%u sent, window %s)", sent,
                    GetForegroundWindow() == hwnd ? "in front" : "NOT in front");
            INPUT up = in[1];
            std::thread([up, hold]() mutable {  // the release on its own, so later items run meanwhile
                Sleep(hold);
                SendInput(1, &up, sizeof(INPUT));
                rt::Log("settings menu: script real key up");
            }).detach();
            continue;
        }
        bool sys = vk == VK_F10;
        PostMessageW(hwnd, sys ? WM_SYSKEYDOWN : WM_KEYDOWN, WPARAM(vk), LPARAM(1 | scan << 16));
        Sleep(hold);
        PostMessageW(hwnd, sys ? WM_SYSKEYUP : WM_KEYUP, WPARAM(vk), LPARAM(1 | scan << 16 | 0xC0000000u));
    }
}

// the key or mouse button a window message presses, 0 for none (left and right Shift, Ctrl, Alt apart)
int CodeOf(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        if (lp & (1 << 30)) return 0;  // a repeat of a held key
        int vk = int(wp);
        bool extended = (lp >> 24) & 1;
        if (vk == VK_SHIFT) vk = int(MapVirtualKeyW(UINT((lp >> 16) & 0xFF), MAPVK_VSC_TO_VK_EX));
        else if (vk == VK_CONTROL) vk = extended ? VK_RCONTROL : VK_LCONTROL;
        else if (vk == VK_MENU) vk = extended ? VK_RMENU : VK_LMENU;
        return vk;
    }
    case WM_LBUTTONDOWN: return VK_LBUTTON;
    case WM_RBUTTONDOWN: return VK_RBUTTON;
    case WM_MBUTTONDOWN: return VK_MBUTTON;
    case WM_XBUTTONDOWN: return HIWORD(wp) == XBUTTON2 ? VK_XBUTTON2 : VK_XBUTTON1;
    case WM_MOUSEWHEEL: return hle::pc::WheelCode(GET_WHEEL_DELTA_WPARAM(wp) > 0);
    }
    return 0;
}

}  // namespace

bool MenuOpen() { return g_open.load(std::memory_order_relaxed); }

void ToggleMenu() {
    bool open = !g_open.load();
    if (open && !gpu::present::MenuAvailable()) {
        static int told = 0;
        if (told++ < 3) rt::Log("settings menu: not available (needs the HLE graphics and a first picture)");
        return;
    }
    {
        std::lock_guard lock(g_ui);
        g_open = open;
        g_capture = -1;
        g_capture_pad = false;
        g_close = false;
        g_pad_armed = false;
        g_focus_first = open ? 1 : 0;
        if (g_context) {
            ImGuiIO& io = ImGui::GetIO();
            io.ClearEventsQueue();
            io.ClearInputKeys();
        }
    }
    rt::Log("settings menu: %s", open ? "open" : "closed");
    display::MenuShown(open);
    MenuPause(open);
}

void RequestMenu() {
    if (!MenuOpen()) ToggleMenu();
}

bool MenuWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* result) {
    static std::once_flag script;
    std::call_once(script, [hwnd] {
        char text[4096] = {};
        if (GetEnvironmentVariableA("FCI_MENU_SCRIPT", text, sizeof text)) std::thread(ScriptThread, std::string(text), hwnd).detach();
    });
    *result = 0;
    // F10 opens and closes it; its key up must not reach DefWindowProc, which would open the window's menu
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && wp == VK_F10) {
        if (!(lp & (1 << 30))) ToggleMenu();
        return true;
    }
    if ((msg == WM_KEYUP || msg == WM_SYSKEYUP) && wp == VK_F10) return true;
    if (!MenuOpen()) return false;
    // a binding takes the next key or mouse button
    if (g_capture >= 0) {
        if (int code = CodeOf(msg, wp, lp)) {
            // a controller button's capture takes only Escape (to cancel) from the keyboard and mouse
            if (!g_capture_pad || code == VK_ESCAPE) g_caught = code;
            return true;
        }
        // releases still go to ImGui: the Enter that started the capture would stay down there otherwise, and the
        // next Enter would start nothing
        if (msg == WM_CHAR) return true;
    }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
        if (!(lp & (1 << 30))) ToggleMenu();
        return true;
    }
    {
        std::lock_guard lock(g_ui);
        if (g_context) ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
    }
    // the keyboard is the menu's; F11 and Alt+Enter still switch the display mode
    if ((msg == WM_KEYDOWN && wp != VK_F11) || msg == WM_KEYUP || msg == WM_CHAR) return true;
    return false;
}

void MenuFrame() {
    {
        std::lock_guard lock(g_ui);
        if (!EnsureContext()) return;
    }
    gpu::present::MenuFrame(Build);
    if (g_close) {
        g_close = false;
        if (MenuOpen()) ToggleMenu();
    }
}

void MenuClosed() {
    hle::pc::SettleAfterMenu();
    hle::SettlePads();
    SaveSettings();
}

}  // namespace app
