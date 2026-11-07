#include "video/display.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "app/output.h"  // display output options
#include "app/pcmode.h"
#include "app/focus_pause.h"  // PC comfort features
#include "app/menu.h"         // settings menu
#include "gpu/nv2a.h"
#include "gpu/present.h"   // relaunch handover: GPU memory
#include "gpu/renderer.h"  // CompileHlsl
#include "kernel/kernel.h"  // relaunch handover
#include "hle/host_pad.h"  // controllers: motors off at exit
#include "runtime/guest.h"
#include "runtime/log.h"
#include "runtime/memory.h"
#include "xdk/xdk.h"

namespace display {

namespace {

constexpr int kWidth = 640, kHeight = 480;  // the mode D3D sets up; read from the CRTC later
constexpr int kScale = 2;

HANDLE g_vblank = nullptr;
std::atomic<HWND> g_window{nullptr};
HANDLE g_window_ready = nullptr;
// HLE graphics: the renderer presents into the window itself (gpu/present.cpp); this thread keeps the window,
// its messages and the mouse
bool g_hle_output = false;
std::vector<uint32_t> g_frame(kWidth * kHeight);
int g_dump_every = 0;
uint64_t g_frames = 0;

uint32_t Reg(uint32_t offset) { return gpu::Read(offset, 4); }

uint8_t Clamp(int v) { return uint8_t(std::clamp(v, 0, 255)); }

uint32_t YuvToRgb(int y, int u, int v) {
    int c = y - 16, d = u - 128, e = v - 128;
    int r = (298 * c + 409 * e + 128) >> 8;
    int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
    int b = (298 * c + 516 * d + 128) >> 8;
    return 0xFF000000u | (Clamp(r) << 16) | (Clamp(g) << 8) | Clamp(b);
}

// the overlay buffer last handed to PVIDEO, scaled into its output rectangle
void ComposeOverlay() {
    if (!gpu::OverlayActive()) return;
    int buffer = gpu::OverlayBuffer();
    uint32_t base = Reg(gpu::reg::PVIDEO_BASE_0 + 4 * buffer);
    uint32_t offset = Reg(gpu::reg::PVIDEO_OFFSET_0 + 4 * buffer);
    uint32_t size_in = Reg(gpu::reg::PVIDEO_SIZE_IN_0 + 4 * buffer);
    uint32_t point_out = Reg(gpu::reg::PVIDEO_POINT_OUT_0 + 4 * buffer);
    uint32_t size_out = Reg(gpu::reg::PVIDEO_SIZE_OUT_0 + 4 * buffer);
    uint32_t format = Reg(gpu::reg::PVIDEO_FORMAT_0 + 4 * buffer);
    int in_w = size_in & 0x7FF, in_h = (size_in >> 16) & 0x7FF;
    int out_x = point_out & 0xFFF, out_y = (point_out >> 16) & 0xFFF;
    int out_w = size_out & 0xFFF, out_h = (size_out >> 16) & 0xFFF;
    uint32_t pitch = format & 0x1FFF;
    // COLOR: 1 is LE_CR8YB8CB8YA8 (bytes Y0 Cb Y1 Cr, YUY2), 0 is LE_YB8CR8YA8CB8 (Cb Y0 Cr Y1, UYVY)
    bool uyvy = !((format >> 16) & 1);
    if (!in_w || !in_h || !out_w || !out_h || !pitch) return;
    const uint8_t* src = mem::Physical(base + offset);
    for (int y = 0; y < out_h; y++) {
        int dy = out_y + y;
        if (dy < 0 || dy >= kHeight) continue;
        const uint8_t* row = src + size_t(y * in_h / out_h) * pitch;
        for (int x = 0; x < out_w; x++) {
            int dx = out_x + x;
            if (dx < 0 || dx >= kWidth) continue;
            int sx = x * in_w / out_w;
            const uint8_t* p = row + (sx & ~1) * 2;
            int y0 = uyvy ? p[1] : p[0], u = uyvy ? p[0] : p[1], y1 = uyvy ? p[3] : p[2], v = uyvy ? p[2] : p[3];
            g_frame[dy * kWidth + dx] = YuvToRgb((sx & 1) ? y1 : y0, u, v);
        }
    }
}

void Compose() {
    uint32_t start = gpu::DisplayAddress();
    if (start) {
        const uint8_t* fb = mem::Physical(start);
        for (int y = 0; y < kHeight; y++) std::memcpy(&g_frame[y * kWidth], fb + size_t(y) * kWidth * 4, kWidth * 4);
    } else {
        std::fill(g_frame.begin(), g_frame.end(), 0xFF000000u);
    }
    ComposeOverlay();
    // the RAMDAC maps every pixel through the gamma ramp on its way out (the game's brightness setting)
    static uint8_t ramp[256][3];
    if (gpu::GammaRamp(ramp))
        for (uint32_t& p : g_frame)
            p = 0xFF000000u | (uint32_t(ramp[(p >> 16) & 255][0]) << 16) | (uint32_t(ramp[(p >> 8) & 255][1]) << 8) |
                ramp[p & 255][2];
}

void DumpFrame() {
    char name[64];
    snprintf(name, sizeof name, "frame_%05llu.bmp", (unsigned long long)g_frames);
    FILE* f = std::fopen(name, "wb");
    if (!f) return;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof ih;
    ih.biWidth = kWidth;
    ih.biHeight = -kHeight;
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof fh + sizeof ih;
    fh.bfSize = fh.bfOffBits + kWidth * kHeight * 4;
    std::fwrite(&fh, sizeof fh, 1, f);
    std::fwrite(&ih, sizeof ih, 1, f);
    std::fwrite(g_frame.data(), 4, g_frame.size(), f);
    std::fclose(f);
}

// The 4:3 picture keeps its shape in any window: letterboxed, scaled by the GPU with bilinear filtering
RECT Letterbox(int width, int height) {
    int w = width, h = height;
    if (int64_t(w) * kHeight > int64_t(h) * kWidth) w = int(int64_t(h) * kWidth / kHeight);
    else h = int(int64_t(w) * kHeight / kWidth);
    int x = (width - w) / 2, y = (height - h) / 2;
    return {x, y, x + w, y + h};
}

// The frame goes to a flip-model swap chain of the window's size, drawn as a textured triangle (GDI's
// StretchDIBits took ~30 % of a core). Own device: the renderer's context belongs to the pusher thread.
struct Swap {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> chain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> frame;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> frame_srv;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler;
    int width = 0, height = 0;  // of the back buffers
} g_swap;

bool g_resized = false;     // the client area changed: the back buffers follow before the next frame
// mouse look: raw relative motion, collected while the window holds the mouse (FCI_NO_MOUSE=1: never)
const bool g_mouse_enabled = GetEnvironmentVariableA("FCI_NO_MOUSE", nullptr, 0) == 0;
std::atomic<bool> g_mouse_captured{false};
std::atomic<int> g_mouse_dx{0}, g_mouse_dy{0};
std::atomic<int> g_wheel{0};  // PC comfort features: wheel delta (WHEEL_DELTA a notch) not yet taken

void CaptureMouse(bool on) {
    if (!g_mouse_enabled || on == g_mouse_captured) return;
    if (on && app::MenuOpen()) return;  // settings menu: the pointer is the menu's
    g_mouse_captured = on;
    if (on) {
        RECT rc;
        GetClientRect(g_window, &rc);
        MapWindowPoints(g_window, nullptr, reinterpret_cast<POINT*>(&rc), 2);
        ClipCursor(&rc);
        g_mouse_dx = 0, g_mouse_dy = 0;
    } else {
        ClipCursor(nullptr);
    }
}
bool g_fullscreen = false;  // borderless over the whole monitor
WINDOWPLACEMENT g_windowed{sizeof(WINDOWPLACEMENT)};
// ---- display output options (app/output.h) ----
bool g_have_placement = false;  // g_windowed holds the window's last windowed placement
RECT g_windowed_rect{};         // else: where the window goes when it leaves borderless/fullscreen
std::atomic<bool> g_want_exclusive{false};  // display_mode = fullscreen: exclusive wanted (present.cpp acts on it)
std::atomic<bool> g_exclusive_on{false};    // the swap chain is in exclusive fullscreen
bool g_closing = false;
constexpr UINT kExclusiveLeft = WM_APP + 1;  // present.cpp left exclusive fullscreen
constexpr UINT kMenuShown = WM_APP + 2;      // settings menu: opened (wParam 1) or closed
// ---- end display output options ----

constexpr char kShaders[] = R"(
Texture2D frame : register(t0);
SamplerState linear_clamp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs_main(uint id : SV_VertexID) {
    VSOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 ps_main(VSOut i) : SV_Target { return frame.Sample(linear_clamp, i.uv); }
)";

bool ResizeSwap() {
    RECT rc;
    GetClientRect(g_window, &rc);
    int w = std::max<int>(rc.right, 1), h = std::max<int>(rc.bottom, 1);
    g_swap.rtv.Reset();
    HRESULT hr = g_swap.width ? g_swap.chain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0) : S_OK;
    if (FAILED(hr)) return rt::Log("display: ResizeBuffers failed %08X", unsigned(hr)), false;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> back;
    g_swap.chain->GetBuffer(0, IID_PPV_ARGS(&back));
    g_swap.device->CreateRenderTargetView(back.Get(), nullptr, &g_swap.rtv);
    g_swap.width = w, g_swap.height = h;
    return true;
}

bool InitSwap() {
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &level,
                                   1, D3D11_SDK_VERSION, &g_swap.device, nullptr, &g_swap.context);
    if (FAILED(hr)) return rt::Log("display: D3D11CreateDevice failed %08X, using GDI", unsigned(hr)), false;
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    g_swap.device.As(&dxgi);
    dxgi->GetAdapter(&adapter);
    adapter->GetParent(IID_PPV_ARGS(&factory));
    RECT rc;
    GetClientRect(g_window, &rc);
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = std::max<UINT>(rc.right, 1);
    desc.Height = std::max<UINT>(rc.bottom, 1);
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    hr = factory->CreateSwapChainForHwnd(g_swap.device.Get(), g_window, &desc, nullptr, nullptr, &g_swap.chain);
    if (FAILED(hr)) return rt::Log("display: CreateSwapChainForHwnd failed %08X, using GDI", unsigned(hr)), false;
    factory->MakeWindowAssociation(g_window, DXGI_MWA_NO_ALT_ENTER);  // our own borderless toggle instead

    D3D11_TEXTURE2D_DESC td{};
    td.Width = kWidth, td.Height = kHeight, td.MipLevels = 1, td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    g_swap.device->CreateTexture2D(&td, nullptr, &g_swap.frame);
    g_swap.device->CreateShaderResourceView(g_swap.frame.Get(), nullptr, &g_swap.frame_srv);
    Microsoft::WRL::ComPtr<ID3DBlob> vs, ps, errors;
    if (FAILED(gpu::render::CompileHlsl(kShaders, sizeof kShaders - 1, "display", nullptr, nullptr, "vs_main", "vs_5_0",
                                        0, 0, &vs, &errors)) ||
        FAILED(gpu::render::CompileHlsl(kShaders, sizeof kShaders - 1, "display", nullptr, nullptr, "ps_main", "ps_5_0",
                                        0, 0, &ps, &errors))) {
        rt::Log("display: shader compile failed: %s, using GDI",
                errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        g_swap = {};
        return false;
    }
    g_swap.device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_swap.vs);
    g_swap.device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_swap.ps);
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    g_swap.device->CreateSamplerState(&sd, &g_swap.sampler);
    return ResizeSwap();
}

void Present() {
    if (g_resized) {
        g_resized = false;
        ResizeSwap();
    }
    ID3D11DeviceContext* c = g_swap.context.Get();
    c->UpdateSubresource(g_swap.frame.Get(), 0, nullptr, g_frame.data(), kWidth * 4, 0);
    const float black[4] = {0, 0, 0, 1};
    c->ClearRenderTargetView(g_swap.rtv.Get(), black);
    RECT box = Letterbox(g_swap.width, g_swap.height);
    D3D11_VIEWPORT vp{float(box.left), float(box.top), float(box.right - box.left), float(box.bottom - box.top), 0, 1};
    c->RSSetViewports(1, &vp);
    c->OMSetRenderTargets(1, g_swap.rtv.GetAddressOf(), nullptr);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->IASetInputLayout(nullptr);
    c->VSSetShader(g_swap.vs.Get(), nullptr, 0);
    c->PSSetShader(g_swap.ps.Get(), nullptr, 0);
    c->PSSetShaderResources(0, 1, g_swap.frame_srv.GetAddressOf());
    c->PSSetSamplers(0, 1, g_swap.sampler.GetAddressOf());
    c->Draw(3, 0);
    // paced by the guest's vblank, not the host's
    HRESULT hr = g_swap.chain->Present(0, 0);
    if (FAILED(hr)) {
        rt::Log("display: Present failed %08X, using GDI", unsigned(hr));
        g_swap = {};
    }
}

void Show() {
    if (g_swap.chain) return Present();
    HDC dc = GetDC(g_window);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = kWidth;
    bi.bmiHeader.biHeight = -kHeight;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    RECT rc;
    GetClientRect(g_window, &rc);
    RECT box = Letterbox(rc.right, rc.bottom);
    SetStretchBltMode(dc, HALFTONE);
    StretchDIBits(dc, box.left, box.top, box.right - box.left, box.bottom - box.top, 0, 0, kWidth, kHeight,
                  g_frame.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(g_window, dc);
}

// F11 or Alt+Enter: borderless over the monitor the window is on, and back
void ToggleFullscreen() {
    g_fullscreen = !g_fullscreen;
    if (g_fullscreen) {
        g_have_placement = GetWindowPlacement(g_window, &g_windowed) != 0;
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(g_window, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(g_window, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(g_window, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
        CaptureMouse(true);
    } else {
        CaptureMouse(false);
        SetWindowLongW(g_window, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        // display output options: a window that started borderless/fullscreen goes to its windowed rectangle
        if (!g_have_placement) {
            const RECT& r = g_windowed_rect;
            SetWindowPos(g_window, HWND_NOTOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top,
                         SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
            return;
        }
        SetWindowPlacement(g_window, &g_windowed);
        SetWindowPos(g_window, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    }
}

// ---- display output options: F11/Alt+Enter; with display_mode = fullscreen exclusive and windowed ----
void ToggleDisplayMode() {
    if (output::Get().mode != output::Mode::Fullscreen || !g_hle_output) return ToggleFullscreen();
    if (g_want_exclusive) {
        g_want_exclusive = false;  // present.cpp leaves it and sends kExclusiveLeft, then the window goes windowed
        rt::Log("display: exclusive fullscreen off (F11)");
        if (!g_exclusive_on && g_fullscreen) ToggleFullscreen();
    } else {
        if (!g_fullscreen) ToggleFullscreen();  // borderless over the monitor first, DXGI takes it from there
        g_want_exclusive = true;
        rt::Log("display: exclusive fullscreen on (F11)");
    }
}

// Tests of the display modes: FCI_TEST_ALTTAB=<s>[,<s>...] presses Alt+Tab at those seconds after start and again
// 3 s later (the second one goes back to the game), FCI_TEST_CLOSE=<s> closes the window
void SendAltTab() {
    INPUT in[4] = {};
    for (INPUT& i : in) i.type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_MENU, in[1].ki.wVk = VK_TAB, in[2].ki.wVk = VK_TAB, in[3].ki.wVk = VK_MENU;
    in[2].ki.dwFlags = in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    UINT sent = SendInput(4, in, sizeof(INPUT));
    rt::Log("display test: Alt+Tab (%u events), foreground %s", sent, GetForegroundWindow() == g_window ? "game" : "other");
}

void TestThread(std::string alttab, int close_at, int f11_at) {
    auto start = GetTickCount64();
    auto wait_until = [&](double s) {
        while (GetTickCount64() - start < uint64_t(s * 1000)) Sleep(50);
    };
    if (f11_at > 0) {  // FCI_TEST_F11=<s>: F11 then, and again 5 s later
        for (int i = 0; i < 2; i++) {
            wait_until(f11_at + 5.0 * i);
            rt::Log("display test: F11");
            PostMessageW(g_window, WM_KEYDOWN, VK_F11, 0);
            Sleep(2500);
            RECT rc{};
            GetClientRect(g_window, &rc);
            rt::Log("display test: after F11 style %s, client %ldx%ld",
                    (GetWindowLongW(g_window, GWL_STYLE) & WS_CAPTION) == WS_CAPTION ? "windowed" : "popup", rc.right,
                    rc.bottom);
        }
    }
    for (size_t pos = 0; !alttab.empty() && pos != std::string::npos;) {
        double at = std::atof(alttab.c_str() + pos);
        wait_until(at);
        SendAltTab();
        Sleep(3000);
        rt::Log("display test: after 3 s foreground %s, iconic %d", GetForegroundWindow() == g_window ? "game" : "other",
                int(IsIconic(g_window)));
        SendAltTab();
        Sleep(1500);
        if (GetForegroundWindow() != g_window) {  // Alt+Tab went elsewhere: bring the game back
            ShowWindow(g_window, SW_RESTORE);
            SetForegroundWindow(g_window);
        }
        rt::Log("display test: back, foreground %s, iconic %d", GetForegroundWindow() == g_window ? "game" : "other",
                int(IsIconic(g_window)));
        pos = alttab.find(',', pos);
        if (pos != std::string::npos) pos++;
    }
    if (close_at > 0) {
        wait_until(close_at);
        rt::Log("display test: closing the window");
        PostMessageW(g_window, WM_CLOSE, 0, 0);
    }
}

// the window waits (handling its messages, which DXGI sends) for present.cpp to give the display mode back
void WaitExclusiveOff(int ms) {
    for (int i = 0; i < ms / 50 && g_exclusive_on; i++) {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

void LeaveExclusiveForExit() {
    if (!g_exclusive_on || g_closing) return;
    g_closing = true;
    g_want_exclusive = false;
    WaitExclusiveOff(2000);
    rt::Log("display: exclusive fullscreen %s before exit", g_exclusive_on ? "still on (left to Windows)" : "left");
}
// ---- end display output options ----

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // ---- settings menu: F10 and, while it is open, the keyboard and mouse go to the menu ----
    LRESULT menu_result = 0;
    if (app::MenuWndProc(hwnd, msg, wp, lp, &menu_result)) return menu_result;
    // ---- end settings menu ----
    switch (msg) {
    case WM_CLOSE:
        rt::Log("window closed");
        hle::host::StopRumble();
        LeaveExclusiveForExit();  // display output options
        rt::FlushLog();
        ExitProcess(0);
    // ---- display output options ----
    case kExclusiveLeft:
        // F11 left exclusive fullscreen: on to windowed; after focus loss the window waits minimized and goes
        // fullscreen again once it is in front (present.cpp)
        if (!g_want_exclusive && !g_closing && g_fullscreen) ToggleFullscreen();
        else if (g_want_exclusive && !g_closing && GetForegroundWindow() != hwnd && !IsIconic(hwnd))
            ShowWindow(hwnd, SW_MINIMIZE);
        return 0;
    case WM_DPICHANGED:
        if (!g_fullscreen) {  // windowed: the size Windows suggests for the new monitor's DPI
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    // ---- end display output options ----
    case WM_SIZE:
        g_resized = true;
        if (g_mouse_captured) CaptureMouse(false), CaptureMouse(true);  // the clip follows the client area
        break;
    case WM_KEYDOWN:
        if (wp == VK_F11) return ToggleDisplayMode(), 0;
        break;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && (lp & (1 << 29))) return ToggleDisplayMode(), 0;  // Alt held
        break;
    // ---- settings menu: the pointer shows while it is open; borderless takes the mouse again after it ----
    case kMenuShown:
        if (wp) CaptureMouse(false);
        else if (g_fullscreen && GetForegroundWindow() == hwnd) CaptureMouse(true);
        SetCursor(wp || !(g_fullscreen || g_mouse_captured) ? LoadCursor(nullptr, IDC_ARROW) : nullptr);
        return 0;
    // ---- end settings menu ----
    case WM_SETCURSOR:
        if ((g_fullscreen || g_mouse_captured) && LOWORD(lp) == HTCLIENT && !app::MenuOpen()) return SetCursor(nullptr), TRUE;
        break;
    case WM_LBUTTONDOWN:
        CaptureMouse(true);
        break;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) CaptureMouse(false);
        // display output options: borderless/fullscreen take the mouse again on return (as F11 does)
        else if (g_fullscreen && !HIWORD(wp)) CaptureMouse(true);
        if (g_want_exclusive || g_exclusive_on)
            rt::Log("display: window %s (exclusive fullscreen %s)", LOWORD(wp) == WA_INACTIVE ? "inactive" : "active",
                    g_exclusive_on ? "on" : "off");
        // exclusive fullscreen gives the desktop its mode back before a focus pause stops the frames
        if (LOWORD(wp) == WA_INACTIVE && g_exclusive_on && !g_closing) WaitExclusiveOff(1000);
        // ---- PC comfort features: pause on focus loss (app/focus_pause.cpp); minimized counts as inactive ----
        app::WindowActivated(LOWORD(wp) != WA_INACTIVE && !HIWORD(wp));
        // ---- end PC comfort features ----
        break;
    case WM_INPUT: {
        RAWINPUT input;
        UINT size = sizeof input;
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER)) !=
                UINT(-1) &&
            input.header.dwType == RIM_TYPEMOUSE && !(input.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) &&
            g_mouse_captured) {
            g_mouse_dx += input.data.mouse.lLastX;
            g_mouse_dy += input.data.mouse.lLastY;
        }
        // ---- PC comfort features: wheel notches for the bindings (hle/pc_input.cpp), focused window only ----
        if (input.header.dwType == RIM_TYPEMOUSE && (input.data.mouse.usButtonFlags & RI_MOUSE_WHEEL) &&
            GetForegroundWindow() == hwnd)
            g_wheel += SHORT(input.data.mouse.usButtonData);
        // ---- end PC comfort features ----
        break;
    }
    case WM_ERASEBKGND:
        return 1;  // every frame fills the client area
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- relaunch handover (kernel::Relaunch): the previous instance keeps its window until this one has frames ----
HANDLE g_handover = nullptr;  // the previous instance's event; set (and the window shown) by FinishHandover
std::atomic<bool> g_handing{false};

void DisplayThread() {
    g_handover = OpenEventW(EVENT_MODIFY_STATE, FALSE, kernel::HandoverEvent(GetCurrentProcessId()).c_str());
    g_handing = g_handover != nullptr;
    if (g_handing) {
        rt::Log("display: relaunched, the window opens with the first frames (handover)");
        // fallback: shown after 10 s whatever happens (no frames from the presenter, LLE graphics)
        std::thread([] {
            Sleep(10000);
            FinishHandover();
        }).detach();
    }
    const DWORD visible = g_handing ? 0 : WS_VISIBLE;
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"FarCryInstincts";
    RegisterClassW(&wc);
    // ---- display output options (app/output.h): mode, monitor, resolution ----
    const output::Options& o = output::Get();
    // windowed: the resolution, else twice the console's 480 lines at the monitor's DPI (the size the DPI unaware
    // window had)
    RECT rc{0, 0, LONG(kWidth * kScale * o.dpi_scale + 0.5f), LONG(kHeight * kScale * o.dpi_scale + 0.5f)};
    // PC features: with widescreen the window starts at the widescreen shape
    if (g_hle_output) rc.right = LONG(kHeight * kScale * o.dpi_scale * pcmode::Aspect() + 0.5f);
    if (o.width) rc.right = o.width, rc.bottom = o.height;
    AdjustWindowRectExForDpi(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0, UINT(o.dpi_scale * 96 + 0.5f));
    int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    int wx = o.work_x + std::max(0, (o.work_w - ww) / 2), wy = o.work_y + std::max(0, (o.work_h - wh) / 2);
    g_windowed_rect = {wx, wy, wx + ww, wy + wh};
    bool chosen_place = o.monitor_set || o.width;  // else where Windows puts it, as before
    bool borderless = o.mode != output::Mode::Windowed;
    if (o.mode == output::Mode::Fullscreen && !g_hle_output)
        rt::Log("display: exclusive fullscreen needs the HLE graphics, borderless instead");
    if (borderless)
        g_window = CreateWindowW(wc.lpszClassName, L"Far Cry Instincts", WS_POPUP | visible, o.mon_x, o.mon_y,
                                 o.mon_w, o.mon_h, nullptr, nullptr, wc.hInstance, nullptr);
    else
        g_window = CreateWindowW(wc.lpszClassName, L"Far Cry Instincts", WS_OVERLAPPEDWINDOW | visible,
                                 chosen_place ? wx : CW_USEDEFAULT, chosen_place ? wy : CW_USEDEFAULT, ww, wh, nullptr,
                                 nullptr, wc.hInstance, nullptr);
    g_fullscreen = borderless;
    g_want_exclusive = o.mode == output::Mode::Fullscreen && g_hle_output;
    RECT client{};
    GetClientRect(g_window, &client);
    rt::Log("display: window %s, client %ldx%ld", borderless ? "borderless" : "windowed", client.right, client.bottom);
    char test[128] = {}, close_at[16] = {}, f11[16] = {};
    GetEnvironmentVariableA("FCI_TEST_ALTTAB", test, sizeof test);
    GetEnvironmentVariableA("FCI_TEST_CLOSE", close_at, sizeof close_at);
    GetEnvironmentVariableA("FCI_TEST_F11", f11, sizeof f11);
    if (test[0] || close_at[0] || f11[0])
        std::thread(TestThread, std::string(test), std::atoi(close_at), std::atoi(f11)).detach();
    // ---- end display output options ----
    if (!g_hle_output && !GetEnvironmentVariableA("FCI_GDI_DISPLAY", nullptr, 0)) InitSwap();
    SetEvent(g_window_ready);
    if (g_mouse_enabled) {
        RAWINPUTDEVICE mouse{0x01, 0x02, 0, g_window};  // generic desktop, mouse
        if (!RegisterRawInputDevices(&mouse, 1, sizeof mouse)) rt::Log("display: no raw mouse input");
    }
    for (;;) {
        DWORD r = MsgWaitForMultipleObjects(1, &g_vblank, FALSE, INFINITE, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0 && !g_hle_output) {
            Compose();
            Show();
            g_frames++;
            if (g_dump_every && g_frames % g_dump_every == 0) DumpFrame();
        }
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

}  // namespace

void Init() {
    output::Get();  // display output options: DPI awareness and the options, before the window is made
    char every[16] = {};
    if (GetEnvironmentVariableA("FCI_DUMP_FRAMES", every, sizeof every)) g_dump_every = std::atoi(every);
    g_hle_output = xdk::HleGraphics();
    g_vblank = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_window_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread(DisplayThread).detach();
}

void* Window() {
    WaitForSingleObject(g_window_ready, 10000);
    return g_window.load();
}

void OutputRect(int width, int height, int* x, int* y, int* w, int* h) {
    RECT r = Letterbox(width, height);
    *x = r.left, *y = r.top, *w = r.right - r.left, *h = r.bottom - r.top;
}

int DumpEvery() { return g_dump_every; }

void FinishHandover() {
    if (!g_handing.exchange(false)) return;
    ShowWindow(g_window, SW_SHOW);
    SetForegroundWindow(g_window);
    SetEvent(g_handover);
    CloseHandle(g_handover);
    int budget = 0, used = gpu::present::VideoMemoryUsage(&budget);
    rt::Log("display: handover done, window shown (GPU memory of this process %d MB, budget %d MB)", used, budget);
}

void Vblank() {
    if (g_vblank) SetEvent(g_vblank);
}

bool TakeMouseMotion(int* dx, int* dy) {
    if (!g_mouse_captured) return false;
    *dx = g_mouse_dx.exchange(0), *dy = g_mouse_dy.exchange(0);
    return true;
}

// ---- PC comfort features ----
int TakeWheelNotches() {
    int delta = g_wheel.load(), notches = delta / WHEEL_DELTA;
    if (notches) g_wheel -= notches * WHEEL_DELTA;
    return notches;
}

bool MouseCaptured() { return g_mouse_captured; }

// settings menu
void MenuShown(bool open) {
    if (HWND w = g_window.load()) PostMessageW(w, kMenuShown, open, 0);
}
// ---- end PC comfort features ----

// ---- display output options ----
bool WantExclusive() { return g_want_exclusive; }

void ExclusiveChanged(bool on) {
    g_exclusive_on = on;
    if (!on && g_window) PostMessageW(g_window, kExclusiveLeft, 0, 0);
}
// ---- end display output options ----

}  // namespace display
