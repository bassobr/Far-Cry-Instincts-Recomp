// Button prompts that follow the input device (GitHub #15): which style the prompt glyphs are drawn in.
// See hle/prompts.h; the drawing is gpu/prompt_icons.cpp.
#include "hle/prompts.h"

#include <windows.h>

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <string>

#include "app/settings.h"
#include "runtime/log.h"

namespace hle::prompts {

namespace {

enum class Setting : uint8_t { kAuto, kXbox, kKeyboard, kPlayStation, kSwitch };

std::atomic<Setting> g_setting{Setting::kAuto};
std::atomic<bool> g_setting_read{false};
// auto: the device used last; before anything is used a connected controller's, else keyboard and mouse
std::atomic<Style> g_last{Style::kKeyboard};
std::atomic<bool> g_used{false};  // something was used (else a connected pad decides)
std::atomic<int> g_generation{4};
std::atomic<uint32_t> g_revision{1};

Setting Parse(std::string v) {
    for (char& ch : v) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
    if (v == "xbox") return Setting::kXbox;
    if (v == "keyboard" || v == "kbm" || v == "pc") return Setting::kKeyboard;
    if (v == "playstation" || v == "ps" || v == "ps4" || v == "ps5") return Setting::kPlayStation;
    if (v == "switch" || v == "nintendo") return Setting::kSwitch;
    if (v != "auto" && !v.empty()) rt::Log("prompts: unknown button_prompts '%s', using auto", v.c_str());
    return Setting::kAuto;
}

void ReadSetting() {
    if (g_setting_read.exchange(true)) return;
    g_setting = Parse(settings::String("button_prompts", "auto"));
}

double Now() {
    static const double frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return double(f.QuadPart);
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart) / frequency;
}

void Use(Style s, const char* why) {
    g_used = true;
    Style was = g_last.exchange(s);
    if (was == s) return;
    ReadSetting();
    if (g_setting == Setting::kAuto) rt::Log("prompts: %s (%s)", Name(s), why);
}

// mouse motion: this many counts within kMouseWindow seconds switch to keyboard and mouse; a nudge of the desk or
// a mouse's jitter stays below
constexpr double kMouseWindow = 0.25;
constexpr int kMouseCounts = 80;
double g_mouse_from = 0;
int g_mouse_sum = 0;

}  // namespace

const char* Name(Style s) {
    switch (s) {
    case Style::kXbox: return "xbox";
    case Style::kKeyboard: return "keyboard";
    case Style::kPlayStation: return "playstation";
    case Style::kSwitch: return "switch";
    }
    return "?";
}

Style Current() {
    ReadSetting();
    switch (g_setting.load()) {
    case Setting::kXbox: return Style::kXbox;
    case Setting::kKeyboard: return Style::kKeyboard;
    case Setting::kPlayStation: return Style::kPlayStation;
    case Setting::kSwitch: return Style::kSwitch;
    case Setting::kAuto: break;
    }
    return g_last;
}

int PlayStationGeneration() { return g_generation; }

uint32_t Revision() { return g_revision; }

void NoteKeyboard() {
    if (g_last != Style::kKeyboard || !g_used) Use(Style::kKeyboard, "key or mouse button");
}

void NoteMouseMotion(int dx, int dy) {
    if (g_last == Style::kKeyboard) return;
    double now = Now();
    if (now - g_mouse_from > kMouseWindow) g_mouse_from = now, g_mouse_sum = 0;
    g_mouse_sum += std::abs(dx) + std::abs(dy);
    if (g_mouse_sum >= kMouseCounts) {
        g_mouse_sum = 0;
        Use(Style::kKeyboard, "mouse motion");
    }
}

void NotePad(PadFamily family, int generation) {
    Style s = family == PadFamily::kPlayStation ? Style::kPlayStation
              : family == PadFamily::kSwitch    ? Style::kSwitch
                                                : Style::kXbox;
    if (s == Style::kPlayStation && generation && g_generation.exchange(generation) != generation) g_revision++;
    if (g_last != s || !g_used) Use(s, "controller");
}

void NotePadConnected(PadFamily family, int generation) {
    // before anything was used, a connected controller decides (a Steam Deck starts with its pad's prompts)
    if (g_used) return;
    Style s = family == PadFamily::kPlayStation ? Style::kPlayStation
              : family == PadFamily::kSwitch    ? Style::kSwitch
                                                : Style::kXbox;
    if (s == Style::kPlayStation && generation && g_generation.exchange(generation) != generation) g_revision++;
    g_last = s;
}

void Reload() {
    g_setting_read = false;
    ReadSetting();
    g_revision++;
    rt::Log("prompts: button_prompts = %s, showing %s", settings::String("button_prompts", "auto").c_str(),
            Name(Current()));
}

void BindingsChanged() { g_revision++; }

}  // namespace hle::prompts
