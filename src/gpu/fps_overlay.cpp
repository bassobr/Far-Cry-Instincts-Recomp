// fps_counter = 1, see fps_overlay.h. Averages over half a second: frames per second, mean and longest frame time.
#include "gpu/fps_overlay.h"

#include <windows.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include "app/settings.h"
#include "runtime/log.h"

namespace gpu::fps {

namespace {

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

// 5x7 glyphs, a row per byte, bit 4 = left column
struct Glyph {
    char c;
    uint8_t rows[7];
};
constexpr Glyph kFont[] = {
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}}, {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}}, {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}}, {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}}, {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}}, {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}}, {'/', {0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10}},
    {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}}, {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}}, {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}}, {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
};
constexpr uint32_t kCell = 6, kLine = 9, kChars = 32;          // glyph advance, line height, characters at most
constexpr uint32_t kTexW = kChars * kCell + 3, kTexH = kLine;  // a texel of margin all round

const char kShaders[] = R"(
cbuffer Rect : register(b0) { float4 rect; float4 used; };  // NDC x0 y0 x1 y1; the texture's used share
Texture2D<float> text : register(t0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs_main(uint id : SV_VertexID) {
    float2 t = float2(id & 1, id >> 1);
    V o;
    o.pos = float4(lerp(rect.xy, rect.zw, t), 0, 1);
    o.uv = t;
    return o;
}
// 1: glyph (white), 0.5: its shadow (black), 0: background (dark, translucent)
float4 ps_main(V i) : SV_Target {
    uint w, h;
    text.GetDimensions(w, h);
    float v = text.Load(int3(min(uint2(i.uv * used.xy * float2(w, h)), uint2(w - 1, h - 1)), 0));
    return v > 0.75 ? float4(1, 1, 1, 1) : v > 0.25 ? float4(0, 0, 0, 1) : float4(0, 0, 0, 0.45);
}
)";

struct State {
    bool init = false, enabled = false, failed = false;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11Buffer> cb;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11DepthStencilState> no_depth;
    uint32_t width = 0;  // texels of the current text, margin included
    // measurement
    Clock::time_point window_start{}, last{};
    uint32_t frames = 0;
    double longest = 0;
};
State g;

bool InitPipeline(ID3D11Device* device) {
    ComPtr<ID3DBlob> code, errors;
    auto compile = [&](const char* entry, const char* profile) {
        code.Reset(), errors.Reset();
        HRESULT hr = D3DCompile(kShaders, sizeof kShaders - 1, "fps", nullptr, nullptr, entry, profile, 0, 0, &code,
                                &errors);
        if (FAILED(hr)) rt::Log("fps: %s: %s", entry, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        return SUCCEEDED(hr);
    };
    if (!compile("vs_main", "vs_5_0") ||
        FAILED(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g.vs)))
        return false;
    if (!compile("ps_main", "ps_5_0") ||
        FAILED(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g.ps)))
        return false;
    D3D11_TEXTURE2D_DESC td{kTexW, kTexH, 1, 1, DXGI_FORMAT_R8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                            D3D11_BIND_SHADER_RESOURCE, 0, 0};
    if (FAILED(device->CreateTexture2D(&td, nullptr, &g.tex))) return false;
    device->CreateShaderResourceView(g.tex.Get(), nullptr, &g.srv);
    D3D11_BUFFER_DESC cb{32, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    device->CreateBuffer(&cb, nullptr, &g.cb);
    D3D11_BLEND_DESC bd{};
    auto& rt = bd.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_SRC_ALPHA, rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA, rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE, rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA, rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device->CreateBlendState(&bd, &g.blend);
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID, rd.CullMode = D3D11_CULL_NONE;
    device->CreateRasterizerState(&rd, &g.raster);
    D3D11_DEPTH_STENCIL_DESC dd{};
    device->CreateDepthStencilState(&dd, &g.no_depth);
    return g.vs && g.ps && g.srv && g.cb && g.blend && g.raster && g.no_depth;
}

// the text into the texture: 255 glyph, 128 its shadow one texel down and right, 0 background
void SetText(ID3D11DeviceContext* context, const char* text) {
    static uint8_t texels[kTexH][kTexW];
    std::memset(texels, 0, sizeof texels);
    uint32_t n = uint32_t(std::min<size_t>(std::strlen(text), kChars));
    for (int pass = 0; pass < 2; pass++)  // shadows first, glyphs over them
        for (uint32_t i = 0; i < n; i++) {
            const Glyph* glyph = nullptr;
            for (const Glyph& f : kFont)
                if (f.c == text[i]) glyph = &f;
            if (!glyph) continue;
            for (uint32_t y = 0; y < 7; y++)
                for (uint32_t x = 0; x < 5; x++)
                    if (glyph->rows[y] & (0x10 >> x))
                        texels[1 + y + (pass ? 0 : 1)][1 + i * kCell + x + (pass ? 0 : 1)] = pass ? 255 : 128;
        }
    context->UpdateSubresource(g.tex.Get(), 0, nullptr, texels, kTexW, 0);
    g.width = std::min(kTexW, n * kCell + 2);
}

}  // namespace

bool Enabled() {
    if (!g.init) {
        g.init = true;
        g.enabled = settings::Bool("fps_counter", false);
        if (g.enabled) rt::Log("fps: counter on");
    }
    return g.enabled;
}

// settings menu: on or off from the next frame; the counting starts again
void SetEnabled(bool on) {
    Enabled();
    if (g.enabled == on) return;
    g.enabled = on;
    g.last = Clock::time_point{};
    rt::Log("fps: counter %s (settings menu)", on ? "on" : "off");
}

void Draw(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11RenderTargetView* target, uint32_t w, uint32_t h) {
    if (!Enabled() || g.failed || !target || !w || !h) return;
    if (!g.vs && !InitPipeline(device)) {
        g.failed = true;
        rt::Log("fps: counter not available");
        return;
    }
    Clock::time_point now = Clock::now();
    if (g.last == Clock::time_point{}) {
        g.window_start = g.last = now;
        SetText(context, "FPS");
    } else {
        g.longest = std::max(g.longest, std::chrono::duration<double, std::milli>(now - g.last).count());
        g.last = now;
        g.frames++;
        double span = std::chrono::duration<double>(now - g.window_start).count();
        if (span >= 0.5) {
            char text[kChars + 1];
            std::snprintf(text, sizeof text, "%.1f FPS  %.1f MS  MAX %.1f", g.frames / span, span * 1000 / g.frames,
                          g.longest);
            SetText(context, text);
            g.window_start = now, g.frames = 0, g.longest = 0;
        }
    }
    // integer texel size: about a 60th of the picture's height a line, 8 pixels in from the corner
    float px = float(std::max(1u, (h + 270) / 540));
    float x0 = 8 * px, y0 = 8 * px, x1 = x0 + g.width * px, y1 = y0 + kTexH * px;
    const float rect[8] = {x0 / w * 2 - 1, 1 - y0 / h * 2, x1 / w * 2 - 1, 1 - y1 / h * 2, float(g.width) / kTexW, 1, 0, 0};
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(context->Map(g.cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return;
    std::memcpy(map.pData, rect, sizeof rect);
    context->Unmap(g.cb.Get(), 0);
    D3D11_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
    context->RSSetViewports(1, &vp);
    context->OMSetRenderTargets(1, &target, nullptr);
    context->OMSetBlendState(g.blend.Get(), nullptr, 0xFFFFFFFF);
    context->OMSetDepthStencilState(g.no_depth.Get(), 0);
    context->RSSetState(g.raster.Get());
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context->VSSetShader(g.vs.Get(), nullptr, 0);
    context->PSSetShader(g.ps.Get(), nullptr, 0);
    ID3D11Buffer* cb = g.cb.Get();
    context->VSSetConstantBuffers(0, 1, &cb);
    context->PSSetConstantBuffers(0, 1, &cb);
    ID3D11ShaderResourceView* srv = g.srv.Get();
    context->PSSetShaderResources(0, 1, &srv);
    context->Draw(4, 0);
    ID3D11ShaderResourceView* none = nullptr;
    context->PSSetShaderResources(0, 1, &none);
    ID3D11Buffer* no_cb = nullptr;
    context->PSSetConstantBuffers(0, 1, &no_cb);
}

}  // namespace gpu::fps
