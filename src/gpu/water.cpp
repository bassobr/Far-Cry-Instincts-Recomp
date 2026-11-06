#include "gpu/water.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <thread>

#include "app/settings.h"
#include "gpu/fc1_data.h"
#include "gpu/vsh.h"
#include "runtime/log.h"

namespace gpu::water {

namespace {

std::atomic<int> g_live{-1};  // settings menu: -1 = as at start

// The surface, written for this port. The technique follows Far Cry (PC)'s ocean as its shader scripts describe it
// (studied, not copied): two layers of a large wave normal map at world scale scrolling against each other, a Fresnel
// term 1 / (1 + E.N)^8 between a low and a high reflection weight, a body colour between a deep and a light blue by the
// view angle, opacity growing with the water's depth (0.5 per metre), a sun highlight from the reflected view vector
// (power 64, warm). What the game hands over (see renderer.cpp): its reflection target (tex1) with its projection
// (refl_*), the vertex colour (the water colour the game animates per level and time of day: the brightness of ours),
// the fog of the vertex program, the sun's screen position (its sky sprite). Ours: the wave map (t6, Far Cry's
// Waterbump heights when the user has Far Cry installed, else our own sum of waves), the scene's depth (t4) and, for
// high, a resolved copy of the scene behind the water (t5).
const char* const kBody = R"(
cbuffer Pixel : register(b1) {
    float4 factor0[8];
    float4 factor1[8];
    float4 fog_color;
    float4 fog_params;
    float4 alpha_ref;
    float4 tex_scale[4];
    float4 bump[4];
    float4 final_factor[2];
    float4 depth_bias;
};
cbuffer Water : register(b4) {
    float4 eye;        // xyz: the camera in world space (the view projection's centre); w: seconds (wraps)
    float4 refl_s;     // the reflection target's coordinates of a world point: (s, t) / q
    float4 refl_t;
    float4 refl_q;
    float4 screen_px;  // xy: 1 / host size of the target (scene copy and depth)
    float4 view_uv;    // the 3D view's rectangle in the target (left, top, right, bottom), 0..1 (split screen)
    float4 depth_c;    // x: reversed depth constant c (stored = c / d), 0: stored = d; y: samples
    float4 tune;       // x: wave strength, y: reflection bend, z: refraction bend, w: shore width (m)
    float4 tune2;      // x: opacity per metre of depth, y: foam, z: sun, w: reflection scale
    float4 sun;        // xyz: direction to the sun (world), w: 1 when known
};
Texture2D tex1 : register(t1);
SamplerState smp1 : register(s1);
#if SAMPLES > 1
Texture2DMS<float> scene_depth : register(t4);
#else
Texture2D<float> scene_depth : register(t4);
#endif
Texture2D scene_color : register(t5);
Texture2D<float2> waves : register(t6);  // the height's gradient per texel, x 4 (tiles)
SamplerState smp_clamp : register(s4);
SamplerState smp_wave : register(s5);

// the guest's depth (w / zmax, linear in view depth) of what the scene has at a pixel; huge for the sky
float SceneD(float stored) {
    if (depth_c.x == 0) return stored >= 1 ? 1e6 : stored;
    return depth_c.x / max(stored, 1e-30);
}
// the farther of two opposite samples of the pixel: at edges the water covers the samples behind a foreground object's
// edge only (two opposite corners of the standard pattern catch an edge through the pixel, at half the loads of all)
float SceneDepthFar(int2 p) {
#if SAMPLES > 1
    return max(SceneD(scene_depth.Load(p, 0)), SceneD(scene_depth.Load(p, SAMPLES - 1)));
#else
    return SceneD(scene_depth.Load(int3(p, 0)));
#endif
}
float SceneDepthAt(int2 p) {
#if SAMPLES > 1
    return SceneD(scene_depth.Load(p, 0));
#else
    return SceneD(scene_depth.Load(int3(p, 0)));
#endif
}
float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

// the slope (dh/dx, dh/dy in metres per metre) of one layer of the wave map, tiling every 1 / freq metres
float2 Layer(float2 xy, float freq, float2 shift) {
    return waves.Sample(smp_wave, xy * freq + shift) * (freq * 128.0);  // gradient x 4 per texel of 512, height ~1 m
}
)";

const char* const kMain = R"(
float4 main(VSOut input) : SV_Target {
    float3 P = input.wpos.xyz;
    float3 toEye = eye.xyz - P;
    float dist = length(toEye);
    float3 V = toEye / max(dist, 1e-4);
    float above = max(toEye.z, 0.01);  // the eye's height over the surface

    // fog: as the game's combiners take it (the vertex program writes the fog factor)
    float fog_factor = 1;
    FOG_CODE

    // waves: two layers scrolling against each other (80 m and 118 m tiles, the second turned by swapping x and y),
    // a finer one for the near water and a long swell
    float t = eye.w;
    float2 scroll = float2(t, t) * 0.0025;
    float2 slope = 0.5 * (Layer(P.xy, 0.0125, scroll * 2.0) + Layer(P.yx, 0.0085, -scroll).yx);
    slope += 0.35 * Layer(P.yx, 0.031, float2(0.37, -0.21) + scroll * 3.0).yx;
    slope += 0.6 * Layer(P.xy, 0.0031, float2(-0.11, 0.23) + scroll * 0.7);
    slope *= tune.x;
    float3 N = normalize(float3(-slope, 1));
    float cosv = saturate(dot(N, V));

    // Fresnel: 1 / (1 + E.N)^8 between a low reflection looking down and a full one at grazing angles
    float f8 = 1.0 / pow(1.0 + cosv, 8.0);
    float3 refl_w = saturate(lerp(float3(0.10, 0.10, 0.05), float3(1.2, 1.2, 1.05), f8) * tune2.w);

    // reflection: the game's mirrored view at the exact projection of this point, bent by the waves
    float q = dot(float4(P, 1), refl_q);
    float2 ruv = float2(dot(float4(P, 1), refl_s), dot(float4(P, 1), refl_t)) / (abs(q) < 1e-6 ? 1e-6 : q);
    ruv += slope * tune.y * saturate(30.0 / max(dist, 1.0));
    float3 R = tex1.Sample(smp1, ruv).rgb;

    // the brightness of the game's water colour (its vertex colour, dark at night and in fog; 0.19 by day)
    float light = saturate(Luma(input.d0.rgb) / 0.19);

    // what lies behind the surface: the scene's depth against the water's along the same ray
    int2 px = int2(input.pos.xy);
    float dw = input.wdepth;
    float ds = SceneDepthFar(px);
    float r = max(ds / max(dw, 1e-9) - 1, 0);     // the scene point is (1 + r) times as far as the water point
    float h = above * r;                            // its depth below the surface (m)
    float L = min(dist * r, 1e4);                   // the light's path through the water (m)
    // opaque by the depth of the ground seen there (as Far Cry by the water's depth; at grazing angles the Fresnel
    // term hides the ground anyway)
    float opacity = saturate(h * tune2.x);
    // the water's own colour: deep blue seen at a grazing angle, a lighter blue-green looking down, greener over
    // shallow ground (the light the sand sends back up)
    float3 body = lerp(float3(0.04, 0.148, 0.26), float3(0.16, 0.38, 0.45), cosv);
    body = lerp(float3(0.13, 0.46, 0.47), body, saturate(h / 6.0)) * light;

    // the sun: the view reflected at the waves against the sun's direction, a glow and a sharp glitter
    float3 spec = 0;
    if (sun.w > 0) {
        float3 Rv = reflect(-V, N);
        float s = saturate(dot(Rv, sun.xyz));
        spec = (2.0 * pow(s, 64.0) * f8 + 6.0 * pow(s, 900.0)) * float3(1.0, 0.8, 0.5) * light * tune2.z;
        spec *= saturate(sun.z * 8.0);  // not below the horizon
    }

    // foam at the waterline (by the path through the water), broken up by the waves
    float foamy = (1 - smoothstep(0.0, 0.4, L)) * saturate(0.4 + 6.0 * (slope.x * slope.y + 0.05 * slope.x)) * tune2.y;
    // the shoreline: fades out over the last centimetres of the path instead of a hard edge
    float shore = saturate(L / max(tune.w, 1e-3));

#if QUALITY >= 2
    // refraction: the scene behind the water, displaced by the waves (less in shallow water and far away); what the
    // displaced point finds in front of the water (a pole, a leg) is not pulled in: then the undisplaced one
    float2 at = input.pos.xy * screen_px.xy;
    float2 shift = slope * tune.z * saturate(h * 0.7) / max(dist, 4.0);
    float2 at2 = clamp(at + shift * float2(screen_px.x / screen_px.y, 1), view_uv.xy + screen_px.xy,
                       view_uv.zw - screen_px.xy);
    float ds2 = SceneDepthAt(int2(at2 / screen_px.xy));
    if (ds2 <= dw * 1.002) at2 = at;
    float3 behind = scene_color.SampleLevel(smp_clamp, at2, 0).rgb;
    // the light from behind is fogged already; the water's own part gets the fog here
    float3 own = body * opacity * (1 - refl_w) + R * refl_w + spec + foamy * 0.8 * light;
    // and what comes through is tinted on its way (red goes first: shallow water over sand turns turquoise)
    float3 through = (1 - opacity) * (1 - refl_w) * (1 - foamy) * exp(-L * float3(0.30, 0.07, 0.06));
    float3 color = lerp(fog_color.rgb * (1 - through), own, fog_factor) + through * behind;
    return float4(color, shore);
#else
    // without a copy of the scene: blending does the light from behind (undisplaced, one weight for all channels)
    float3 own = body * opacity * (1 - refl_w) + R * refl_w + spec + foamy * 0.8 * light;
    float through = Luma((1 - opacity) * (1 - refl_w)) * (1 - foamy);
    float3 color = lerp(fog_color.rgb * (1 - through), own, fog_factor);
    float a = saturate(1 - through);
    return float4(color / max(a, 1e-3), a * shore);
#endif
}
)";

// ---- the wave map ----
WaveMap g_waves;
std::atomic<bool> g_waves_ready{false};

// heights (0..1, tiling) to gradients x 4 per texel, as R16G16 snorm, with a full mip chain (gradients average)
void Build(const std::vector<float>& height, uint32_t size) {
    std::vector<float> gx(size_t(size) * size), gy(gx.size());
    for (uint32_t y = 0; y < size; y++)
        for (uint32_t x = 0; x < size; x++) {
            auto at = [&](uint32_t xx, uint32_t yy) { return height[size_t(yy % size) * size + (xx % size)]; };
            gx[size_t(y) * size + x] = (at(x + 1, y) - at(x + size - 1, y)) * 0.5f;
            gy[size_t(y) * size + x] = (at(x, y + 1) - at(x, y + size - 1)) * 0.5f;
        }
    g_waves.size = size;
    g_waves.levels.clear();
    for (uint32_t s = size; s >= 1; s /= 2) {
        std::vector<uint32_t> level(size_t(s) * s);
        for (size_t i = 0; i < level.size(); i++) {
            auto pack = [](float v) { return uint32_t(int16_t(std::clamp(v * 4.0f, -1.0f, 1.0f) * 32767.0f)) & 0xFFFF; };
            level[i] = pack(gx[i]) | pack(gy[i]) << 16;
        }
        g_waves.levels.push_back(std::move(level));
        if (s == 1) break;
        // the next level: the average of 2x2 (still per texel of level 0: the shader scales by level 0's density)
        const uint32_t n = s / 2;
        std::vector<float> nx(size_t(n) * n), ny(nx.size());
        for (uint32_t y = 0; y < n; y++)
            for (uint32_t x = 0; x < n; x++) {
                const size_t a = size_t(2 * y) * s + 2 * x, b = a + 1, c = a + s, d = c + 1;
                nx[size_t(y) * n + x] = (gx[a] + gx[b] + gx[c] + gx[d]) * 0.25f;
                ny[size_t(y) * n + x] = (gy[a] + gy[b] + gy[c] + gy[d]) * 0.25f;
            }
        gx = std::move(nx), gy = std::move(ny);
    }
}

// our own waves without Far Cry: a sum of sine waves with whole wave numbers per tile (so the map tiles), many
// directions and lengths with a falling spectrum, random phases; no grid, no cells
void Procedural(uint32_t size, std::vector<float>& height) {
    height.assign(size_t(size) * size, 0.0f);
    uint32_t seed = 0x9E3779B9u;
    auto rnd = [&] {
        seed ^= seed << 13, seed ^= seed >> 17, seed ^= seed << 5;
        return float(seed) / 4294967296.0f;
    };
    struct Wave { float kx, ky, amp, phase; };
    std::vector<Wave> list;
    for (int i = 0; i < 96; i++) {
        const float k = 5.0f + 60.0f * std::pow(rnd(), 1.4f);  // wave number per tile
        const float a = rnd() * 6.2831853f;
        Wave w{std::round(k * std::cos(a)), std::round(k * std::sin(a)), 0, rnd() * 6.2831853f};
        const float kk = std::sqrt(w.kx * w.kx + w.ky * w.ky);
        if (kk < 1.0f) continue;
        w.amp = 1.0f / std::pow(kk, 1.3f);
        list.push_back(w);
    }
    float lo = 1e30f, hi = -1e30f;
    for (uint32_t y = 0; y < size; y++)
        for (uint32_t x = 0; x < size; x++) {
            float v = 0;
            const float u = float(x) / float(size) * 6.2831853f, t = float(y) / float(size) * 6.2831853f;
            for (const Wave& w : list) v += w.amp * std::sin(w.kx * u + w.ky * t + w.phase);
            // sharper crests than a sine (choppy): fold a little
            v = v - 0.25f * std::fabs(v);
            height[size_t(y) * size + x] = v;
            lo = std::min(lo, v), hi = std::max(hi, v);
        }
    for (float& v : height) v = (v - lo) / std::max(hi - lo, 1e-6f);
    // as steep as Far Cry's Waterbump on average (its mean gradient is 0.064 per texel of 512)
    double sum = 0;
    for (uint32_t y = 0; y < size; y++)
        for (uint32_t x = 0; x < size; x++) {
            const float gx = height[size_t(y) * size + (x + 1) % size] - height[size_t(y) * size + (x + size - 1) % size];
            const float gy = height[size_t((y + 1) % size) * size + x] - height[size_t((y + size - 1) % size) * size + x];
            sum += 0.5 * std::sqrt(double(gx) * gx + double(gy) * gy);
        }
    const float k = float(0.064 / std::max(sum / (double(size) * size), 1e-9));
    for (float& v : height) v *= k;
}

void MakeWaves() {
    std::vector<uint8_t> dds, gray;
    uint32_t w = 0, h = 0;
    std::vector<float> height;
    if (fc1::ReadPakFile("Textures.pak", "Textures/Waterbump.dds", dds) && fc1::DdsGray(dds, w, h, gray) && w == h &&
        (w & (w - 1)) == 0) {
        height.resize(gray.size());
        for (size_t i = 0; i < gray.size(); i++) height[i] = gray[i] / 255.0f;
        g_waves.from_fc1 = true;
        rt::Log("water: wave normals from Far Cry's Waterbump (%ux%u)", w, h);
    } else {
        w = 512;
        Procedural(w, height);
        rt::Log("water: wave normals of our own (%ux%u, no Far Cry data)", w, w);
    }
    Build(height, w);
    g_waves_ready = true;
}

}  // namespace

int BootQuality() {
    static const int quality = [] {
        std::string v = settings::String("water_quality", "high");
        for (char& ch : v) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        int q = v == "console" || v == "0" ? kConsole : v == "medium" || v == "1" ? kMedium : kHigh;
        rt::Log("water: %s", q == kConsole ? "console (the game's combiners)" : q == kMedium ? "medium" : "high");
        return q;
    }();
    return quality;
}

int Current() {
    const int q = g_live.load(std::memory_order_relaxed);
    return q >= 0 ? q : BootQuality();
}

bool SetQuality(int quality) {
    g_live = std::clamp(quality, 0, 2);
    rt::Log("water: %s (settings menu)", quality == kConsole ? "console" : quality == kMedium ? "medium" : "high");
    return true;
}

void Prepare() {
    static std::once_flag once;
    std::call_once(once, [] { std::thread(MakeWaves).detach(); });
}

const WaveMap* Waves() { return g_waves_ready.load() ? &g_waves : nullptr; }

std::string PixelShader(int quality, uint32_t samples, uint32_t fog_mode) {
    char defines[96];
    snprintf(defines, sizeof defines, "#define QUALITY %d\n#define SAMPLES %u\n", quality, samples);
    std::string out = defines;
    out += kBody;
    // the vertex shader's outputs (vsh.h) and the world position the water's vertex shader adds
    std::string input = vsh::kOutputStruct;
    input.insert(input.rfind("};"), "    float4 wpos : WPOS;\n");
    out += input;
    std::string main = kMain;
    // fog as psh.cpp computes it from the fog coordinate
    std::string fog;
    if (fog_mode) {
        const bool abs_mode = fog_mode == 0x802 || fog_mode == 0x803 || fog_mode == 0x804;
        fog = abs_mode ? "float fd = abs(input.fog.x);\n" : "float fd = input.fog.x;\n";
        if (fog_mode == 0x800 || fog_mode == 0x802)
            fog += "    fog_factor = fog_params.x + exp2(fd * fog_params.y * 16.0) - 1.5;\n";
        else if (fog_mode == 0x801 || fog_mode == 0x803)
            fog += "    fog_factor = fog_params.x + exp2(-fd * fd * fog_params.y * fog_params.y * 32.0) - 1.5;\n";
        else
            fog += "    fog_factor = fog_params.x + fd * fog_params.y - 1.0;\n";
        fog += "    fog_factor = saturate(fog_factor);\n";
    }
    main.replace(main.find("FOG_CODE"), 8, fog);
    out += main;
    return out;
}

}  // namespace gpu::water
