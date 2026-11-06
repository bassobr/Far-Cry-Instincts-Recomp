// HLE graphics: the frame goes from the renderer's own target to the window, at render resolution, through SMAA,
// with the video overlay over it and the game's gamma ramp; then letterboxed 4:3 into the window's swap chain.
// (LLE: video/display.cpp shows the guest frame buffer instead.)
// The SMAA passes are adapted from SCDA-XBERecomp (MIT, Copyright (c) 2026 TGP482, Joshhhuaaa), src/gpu/d3d12.cpp,
// here on Direct3D 11; so are the colour blind modes (ColourBlindMatrix, the colour matrix of its blit shader).
#include "gpu/present.h"

#include <windows.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>  // display output options: colour space (IDXGISwapChain3), outputs
#include <d3d11_1.h>  // settings menu: its own device context state

#include <imgui.h>  // settings menu
#include <imgui_impl_dx11.h>

#include <algorithm>
#include <atomic>
#include <cctype>  // PC features: colour_blind
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "app/output.h"  // display output options
#include "app/pcmode.h"
#include "app/settings.h"
#include "gpu/fmv.h"          // PC features: fmv_scale
#include "gpu/fps_overlay.h"  // PC features: fps_counter
#include "gpu/nv2a.h"
#include "gpu/renderer.h"  // performance: ContextLock; CompileHlsl
#include "gpu/smaa_shader.h"
#include "gpu/smaa_textures.h"
#include "runtime/log.h"
#include "runtime/memory.h"
#include "video/display.h"

namespace gpu::present {

namespace {

using Microsoft::WRL::ComPtr;

constexpr uint32_t kGuestWidth = 640, kGuestHeight = 480;

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
HWND g_window = nullptr;
ComPtr<IDXGISwapChain1> g_chain;
ComPtr<ID3D11RenderTargetView> g_back;
int g_back_w = 0, g_back_h = 0;
bool g_failed = false;
// PC features: vsync (swap chain flags, the frame latency waitable object, refreshes per frame)
UINT g_chain_flags = 0;
HANDLE g_waitable = nullptr;
int g_interval = 1;
constexpr UINT kVsyncFlag = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;  // display output options
// performance: without vsync, presents may tear (windowed flip model otherwise waits for the display: 120 fps on a
// 120 Hz monitor however fast a frame is)
bool g_tearing = false;
bool g_resize_pending = false;  // display output options: the buffers follow a fullscreen transition
double g_refresh = 60;          // settings menu: the display's rate and the fps_limit g_interval is for
int g_interval_limit = -1;

// the composed frame at render resolution (frame, overlay)
ComPtr<ID3D11Texture2D> g_out;
ComPtr<ID3D11RenderTargetView> g_out_rtv;
ComPtr<ID3D11ShaderResourceView> g_out_srv;
uint32_t g_out_w = 0, g_out_h = 0;

ComPtr<ID3D11Texture2D> g_overlay;
ComPtr<ID3D11ShaderResourceView> g_overlay_srv;
uint32_t g_overlay_w = 0, g_overlay_h = 0;

ComPtr<ID3D11Texture2D> g_gamma_tex;
ComPtr<ID3D11ShaderResourceView> g_gamma_srv;
uint8_t g_gamma[768];
bool g_gamma_dirty = true, g_gamma_identity = true;

ComPtr<ID3D11VertexShader> g_vs;
ComPtr<ID3D11PixelShader> g_ps_copy, g_ps_gamma;
ComPtr<ID3D11SamplerState> g_linear, g_point;
ComPtr<ID3D11BlendState> g_opaque;
ComPtr<ID3D11RasterizerState> g_raster;
ComPtr<ID3D11DepthStencilState> g_no_depth;
ComPtr<ID3D11Buffer> g_rect_cb;

// SMAA: edges, blend weights, the blended frame; the area and search lookups
struct Smaa {
    bool enabled = false, failed = false;
    uint32_t w = 0, h = 0;
    ComPtr<ID3D11Texture2D> tex[3];
    ComPtr<ID3D11RenderTargetView> rtv[3];
    ComPtr<ID3D11ShaderResourceView> srv[3];
    ComPtr<ID3D11ShaderResourceView> area, search;
    ComPtr<ID3D11VertexShader> vs[3];
    ComPtr<ID3D11PixelShader> ps[3];
    ComPtr<ID3D11Buffer> metrics;
} g_smaa;

// ---- PC features: colour_blind ----
// 0 off, 1 protanopia, 2 deuteranopia, 3 tritanopia. Machado 2009 simulation at full severity, then Fidaner's
// daltonize, which moves the difference the eye loses into channels it still sees (SCDA-XBERecomp, src/gpu/d3d12.cpp).
// Applied once, in the final pass after the gamma ramp, to linear colour (gamma 2.2) and clamped to 0..1 there, as
// SCDA does; in HDR10 before the highlights are stretched, so both outputs get the same colours. White and greys stay.
constexpr float kColourBlindSimulate[3][9] = {
    {0.152286f, 1.052583f, -0.204868f, 0.114503f, 0.786281f, 0.099216f, -0.003882f, -0.048116f, 1.051998f},
    {0.367322f, 0.860646f, -0.227968f, 0.280085f, 0.672501f, 0.047413f, -0.011820f, 0.042940f, 0.968881f},
    {1.255528f, -0.076749f, -0.178779f, -0.078411f, 0.930809f, 0.147602f, 0.004733f, 0.691367f, 0.303900f},
};
constexpr float kColourBlindShift[3][9] = {
    {0, 0, 0, 0.7f, 1, 0, 0.7f, 0, 1},
    {0, 0, 0, 0.7f, 1, 0, 0.7f, 0, 1},
    {1, 0, 0.7f, 0, 1, 0.7f, 0, 0, 0},
};
int g_colour_blind = 0;
float g_colour[3][4] = {};  // rows of the matrix; [0][3] = 1 when on (the shaders' switch)
ComPtr<ID3D11PixelShader> g_ps_gamma_cb;
ComPtr<ID3D11Buffer> g_colour_cb;

// mode 1 to 3: identity + shift * (identity - simulate)
void ColourBlindMatrix(int mode, float m[3][4]) {
    const float* sim = kColourBlindSimulate[mode - 1];
    const float* shift = kColourBlindShift[mode - 1];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) {
            float lost = 0;
            for (int k = 0; k < 3; k++) lost += shift[r * 3 + k] * (float(k == c) - sim[k * 3 + c]);
            m[r][c] = float(r == c) + lost;
        }
    m[0][3] = 1;
}

// ps_gamma with the colour matrix after it (its own source, so ps_gamma and the SDR path stay byte for byte as before)
const char kColourShaders[] = R"(
cbuffer Rect : register(b0) { float4 rect; float4 uv; };
cbuffer Colour : register(b2) { float4 colour[3]; };  // rows of the colour matrix
Texture2D src : register(t0);
Texture2D<float4> lut : register(t1);
SamplerState smp : register(s0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
float4 ps_gamma_cb(V i) : SV_Target {
    float3 c = saturate(src.Sample(smp, i.uv).rgb);
    float3 x = c * 255;
    int3 lo = int3(floor(x)), hi = min(lo + 1, 255);
    float3 f = x - lo;
    float3 a = float3(lut.Load(int3(lo.r, 0, 0)).r, lut.Load(int3(lo.g, 0, 0)).g, lut.Load(int3(lo.b, 0, 0)).b);
    float3 b = float3(lut.Load(int3(hi.r, 0, 0)).r, lut.Load(int3(hi.g, 0, 0)).g, lut.Load(int3(hi.b, 0, 0)).b);
    float3x3 m = float3x3(colour[0].xyz, colour[1].xyz, colour[2].xyz);
    return float4(pow(saturate(mul(m, pow(saturate(lerp(a, b, f)), 2.2))), 1 / 2.2), 1);
}
)";

// reads colour_blind; the matrix (also for HDR10, all zero when off) and the SDR shader
bool InitColourBlind() {
    std::string v = settings::String("colour_blind", "0");
    for (char& ch : v) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    static const char* const kNames[] = {"off", "protanopia", "deuteranopia", "tritanopia"};
    g_colour_blind = std::clamp(std::atoi(v.c_str()), 0, 3);
    for (int i = 1; i < 4; i++)
        if (v == kNames[i]) g_colour_blind = i;
    if (g_colour_blind) ColourBlindMatrix(g_colour_blind, g_colour);
    D3D11_BUFFER_DESC cb{sizeof g_colour, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    D3D11_SUBRESOURCE_DATA init{g_colour, 0, 0};
    if (FAILED(g_device->CreateBuffer(&cb, &init, &g_colour_cb))) return false;
    if (!g_colour_blind) return true;
    ComPtr<ID3DBlob> code, errors;
    if (FAILED(gpu::render::CompileHlsl(kColourShaders, sizeof kColourShaders - 1, "present_colour", nullptr, nullptr,
                                        "ps_gamma_cb", "ps_5_0", 0, 0, &code, &errors)))
        return rt::Log("present: ps_gamma_cb: %s", errors ? (const char*)errors->GetBufferPointer() : "?"), false;
    g_device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_ps_gamma_cb);
    rt::Log("present: colour blind mode %s: %.3f %.3f %.3f / %.3f %.3f %.3f / %.3f %.3f %.3f", kNames[g_colour_blind],
            g_colour[0][0], g_colour[0][1], g_colour[0][2], g_colour[1][0], g_colour[1][1], g_colour[1][2], g_colour[2][0],
            g_colour[2][1], g_colour[2][2]);
    return true;
}

// the same on the CPU for FCI_DUMP_FRAMES (gamma-ramped 8 bit in, 8 bit out)
void ColourBlindPixel(uint32_t& r, uint32_t& g, uint32_t& b) {
    static float lin[256];
    static bool init = false;
    if (!init) {
        for (int i = 0; i < 256; i++) lin[i] = std::pow(i / 255.0f, 2.2f);
        init = true;
    }
    float in[3] = {lin[r], lin[g], lin[b]}, out[3];
    for (int k = 0; k < 3; k++) {
        float v = g_colour[k][0] * in[0] + g_colour[k][1] * in[1] + g_colour[k][2] * in[2];
        out[k] = std::pow(std::clamp(v, 0.0f, 1.0f), 1 / 2.2f) * 255 + 0.5f;
    }
    r = uint32_t(out[0]), g = uint32_t(out[1]), b = uint32_t(out[2]);
}
// ---- end PC features ----

uint64_t g_presents = 0;
uint64_t g_last_dump = 0;
const auto g_start = std::chrono::steady_clock::now();

const char kShaders[] = R"(
cbuffer Rect : register(b0) { float4 rect; float4 uv; };  // NDC x0 y0 x1 y1, source u0 v0 u1 v1
Texture2D src : register(t0);
Texture2D<float4> lut : register(t1);
SamplerState smp : register(s0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs_main(uint id : SV_VertexID) {
    float2 t = float2(id & 1, id >> 1);
    V o;
    o.pos = float4(lerp(rect.xy, rect.zw, t), 0, 1);
    o.uv = lerp(uv.xy, uv.zw, t);
    return o;
}
float4 ps_copy(V i) : SV_Target { return float4(src.Sample(smp, i.uv).rgb, 1); }
float4 ps_gamma(V i) : SV_Target {
    float3 c = saturate(src.Sample(smp, i.uv).rgb);
    float3 x = c * 255;
    int3 lo = int3(floor(x)), hi = min(lo + 1, 255);
    float3 f = x - lo;
    float3 a = float3(lut.Load(int3(lo.r, 0, 0)).r, lut.Load(int3(lo.g, 0, 0)).g, lut.Load(int3(lo.b, 0, 0)).b);
    float3 b = float3(lut.Load(int3(hi.r, 0, 0)).r, lut.Load(int3(hi.g, 0, 0)).g, lut.Load(int3(hi.b, 0, 0)).b);
    return float4(lerp(a, b, f), 1);
}
)";

bool InitPipeline() {
    ComPtr<ID3DBlob> code, errors;
    auto compile = [&](const char* entry, const char* profile) -> bool {
        code.Reset(), errors.Reset();
        HRESULT hr = gpu::render::CompileHlsl(kShaders, sizeof kShaders - 1, "present", nullptr, nullptr, entry,
                                              profile, 0, 0, &code, &errors);
        if (FAILED(hr))
            rt::Log("present: %s: %s", entry, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        return SUCCEEDED(hr);
    };
    if (!compile("vs_main", "vs_5_0")) return false;
    g_device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_vs);
    if (!compile("ps_copy", "ps_5_0")) return false;
    g_device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_ps_copy);
    if (!compile("ps_gamma", "ps_5_0")) return false;
    g_device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_ps_gamma);
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    g_device->CreateSamplerState(&sd, &g_linear);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    g_device->CreateSamplerState(&sd, &g_point);
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g_device->CreateBlendState(&bd, &g_opaque);
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID, rd.CullMode = D3D11_CULL_NONE;
    g_device->CreateRasterizerState(&rd, &g_raster);
    D3D11_DEPTH_STENCIL_DESC dd{};
    g_device->CreateDepthStencilState(&dd, &g_no_depth);
    D3D11_BUFFER_DESC cb{32, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    g_device->CreateBuffer(&cb, nullptr, &g_rect_cb);
    D3D11_TEXTURE2D_DESC td{256, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                            D3D11_BIND_SHADER_RESOURCE, 0, 0};
    g_device->CreateTexture2D(&td, nullptr, &g_gamma_tex);
    g_device->CreateShaderResourceView(g_gamma_tex.Get(), nullptr, &g_gamma_srv);
    for (int i = 0; i < 256; i++) g_gamma[i] = g_gamma[256 + i] = g_gamma[512 + i] = uint8_t(i);
    return true;
}


const char* kSmaaConfig = R"(
cbuffer P : register(b1) { float4 metrics; };
#define SMAA_RT_METRICS metrics
#define SMAA_CUSTOM_SL
#define SMAA_THRESHOLD 0.05
#define SMAA_MAX_SEARCH_STEPS 16
#define SMAA_MAX_SEARCH_STEPS_DIAG 8
#define SMAA_CORNER_ROUNDING 25
SamplerState LinearSampler : register(s0);
SamplerState PointSampler : register(s1);
#define SMAATexture2D(tex) Texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(LinearSampler, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(PointSampler, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(LinearSampler, coord, 0, offset)
#define SMAASample(tex, coord) tex.Sample(LinearSampler, coord)
#define SMAASamplePoint(tex, coord) tex.Sample(PointSampler, coord)
#define SMAASampleOffset(tex, coord, offset) tex.Sample(LinearSampler, coord, offset)
#define SMAAGather(tex, coord) tex.Gather(LinearSampler, coord, 0)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
)";

// after kSmaaHlsl: edges from the frame's luma, blend weights from edges, area and search, then the frame blended
const char* kSmaaPasses = R"(
Texture2D tex0 : register(t0);
Texture2D tex1 : register(t1);
Texture2D tex2 : register(t2);
struct SmaaEdgeV { float4 p : SV_Position; float2 uv : TEXCOORD0; float4 o[3] : TEXCOORD1; };
struct SmaaWeightV { float4 p : SV_Position; float2 uv : TEXCOORD0; float2 pix : TEXCOORD1; float4 o[3] : TEXCOORD2; };
struct SmaaBlendV { float4 p : SV_Position; float2 uv : TEXCOORD0; float4 o : TEXCOORD1; };
float4 corner(uint id, out float2 uv) {
    uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
SmaaEdgeV vs_edges(uint id : SV_VertexID) {
    SmaaEdgeV o;
    o.p = corner(id, o.uv);
    SMAAEdgeDetectionVS(o.uv, o.o);
    return o;
}
SmaaWeightV vs_weights(uint id : SV_VertexID) {
    SmaaWeightV o;
    o.p = corner(id, o.uv);
    SMAABlendingWeightCalculationVS(o.uv, o.pix, o.o);
    return o;
}
SmaaBlendV vs_blend(uint id : SV_VertexID) {
    SmaaBlendV o;
    o.p = corner(id, o.uv);
    SMAANeighborhoodBlendingVS(o.uv, o.o);
    return o;
}
float4 smaa_edges(SmaaEdgeV i) : SV_Target { return float4(SMAALumaEdgeDetectionPS(i.uv, i.o, tex0), 0, 0); }
float4 smaa_weights(SmaaWeightV i) : SV_Target { return SMAABlendingWeightCalculationPS(i.uv, i.pix, i.o, tex0, tex1, tex2, 0); }
float4 smaa_blend(SmaaBlendV i) : SV_Target { return SMAANeighborhoodBlendingPS(i.uv, i.o, tex0, tex1); }
)";

bool InitSmaa() {
    std::string hlsl = std::string(kSmaaConfig) + gpu::kSmaaHlsl + kSmaaPasses;
    const char* vs[3] = {"vs_edges", "vs_weights", "vs_blend"};
    const char* ps[3] = {"smaa_edges", "smaa_weights", "smaa_blend"};
    for (int i = 0; i < 3; i++) {
        ComPtr<ID3DBlob> code, errors;
        if (FAILED(gpu::render::CompileHlsl(hlsl.data(), hlsl.size(), "smaa", nullptr, nullptr, vs[i], "vs_5_0",
                                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)))
            return rt::Log("present: SMAA %s: %s", vs[i], errors ? (const char*)errors->GetBufferPointer() : "?"), false;
        g_device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_smaa.vs[i]);
        if (FAILED(gpu::render::CompileHlsl(hlsl.data(), hlsl.size(), "smaa", nullptr, nullptr, ps[i], "ps_5_0",
                                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)))
            return rt::Log("present: SMAA %s: %s", ps[i], errors ? (const char*)errors->GetBufferPointer() : "?"), false;
        g_device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_smaa.ps[i]);
    }
    // smaa_textures.h: area 160 x 560 in two channels, search 64 x 16 in one
    static_assert(sizeof(gpu::kSmaaArea) == 160 * 560 * 2 && sizeof(gpu::kSmaaSearch) == 64 * 16);
    auto lookup = [](DXGI_FORMAT fmt, UINT w, UINT h, UINT bpp, const uint8_t* data, ComPtr<ID3D11ShaderResourceView>& srv) {
        D3D11_TEXTURE2D_DESC td{w, h, 1, 1, fmt, {1, 0}, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0};
        D3D11_SUBRESOURCE_DATA init{data, w * bpp, 0};
        ComPtr<ID3D11Texture2D> tex;
        return SUCCEEDED(g_device->CreateTexture2D(&td, &init, &tex)) &&
               SUCCEEDED(g_device->CreateShaderResourceView(tex.Get(), nullptr, &srv));
    };
    if (!lookup(DXGI_FORMAT_R8G8_UNORM, 160, 560, 2, gpu::kSmaaArea, g_smaa.area) ||
        !lookup(DXGI_FORMAT_R8_UNORM, 64, 16, 1, gpu::kSmaaSearch, g_smaa.search))
        return false;
    D3D11_BUFFER_DESC cb{16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    g_device->CreateBuffer(&cb, nullptr, &g_smaa.metrics);
    rt::Log("present: SMAA on");
    return true;
}

bool EnsureSmaaTargets(uint32_t w, uint32_t h) {
    if (g_smaa.tex[2] && g_smaa.w == w && g_smaa.h == h) return true;
    const DXGI_FORMAT f[3] = {DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM};
    for (int i = 0; i < 3; i++) {
        g_smaa.tex[i].Reset(), g_smaa.rtv[i].Reset(), g_smaa.srv[i].Reset();
        D3D11_TEXTURE2D_DESC td{w, h, 1, 1, f[i], {1, 0}, D3D11_USAGE_DEFAULT,
                                D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
        if (FAILED(g_device->CreateTexture2D(&td, nullptr, &g_smaa.tex[i]))) return false;
        g_device->CreateRenderTargetView(g_smaa.tex[i].Get(), nullptr, &g_smaa.rtv[i]);
        g_device->CreateShaderResourceView(g_smaa.tex[i].Get(), nullptr, &g_smaa.srv[i]);
    }
    g_smaa.w = w, g_smaa.h = h;
    return true;
}

void SmaaPass(int i, ID3D11ShaderResourceView* t0, ID3D11ShaderResourceView* t1, ID3D11ShaderResourceView* t2,
              ID3D11RenderTargetView* into = nullptr) {
    ID3D11RenderTargetView* rtv = into ? into : g_smaa.rtv[i].Get();
    g_context->OMSetRenderTargets(1, &rtv, nullptr);
    g_context->VSSetShader(g_smaa.vs[i].Get(), nullptr, 0);
    g_context->PSSetShader(g_smaa.ps[i].Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[3] = {t0, t1, t2};
    g_context->PSSetShaderResources(0, 3, srvs);
    g_context->Draw(3, 0);
    ID3D11ShaderResourceView* none[3] = {};
    g_context->PSSetShaderResources(0, 3, none);
    g_context->OMSetRenderTargets(0, nullptr, nullptr);
}

// SMAA 1x over the frame; the blended frame, or the frame itself when it cannot run (performance: into, when given,
// receives the blended frame directly and null comes back: one copy of the frame less)
ID3D11ShaderResourceView* Smaa(ID3D11ShaderResourceView* frame, uint32_t w, uint32_t h,
                               ID3D11RenderTargetView* into = nullptr) {
    if (!g_smaa.enabled || g_smaa.failed) return frame;
    if (!g_smaa.vs[0] && !InitSmaa()) return g_smaa.failed = true, frame;
    if (!EnsureSmaaTargets(w, h)) return g_smaa.failed = true, frame;
    float metrics[4] = {1.0f / w, 1.0f / h, float(w), float(h)};
    D3D11_MAPPED_SUBRESOURCE map;
    if (SUCCEEDED(g_context->Map(g_smaa.metrics.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        std::memcpy(map.pData, metrics, sizeof metrics);
        g_context->Unmap(g_smaa.metrics.Get(), 0);
    }
    D3D11_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
    g_context->RSSetViewports(1, &vp);
    g_context->OMSetBlendState(g_opaque.Get(), nullptr, 0xFFFFFFFF);
    g_context->OMSetDepthStencilState(g_no_depth.Get(), 0);
    g_context->RSSetState(g_raster.Get());
    g_context->IASetInputLayout(nullptr);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11Buffer* cb = g_smaa.metrics.Get();
    g_context->VSSetConstantBuffers(1, 1, &cb);
    g_context->PSSetConstantBuffers(1, 1, &cb);
    ID3D11SamplerState* samplers[2] = {g_linear.Get(), g_point.Get()};
    g_context->PSSetSamplers(0, 2, samplers);
    const float clear[4] = {};
    g_context->ClearRenderTargetView(g_smaa.rtv[0].Get(), clear);  // edge detection discards where there is none
    g_context->ClearRenderTargetView(g_smaa.rtv[1].Get(), clear);
    SmaaPass(0, frame, nullptr, nullptr);
    SmaaPass(1, g_smaa.srv[0].Get(), g_smaa.area.Get(), g_smaa.search.Get());
    SmaaPass(2, frame, g_smaa.srv[1].Get(), nullptr, into);
    if (into) return nullptr;
    return g_smaa.srv[2].Get();
}

// ---- display output options (app/output.h): HDR10 output, exclusive fullscreen ----
void Quad(ID3D11RenderTargetView* rtv, float vx, float vy, float vw, float vh, const float rect[4], const float uv[4],
          ID3D11ShaderResourceView* src, ID3D11PixelShader* ps, ID3D11SamplerState* smp,
          ID3D11BlendState* blend = nullptr);  // settings menu: the blend (opaque when null)

// HDR10: the back buffers are R10G10B10A2 in PQ over BT.2020 (DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020). The final
// pass takes the SDR picture through the gamma ramp as before, to light as an SDR monitor shows it (gamma 2.2),
// white at the paper white, optionally stretches the highlights towards the peak, and encodes it as PQ. The HDR10
// shader is adapted from SCDA-XBERecomp (MIT, Copyright (c) 2026 TGP482, Joshhhuaaa), src/gpu/d3d12.cpp (kHdrHlsl).
// Without HDR the SDR swap chain, shaders and passes stay exactly as they were.
bool g_hdr = false;                                     // HDR10 back buffers in use
DXGI_FORMAT g_back_format = DXGI_FORMAT_B8G8R8A8_UNORM;
float g_hdr_params[4] = {200, 1000, 1, 0.5f};           // paper white nits, peak nits, gain at white, knee
ComPtr<ID3D11PixelShader> g_ps_hdr;
ComPtr<ID3D11Buffer> g_hdr_cb;
bool g_exclusive_mode = false;                          // display_mode = fullscreen
bool g_exclusive = false;                               // the swap chain is in exclusive fullscreen
int g_exclusive_failures = 0;
std::chrono::steady_clock::time_point g_exclusive_tried{};
int g_probe_every = 0;                                  // FCI_PRESENT_PROBE: back buffer readback every n-th present
bool g_test_pattern = false;                            // FCI_HDR_TEST_PATTERN: known bars instead of the frame
ComPtr<ID3D11ShaderResourceView> g_pattern_srv;

const char kHdrShaders[] = R"(
cbuffer Hdr : register(b1) { float4 hdr; };  // paper white nits, peak nits, highlight gain at white (1 = off), knee
cbuffer Colour : register(b2) { float4 colour[3]; };  // colour_blind: rows of the colour matrix, [0].w = 1 when on
Texture2D src : register(t0);
Texture2D<float4> lut : register(t1);
SamplerState smp : register(s0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
static const float3 kLum = float3(0.2126, 0.7152, 0.0722);
static const float3x3 k709To2020 = {0.6274, 0.3293, 0.0433, 0.0691, 0.9195, 0.0114, 0.0164, 0.0880, 0.8956};
float3 pq(float3 nits) {
    float3 y = pow(saturate(nits / 10000), 0.1593017578125);
    return pow((0.8359375 + 18.8515625 * y) / (1 + 18.6875 * y), 78.84375);
}
float4 ps_hdr(V i) : SV_Target {
    // the gamma ramp as in ps_gamma
    float3 c = saturate(src.Sample(smp, i.uv).rgb);
    float3 x = c * 255;
    int3 lo = int3(floor(x)), hi = min(lo + 1, 255);
    float3 f = x - lo;
    float3 a = float3(lut.Load(int3(lo.r, 0, 0)).r, lut.Load(int3(lo.g, 0, 0)).g, lut.Load(int3(lo.b, 0, 0)).b);
    float3 b = float3(lut.Load(int3(hi.r, 0, 0)).r, lut.Load(int3(hi.g, 0, 0)).g, lut.Load(int3(hi.b, 0, 0)).b);
    // gamma 2.2 as SDR monitors show it
    float3 lin = pow(saturate(lerp(a, b, f)), 2.2);
    // colour_blind as in ps_gamma_cb: on linear colour, clamped to SDR's 0..1, before the highlights are stretched
    if (colour[0].w > 0) lin = saturate(mul(float3x3(colour[0].xyz, colour[1].xyz, colour[2].xyz), lin));
    // hdr_boost: above the knee the luminance bends up to gain x at white, colours keep their hue
    if (hdr.z > 1) {
        float l = dot(lin, kLum);
        float t = saturate((l - hdr.w) / (1 - hdr.w));
        lin *= (l + (hdr.z - 1) * t * t) / max(l, 1e-6);
    }
    return float4(pq(min(mul(k709To2020, lin) * hdr.x, hdr.y)), 1);
}
// settings menu: its premultiplied SDR layer over the HDR10 picture, white at the paper white
float4 ps_ui_hdr(V i) : SV_Target {
    float4 c = src.Sample(smp, i.uv);
    if (c.a <= 0) discard;
    float3 lin = pow(saturate(c.rgb / c.a), 2.2);
    return float4(pq(mul(k709To2020, lin) * hdr.x) * c.a, c.a);
}
)";

ComPtr<ID3D11PixelShader> g_ps_ui_hdr;  // settings menu
float g_display_white = 0, g_display_peak = 0;  // settings menu: what Windows reports, for SetHdrLevels

bool InitHdrShader() {
    ComPtr<ID3DBlob> code, errors;
    if (FAILED(gpu::render::CompileHlsl(kHdrShaders, sizeof kHdrShaders - 1, "present_hdr", nullptr, nullptr, "ps_hdr",
                                        "ps_5_0", 0, 0, &code, &errors)))
        return rt::Log("present: ps_hdr: %s", errors ? (const char*)errors->GetBufferPointer() : "?"), false;
    D3D11_BUFFER_DESC cb{16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    D3D11_SUBRESOURCE_DATA init{g_hdr_params, 0, 0};
    bool ok = SUCCEEDED(g_device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_ps_hdr)) &&
              SUCCEEDED(g_device->CreateBuffer(&cb, &init, &g_hdr_cb));
    // settings menu: the panel over the HDR10 picture
    code.Reset(), errors.Reset();
    if (SUCCEEDED(gpu::render::CompileHlsl(kHdrShaders, sizeof kHdrShaders - 1, "present_hdr", nullptr, nullptr,
                                           "ps_ui_hdr", "ps_5_0", 0, 0, &code, &errors)))
        g_device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_ps_ui_hdr);
    return ok;
}

const char* ColorSpaceName(DXGI_COLOR_SPACE_TYPE cs) {
    return cs == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ? "G2084/P2020 (HDR10)"
           : cs == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709  ? "G22/P709 (SDR)"
           : cs == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709  ? "G10/P709 (scRGB)"
                                                            : "other";
}

// whether HDR10 output goes on (hdr = on, or auto with Windows HDR on the window's monitor); the nits it uses
bool DecideHdr() {
    const output::Options& o = output::Get();
    HMONITOR mon = MonitorFromWindow(g_window, MONITOR_DEFAULTTOPRIMARY);
    output::HdrInfo info = output::QueryHdr(mon);
    rt::Log("present: display HDR %s (DXGI output %s, %u bits, peak %.0f nits, full frame %.0f, min %.4f, Windows SDR "
            "white %.0f nits)",
            info.enabled ? "on" : "off", info.found ? "found" : "not found", info.bits, info.max_nits,
            info.max_full_frame_nits, info.min_nits, info.sdr_white_nits);
    bool want = o.hdr == output::Hdr::On || (o.hdr == output::Hdr::Auto && info.enabled);
    if (!want) return false;
    if (!info.enabled) rt::Log("present: hdr = on although Windows HDR is off for this monitor");
    g_display_white = info.sdr_white_nits, g_display_peak = info.max_nits;  // settings menu
    float paper = o.paper_white ? o.paper_white : info.sdr_white_nits > 0 ? info.sdr_white_nits : 200.0f;
    float peak = o.peak ? o.peak : info.max_nits > 0 ? info.max_nits : 1000.0f;
    peak = std::max(peak, paper);
    g_hdr_params[0] = paper, g_hdr_params[1] = peak;
    g_hdr_params[2] = 1 + o.boost * (peak / paper - 1);  // hdr_boost 1: white reaches the peak
    return true;
}

// the swap chain's colour space; false when it cannot present in it
bool SetColorSpace(DXGI_COLOR_SPACE_TYPE cs) {
    ComPtr<IDXGISwapChain3> chain3;
    UINT support = 0;
    if (FAILED(g_chain.As(&chain3)) || FAILED(chain3->CheckColorSpaceSupport(cs, &support))) return false;
    rt::Log("present: CheckColorSpaceSupport %s: %s%s", ColorSpaceName(cs),
            support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT ? "present" : "-",
            support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_OVERLAY_PRESENT ? ", overlay" : "");
    if (!(support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)) return false;
    HRESULT hr = chain3->SetColorSpace1(cs);
    if (FAILED(hr)) rt::Log("present: SetColorSpace1 failed %08X", unsigned(hr));
    return SUCCEEDED(hr);
}

void LogDisplayMode(const char* when) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof mi;
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    if (GetMonitorInfoW(MonitorFromWindow(g_window, MONITOR_DEFAULTTOPRIMARY), &mi) &&
        EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm))
        rt::Log("present: %s: display mode %lux%lu %lu Hz", when, dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency);
}

// exclusive fullscreen at the resolution's mode (else the desktop's) on the output the window is on
void EnterExclusive() {
    const output::Options& o = output::Get();
    g_exclusive_tried = std::chrono::steady_clock::now();
    ComPtr<IDXGIOutput> out;
    if (FAILED(g_chain->GetContainingOutput(&out))) {
        rt::Log("present: exclusive fullscreen: no output");
        g_exclusive_failures++;
        return;
    }
    DXGI_OUTPUT_DESC od{};
    out->GetDesc(&od);
    int want_w = o.width ? o.width : od.DesktopCoordinates.right - od.DesktopCoordinates.left;
    int want_h = o.height ? o.height : od.DesktopCoordinates.bottom - od.DesktopCoordinates.top;
    UINT n = 0;
    out->GetDisplayModeList(g_back_format, 0, &n, nullptr);
    std::vector<DXGI_MODE_DESC> modes(n);
    if (n) out->GetDisplayModeList(g_back_format, 0, &n, modes.data());
    // that size, at the desktop's refresh rate when it has it, else the highest
    DXGI_MODE_DESC mode{};
    double best = -1;
    for (const DXGI_MODE_DESC& m : modes) {
        if (int(m.Width) != want_w || int(m.Height) != want_h || !m.RefreshRate.Denominator) continue;
        double hz = double(m.RefreshRate.Numerator) / m.RefreshRate.Denominator;
        double score = o.desktop_hz && std::fabs(hz - o.desktop_hz) < 0.5 ? 1e6 + hz : hz;
        if (score > best) best = score, mode = m;
    }
    if (best < 0) {
        DXGI_MODE_DESC ask{UINT(want_w), UINT(want_h), {0, 0}, g_back_format};
        if (FAILED(out->FindClosestMatchingMode(&ask, &mode, g_device))) mode = ask;
        rt::Log("present: exclusive fullscreen: no %dx%d mode (%u modes), closest %ux%u", want_w, want_h, n, mode.Width,
                mode.Height);
    }
    g_back.Reset(), g_resize_pending = true;
    g_context->OMSetRenderTargets(0, nullptr, nullptr);
    g_context->Flush();
    // as DXGI's best practices: the target to the mode, fullscreen, then the target again without refresh rate
    g_chain->ResizeTarget(&mode);
    HRESULT hr = g_chain->SetFullscreenState(TRUE, out.Get());
    if (FAILED(hr)) {
        rt::Log("present: SetFullscreenState(TRUE) failed %08X, borderless instead", unsigned(hr));
        g_exclusive_failures++;
        return;
    }
    DXGI_MODE_DESC again = mode;
    again.RefreshRate = {0, 0};
    g_chain->ResizeTarget(&again);
    BOOL fs = FALSE;
    g_chain->GetFullscreenState(&fs, nullptr);
    g_exclusive = fs != FALSE;
    display::ExclusiveChanged(g_exclusive);
    rt::Log("present: exclusive fullscreen %s on %ls: %ux%u %.3f Hz (%u modes of the format)",
            g_exclusive ? "on" : "NOT on", od.DeviceName, mode.Width, mode.Height,
            mode.RefreshRate.Denominator ? double(mode.RefreshRate.Numerator) / mode.RefreshRate.Denominator : 0.0, n);
    LogDisplayMode("entered");
}

// once a frame: follows display::WantExclusive(); DXGI leaves on its own when the window loses the focus (alt-tab)
void UpdateExclusive() {
    if (!g_exclusive_mode) return;
    BOOL fs = FALSE;
    g_chain->GetFullscreenState(&fs, nullptr);
    if ((fs != FALSE) != g_exclusive) {
        g_exclusive = fs != FALSE;
        g_back.Reset(), g_resize_pending = true;
        rt::Log("present: exclusive fullscreen %s by the system", g_exclusive ? "entered" : "left");
        display::ExclusiveChanged(g_exclusive);
        LogDisplayMode(g_exclusive ? "entered" : "left");
    }
    // another window in front (alt-tab): the desktop gets its mode back, the window waits minimized (display.cpp)
    bool front = GetForegroundWindow() == g_window;
    bool want = display::WantExclusive() && (front || !g_exclusive);
    if (g_exclusive && !front) rt::Log("present: the window lost the focus");
    if (want && !g_exclusive) {
        // back once the window is in front again; not more than once a second, given up after 3 failures
        if (g_exclusive_failures >= 3 || GetForegroundWindow() != g_window || IsIconic(g_window)) return;
        if (std::chrono::steady_clock::now() - g_exclusive_tried < std::chrono::seconds(1)) return;
        EnterExclusive();
    } else if (!want && g_exclusive) {
        g_back.Reset(), g_resize_pending = true;
        g_context->OMSetRenderTargets(0, nullptr, nullptr);
        HRESULT hr = g_chain->SetFullscreenState(FALSE, nullptr);
        g_exclusive = false;
        rt::Log("present: exclusive fullscreen off (%08X)", unsigned(hr));
        LogDisplayMode("left");
        display::ExclusiveChanged(false);
    }
}

// FCI_HDR_TEST_PATTERN=1: eight bars of known colours instead of the picture (white, 75/50/25 % grey, black, red,
// green, blue), drawn point sampled so every back buffer pixel of a bar has the same input
void DrawTestPattern(uint32_t w, uint32_t h) {
    if (!g_pattern_srv) {
        static const uint32_t bars[8] = {0xFFFFFFFF, 0xFFC0C0C0, 0xFF808080, 0xFF404040,
                                         0xFF000000, 0xFFFF0000, 0xFF00FF00, 0xFF0000FF};  // BGRA in memory
        D3D11_TEXTURE2D_DESC td{8, 1, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_IMMUTABLE,
                                D3D11_BIND_SHADER_RESOURCE, 0, 0};
        D3D11_SUBRESOURCE_DATA init{bars, sizeof bars, 0};
        ComPtr<ID3D11Texture2D> tex;
        if (FAILED(g_device->CreateTexture2D(&td, &init, &tex)) ||
            FAILED(g_device->CreateShaderResourceView(tex.Get(), nullptr, &g_pattern_srv)))
            return;
    }
    const float full[4] = {-1, 1, 1, -1}, uv[4] = {0, 0, 1, 1};
    Quad(g_out_rtv.Get(), 0, 0, float(w), float(h), full, uv, g_pattern_srv.Get(), g_ps_copy.Get(), g_point.Get());
}

// FCI_PRESENT_PROBE=<n>: every n-th present the back buffer read back: format, colour space, fullscreen state and the
// codes at the centres of the eight test bars (x = 1/16, 3/16, ... of the picture) on its middle line
void Probe(float px, float py, float pw, float ph) {
    if (!g_probe_every || g_presents % uint64_t(g_probe_every) != uint64_t(g_probe_every - 1)) return;
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(g_chain->GetBuffer(0, IID_PPV_ARGS(&back)))) return;
    D3D11_TEXTURE2D_DESC bd;
    back->GetDesc(&bd);
    bd.Usage = D3D11_USAGE_STAGING, bd.BindFlags = 0, bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ, bd.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(g_device->CreateTexture2D(&bd, nullptr, &staging))) return;
    g_context->CopyResource(staging.Get(), back.Get());
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(g_context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) return;
    BOOL fs = FALSE;
    g_chain->GetFullscreenState(&fs, nullptr);
    std::string line;
    uint32_t y = std::min(uint32_t(py + ph / 2), bd.Height - 1);
    for (int i = 0; i < 8; i++) {
        uint32_t x = std::min(uint32_t(px + pw * (2 * i + 1) / 16), bd.Width - 1);
        uint32_t p = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(map.pData) + size_t(y) * map.RowPitch)[x];
        char cell[48];
        if (bd.Format == DXGI_FORMAT_R10G10B10A2_UNORM)
            snprintf(cell, sizeof cell, " %u,%u,%u", p & 1023, (p >> 10) & 1023, (p >> 20) & 1023);
        else
            snprintf(cell, sizeof cell, " %u,%u,%u", (p >> 16) & 255, (p >> 8) & 255, p & 255);
        line += cell;
    }
    g_context->Unmap(staging.Get(), 0);
    rt::Log("present: probe %llu: back %ux%u format %u (%s), fullscreen %d, ramp 255->%u,%u,%u 128->%u, rgb:%s",
            (unsigned long long)g_presents, bd.Width, bd.Height, unsigned(bd.Format),
            g_hdr ? "HDR10 PQ/BT.2020" : "SDR", int(fs), g_gamma[255], g_gamma[511], g_gamma[767], g_gamma[256 + 128],
            line.c_str());
}

// a texture's pixels through a staging copy, rows packed (4 bytes a pixel)
bool ReadBack(ID3D11Texture2D* tex, std::vector<uint32_t>& px, D3D11_TEXTURE2D_DESC& d) {
    tex->GetDesc(&d);
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING, sd.BindFlags = 0, sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ, sd.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(g_device->CreateTexture2D(&sd, nullptr, &staging))) return false;
    g_context->CopyResource(staging.Get(), tex);
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(g_context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) return false;
    px.resize(size_t(d.Width) * d.Height);
    for (uint32_t y = 0; y < d.Height; y++)
        std::memcpy(&px[size_t(y) * d.Width], static_cast<const uint8_t*>(map.pData) + size_t(y) * map.RowPitch,
                    d.Width * 4);
    g_context->Unmap(staging.Get(), 0);
    return true;
}

// FCI_DUMP_HDR=<n> (checking HDR10 without an HDR display): at every n-th vblank (numbered as FCI_DUMP_FRAMES does)
// the HDR10 back buffer as presented, hdr_<vblank>.raw ("FCIHDR10", then uint32 width, height and the picture's x, y,
// w, h, then width x height R10G10B10A2 words), the same frame as the SDR path draws it (ps_gamma into a BGRA8 target
// of the back buffer's size, same viewport and sampler), sdr_<vblank>.bmp, and the parameters, hdr_<vblank>.txt.
// tools/hdr_check.py decodes and compares them.
void DumpHdr(float px, float py, float pw, float ph) {
    static const int every = [] {
        char v[16] = {};
        return GetEnvironmentVariableA("FCI_DUMP_HDR", v, sizeof v) ? std::max(std::atoi(v), 1) : 0;
    }();
    if (!every || !g_hdr) return;
    static uint64_t last = 0;
    uint64_t vblank = uint64_t(std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count() * 59.94);
    if (vblank / every == last / every) return;
    last = vblank;
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(g_chain->GetBuffer(0, IID_PPV_ARGS(&back)))) return;
    std::vector<uint32_t> hdr, sdr;
    D3D11_TEXTURE2D_DESC bd, sd;
    if (!ReadBack(back.Get(), hdr, bd)) return;
    // the SDR picture of this frame
    D3D11_TEXTURE2D_DESC td{bd.Width, bd.Height, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                            D3D11_BIND_RENDER_TARGET, 0, 0};
    ComPtr<ID3D11Texture2D> ref;
    ComPtr<ID3D11RenderTargetView> ref_rtv;
    if (FAILED(g_device->CreateTexture2D(&td, nullptr, &ref)) ||
        FAILED(g_device->CreateRenderTargetView(ref.Get(), nullptr, &ref_rtv)))
        return;
    const float black[4] = {0, 0, 0, 1}, full[4] = {-1, 1, 1, -1}, uv[4] = {0, 0, 1, 1};
    g_context->ClearRenderTargetView(ref_rtv.Get(), black);
    Quad(ref_rtv.Get(), px, py, pw, ph, full, uv, g_out_srv.Get(), g_colour_blind ? g_ps_gamma_cb.Get() : g_ps_gamma.Get(),
         g_test_pattern ? g_point.Get() : g_linear.Get());
    if (!ReadBack(ref.Get(), sdr, sd)) return;
    char name[64];
    snprintf(name, sizeof name, "hdr_%05llu.raw", (unsigned long long)vblank);
    if (FILE* f = std::fopen(name, "wb")) {
        uint32_t head[6] = {bd.Width, bd.Height, uint32_t(px), uint32_t(py), uint32_t(pw), uint32_t(ph)};
        std::fwrite("FCIHDR10", 1, 8, f);
        std::fwrite(head, 4, 6, f);
        std::fwrite(hdr.data(), 4, hdr.size(), f);
        std::fclose(f);
    }
    snprintf(name, sizeof name, "sdr_%05llu.bmp", (unsigned long long)vblank);
    if (FILE* f = std::fopen(name, "wb")) {
        BITMAPFILEHEADER fh{};
        BITMAPINFOHEADER ih{};
        ih.biSize = sizeof ih, ih.biWidth = LONG(sd.Width), ih.biHeight = -LONG(sd.Height), ih.biPlanes = 1;
        ih.biBitCount = 32;
        fh.bfType = 0x4D42, fh.bfOffBits = sizeof fh + sizeof ih, fh.bfSize = DWORD(fh.bfOffBits + sdr.size() * 4);
        std::fwrite(&fh, sizeof fh, 1, f);
        std::fwrite(&ih, sizeof ih, 1, f);
        std::fwrite(sdr.data(), 4, sdr.size(), f);
        std::fclose(f);
    }
    snprintf(name, sizeof name, "hdr_%05llu.txt", (unsigned long long)vblank);
    if (FILE* f = std::fopen(name, "w")) {
        std::fprintf(f, "paper_white %g\npeak %g\ngain %g\nknee %g\ncolour_blind %d\nformat %u\n", g_hdr_params[0],
                     g_hdr_params[1], g_hdr_params[2], g_hdr_params[3], g_colour_blind, unsigned(bd.Format));
        std::fclose(f);
    }
    rt::Log("present: HDR dump %llu: %ux%u, picture %.0f,%.0f %.0fx%.0f", (unsigned long long)vblank, bd.Width,
            bd.Height, px, py, pw, ph);
}
// ---- end display output options ----

bool InitSwap() {
    g_window = static_cast<HWND>(display::Window());
    if (!g_window) return rt::Log("present: no window"), false;
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(g_device->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&adapter)) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))))
        return rt::Log("present: no DXGI factory"), false;
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
    // PC features: with vsync the frame waits for the display on a waitable object, with the guest CPU free
    if (pcmode::Vsync()) desc.Flags = g_chain_flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    // ---- display output options: HDR10 back buffers; exclusive fullscreen may change the display mode ----
    g_exclusive_mode = output::Get().mode == output::Mode::Fullscreen;
    if (g_exclusive_mode) desc.Flags = g_chain_flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    // ---- performance: unlimited frame rate without vsync (the frame limiter is fps_limit, d3d8.cpp) ----
    if (!pcmode::Vsync()) {
        ComPtr<IDXGIFactory5> factory5;
        BOOL tearing = FALSE;
        if (SUCCEEDED(factory.As(&factory5)) &&
            SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof tearing)) &&
            tearing) {
            desc.Flags = g_chain_flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
            g_tearing = true;
        }
    }
    // ---- end performance ----
    bool hdr = DecideHdr();
    if (hdr) desc.Format = g_back_format = DXGI_FORMAT_R10G10B10A2_UNORM;
    // ---- end display output options ----
    HRESULT hr = factory->CreateSwapChainForHwnd(g_device, g_window, &desc, nullptr, nullptr, &g_chain);
    if (FAILED(hr)) return rt::Log("present: CreateSwapChainForHwnd failed %08X", unsigned(hr)), false;
    factory->MakeWindowAssociation(g_window, DXGI_MWA_NO_ALT_ENTER);  // display.cpp toggles borderless itself
    rt::Log("present: swap chain %ux%u%s", desc.Width, desc.Height, g_tearing ? ", tearing allowed (no vsync)" : "");
    // ---- display output options: HDR10 when the swap chain can present it, else SDR as before ----
    if (hdr) {
        g_hdr = SetColorSpace(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) && InitHdrShader();
        if (!g_hdr) {
            rt::Log("present: no HDR10 output, SDR");
            g_back_format = DXGI_FORMAT_B8G8R8A8_UNORM;
            g_chain->ResizeBuffers(0, desc.Width, desc.Height, g_back_format, g_chain_flags);
            SetColorSpace(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        } else {
            rt::Log("present: HDR10 output (R10G10B10A2, PQ/BT.2020): paper white %.0f nits, peak %.0f nits, highlight "
                    "gain at white x%.2f",
                    g_hdr_params[0], g_hdr_params[1], g_hdr_params[2]);
        }
    }
    char probe[16] = {};
    if (GetEnvironmentVariableA("FCI_PRESENT_PROBE", probe, sizeof probe)) g_probe_every = std::max(std::atoi(probe), 1);
    g_test_pattern = GetEnvironmentVariableA("FCI_HDR_TEST_PATTERN", nullptr, 0) > 0;
    // FCI_TEST_GAMMA=<g>: a gamma ramp 255 * (i / 255)^(1 / g) as if the game had set it (brightness setting)
    char gamma[16] = {};
    if (GetEnvironmentVariableA("FCI_TEST_GAMMA", gamma, sizeof gamma) && std::atof(gamma) > 0) {
        double g = std::atof(gamma);
        for (int i = 0; i < 256; i++)
            g_gamma[i] = g_gamma[256 + i] = g_gamma[512 + i] = uint8_t(std::lround(255 * std::pow(i / 255.0, 1 / g)));
        g_gamma_dirty = true;
        rt::Log("present: test gamma ramp %.2f: 64->%u 128->%u 192->%u", g, g_gamma[64], g_gamma[128], g_gamma[192]);
    }
    // ---- end display output options ----
    // ---- PC features: vsync ----
    if (g_chain_flags & kVsyncFlag) {  // display output options: not ALLOW_MODE_SWITCH
        ComPtr<IDXGISwapChain2> chain2;
        if (SUCCEEDED(g_chain.As(&chain2)) && SUCCEEDED(chain2->SetMaximumFrameLatency(1)))
            g_waitable = chain2->GetFrameLatencyWaitableObject();
        // fps_limit below the refresh rate: every n-th refresh (30 on 60 Hz: 2)
        DEVMODEW mode{};
        mode.dmSize = sizeof mode;
        MONITORINFOEXW info{};
        info.cbSize = sizeof info;
        double refresh = 60;
        if (GetMonitorInfoW(MonitorFromWindow(g_window, MONITOR_DEFAULTTOPRIMARY), &info) &&
            EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1)
            refresh = mode.dmDisplayFrequency;
        int limit = pcmode::FpsLimit();
        g_interval = limit > 0 ? std::clamp(int(refresh / limit + 0.5), 1, 4) : 1;
        g_refresh = refresh, g_interval_limit = limit;  // settings menu: fps_limit may change
        rt::Log("present: vsync, %.0f Hz display, every %d refresh (%.1f fps)%s", refresh, g_interval,
                refresh / g_interval, g_waitable ? "" : ", no waitable object");
    }
    // ---- end PC features ----
    return true;
}

bool EnsureBack() {
    RECT rc;
    GetClientRect(g_window, &rc);
    int w = std::max<int>(rc.right, 1), h = std::max<int>(rc.bottom, 1);
    if (g_back && w == g_back_w && h == g_back_h) return true;
    g_back.Reset();
    g_context->OMSetRenderTargets(0, nullptr, nullptr);
    if (g_back_w || g_resize_pending) {  // display output options: also after a fullscreen transition
        g_resize_pending = false;
        HRESULT hr = g_chain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, g_chain_flags);
        if (FAILED(hr)) return rt::Log("present: ResizeBuffers failed %08X", unsigned(hr)), false;
    }
    ComPtr<ID3D11Texture2D> back;
    g_chain->GetBuffer(0, IID_PPV_ARGS(&back));
    g_device->CreateRenderTargetView(back.Get(), nullptr, &g_back);
    g_back_w = w, g_back_h = h;
    return true;
}

bool EnsureOut(uint32_t w, uint32_t h) {
    if (g_out && g_out_w == w && g_out_h == h) return true;
    g_out.Reset(), g_out_rtv.Reset(), g_out_srv.Reset();
    D3D11_TEXTURE2D_DESC td{w, h, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
    if (FAILED(g_device->CreateTexture2D(&td, nullptr, &g_out))) return false;
    g_device->CreateRenderTargetView(g_out.Get(), nullptr, &g_out_rtv);
    g_device->CreateShaderResourceView(g_out.Get(), nullptr, &g_out_srv);
    g_out_w = w, g_out_h = h;
    return true;
}

void Quad(ID3D11RenderTargetView* rtv, float vx, float vy, float vw, float vh, const float rect[4], const float uv[4],
          ID3D11ShaderResourceView* src, ID3D11PixelShader* ps, ID3D11SamplerState* smp, ID3D11BlendState* blend) {
    D3D11_MAPPED_SUBRESOURCE map;
    if (SUCCEEDED(g_context->Map(g_rect_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        std::memcpy(map.pData, rect, 16);
        std::memcpy(static_cast<uint8_t*>(map.pData) + 16, uv, 16);
        g_context->Unmap(g_rect_cb.Get(), 0);
    }
    D3D11_VIEWPORT vp{vx, vy, vw, vh, 0, 1};
    g_context->RSSetViewports(1, &vp);
    g_context->OMSetRenderTargets(1, &rtv, nullptr);
    g_context->OMSetBlendState(blend ? blend : g_opaque.Get(), nullptr, 0xFFFFFFFF);
    g_context->OMSetDepthStencilState(g_no_depth.Get(), 0);
    g_context->RSSetState(g_raster.Get());
    g_context->IASetInputLayout(nullptr);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    g_context->VSSetShader(g_vs.Get(), nullptr, 0);
    g_context->PSSetShader(ps, nullptr, 0);
    ID3D11Buffer* cb = g_rect_cb.Get();
    g_context->VSSetConstantBuffers(0, 1, &cb);
    ID3D11ShaderResourceView* srvs[2] = {src, g_gamma_srv.Get()};
    g_context->PSSetShaderResources(0, 2, srvs);
    g_context->PSSetSamplers(0, 1, &smp);
    g_context->Draw(4, 0);
    ID3D11ShaderResourceView* none[2] = {};
    g_context->PSSetShaderResources(0, 2, none);
}

uint8_t Clamp(int v) { return uint8_t(std::clamp(v, 0, 255)); }

uint32_t YuvToRgb(int y, int u, int v) {
    int c = y - 16, d = u - 128, e = v - 128;
    int r = (298 * c + 409 * e + 128) >> 8;
    int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
    int b = (298 * c + 516 * d + 128) >> 8;
    return 0xFF000000u | (Clamp(r) << 16) | (Clamp(g) << 8) | Clamp(b);
}

uint32_t Reg(uint32_t offset) { return gpu::Read(offset, 4); }

// the overlay buffer D3D's UpdateOverlay handed to PVIDEO (Bink videos), drawn over the frame; wide: the frame is
// shown at the widescreen aspect (fmv_scale fill, gpu/fmv.h), the video scaled to fill
void DrawOverlay(uint32_t w, uint32_t h, bool wide) {
    if (!gpu::OverlayActive()) return;
    int buffer = gpu::OverlayBuffer();
    uint32_t base = Reg(gpu::reg::PVIDEO_BASE_0 + 4 * buffer);
    uint32_t offset = Reg(gpu::reg::PVIDEO_OFFSET_0 + 4 * buffer);
    uint32_t size_in = Reg(gpu::reg::PVIDEO_SIZE_IN_0 + 4 * buffer);
    uint32_t point_out = Reg(gpu::reg::PVIDEO_POINT_OUT_0 + 4 * buffer);
    uint32_t size_out = Reg(gpu::reg::PVIDEO_SIZE_OUT_0 + 4 * buffer);
    uint32_t format = Reg(gpu::reg::PVIDEO_FORMAT_0 + 4 * buffer);
    uint32_t in_w = size_in & 0x7FF, in_h = (size_in >> 16) & 0x7FF;
    int out_x = point_out & 0xFFF, out_y = (point_out >> 16) & 0xFFF;
    int out_w = size_out & 0xFFF, out_h = (size_out >> 16) & 0xFFF;
    uint32_t pitch = format & 0x1FFF;
    bool uyvy = !((format >> 16) & 1);  // COLOR 1: YUY2 (Y0 Cb Y1 Cr), 0: UYVY
    if (!in_w || !in_h || !out_w || !out_h || !pitch) return;
    // PC features, fmv_scale: where the video goes (guest pixels), scaled to fill when the frame is shown wide
    float s[4] = {1, 0, 1, 0}, z = 1;
    if (wide) z = fmv::Transform(float(out_x), float(out_y), float(out_x + out_w), float(out_y + out_h), in_h, s);
    static uint64_t logged = 0;
    uint64_t geometry = uint64_t(size_in) << 32 ^ point_out ^ uint64_t(size_out) << 16 ^ uint64_t(z * 1000) << 48;
    if (geometry != logged)
        rt::Log("present: overlay %ux%u at %d,%d %dx%d, shown x%.3f", in_w, in_h, out_x, out_y, out_w, out_h, z);
    logged = geometry;
    fmv::MeasureRows(mem::Physical(base + offset), in_w, in_h, pitch, uyvy);  // FCI_FMV_ROWS (diagnostics)
    if (!g_overlay || g_overlay_w != in_w || g_overlay_h != in_h) {
        g_overlay.Reset(), g_overlay_srv.Reset();
        D3D11_TEXTURE2D_DESC td{in_w, in_h, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                D3D11_BIND_SHADER_RESOURCE, 0, 0};
        if (FAILED(g_device->CreateTexture2D(&td, nullptr, &g_overlay))) return;
        g_device->CreateShaderResourceView(g_overlay.Get(), nullptr, &g_overlay_srv);
        g_overlay_w = in_w, g_overlay_h = in_h;
    }
    static std::vector<uint32_t> rgb;
    rgb.resize(size_t(in_w) * in_h);
    const uint8_t* src = mem::Physical(base + offset);
    for (uint32_t y = 0; y < in_h; y++) {
        const uint8_t* row = src + size_t(y) * pitch;
        uint32_t* out = &rgb[size_t(y) * in_w];
        for (uint32_t x = 0; x + 1 < in_w + 1; x += 2) {
            const uint8_t* p = row + x * 2;
            int y0 = uyvy ? p[1] : p[0], u = uyvy ? p[0] : p[1], y1 = uyvy ? p[3] : p[2], v = uyvy ? p[2] : p[3];
            out[x] = YuvToRgb(y0, u, v);
            if (x + 1 < in_w) out[x + 1] = YuvToRgb(y1, u, v);
        }
    }
    g_context->UpdateSubresource(g_overlay.Get(), 0, nullptr, rgb.data(), in_w * 4, 0);
    float sx = float(w) / kGuestWidth, sy = float(h) / kGuestHeight;
    const float rect[4] = {-1, 1, 1, -1}, uv[4] = {0, 0, 1, 1};
    float x0 = out_x * s[0] + s[1], y0 = out_y * s[2] + s[3];
    Quad(g_out_rtv.Get(), x0 * sx, y0 * sy, out_w * s[0] * sx, out_h * s[2] * sy, rect, uv, g_overlay_srv.Get(),
         g_ps_copy.Get(), g_linear.Get());
}

void UpdateGamma() {
    if (!g_gamma_dirty) return;
    g_gamma_dirty = false;
    uint32_t texels[256];
    g_gamma_identity = true;
    for (int i = 0; i < 256; i++) {
        texels[i] = g_gamma[i] | g_gamma[256 + i] << 8 | g_gamma[512 + i] << 16 | 0xFF000000u;
        g_gamma_identity &= g_gamma[i] == i && g_gamma[256 + i] == i && g_gamma[512 + i] == i;
    }
    g_context->UpdateSubresource(g_gamma_tex.Get(), 0, nullptr, texels, sizeof texels, 0);
}

// FCI_DUMP_FRAMES=<n>: the frame as the TV would show it at every n-th vblank (as video/display.cpp numbers
// them), shrunk to 640 x 480 so it compares with LLE dumps
const char* g_dump_tag = nullptr;  // performance: FCI_DUMP_LEFT_OUT

void Dump(uint32_t w, uint32_t h, float aspect) {
    int every = display::DumpEvery();
    const char* tag = g_dump_tag;
    g_dump_tag = nullptr;
    if (!every && !tag) return;
    uint64_t vblank = uint64_t(std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count() * 59.94);
    if (!tag && vblank / every == g_last_dump / every) return;
    if (!tag) g_last_dump = vblank;
    D3D11_TEXTURE2D_DESC td{w, h, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_STAGING, 0,
                            D3D11_CPU_ACCESS_READ, 0};
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(g_device->CreateTexture2D(&td, nullptr, &staging))) return;
    g_context->CopyResource(staging.Get(), g_out.Get());
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(g_context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) return;
    static const bool full = GetEnvironmentVariableA("FCI_DUMP_FRAMES_FULL", nullptr, 0) > 0;
    // shown at its shape: 4:3 at 640x480 (or render height), widescreen frames wider (PC features)
    uint32_t dh = full ? h : kGuestHeight, dw = uint32_t(float(dh) * aspect + 0.5f);
    std::vector<uint32_t> px(size_t(dw) * dh);
    for (uint32_t y = 0; y < dh; y++) {
        for (uint32_t x = 0; x < dw; x++) {
            // box filter over the source texels that make up this pixel
            uint32_t x0 = x * w / dw, x1 = std::max(x0 + 1, (x + 1) * w / dw);
            uint32_t y0 = y * h / dh, y1 = std::max(y0 + 1, (y + 1) * h / dh);
            uint32_t sum[3] = {}, n = 0;
            for (uint32_t sy = y0; sy < y1; sy++)
                for (uint32_t sx = x0; sx < x1; sx++, n++) {
                    uint32_t p = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(map.pData) +
                                                                   size_t(sy) * map.RowPitch)[sx];
                    sum[0] += p & 255, sum[1] += (p >> 8) & 255, sum[2] += (p >> 16) & 255;
                }
            uint32_t b = sum[0] / n, g = sum[1] / n, r = sum[2] / n;
            r = g_gamma[r], g = g_gamma[256 + g], b = g_gamma[512 + b];
            if (g_colour_blind) ColourBlindPixel(r, g, b);  // PC features: colour_blind
            px[size_t(y) * dw + x] = 0xFF000000u | r << 16 | g << 8 | b;
        }
    }
    g_context->Unmap(staging.Get(), 0);
    char name[64];
    snprintf(name, sizeof name, "frame_%05llu%s%s.bmp", (unsigned long long)vblank, tag ? "_" : "", tag ? tag : "");
    if (FILE* f = std::fopen(name, "wb")) {
        BITMAPFILEHEADER fh{};
        BITMAPINFOHEADER ih{};
        ih.biSize = sizeof ih, ih.biWidth = LONG(dw), ih.biHeight = -LONG(dh), ih.biPlanes = 1, ih.biBitCount = 32;
        fh.bfType = 0x4D42, fh.bfOffBits = sizeof fh + sizeof ih, fh.bfSize = DWORD(fh.bfOffBits + px.size() * 4);
        std::fwrite(&fh, sizeof fh, 1, f);
        std::fwrite(&ih, sizeof ih, 1, f);
        std::fwrite(px.data(), 4, px.size(), f);
        std::fclose(f);
    }
}

}  // namespace

bool WaitForDisplay() {
    if (!(g_chain_flags & kVsyncFlag)) return pcmode::Vsync();  // vsync, before the swap chain exists: nothing to wait for yet
    if (g_waitable) WaitForSingleObjectEx(g_waitable, 100, TRUE);
    return true;
}

void SetGamma(const uint8_t ramp[768]) {
    if (std::memcmp(ramp, g_gamma, 768) == 0) return;
    std::memcpy(g_gamma, ramp, 768);
    g_gamma_dirty = true;
    static int logged = 0;
    if (logged++ < 4)
        rt::Log("present: gamma ramp 0->%u 64->%u 128->%u 192->%u 255->%u (green)", ramp[256], ramp[256 + 64],
                ramp[256 + 128], ramp[256 + 192], ramp[256 + 255]);
}

namespace {

// ---- settings menu (app/menu.cpp): what changes while running, the menu's frames ----
std::atomic<int> g_smaa_want{-1};  // SetSmaa: 0 / 1, -1 when unchanged
std::atomic<bool> g_hdr_levels_dirty{false};
float g_hdr_want[3] = {};          // SetHdrLevels: paper white (0 auto), peak (0 the display's), boost
std::mutex g_hdr_want_lock;
float g_last_aspect = 4.0f / 3.0f;  // the shape the last picture was shown at

// the changes the menu asked for, on the thread that has the device context (the guest's or the menu's)
void ApplyChanges() {
    if (int s = g_smaa_want.exchange(-1); s >= 0) {
        g_smaa.enabled = s != 0;
        rt::Log("present: SMAA %s (settings menu)", s ? "on" : "off");
    }
    if (g_hdr_levels_dirty.exchange(false) && g_hdr && g_hdr_cb) {
        std::lock_guard lock(g_hdr_want_lock);
        float paper = g_hdr_want[0] ? g_hdr_want[0] : g_display_white > 0 ? g_display_white : 200.0f;
        float peak = g_hdr_want[1] ? g_hdr_want[1] : g_display_peak > 0 ? g_display_peak : 1000.0f;
        peak = std::max(peak, paper);
        g_hdr_params[0] = paper, g_hdr_params[1] = peak;
        g_hdr_params[2] = 1 + g_hdr_want[2] * (peak / paper - 1);
        g_context->UpdateSubresource(g_hdr_cb.Get(), 0, nullptr, g_hdr_params, 0, 0);
        rt::Log("present: HDR10 paper white %.0f nits, peak %.0f nits, highlight gain x%.2f (settings menu)", paper, peak,
                g_hdr_params[2]);
    }
    // PC features, vsync: fps_limit below the refresh rate is every n-th refresh
    if ((g_chain_flags & kVsyncFlag) && pcmode::FpsLimit() != g_interval_limit) {
        g_interval_limit = pcmode::FpsLimit();
        g_interval = g_interval_limit > 0 ? std::clamp(int(g_refresh / g_interval_limit + 0.5), 1, 4) : 1;
        rt::Log("present: vsync every %d refresh (%.1f fps)", g_interval, g_refresh / g_interval);
    }
}

// the composed picture (g_out) at its shape, letterboxed into the back buffer
void ShowOut(uint32_t w, uint32_t h, float aspect) {
    const float full[4] = {-1, 1, 1, -1}, uv[4] = {0, 0, 1, 1};
    const float black[4] = {0, 0, 0, 1};
    g_context->ClearRenderTargetView(g_back.Get(), black);
    // ---- PC features: the picture at its shape (4:3 or the widescreen aspect), letterboxed into the window ----
    float bw = float(g_back_w), bh = float(g_back_h);
    if (bw > bh * aspect) bw = bh * aspect;
    else bh = bw / aspect;
    float x = std::floor((g_back_w - bw) * 0.5f), y = std::floor((g_back_h - bh) * 0.5f);
    // ---- display output options: HDR10 through its own final pass, the test pattern, the back buffer probe ----
    if (g_test_pattern) DrawTestPattern(w, h);
    ID3D11Buffer* colour = g_colour_cb.Get();  // PC features: colour_blind
    g_context->PSSetConstantBuffers(2, 1, &colour);
    if (g_hdr) {
        ID3D11Buffer* cb = g_hdr_cb.Get();
        g_context->PSSetConstantBuffers(1, 1, &cb);
        Quad(g_back.Get(), x, y, std::round(bw), std::round(bh), full, uv, g_out_srv.Get(), g_ps_hdr.Get(),
             g_test_pattern ? g_point.Get() : g_linear.Get());
    } else {
        Quad(g_back.Get(), x, y, std::round(bw), std::round(bh), full, uv, g_out_srv.Get(),
             g_colour_blind ? g_ps_gamma_cb.Get() : g_ps_gamma.Get(), g_test_pattern ? g_point.Get() : g_linear.Get());
    }
    Probe(x, y, std::round(bw), std::round(bh));
    DumpHdr(x, y, std::round(bw), std::round(bh));
    // ---- end display output options ----
}

bool PresentNow() {
    // (performance: ALLOW_TEARING is not for exclusive fullscreen, where interval 0 does not wait anyway)
    const bool vsync = g_chain_flags & kVsyncFlag;
    HRESULT hr = g_chain->Present(vsync ? g_interval : 0, !vsync && g_tearing && !g_exclusive ? DXGI_PRESENT_ALLOW_TEARING : 0);
    if (FAILED(hr)) {
        rt::Log("present: Present failed %08X, device removed reason %08X", unsigned(hr),
                unsigned(g_device->GetDeviceRemovedReason()));
        g_failed = true;
    }
    g_presents++;
    if (g_presents == 2) display::FinishHandover();  // a relaunched instance's window opens with its first frames
    return SUCCEEDED(hr);
}

// the menu draws with a state of its own: the game is held anywhere in a frame, and its renderer finds the
// context as it left it (ID3D11DeviceContext1::SwapDeviceContextState)
ComPtr<ID3D11DeviceContext1> g_context1;
ComPtr<ID3DDeviceContextState> g_menu_state;
bool g_menu_ready = false, g_menu_failed = false;
// HDR10: the panel goes into an SDR layer first, then over the picture
ComPtr<ID3D11Texture2D> g_ui;
ComPtr<ID3D11RenderTargetView> g_ui_rtv;
ComPtr<ID3D11ShaderResourceView> g_ui_srv;
int g_ui_w = 0, g_ui_h = 0;
ComPtr<ID3D11BlendState> g_premultiplied;
uint64_t g_last_menu_dump = 0;

bool InitMenu() {
    if (g_menu_ready || g_menu_failed) return g_menu_ready;
    ComPtr<ID3D11Device1> device1;
    D3D_FEATURE_LEVEL level = g_device->GetFeatureLevel();
    if (FAILED(g_device->QueryInterface(IID_PPV_ARGS(&device1))) || FAILED(g_context->QueryInterface(IID_PPV_ARGS(&g_context1))) ||
        FAILED(device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr,
                                                 &g_menu_state))) {
        rt::Log("present: no separate device context state (Direct3D 11.1), no settings menu");
        g_menu_failed = true;
        return false;
    }
    ComPtr<ID3DDeviceContextState> previous;
    g_context1->SwapDeviceContextState(g_menu_state.Get(), &previous);
    bool ok = ImGui_ImplDX11_Init(g_device, g_context);
    g_context1->SwapDeviceContextState(previous.Get(), nullptr);
    if (!ok) {
        rt::Log("present: ImGui_ImplDX11_Init failed, no settings menu");
        g_menu_failed = true;
        return false;
    }
    D3D11_BLEND_DESC bd{};
    auto& t = bd.RenderTarget[0];
    t.BlendEnable = TRUE;
    t.SrcBlend = D3D11_BLEND_ONE, t.DestBlend = D3D11_BLEND_INV_SRC_ALPHA, t.BlendOp = D3D11_BLEND_OP_ADD;
    t.SrcBlendAlpha = D3D11_BLEND_ONE, t.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA, t.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    t.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g_device->CreateBlendState(&bd, &g_premultiplied);
    g_menu_ready = true;
    rt::Log("present: settings menu ready (Dear ImGui %s, Direct3D 11)", IMGUI_VERSION);
    return true;
}

bool EnsureUiLayer() {
    if (g_ui && g_ui_w == g_back_w && g_ui_h == g_back_h) return true;
    g_ui.Reset(), g_ui_rtv.Reset(), g_ui_srv.Reset();
    D3D11_TEXTURE2D_DESC td{UINT(g_back_w), UINT(g_back_h), 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
    if (FAILED(g_device->CreateTexture2D(&td, nullptr, &g_ui))) return false;
    g_device->CreateRenderTargetView(g_ui.Get(), nullptr, &g_ui_rtv);
    g_device->CreateShaderResourceView(g_ui.Get(), nullptr, &g_ui_srv);
    g_ui_w = g_back_w, g_ui_h = g_back_h;
    return true;
}

// FCI_DUMP_FRAMES=<n>: the menu's frames too (the window's back buffer with the panel, at its size) as
// menu_<vblank>.bmp, every n-th vblank
void DumpBack() {
    int every = display::DumpEvery();
    if (!every) return;
    uint64_t vblank = uint64_t(std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count() * 59.94);
    if (g_last_menu_dump && vblank / every == g_last_menu_dump / every) return;
    g_last_menu_dump = vblank;
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(g_chain->GetBuffer(0, IID_PPV_ARGS(&back)))) return;
    D3D11_TEXTURE2D_DESC bd;
    back->GetDesc(&bd);
    bd.Usage = D3D11_USAGE_STAGING, bd.BindFlags = 0, bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ, bd.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(g_device->CreateTexture2D(&bd, nullptr, &staging))) return;
    g_context->CopyResource(staging.Get(), back.Get());
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(g_context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) return;
    std::vector<uint32_t> px(size_t(bd.Width) * bd.Height);
    for (uint32_t y = 0; y < bd.Height; y++) {
        const uint32_t* row = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(map.pData) + size_t(y) * map.RowPitch);
        for (uint32_t x = 0; x < bd.Width; x++) {
            uint32_t p = row[x];
            // HDR10 codes as they are (PQ, only to see the layout)
            if (bd.Format == DXGI_FORMAT_R10G10B10A2_UNORM)
                p = 0xFF000000u | ((p >> 2) & 255) << 16 | ((p >> 12) & 255) << 8 | ((p >> 22) & 255);
            px[size_t(y) * bd.Width + x] = p | 0xFF000000u;
        }
    }
    g_context->Unmap(staging.Get(), 0);
    char name[64];
    snprintf(name, sizeof name, "menu_%05llu.bmp", (unsigned long long)vblank);
    if (FILE* f = std::fopen(name, "wb")) {
        BITMAPFILEHEADER fh{};
        BITMAPINFOHEADER ih{};
        ih.biSize = sizeof ih, ih.biWidth = LONG(bd.Width), ih.biHeight = -LONG(bd.Height), ih.biPlanes = 1, ih.biBitCount = 32;
        fh.bfType = 0x4D42, fh.bfOffBits = sizeof fh + sizeof ih, fh.bfSize = DWORD(fh.bfOffBits + px.size() * 4);
        std::fwrite(&fh, sizeof fh, 1, f);
        std::fwrite(&ih, sizeof ih, 1, f);
        std::fwrite(px.data(), 4, px.size(), f);
        std::fclose(f);
    }
}
// ---- end settings menu ----

}  // namespace

void DumpNext(const char* tag) { g_dump_tag = tag; }

void Frame(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11ShaderResourceView* frame, uint32_t w,
           uint32_t h, float aspect) {
    if (g_failed) return;
    if (!g_device) {
        g_device = device, g_context = context;
        if (!InitPipeline() || !InitSwap()) {
            g_failed = true;
            return;
        }
        g_smaa.enabled = settings::Int("smaa", 1) != 0;
        if (!InitColourBlind()) {  // PC features: colour_blind
            g_failed = true;
            return;
        }
    }
    if (!w || !h) w = kGuestWidth, h = kGuestHeight;
    ApplyChanges();     // settings menu
    UpdateExclusive();  // display output options
    {  // fps_limit = auto: the refresh rate of the monitor the window is on now, looked at every second
        static ULONGLONG checked = 0;
        if (GetTickCount64() - checked >= 1000) {
            checked = GetTickCount64();
            MONITORINFOEXW info{};
            info.cbSize = sizeof info;
            DEVMODEW mode{};
            mode.dmSize = sizeof mode;
            if (GetMonitorInfoW(MonitorFromWindow(g_window, MONITOR_DEFAULTTOPRIMARY), &info) &&
                EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode))
                pcmode::SetDisplayRefresh(int(mode.dmDisplayFrequency));
        }
    }
    if (!EnsureBack() || !EnsureOut(w, h)) return;
    UpdateGamma();
    const float full[4] = {-1, 1, 1, -1}, uv[4] = {0, 0, 1, 1};
    if (frame) {
        frame = Smaa(frame, w, h, g_out_rtv.Get());
        if (frame) Quad(g_out_rtv.Get(), 0, 0, float(w), float(h), full, uv, frame, g_ps_copy.Get(), g_point.Get());
    } else {
        const float black[4] = {0, 0, 0, 1};
        g_context->ClearRenderTargetView(g_out_rtv.Get(), black);
    }
    DrawOverlay(w, h, aspect > 4.0f / 3.0f * 1.001f);
    fps::Draw(g_device, g_context, g_out_rtv.Get(), w, h);  // ---- PC features: fps_counter (gpu/fps_overlay.h) ----
    Dump(w, h, aspect);
    g_last_aspect = aspect;  // settings menu
    ShowOut(w, h, aspect);
    PresentNow();
}

// ---- settings menu ----
bool MenuAvailable() { return g_device && g_chain && !g_failed && !g_menu_failed && g_out; }

bool MenuFrame(ImDrawData* (*build)()) {
    std::lock_guard ctx(render::ContextLock());  // performance: not while the render thread draws
    if (!MenuAvailable()) return false;
    if (!InitMenu()) return false;
    ComPtr<ID3DDeviceContextState> previous;
    g_context1->SwapDeviceContextState(g_menu_state.Get(), &previous);
    ApplyChanges();
    UpdateExclusive();
    bool ok = EnsureBack();
    if (ok) {
        UpdateGamma();
        ImGui_ImplDX11_NewFrame();
        ImDrawData* draw = build();
        ShowOut(g_out_w, g_out_h, g_last_aspect);
        if (draw && g_hdr && g_ps_ui_hdr && EnsureUiLayer()) {
            const float clear[4] = {};
            g_context->ClearRenderTargetView(g_ui_rtv.Get(), clear);
            ID3D11RenderTargetView* rtv = g_ui_rtv.Get();
            g_context->OMSetRenderTargets(1, &rtv, nullptr);
            ImGui_ImplDX11_RenderDrawData(draw);
            const float full[4] = {-1, 1, 1, -1}, uv[4] = {0, 0, 1, 1};
            ID3D11Buffer* cb = g_hdr_cb.Get();
            g_context->PSSetConstantBuffers(1, 1, &cb);
            Quad(g_back.Get(), 0, 0, float(g_back_w), float(g_back_h), full, uv, g_ui_srv.Get(), g_ps_ui_hdr.Get(),
                 g_point.Get(), g_premultiplied.Get());
        } else if (draw) {
            ID3D11RenderTargetView* rtv = g_back.Get();
            g_context->OMSetRenderTargets(1, &rtv, nullptr);
            ImGui_ImplDX11_RenderDrawData(draw);
        }
        DumpBack();
        ok = PresentNow();
    }
    g_context->OMSetRenderTargets(0, nullptr, nullptr);
    g_context1->SwapDeviceContextState(previous.Get(), nullptr);
    return ok;
}

void SetSmaa(bool on) { g_smaa_want = on ? 1 : 0; }

bool HdrActive() { return g_hdr; }

void SetHdrLevels(float paper_white, float peak, float boost) {
    {
        std::lock_guard lock(g_hdr_want_lock);
        g_hdr_want[0] = paper_white, g_hdr_want[1] = peak, g_hdr_want[2] = std::clamp(boost, 0.0f, 1.0f);
    }
    g_hdr_levels_dirty = true;
}
// ---- end settings menu ----

}  // namespace gpu::present

