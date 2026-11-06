// Display output options (FarCryInstincts.ini / FCI_<KEY>, see README "Configuration"):
//   display_mode    = windowed | borderless | fullscreen   (fullscreen = exclusive through DXGI, HLE graphics only)
//   monitor         = 0 (the primary) | 1, 2, ... (the n-th display in \\.\DISPLAYn order) | \\.\DISPLAYn
//   resolution      = <width>x<height>   windowed: client size; fullscreen: the display mode; default: as before
//   hdr             = auto | on | off    HDR10 output (auto: when Windows HDR is on for the chosen monitor)
//   hdr_paper_white = nits of the game's white in HDR (default 200; auto = Windows' SDR content brightness)
//   hdr_peak        = the display's peak nits (default: what Windows reports, else 1000)
//   hdr_boost       = 0..1 highlight expansion towards the peak (default 0 = off)
// Read once, on first use; the first use also makes the process per-monitor DPI aware (before the window exists).
#pragma once

#include <string>

namespace output {

enum class Mode { Windowed, Borderless, Fullscreen };
enum class Hdr { Auto, On, Off };

struct Options {
    Mode mode = Mode::Windowed;
    bool monitor_set = false;  // a monitor was chosen (else the window is placed as before, on the primary)
    void* monitor = nullptr;   // HMONITOR
    std::wstring device;       // \\.\DISPLAYn
    int mon_x = 0, mon_y = 0, mon_w = 0, mon_h = 0;      // the monitor's rectangle on the desktop
    int work_x = 0, work_y = 0, work_w = 0, work_h = 0;  // without the task bar
    int desktop_hz = 0;
    float dpi_scale = 1;              // the monitor's DPI / 96
    int width = 0, height = 0;        // resolution; 0 = default (window as before, fullscreen at the desktop mode)
    Hdr hdr = Hdr::Auto;
    float paper_white = 200;          // 0: Windows' SDR content brightness
    float peak = 0;                   // 0: the display's
    float boost = 0;
};

const Options& Get();
// the shape aspect = auto follows: the resolution's, else the chosen monitor's
float AutoAspect();
// the height the output has (resolution, else the monitor's): scale = auto follows it
int OutputHeight();

// HDR state of a monitor as DXGI and Windows report it
struct HdrInfo {
    bool found = false;     // an output of a DXGI adapter shows it
    bool enabled = false;   // Windows HDR on (colour space G2084 / P2020)
    float max_nits = 0, min_nits = 0, max_full_frame_nits = 0;
    float sdr_white_nits = 0;  // Windows' SDR content brightness, 0 when unknown
    unsigned bits = 0;
};
HdrInfo QueryHdr(void* monitor);

}  // namespace output
