#include "app/settings.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <vector>

#include "runtime/log.h"

namespace settings {

namespace {

std::string Trim(std::string s) {
    size_t b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

std::string Lower(std::string s) {
    for (char& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

std::wstring IniPath() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path);
    return p.substr(0, p.find_last_of(L"\\/") + 1) + L"FarCryInstincts.ini";
}

// ---- settings menu: values changed while running win over the environment and the file ----
std::recursive_mutex g_lock;
std::map<std::string, std::string> g_changed;  // lower case key -> value, set by the menu
std::set<std::string> g_unsaved;               // changed keys not yet in the file
// ---- end settings menu ----

const std::map<std::string, std::string>& Ini() {
    static std::map<std::string, std::string> values;
    static std::once_flag once;
    std::call_once(once, [] {
        std::ifstream in(IniPath());
        if (!in) return;
        std::string line;
        while (std::getline(in, line)) {
            line = Trim(line.substr(0, line.find_first_of("#;")));
            size_t eq = line.find('=');
            if (line.empty() || line[0] == '[' || eq == std::string::npos) continue;
            values[Lower(Trim(line.substr(0, eq)))] = Trim(line.substr(eq + 1));
        }
        rt::Log("settings: %zu values from FarCryInstincts.ini", values.size());
    });
    return values;
}

bool Environment(const char* key, std::string& out) {
    std::string env = "FCI_";
    for (const char* k = key; *k; k++) env += char(std::toupper(static_cast<unsigned char>(*k)));
    char value[256];
    DWORD n = GetEnvironmentVariableA(env.c_str(), value, sizeof value);
    if (n && n < sizeof value) {
        out = value;
        return true;
    }
    return false;
}

// the value as the program started with it: the environment, else the file
bool BootLookup(const char* key, std::string& out) {
    if (Environment(key, out)) return true;
    auto it = Ini().find(Lower(key));
    if (it == Ini().end()) return false;
    out = it->second;
    return true;
}

bool Lookup(const char* key, std::string& out) {
    {
        std::lock_guard lock(g_lock);  // settings menu
        auto it = g_changed.find(Lower(key));
        if (it != g_changed.end()) {
            out = it->second;
            return true;
        }
    }
    return BootLookup(key, out);
}

// ---- device presets ----
// a text file of the host (under Wine/Proton the Linux root is drive Z:), first line, trimmed; "" when unreadable
std::string ReadLine(const char* path) {
    std::ifstream in(path);
    std::string line;
    if (in) std::getline(in, line);
    return Trim(line);
}

// a value of the firmware's system information as Windows keeps it (Wine fills it from DMI as well)
std::string BiosValue(const char* name) {
    char value[128] = {};
    DWORD size = sizeof value;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\BIOS", name, RRF_RT_REG_SZ, nullptr, value,
                     &size) != ERROR_SUCCESS)
        return "";
    return Trim(value);
}

struct DevicePreset {
    std::string device;
    std::map<std::string, std::string> defaults;
};

// the Steam Deck: board vendor Valve, product Jupiter (LCD) or Galileo (OLED), from the registry (Windows, Wine) or the
// DMI files (Linux through Z:), else SteamDeck=1 (Steam sets it for games on the Deck); FCI_DEVICE=deck|pc forces it
const DevicePreset& Preset() {
    static const DevicePreset preset = [] {
        DevicePreset p;
        char forced[16] = {};
        GetEnvironmentVariableA("FCI_DEVICE", forced, sizeof forced);
        std::string vendor = BiosValue("BaseBoardManufacturer"), product = BiosValue("SystemProductName"), how = "registry";
        if (vendor.empty() && product.empty()) {
            vendor = ReadLine("Z:\\sys\\devices\\virtual\\dmi\\id\\board_vendor");
            product = ReadLine("Z:\\sys\\devices\\virtual\\dmi\\id\\product_name");
            how = "DMI";
        }
        char env[8] = {};
        const bool steam_deck_env = GetEnvironmentVariableA("SteamDeck", env, sizeof env) && env[0] == '1';
        const bool valve = Lower(vendor) == "valve";
        const std::string model = Lower(product);
        bool deck = (valve && (model == "jupiter" || model == "galileo")) || steam_deck_env;
        if (!_stricmp(forced, "deck")) deck = true;
        if (!_stricmp(forced, "pc")) deck = false;
        if (deck) {
            p.device = model == "galileo" ? "Steam Deck OLED" : model == "jupiter" ? "Steam Deck LCD" : "Steam Deck";
            // the high preset measured on the Deck OLED: 90 fps with 1 % lows of 86-90; two
            // frames ahead: the guest's CPU time is ~9.5 of 11.1 ms a frame there, single game frames of 15-25 ms
            // are then absorbed instead of shown late
            p.defaults = {{"ao", "gtao"}, {"shadow_scale", "4"}, {"frame_queue", "2"},
                          // PC level of detail: the console's distances; the game thread has no room for more on
                          // the Deck (lod_distance 2 cost 1-3 fps of the 1 % lows there)
                          {"lod_distance", "1"}, {"view_distance", "1"}};
        }
        rt::Log("settings: device %s (%s: vendor '%s', product '%s', SteamDeck=%s%s%s)",
                deck ? p.device.c_str() : "not a Steam Deck", how.c_str(), vendor.c_str(), product.c_str(),
                steam_deck_env ? "1" : "-", forced[0] ? ", FCI_DEVICE=" : "", forced);
        for (const auto& [k, v] : p.defaults) rt::Log("settings: %s default %s = %s", p.device.c_str(), k.c_str(), v.c_str());
        return p;
    }();
    return preset;
}

bool DeviceDefault(const char* key, std::string& out) {
    const auto& d = Preset().defaults;
    auto it = d.find(Lower(key));
    if (it == d.end()) return false;
    out = it->second;
    return true;
}

// what the program uses for key: set (menu, environment, file), else the device's default
bool Value(const char* key, std::string& out) { return Lookup(key, out) || DeviceDefault(key, out); }
// ---- end device presets ----

}  // namespace

std::string Default(const char* key, const char* fallback) {
    std::string v;
    return DeviceDefault(key, v) ? v : std::string(fallback);
}

const std::string& Device() { return Preset().device; }

int Int(const char* key, int fallback) {
    std::string v;
    return Value(key, v) && !v.empty() ? std::atoi(v.c_str()) : fallback;
}

float Float(const char* key, float fallback) {
    std::string v;
    return Value(key, v) && !v.empty() ? float(std::atof(v.c_str())) : fallback;
}

std::string String(const char* key, const char* fallback) {
    std::string v;
    return Value(key, v) ? v : std::string(fallback);
}

// ---- PC comfort features (input, skip_intro, pause_on_focus_loss) ----
bool Bool(const char* key, bool fallback) {
    std::string v;
    if (!Value(key, v)) return fallback;
    v = Lower(Trim(v));
    if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
    if (v == "0" || v == "false" || v == "no" || v == "off") return false;
    return fallback;
}
// ---- end PC comfort features ----

// ---- settings menu ----
void Set(const char* key, const std::string& value) {
    Ini();  // the file's values stay what the program started with
    std::lock_guard lock(g_lock);
    std::string k = Lower(key);
    auto it = g_changed.find(k);
    if (it != g_changed.end() && it->second == value) return;
    g_changed[k] = value;
    g_unsaved.insert(k);
}

std::string Boot(const char* key, const char* fallback) {
    std::string v;
    return BootLookup(key, v) || DeviceDefault(key, v) ? v : std::string(fallback);
}

bool FromEnvironment(const char* key) {
    std::string v;
    return Environment(key, v);
}

bool Save() {
    std::lock_guard lock(g_lock);
    if (g_unsaved.empty()) return true;
    const std::wstring path = IniPath();
    std::vector<std::string> lines;
    bool crlf = false;
    {
        std::ifstream in(path, std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back(), crlf = true;
            lines.push_back(line);
        }
    }
    std::set<std::string> pending = g_unsaved;
    // a key's last line counts (as when reading), so that is the one changed
    for (size_t i = lines.size(); i-- > 0;) {
        const std::string& line = lines[i];
        size_t comment = line.find_first_of("#;");
        std::string body = line.substr(0, comment);
        size_t eq = body.find('=');
        std::string trimmed = Trim(body);
        if (trimmed.empty() || trimmed[0] == '[' || eq == std::string::npos) continue;
        std::string key = Lower(Trim(body.substr(0, eq)));
        if (!pending.count(key)) continue;
        pending.erase(key);
        // "key = value   # comment": the value replaced, the spacing before the comment and the comment kept
        size_t value_end = body.find_last_not_of(" \t");
        std::string gap = comment == std::string::npos ? std::string() : body.substr(value_end + 1);
        if (comment != std::string::npos && gap.empty()) gap = " ";
        size_t value_start = body.find_first_not_of(" \t", eq + 1);
        std::string head = value_start == std::string::npos ? body.substr(0, eq + 1) + " " : body.substr(0, value_start);
        lines[i] = head + g_changed[key] + (comment == std::string::npos ? std::string() : gap + line.substr(comment));
    }
    if (!pending.empty()) {
        // under one heading at the end: the first save writes it, later ones add to it
        const char* kHeading = "# settings menu (F10)";
        if (std::find(lines.begin(), lines.end(), kHeading) == lines.end()) {
            if (!lines.empty() && !Trim(lines.back()).empty()) lines.push_back("");
            lines.push_back(kHeading);
        }
        for (const std::string& key : pending) lines.push_back(key + " = " + g_changed[key]);
    }
    std::string text;
    for (const std::string& line : lines) text += line + (crlf ? "\r\n" : "\n");
    // into a temporary file first, then over the old one: a failed write leaves the old file whole
    const std::wstring temp = path + L".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(text.data(), std::streamsize(text.size()));
        if (!out) {
            rt::Log("settings: could not write FarCryInstincts.ini.tmp");
            return false;
        }
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        rt::Log("settings: could not replace FarCryInstincts.ini (error %lu)", GetLastError());
        DeleteFileW(temp.c_str());
        return false;
    }
    std::string keys;
    for (const std::string& key : g_unsaved) keys += " " + key + "=" + g_changed[key];
    rt::Log("settings: saved to FarCryInstincts.ini:%s", keys.c_str());
    g_unsaved.clear();
    return true;
}
// ---- end settings menu ----

}  // namespace settings
