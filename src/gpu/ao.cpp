// Screen space ambient occlusion over the opaque 3D view (see gpu/ao.h).
//
// The view reconstruction, the normal from the nearer neighbours, the SSAO and GTAO kernels, the depth aware
// 5 tap blur and the fog fade are adapted from SCDA-XBERecomp (MIT, Copyright (c) 2026 TGP482, Joshhhuaaa),
// src/gpu/d3d12.cpp (kAoHlsl and DrawOcclusionPasses), moved to Direct3D 11. Changed for FCI: depth is linear
// (W-buffering: w / zmax, which the XDK scales to view depth / far plane) instead of a projected z, the occlusion can
// run at half resolution with a depth aware upsampling, the result goes into a multisampled target per sample, sky
// (cleared depth) and the first person view (view depth below 5 cm) get none, and it fades out with distance as
// well as with the game's fog.
#include "gpu/ao.h"

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>

#include "app/settings.h"
#include "gpu/renderer.h"  // CompileHlsl
#include "runtime/log.h"

namespace gpu::ao {

namespace {

using Microsoft::WRL::ComPtr;

// CryEngine units are metres
constexpr float kRadius = 1.0f;       // world radius of the occlusion
constexpr float kMaxSpan = 0.12f;     // most screen the radius may span, as a share of the view's height
// view depth below which nothing gets occlusion: the first person hands and weapon (their own projection puts them
// at a few millimetres; walls the player swims against are 0.3 m and more away)
constexpr float kNear = 0.05f;
constexpr float kFadeStart = 35.0f;   // view depth where the occlusion starts to fade ...
constexpr float kFadeEnd = 90.0f;     // ... and where it is gone
constexpr float kSkyDepth = 0.999f;   // stored depth at or beyond which a pixel is sky (depth is cleared to 1)

const char* const kHlsl = R"(
cbuffer A : register(b4) {
    float4 proj;  // 1 / x scale, 1 / y scale, far plane (view depth of stored depth 1), view depth below which none
    float4 vp;    // the view in occlusion pixels: x, y, width, height
    float4 opt;   // world radius, most occlusion pixels it may span, blur step x, y
    float4 host;  // the view in host pixels: x, y, width, height
    float4 fade;  // fog slope, offset, amount; strength (exponent)
    float4 misc;  // host pixels per occlusion pixel, distance fade start, 1 / fade length, sky depth
    float4 cmp;   // x: host column left of which there is none (FCI_AO_DEBUG=3, a before / after view), else 0
};
#ifdef MS
Texture2DMS<float> depth : register(t0);
float Stored(int2 p, uint s) { return depth.Load(p, s); }
#else
Texture2D<float> depth : register(t0);
float Stored(int2 p, uint s) { return depth.Load(int3(p, 0)); }
#endif
// w / zmax; the renderer's reversed depth (cmp.y = c) stores c / (w / zmax)
float RawDepth(int2 p, uint s) {
    float r = Stored(p, s);
    return cmp.y > 0 ? cmp.y / max(r, cmp.y) : r;
}
Texture2D<float> lin : register(t1);   // view depth per occlusion pixel
Texture2D<float2> ao : register(t2);   // occlusion, view depth
float4 vs(uint id : SV_VertexID) : SV_Position { return float4(float2((id << 1) & 2, id & 2) * float2(2, -2) + float2(-1, 1), 0, 1); }

static const float kFar = 60000;  // sky; stays finite in 16 bit floats
float ViewDepth(float d) { return d >= misc.w ? kFar : min(d * proj.z, kFar); }

// the view depth of an occlusion pixel: the host pixel at its centre, sample 0
float linearize(float4 pos : SV_Position) : SV_Target {
    int2 p = clamp(int2(pos.xy * misc.x), int2(host.xy), int2(host.xy + host.zw) - 1);
    return ViewDepth(RawDepth(p, 0));
}

// view space position of an occlusion pixel (x right, y up, z forward)
float3 View(float2 px) {
    int2 p = clamp(int2(px), int2(vp.xy), int2(vp.xy + vp.zw) - 1);
    float z = lin.Load(int3(p, 0));
    float2 ndc = ((p + 0.5) * misc.x - host.xy) / host.zw * float2(2, -2) + float2(-1, 1);
    return float3(ndc * proj.xy * z, z);
}
static const float kDither[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
// normal from the nearer neighbour each way: no bend over depth edges
float3 Normal(float2 px, float3 c) {
    float3 l = View(px - float2(1, 0)), r = View(px + float2(1, 0));
    float3 u = View(px - float2(0, 1)), d = View(px + float2(0, 1));
    float3 dx = abs(r.z - c.z) < abs(c.z - l.z) ? r - c : c - l;
    float3 dy = abs(d.z - c.z) < abs(c.z - u.z) ? d - c : c - u;
    return normalize(cross(dx, dy));
}
// the radius in occlusion pixels, x and y
float2 Radius(float z) { return opt.x * 0.5 * host.zw / (proj.xy * z) / misc.x; }

// out: occlusion, view z
float2 occlusion(float4 pos : SV_Position) : SV_Target {
    float3 c = View(pos.xy);
    if (c.z < proj.w || c.z >= kFar) return float2(1, c.z);
    float2 rad = Radius(c.z);
    float span = max(rad.x, rad.y);
    if (span < 1) return float2(1, c.z);
    float3 n = Normal(pos.xy, c);
    float k = min(1, opt.y / span), R = opt.x * k;
    int2 cell = int2(pos.xy) & 3;
    float turn = kDither[cell.y * 4 + cell.x] * (6.2831853 / 16), a = 0;
    for (int i = 0; i < 16; i++) {
        float f = (i + 0.5) / 16, t = i * 2.3999632 + turn;
        float3 v = (View(pos.xy + float2(cos(t), sin(t)) * rad * (k * f)) - c) / R;
        float vv = dot(v, v), w = max(1 - vv, 0);
        a += w * w * w * max((dot(v, n) - 0.05) / (vv + 0.01), 0);
    }
    return float2(max(1 - a * 0.25, 0), c.z);
}
static const float kHalfPi = 1.5707963;
// GTAO (Jimenez 2016) at XeGTAO's high preset: 3 slices, 3 steps a side, squared step spread, falloff over the outer
// 61.5% of the radius, power 2.2. Per slice the highest horizon each side, the visible arc between integrated against
// the normal projected into the slice
float2 gtao(float4 pos : SV_Position) : SV_Target {
    float3 c = View(pos.xy);
    if (c.z < proj.w || c.z >= kFar) return float2(1, c.z);
    float2 rad = Radius(c.z);
    float span = max(rad.x, rad.y);
    if (span < 1) return float2(1, c.z);
    float3 n = Normal(pos.xy, c);
    float3 v = normalize(-c);
    float k = min(1, opt.y / span), R = opt.x * k;
    int2 cell = int2(pos.xy) & 3;
    float slice_noise = kDither[cell.y * 4 + cell.x] / 16;
    float step_noise = kDither[cell.x * 4 + cell.y] / 16;
    float vis = 0;
    for (int s = 0; s < 3; s++) {
        float phi = (s + slice_noise) * (kHalfPi * 2 / 3);
        float3 dir = float3(cos(phi), sin(phi), 0);
        // in pixels; screen y runs down, view y up
        float2 omega = float2(dir.x, -dir.y) * rad * k;
        float3 ortho = dir - dot(dir, v) * v;
        float3 axis = normalize(cross(ortho, v));
        float3 pn = n - axis * dot(n, axis);
        float pn_len = max(length(pn), 1e-4);
        float cos_n = saturate(dot(pn, v) / pn_len);
        float an = sign(dot(ortho, pn)) * acos(cos_n);
        // x: the +omega side, y: the -omega side; lowest is flat along the slice's tangent
        float2 low = float2(cos(an + kHalfPi), cos(an - kHalfPi));
        float2 hc = low;
        for (int j = 0; j < 3; j++) {
            float f = (j + step_noise) / 3;
            // at least 1.3 pixels out, off the centre pixel
            float2 off = omega * (f * f + 1.3 / length(omega));
            float3 d0 = View(pos.xy + off) - c;
            float3 d1 = View(pos.xy - off) - c;
            float2 len = max(float2(length(d0), length(d1)), 1e-4);
            float2 w = saturate((R - len) / (R * 0.615));
            float2 sc = float2(dot(d0, v), dot(d1, v)) / len;
            hc = max(hc, lerp(low, sc, w));
        }
        float h0 = an + clamp(-acos(hc.y) - an, -kHalfPi, kHalfPi);
        float h1 = an + clamp(acos(hc.x) - an, -kHalfPi, kHalfPi);
        float sin_n = sin(an);
        float arc0 = (cos_n + 2 * h0 * sin_n - cos(2 * h0 - an)) / 4;
        float arc1 = (cos_n + 2 * h1 * sin_n - cos(2 * h1 - an)) / 4;
        vis += pn_len * (arc0 + arc1);
    }
    return float2(pow(saturate(vis / 3), 2.2), c.z);
}
// opt.zw: step; 5 taps average the 4 pixel dither, depth edges left out
float2 blur(float4 pos : SV_Position) : SV_Target {
    int2 p = int2(pos.xy), lo = int2(vp.xy), hi = int2(vp.xy + vp.zw) - 1;
    float z = ao.Load(int3(p, 0)).y;
    float sum = 0, n = 0;
    for (int i = -2; i <= 2; i++) {
        float2 s = ao.Load(int3(clamp(p + i * int2(opt.zw), lo, hi), 0));
        float w = (abs(i) == 2 ? 0.5 : 1) * saturate(2 - abs(s.y - z) / (z * 0.02));
        sum += s.x * w;
        n += w;
    }
    return float2(sum / max(n, 1e-4), z);
}
// into the scene: the nearest occlusion pixels of a similar depth, faded with distance and fog, for view depth z
float4 AoAt(float4 pos, float z) {
    if (z < proj.w || z >= kFar) return 1;
    float2 q = pos.xy / misc.x - 0.5;
    int2 b = int2(floor(q)), lo = int2(vp.xy), hi = int2(vp.xy + vp.zw) - 1;
    float2 f = q - b;
    float sum = 0, n = 0, nearest = 1, gap = 1e30;
    for (int i = 0; i < 4; i++) {
        int2 o = int2(i & 1, i >> 1);
        float2 t = ao.Load(int3(clamp(b + o, lo, hi), 0));
        float dz = abs(t.y - z);
        float w = (o.x ? f.x : 1 - f.x) * (o.y ? f.y : 1 - f.y) * saturate(1 - dz / (z * 0.04));
        sum += t.x * w;
        n += w;
        if (dz < gap) gap = dz, nearest = t.x;
    }
    float a = n > 1e-3 ? sum / n : nearest;
    a = lerp(a, 1, saturate((z - misc.y) * misc.z));
    a = lerp(a, 1, fade.z * saturate(fade.x * z + fade.y));
    a = pow(saturate(a), fade.w);
    if (pos.x < cmp.x) a = 1;
    return float4(a.xxx, 1);
}
#ifdef MS
// performance: per pixel instead of per sample. The samples at the depth of sample 0 get its occlusion (most pixels:
// all of them), a second pass gives the others (the far or near side of an edge) the occlusion at their depth; the
// coverage output keeps each to its samples. Before: one shader run per sample (4x MSAA: four)
uint SameDepth(int2 p, float z, uint n) {
    uint mask = 0;
    for (uint i = 0; i < n; i++)
        if (abs(ViewDepth(RawDepth(p, i)) - z) <= z * 0.01) mask |= 1u << i;
    return mask;
}
float4 apply(float4 pos : SV_Position, out uint coverage : SV_Coverage) : SV_Target {
    uint w, h, n;
    depth.GetDimensions(w, h, n);
    int2 p = int2(pos.xy);
    float z = ViewDepth(RawDepth(p, 0));
    coverage = SameDepth(p, z, n);
    return AoAt(pos, z);
}
float4 apply_rest(float4 pos : SV_Position, out uint coverage : SV_Coverage) : SV_Target {
    uint w, h, n;
    depth.GetDimensions(w, h, n);
    int2 p = int2(pos.xy);
    uint rest = ((1u << n) - 1) & ~SameDepth(p, ViewDepth(RawDepth(p, 0)), n);
    if (!rest) discard;
    coverage = rest;
    return AoAt(pos, ViewDepth(RawDepth(p, firstbitlow(rest))));
}
#else
float4 apply(float4 pos : SV_Position) : SV_Target { return AoAt(pos, ViewDepth(RawDepth(int2(pos.xy), 0))); }
#endif
// FCI_AO_DEBUG=2: view depth, red to 2 m, green to 20 m, blue to 200 m
float4 depth_view(float4 pos : SV_Position) : SV_Target {
    float z = ViewDepth(RawDepth(int2(pos.xy), 0));
    return float4(saturate(z / 2), saturate(z / 20), saturate(z / 200), 1);
}
)";

struct Constants {
    float proj[4], vp[4], opt[4], host[4], fade[4], misc[4], cmp[4];
};

struct Targets {
    uint32_t w = 0, h = 0;
    ComPtr<ID3D11Texture2D> tex[3];  // view depth, occlusion twice
    ComPtr<ID3D11RenderTargetView> rtv[3];
    ComPtr<ID3D11ShaderResourceView> srv[3];
};

struct State {
    bool init = false, failed = false;
    ID3D11Device* device = nullptr;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> linearize[2], apply[2], depth_view[2], occlusion, gtao, blur;  // [ms]
    ComPtr<ID3D11PixelShader> apply_rest;  // performance: multisampled, the samples at another depth
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11BlendState> opaque, multiply;
    ComPtr<ID3D11DepthStencilState> no_depth;
    ComPtr<ID3D11RasterizerState> raster;
    Targets t;
    // FCI_AO_TIMING=1: GPU time of the passes, averaged and logged every 300 frames
    bool timing = false;
    ComPtr<ID3D11Query> disjoint[4], begin[4], end[4];
    uint32_t query_next = 0, query_pending = 0;
    double ms_sum = 0;
    uint32_t ms_count = 0;
};
State g;

float g_x_scale = 0, g_y_scale = 0;  // the game camera's projection; 0 until one is seen
float g_far = 425.0f;                 // its far plane (the first level's), metres

ComPtr<ID3D11PixelShader> Pixel(const char* entry, bool ms) {
    std::string text = std::string(ms ? "#define MS 1\n" : "") + kHlsl;
    ComPtr<ID3DBlob> code, errors;
    ComPtr<ID3D11PixelShader> ps;
    if (FAILED(gpu::render::CompileHlsl(text.data(), text.size(), "ao", nullptr, nullptr, entry, "ps_5_0",
                                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors))) {
        rt::Log("ao: %s compile failed: %s", entry, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        return nullptr;
    }
    g.device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &ps);
    return ps;
}

std::once_flag g_init_once;
void InitOnce(ID3D11Device* device);

bool Init(ID3D11Device* device) {
    std::call_once(g_init_once, InitOnce, device);
    return !g.failed;
}

void InitOnce(ID3D11Device* device) {
    g.init = true;
    g.device = device;
    ComPtr<ID3DBlob> code, errors;
    if (FAILED(gpu::render::CompileHlsl(kHlsl, std::strlen(kHlsl), "ao", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &code,
                                        &errors)) ||
        FAILED(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g.vs))) {
        g.failed = true;
        return;
    }
    for (int ms = 0; ms < 2; ms++) {
        g.linearize[ms] = Pixel("linearize", ms != 0);
        g.apply[ms] = Pixel("apply", ms != 0);
        g.depth_view[ms] = Pixel("depth_view", ms != 0);
    }
    g.apply_rest = Pixel("apply_rest", true);
    g.occlusion = Pixel("occlusion", false);
    g.gtao = Pixel("gtao", false);
    g.blur = Pixel("blur", false);
    if (!g.linearize[0] || !g.linearize[1] || !g.apply[0] || !g.apply[1] || !g.apply_rest || !g.occlusion || !g.gtao ||
        !g.blur) {
        g.failed = true;
        return;
    }
    D3D11_BUFFER_DESC cb{sizeof(Constants), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0,
                         0};
    device->CreateBuffer(&cb, nullptr, &g.constants);
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device->CreateBlendState(&bd, &g.opaque);
    // colour times the occlusion; the alpha the game's blends read stays
    auto& b = bd.RenderTarget[0];
    b.BlendEnable = TRUE;
    b.SrcBlend = b.SrcBlendAlpha = D3D11_BLEND_ZERO;
    b.DestBlend = D3D11_BLEND_SRC_COLOR;
    b.DestBlendAlpha = D3D11_BLEND_ONE;
    b.BlendOp = b.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    b.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN |
                              D3D11_COLOR_WRITE_ENABLE_BLUE;
    device->CreateBlendState(&bd, &g.multiply);
    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
    ds.FrontFace.StencilFunc = ds.BackFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
    device->CreateDepthStencilState(&ds, &g.no_depth);
    D3D11_RASTERIZER_DESC rs{};
    rs.FillMode = D3D11_FILL_SOLID, rs.CullMode = D3D11_CULL_NONE, rs.ScissorEnable = TRUE;
    device->CreateRasterizerState(&rs, &g.raster);
    g.timing = GetEnvironmentVariableA("FCI_AO_TIMING", nullptr, 0) > 0;
    if (g.timing)
        for (int i = 0; i < 4; i++) {
            D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
            device->CreateQuery(&qd, &g.disjoint[i]);
            qd.Query = D3D11_QUERY_TIMESTAMP;
            device->CreateQuery(&qd, &g.begin[i]);
            device->CreateQuery(&qd, &g.end[i]);
        }
    rt::Log("ao: %s, strength %.2f, radius %.2f m, fade %.0f..%.0f m", Mode() == 2 ? "GTAO" : "SSAO",
            settings::Float("ao_strength", 1.0f), kRadius, kFadeStart, kFadeEnd);
}

bool EnsureTargets(uint32_t w, uint32_t h) {
    if (g.t.w == w && g.t.h == h && g.t.tex[2]) return true;
    g.t = {};
    const DXGI_FORMAT formats[3] = {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_FLOAT};
    for (int i = 0; i < 3; i++) {
        D3D11_TEXTURE2D_DESC desc{w, h, 1, 1, formats[i], {1, 0}, D3D11_USAGE_DEFAULT,
                                  D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
        if (FAILED(g.device->CreateTexture2D(&desc, nullptr, &g.t.tex[i]))) return false;
        g.device->CreateRenderTargetView(g.t.tex[i].Get(), nullptr, &g.t.rtv[i]);
        g.device->CreateShaderResourceView(g.t.tex[i].Get(), nullptr, &g.t.srv[i]);
    }
    g.t.w = w, g.t.h = h;
    rt::Log("ao: occlusion targets %ux%u", w, h);
    return true;
}

// a full screen triangle into rtv, inside rect, reading srv t0..t2
void Draw(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, float w, float h, const D3D11_RECT& rect,
          ID3D11PixelShader* ps, ID3D11BlendState* blend, ID3D11ShaderResourceView* t0, ID3D11ShaderResourceView* t1,
          ID3D11ShaderResourceView* t2) {
    ID3D11ShaderResourceView* none[3] = {};
    ctx->PSSetShaderResources(0, 3, none);
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ID3D11ShaderResourceView* srvs[3] = {t0, t1, t2};
    ctx->PSSetShaderResources(0, 3, srvs);
    D3D11_VIEWPORT vp{0, 0, w, h, 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetScissorRects(1, &rect);
    ctx->OMSetBlendState(blend, nullptr, 0xFFFFFFFF);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->Draw(3, 0);
}

void CollectTiming(ID3D11DeviceContext* ctx) {
    while (g.query_pending) {
        uint32_t i = (g.query_next + 4 - g.query_pending) % 4;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        UINT64 t0 = 0, t1 = 0;
        if (ctx->GetData(g.disjoint[i].Get(), &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            ctx->GetData(g.begin[i].Get(), &t0, sizeof t0, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            ctx->GetData(g.end[i].Get(), &t1, sizeof t1, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            break;
        g.query_pending--;
        if (dj.Disjoint || !dj.Frequency) continue;
        g.ms_sum += double(t1 - t0) * 1000.0 / double(dj.Frequency);
        if (++g.ms_count == 300) {
            rt::Log("ao: %.3f ms GPU per pass (average of 300)", g.ms_sum / g.ms_count);
            g.ms_sum = 0, g.ms_count = 0;
        }
    }
}

}  // namespace

int BootMode() {
    static const int mode = [] {
        std::string v = settings::String("ao", "ssao");
        for (char& ch : v) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        if (v == "ssao" || v == "1") return 1;
        if (v == "gtao" || v == "2") return 2;
        return 0;
    }();
    return mode;
}

// ---- settings menu: ao and ao_strength change while running (SetMode, SetStrength) ----
namespace {
std::atomic<int> g_live_mode{-1};          // -1: as read at start
std::atomic<float> g_live_strength{-1.0f};  // < 0: as read at start
}  // namespace

int Mode() {
    int m = g_live_mode.load(std::memory_order_relaxed);
    return m >= 0 ? m : BootMode();
}

bool SetMode(int mode) {
    if (mode && !BootMode()) return false;  // the depth targets were made without the view the pass reads
    g_live_mode = std::clamp(mode, 0, 2);
    rt::Log("ao: %s (settings menu)", mode == 2 ? "GTAO" : mode ? "SSAO" : "off");
    return true;
}

void SetStrength(float strength) { g_live_strength = std::clamp(strength, 0.0f, 4.0f); }
// ---- end settings menu ----

void SetProjection(float x_scale, float y_scale, float far_plane) {
    if (x_scale > 0 && y_scale > 0 && std::isfinite(x_scale) && std::isfinite(y_scale))
        g_x_scale = x_scale, g_y_scale = y_scale;
    if (far_plane > 1 && far_plane < 1e6f) g_far = far_plane;
}

void Prepare(ID3D11Device* device) {
    if (!BootMode()) return;
    std::thread([device] { Init(device); }).detach();
}

void Apply(ID3D11Device* device, ID3D11DeviceContext* ctx, const Pass& p) {
    if (!Mode() || !p.depth || !p.color || !p.width || !p.height || !Init(device)) return;
    const int32_t left = std::max(p.left, 0), top = std::max(p.top, 0);
    const int32_t right = std::min<int32_t>(p.right, p.width), bottom = std::min<int32_t>(p.bottom, p.height);
    if (right - left < 8 || bottom - top < 8) return;
    // half resolution once the view is taller than 720 lines (ao_resolution = auto; performance: was 1080, the
    // picture is the same with the depth aware upsampling, the Steam Deck's 800 lines save 1.3 of 2.6 ms), or as set
    static const int half = [] {
        std::string v = settings::String("ao_resolution", "auto");
        return v == "full" ? 0 : v == "half" ? 1 : -1;
    }();
    const uint32_t s = half == 1 || (half < 0 && bottom - top > 720) ? 2 : 1;
    if (!EnsureTargets((p.width + s - 1) / s, (p.height + s - 1) / s)) return;
    // the camera's projection; before the first one a 60 degree vertical field of view at the view's shape
    float y_scale = g_y_scale, x_scale = g_x_scale;
    if (!y_scale) y_scale = 1.7320508f, x_scale = y_scale * float(bottom - top) / float(right - left);
    static const float boot_strength = std::clamp(settings::Float("ao_strength", 1.0f), 0.0f, 4.0f);
    const float strength = g_live_strength >= 0 ? g_live_strength.load() : boot_strength;  // settings menu
    const int32_t vl = left / int32_t(s), vt = top / int32_t(s);
    const int32_t vr = (right + int32_t(s) - 1) / int32_t(s), vb = (bottom + int32_t(s) - 1) / int32_t(s);
    // FCI_AO_DEBUG=1: the occlusion itself instead of the scene (white = none), 2: the view depth, 3: the occlusion
    // on the right half of the view only (before / after in one frame)
    static const int debug = settings::Int("ao_debug", 0);
    Constants c{};
    c.proj[0] = 1.0f / x_scale, c.proj[1] = 1.0f / y_scale, c.proj[2] = g_far, c.proj[3] = kNear;
    c.vp[0] = float(vl), c.vp[1] = float(vt), c.vp[2] = float(vr - vl), c.vp[3] = float(vb - vt);
    c.opt[0] = kRadius, c.opt[1] = kMaxSpan * float(bottom - top) / float(s);
    c.host[0] = float(left), c.host[1] = float(top), c.host[2] = float(right - left), c.host[3] = float(bottom - top);
    c.fade[0] = p.fog[0] / g_far, c.fade[1] = p.fog[1], c.fade[2] = p.fog[2], c.fade[3] = strength;
    c.misc[0] = float(s), c.misc[1] = kFadeStart, c.misc[2] = 1.0f / (kFadeEnd - kFadeStart), c.misc[3] = kSkyDepth;
    if (debug == 3) c.cmp[0] = float(left + right) * 0.5f;
    c.cmp[1] = p.reversed;
    {
        static uint32_t applied = 0;  // the first few, then every 1800th
        if (applied++ < 4 || applied % 1800 == 0) {
            rt::Log("ao: view %d,%d..%d,%d of %ux%u, %u samples, %s resolution, projection %.4f %.4f, fog %g %g %g",
                    left, top, right, bottom, p.width, p.height, p.samples, s == 2 ? "half" : "full", x_scale, y_scale,
                    p.fog[0], p.fog[1], p.fog[2]);
        }
    }
    if (g.timing) {
        CollectTiming(ctx);
        if (g.query_pending < 4) {
            ctx->Begin(g.disjoint[g.query_next].Get());
            ctx->End(g.begin[g.query_next].Get());
        }
    }
    D3D11_MAPPED_SUBRESOURCE map;
    const bool ms = p.samples > 1;
    const float aw = float(g.t.w), ah = float(g.t.h);
    const D3D11_RECT ao_rect{vl, vt, vr, vb}, host_rect{left, top, right, bottom};
    ID3D11Buffer* cb = g.constants.Get();
    auto step = [&](float x, float y) {
        c.opt[2] = x, c.opt[3] = y;
        if (SUCCEEDED(ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
            std::memcpy(map.pData, &c, sizeof c);
            ctx->Unmap(cb, 0);
        }
    };
    step(0, 0);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g.vs.Get(), nullptr, 0);
    ctx->PSSetConstantBuffers(4, 1, &cb);
    ctx->OMSetDepthStencilState(g.no_depth.Get(), 0);
    ctx->RSSetState(g.raster.Get());
    Draw(ctx, g.t.rtv[0].Get(), aw, ah, ao_rect, g.linearize[ms].Get(), g.opaque.Get(), p.depth, nullptr, nullptr);
    Draw(ctx, g.t.rtv[1].Get(), aw, ah, ao_rect, (Mode() == 2 ? g.gtao : g.occlusion).Get(), g.opaque.Get(), nullptr,
         g.t.srv[0].Get(), nullptr);
    step(1, 0);
    Draw(ctx, g.t.rtv[2].Get(), aw, ah, ao_rect, g.blur.Get(), g.opaque.Get(), nullptr, nullptr, g.t.srv[1].Get());
    step(0, 1);
    Draw(ctx, g.t.rtv[1].Get(), aw, ah, ao_rect, g.blur.Get(), g.opaque.Get(), nullptr, nullptr, g.t.srv[2].Get());
    const bool replace = debug == 1 || debug == 2;
    Draw(ctx, p.color, float(p.width), float(p.height), host_rect, (debug == 2 ? g.depth_view : g.apply)[ms].Get(),
         replace ? g.opaque.Get() : g.multiply.Get(), p.depth, nullptr, g.t.srv[1].Get());
    if (ms && debug != 2)  // performance: the samples of edge pixels at another depth
        Draw(ctx, p.color, float(p.width), float(p.height), host_rect, g.apply_rest.Get(),
             replace ? g.opaque.Get() : g.multiply.Get(), p.depth, nullptr, g.t.srv[1].Get());
    ID3D11ShaderResourceView* none[3] = {};
    ctx->PSSetShaderResources(0, 3, none);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    if (g.timing && g.query_pending < 4) {
        ctx->End(g.end[g.query_next].Get());
        ctx->End(g.disjoint[g.query_next].Get());
        g.query_next = (g.query_next + 1) % 4;
        g.query_pending++;
    }
}

}  // namespace gpu::ao
