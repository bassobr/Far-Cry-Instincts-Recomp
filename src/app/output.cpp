// Display output options: display mode, monitor, resolution, HDR10 (see output.h).
// The monitor list, the DXGI output lookup and Windows' SDR white level are adapted from SCDA-XBERecomp (MIT,
// Copyright (c) 2026 TGP482, Joshhhuaaa): src/app/settings.cpp (Displays, MonitorHandle), src/gpu/d3d12.cpp
// (FindOutput, SdrWhiteNits).
#include "app/output.h"

#include <windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include "app/settings.h"
#include "runtime/log.h"

namespace output {

namespace {

using Microsoft::WRL::ComPtr;

struct Monitor {
    HMONITOR handle;
    MONITORINFOEXW info;
    DEVMODEW mode;
    int number;  // n of \\.\DISPLAYn
    bool primary;
};

std::string Narrow(const wchar_t* s) {
    std::string out;
    for (; *s; s++) out += char(*s);
    return out;
}

std::string Lower(std::string s) {
    for (char& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

// the attached monitors in \\.\DISPLAYn order
std::vector<Monitor> Monitors() {
    std::vector<Monitor> out;
    auto add = [](HMONITOR mon, HDC, LPRECT, LPARAM list) -> BOOL {
        Monitor m{mon, {}, {}, 0, false};
        m.info.cbSize = sizeof m.info;
        m.mode.dmSize = sizeof m.mode;
        if (!GetMonitorInfoW(mon, &m.info)) return TRUE;
        if (!EnumDisplaySettingsW(m.info.szDevice, ENUM_CURRENT_SETTINGS, &m.mode)) m.mode.dmSize = 0;
        m.primary = (m.info.dwFlags & MONITORINFOF_PRIMARY) != 0;
        std::wstring dev = m.info.szDevice;
        size_t digits = dev.find_last_not_of(L"0123456789") + 1;
        m.number = digits < dev.size() ? _wtoi(dev.c_str() + digits) : 0;
        reinterpret_cast<std::vector<Monitor>*>(list)->push_back(m);
        return TRUE;
    };
    EnumDisplayMonitors(nullptr, nullptr, add, LPARAM(&out));
    std::sort(out.begin(), out.end(), [](const Monitor& a, const Monitor& b) { return a.number < b.number; });
    return out;
}

// ---- relaunch: FCI_WINDOW_STATE = "1|<windowed|borderless|exclusive>|<device>|x|y|w|h|maximized|placement|flags|
// showCmd|left|top|right|bottom", only in a relaunched process ----
const char* const kShown[] = {"windowed", "borderless", "exclusive"};

Carried DecodeCarried() {
    Carried c;
    if (GetEnvironmentVariableA("FCI_RELAUNCHED", nullptr, 0) == 0) return c;
    char text[512] = {};
    DWORD n = GetEnvironmentVariableA("FCI_WINDOW_STATE", text, sizeof text);
    if (!n || n >= sizeof text) return c;
    std::vector<std::string> f;
    for (char *p = text, *bar; p; p = bar ? bar + 1 : nullptr) {
        bar = std::strchr(p, '|');
        if (bar) *bar = 0;
        f.push_back(p);
    }
    if (f.size() != 15 || f[0] != "1") {
        rt::Log("output: FCI_WINDOW_STATE '%s' not understood, the settings decide", text);
        return c;
    }
    int shown = -1;
    for (int i = 0; i < 3; i++)
        if (f[1] == kShown[i]) shown = i;
    if (shown < 0) return c;
    auto num = [&](size_t i) { return std::atoi(f[i].c_str()); };
    c.shown = Shown(shown);
    for (char ch : f[2]) c.device += wchar_t(static_cast<unsigned char>(ch));
    c.x = num(3), c.y = num(4), c.w = num(5), c.h = num(6);
    c.maximized = num(7) != 0, c.placement = num(8) != 0;
    c.flags = unsigned(num(9)), c.show = unsigned(num(10));
    c.left = num(11), c.top = num(12), c.right = num(13), c.bottom = num(14);
    c.valid = c.w > 0 && c.h > 0;
    return c;
}
// ---- end relaunch ----

Options Load() {
    // exclusive fullscreen and the resolution are in pixels: per-monitor DPI aware before any window exists (a
    // DPI unaware process sees a scaled desktop, 2752x1152 for 3440x1440 at 125 %)
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
    Options o;
    const bool mode_set = !settings::String("display_mode", "").empty();
    std::string mode = Lower(settings::String("display_mode", "windowed"));
    if (mode == "borderless") o.mode = Mode::Borderless;
    else if (mode == "fullscreen" || mode == "exclusive") o.mode = Mode::Fullscreen;
    else if (mode != "windowed" && !mode.empty()) rt::Log("output: display_mode '%s' unknown, windowed", mode.c_str());

    std::vector<Monitor> list = Monitors();
    std::string want = Lower(settings::String("monitor", ""));
    int chosen = -1;
    for (size_t i = 0; i < list.size(); i++)
        if (list[i].primary) chosen = int(i);
    if (!want.empty() && want != "0" && want != "auto" && want != "primary") {
        int found = -1;
        if (std::isdigit(static_cast<unsigned char>(want[0]))) {
            int n = std::atoi(want.c_str());
            if (n >= 1 && n <= int(list.size())) found = n - 1;
        } else {
            for (size_t i = 0; i < list.size(); i++) {
                std::string dev = Lower(Narrow(list[i].info.szDevice));
                if (dev == want || dev == "\\\\.\\" + want) found = int(i);
            }
        }
        if (found < 0) rt::Log("output: monitor '%s' not found, the primary", want.c_str());
        else chosen = found, o.monitor_set = true;
    }
    // ---- relaunch: the monitor the previous process's window was on (by its device name, else where its rectangle
    // lies now), not the one of the settings: the window may have been dragged there ----
    o.carried = DecodeCarried();
    if (o.carried.valid) {
        RECT r{o.carried.x, o.carried.y, o.carried.x + o.carried.w, o.carried.y + o.carried.h};
        if (!MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) {  // its monitor is gone: as the settings say
            rt::Log("output: relaunch: the previous window (%d,%d %dx%d) is on no monitor now, the settings decide",
                    o.carried.x, o.carried.y, o.carried.w, o.carried.h);
            o.carried.valid = false;
        }
    }
    if (o.carried.valid) {
        int found = -1;
        for (size_t i = 0; i < list.size(); i++)
            if (!_wcsicmp(list[i].info.szDevice, o.carried.device.c_str())) found = int(i);
        if (found < 0) {
            RECT r{o.carried.x, o.carried.y, o.carried.x + o.carried.w, o.carried.y + o.carried.h};
            HMONITOR near_rect = MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST);
            for (size_t i = 0; i < list.size(); i++)
                if (list[i].handle == near_rect) found = int(i);
        }
        if (found >= 0) chosen = found, o.monitor_set = true;
        rt::Log("output: relaunch: window %s at %d,%d %dx%d%s on %s (from the previous process)",
                kShown[int(o.carried.shown)], o.carried.x, o.carried.y, o.carried.w, o.carried.h,
                o.carried.maximized ? ", maximized" : "", Narrow(o.carried.device.c_str()).c_str());
    }
    // ---- end relaunch ----
    for (size_t i = 0; i < list.size(); i++) {
        const Monitor& m = list[i];
        const RECT& r = m.info.rcMonitor;
        rt::Log("output: monitor %zu %s %ldx%ld at %ld,%ld, %lu Hz%s%s", i + 1, Narrow(m.info.szDevice).c_str(),
                r.right - r.left, r.bottom - r.top, r.left, r.top, m.mode.dmSize ? m.mode.dmDisplayFrequency : 0ul,
                m.primary ? ", primary" : "", int(i) == chosen ? "  <- chosen" : "");
    }
    if (chosen < 0 && !list.empty()) chosen = 0;
    if (chosen >= 0) {
        const Monitor& m = list[size_t(chosen)];
        o.monitor = m.handle;
        o.device = m.info.szDevice;
        const RECT& r = m.info.rcMonitor;
        const RECT& w = m.info.rcWork;
        o.mon_x = r.left, o.mon_y = r.top, o.mon_w = r.right - r.left, o.mon_h = r.bottom - r.top;
        o.work_x = w.left, o.work_y = w.top, o.work_w = w.right - w.left, o.work_h = w.bottom - w.top;
        o.desktop_hz = m.mode.dmSize && m.mode.dmDisplayFrequency > 1 ? int(m.mode.dmDisplayFrequency) : 0;
        using DpiForMonitor = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);  // shcore, Windows 8.1
        if (HMODULE shcore = LoadLibraryW(L"shcore.dll"))
            if (auto get = reinterpret_cast<DpiForMonitor>(GetProcAddress(shcore, "GetDpiForMonitor"))) {
                UINT x = 96, y = 96;
                if (SUCCEEDED(get(m.handle, 0 /* MDT_EFFECTIVE_DPI */, &x, &y)) && y) o.dpi_scale = float(y) / 96.0f;
            }
    } else {
        o.mon_w = o.work_w = GetSystemMetrics(SM_CXSCREEN), o.mon_h = o.work_h = GetSystemMetrics(SM_CYSCREEN);
    }

    // display_mode not set and the default window (twice the console's 480 lines at the monitor's DPI, with its
    // frame) taller than the monitor's work area (Steam Deck 1280x800, small laptops): borderless instead, so the
    // picture is the monitor's size (a too tall window was shown scaled down, e.g. 1536x960 on the Deck)
    if (!mode_set && o.mode == Mode::Windowed && settings::String("resolution", "").empty()) {
        int need = int(480 * 2 * o.dpi_scale + 0.5f) + int(40 * o.dpi_scale);
        if (o.work_h > 0 && need > o.work_h) {
            o.mode = Mode::Borderless;
            rt::Log("output: the default window (%d lines) does not fit the monitor's %d, borderless", need, o.work_h);
        }
    }

    std::string res = Lower(settings::String("resolution", ""));
    if (!res.empty() && res != "auto" && res != "desktop" && res != "native") {
        size_t x = res.find_first_of("x*:");
        int w = std::atoi(res.c_str()), h = x == std::string::npos ? 0 : std::atoi(res.c_str() + x + 1);
        if (w >= 320 && h >= 240 && w <= 16384 && h <= 16384) o.width = w, o.height = h;
        else rt::Log("output: resolution '%s' not understood, default", res.c_str());
    }
    if (o.mode == Mode::Borderless && o.width) {
        rt::Log("output: borderless covers the monitor, resolution %dx%d ignored", o.width, o.height);
        o.width = o.height = 0;
    }

    std::string hdr = Lower(settings::String("hdr", "auto"));
    if (hdr == "on" || hdr == "1" || hdr == "true" || hdr == "hdr10") o.hdr = Hdr::On;
    else if (hdr == "off" || hdr == "0" || hdr == "false") o.hdr = Hdr::Off;
    std::string paper = Lower(settings::String("hdr_paper_white", "200"));
    o.paper_white = paper == "auto" ? 0.0f : std::clamp(float(std::atof(paper.c_str())), 80.0f, 1000.0f);
    o.peak = std::max(settings::Float("hdr_peak", 0), 0.0f);
    if (o.peak) o.peak = std::clamp(o.peak, 100.0f, 10000.0f);
    o.boost = std::clamp(settings::Float("hdr_boost", 0), 0.0f, 1.0f);

    static const char* const kModes[] = {"windowed", "borderless", "fullscreen"};
    static const char* const kHdr[] = {"auto", "on", "off"};
    char size[32] = "default";
    if (o.width) snprintf(size, sizeof size, "%dx%d", o.width, o.height);
    rt::Log("output: %s on %s (%dx%d, %d Hz, DPI x%.2f), resolution %s, hdr %s (paper white %s, peak %.0f, boost %.2f)",
            kModes[int(o.mode)], Narrow(o.device.c_str()).c_str(), o.mon_w, o.mon_h, o.desktop_hz, o.dpi_scale, size,
            kHdr[int(o.hdr)], o.paper_white ? std::to_string(int(o.paper_white)).c_str() : "auto", o.peak, o.boost);
    return o;
}

}  // namespace

const Options& Get() {
    static const Options options = Load();
    return options;
}

std::string EncodeCarried(const Carried& c) {
    char text[512];
    snprintf(text, sizeof text, "1|%s|%s|%d|%d|%d|%d|%d|%d|%u|%u|%d|%d|%d|%d", kShown[int(c.shown)],
             Narrow(c.device.c_str()).c_str(), c.x, c.y, c.w, c.h, int(c.maximized), int(c.placement), c.flags, c.show,
             c.left, c.top, c.right, c.bottom);
    return text;
}

float AutoAspect() {
    const Options& o = Get();
    if (o.width && o.height) return float(o.width) / float(o.height);
    return o.mon_w > 0 && o.mon_h > 0 ? float(o.mon_w) / float(o.mon_h) : 4.0f / 3.0f;
}

int OutputHeight() {
    const Options& o = Get();
    return o.height ? o.height : o.mon_h > 0 ? o.mon_h : GetSystemMetrics(SM_CYSCREEN);
}

HdrInfo QueryHdr(void* monitor) {
    HdrInfo info;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return info;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; !info.found && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; a++) {
        ComPtr<IDXGIOutput> out;
        for (UINT i = 0; !info.found && adapter->EnumOutputs(i, &out) != DXGI_ERROR_NOT_FOUND; i++) {
            ComPtr<IDXGIOutput6> out6;
            DXGI_OUTPUT_DESC1 desc{};
            if (FAILED(out.As(&out6)) || FAILED(out6->GetDesc1(&desc)) || desc.Monitor != monitor) continue;
            info.found = true;
            info.enabled = desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            info.max_nits = desc.MaxLuminance, info.min_nits = desc.MinLuminance;
            info.max_full_frame_nits = desc.MaxFullFrameLuminance;
            info.bits = desc.BitsPerColor;
        }
    }
    // Windows' SDR content brightness ("SDR content brightness" slider): 1000 is 80 nits
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof mi;
    UINT32 paths_n = 0, modes_n = 0;
    if (!GetMonitorInfoW(static_cast<HMONITOR>(monitor), &mi) ||
        GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths_n, &modes_n) != ERROR_SUCCESS)
        return info;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(paths_n);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modes_n);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &paths_n, paths.data(), &modes_n, modes.data(), nullptr) !=
        ERROR_SUCCESS)
        return info;
    for (UINT32 i = 0; i < paths_n; i++) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof source;
        source.header.adapterId = paths[i].sourceInfo.adapterId;
        source.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS || std::wcscmp(source.viewGdiDeviceName, mi.szDevice))
            continue;
        DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
        white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        white.header.size = sizeof white;
        white.header.adapterId = paths[i].targetInfo.adapterId;
        white.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS) info.sdr_white_nits = white.SDRWhiteLevel * 0.08f;
        break;
    }
    return info;
}

}  // namespace output
