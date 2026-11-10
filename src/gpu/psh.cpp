#include "gpu/psh.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gpu/vsh.h"
#include "runtime/log.h"

namespace gpu::psh {

namespace {

using namespace kelvin;

int g_shadow_filter = 0;  // PC shadows (SetShadowFilter)
// GitHub #19: FCI_TARGET_MS_DEBUG (diagnostics) colours what reads the multisampled target being drawn (kTargetMS):
// 1 green where a pixel reads its own position (its covered samples), red where it filters the average;
// 2 rg = the coordinates' offset from the pixel centre (0.5 + offset / 2, in host pixels), b = own position;
// 3 r = a partly covered pixel, g = the share of the samples read, blue where it filters the average;
// 4 such draws dimmed, red = how much the value read differs from the resolved copy's (x 8), blue = filtered
const int g_target_ms_debug = [] {
    char v[8] = {};
    return GetEnvironmentVariableA("FCI_TARGET_MS_DEBUG", v, sizeof v) ? atoi(v) : 0;
}();
const int g_shadow_debug = [] {  // PC shadows: FCI_SHADOW_DEBUG (diagnostics)
    char v[8] = {};
    return GetEnvironmentVariableA("FCI_SHADOW_DEBUG", v, sizeof v) ? atoi(v) : 0;
}();

// PC shadows, diagnostics: FCI_CASCADE_TAPS=0 maps each shadow stage's coordinates into the other cascade on its own,
// as 0.1.7 did (A/B for the tap correction, see "cascades" in Generate)
const bool g_cascade_taps = [] {
    char v[8] = {};
    return !(GetEnvironmentVariableA("FCI_CASCADE_TAPS", v, sizeof v) && v[0] == '0');
}();
// PC shadows, diagnostics: FCI_SOLIDS_SOFT=1 reads the cascades' map of solid casters with the soft filter, as the
// first version of that map did (A/B for the cheaper lookup)
const bool g_solids_soft = [] {
    char v[8] = {};
    return GetEnvironmentVariableA("FCI_SOLIDS_SOFT", v, sizeof v) && v[0] == '1';
}();
// PC shadows, diagnostics (issue 16 analysis): FCI_CASCADE_FORCE=near|far: receivers with cascades read only that map
// (near: also outside the near map's square), to tell which lookup changes between near and far frames
const int g_cascade_force = [] {
    char v[8] = {};
    if (!GetEnvironmentVariableA("FCI_CASCADE_FORCE", v, sizeof v)) return 0;
    return v[0] == 'n' ? 1 : v[0] == 'f' ? 2 : 0;
}();

const char* const kPrelude = R"(
cbuffer Pixel : register(b1) {
    float4 factor0[8];
    float4 factor1[8];
    float4 fog_color;
    float4 fog_params;
    float4 alpha_ref;
    float4 tex_scale[4];  // 1 / size for textures addressed in texels
    float4 bump[4];       // bump environment matrices
    float4 final_factor[2];
    float4 depth_bias;    // W-buffered draws: polygon offset units (in depth) and slope factor
};

float2 signed2(float2 x) {
    float2 v = round(x * 255);
    return (v >= 128 ? v - 256 : v) / 127;
}
// signed textures (GitHub #14): the same for a filtered signed texel (fractions kept; 127..128 does not occur)
float2 signed2f(float2 x) {
    float2 v = x * 255;
    return (v > 127.5 ? v - 256 : v) / 127;
}
)";

const char* RegisterName(uint32_t reg) {
    switch (reg) {
    case 1: return "c0";
    case 2: return "c1";
    case 3: return "fogr";
    case 4: return "v0";
    case 5: return "v1";
    case 8: return "t0";
    case 9: return "t1";
    case 10: return "t2";
    case 11: return "t3";
    case 12: return "r0";
    case 13: return "r1";
    default: return nullptr;
    }
}

// one combiner input byte: register, alpha or color, mapping
std::string Input(uint32_t byte, bool alpha_portion) {
    const char* reg = RegisterName(byte & 0xF);
    bool alpha = byte & 0x10;
    std::string x;
    if (!reg)
        x = alpha_portion ? "0.0" : "float3(0, 0, 0)";
    else
        x = std::string(reg) + (alpha_portion ? (alpha ? ".a" : ".b") : (alpha ? ".aaa" : ".rgb"));
    switch (byte >> 5) {
    case 0: return "max(" + x + ", 0)";
    case 1: return "(1 - saturate(" + x + "))";
    case 2: return "(2 * max(" + x + ", 0) - 1)";
    case 3: return "(1 - 2 * max(" + x + ", 0))";
    case 4: return "(max(" + x + ", 0) - 0.5)";
    case 5: return "(0.5 - max(" + x + ", 0))";
    case 6: return "(" + x + ")";
    default: return "(-" + x + ")";
    }
}

// output shift/bias
void Op(uint32_t op, const char*& bias, const char*& scale) {
    bias = "0", scale = "1";
    switch (op) {
    case 1: bias = "-0.5"; break;
    case 2: scale = "2"; break;
    case 3: bias = "-0.5", scale = "2"; break;
    case 4: scale = "4"; break;
    case 6: scale = "0.5"; break;
    default: break;
    }
}

std::string FinalInput(uint32_t byte, bool alpha_portion) {
    uint32_t reg = byte & 0xF;
    bool alpha = byte & 0x10, invert = byte & 0x20;
    std::string src;
    if (reg == 14)
        src = "sum";
    else if (reg == 15)
        src = "ef";
    else if (const char* name = RegisterName(reg))
        src = name;
    else
        src = "float4(0, 0, 0, 0)";
    std::string x = "saturate(" + src + (alpha_portion ? (alpha ? ".a" : ".b") : (alpha ? ".aaa" : ".rgb")) + ")";
    return invert ? "(1 - " + x + ")" : x;
}

const char* Compare(uint32_t func) {
    switch (func & 7) {
    case 0: return "false";
    case 1: return "a < ref";
    case 2: return "a == ref";
    case 3: return "a <= ref";
    case 4: return "a > ref";
    case 5: return "a != ref";
    case 6: return "a >= ref";
    default: return "true";
    }
}

}  // namespace

// ---- PC shadows ----
void SetShadowFilter(int filter) { g_shadow_filter = filter; }
// ---- end PC shadows ----

uint64_t Key(const State& s, const Config& config) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint32_t v) {
        for (int i = 0; i < 4; i++) h = (h ^ ((v >> (8 * i)) & 0xFF)) * 1099511628211ull;
    };
    uint32_t control = s.Reg(SET_COMBINER_CONTROL);
    uint32_t stages = control & 0xFF;
    mix(control);
    for (uint32_t i = 0; i < stages && i < 8; i++) {
        mix(s.Reg(SET_COMBINER_COLOR_ICW + 4 * i)), mix(s.Reg(SET_COMBINER_COLOR_OCW + 4 * i));
        mix(s.Reg(SET_COMBINER_ALPHA_ICW + 4 * i)), mix(s.Reg(SET_COMBINER_ALPHA_OCW + 4 * i));
    }
    mix(s.Reg(SET_COMBINER_SPECULAR_FOG_CW0)), mix(s.Reg(SET_COMBINER_SPECULAR_FOG_CW1));
    mix(s.Reg(SET_SHADER_STAGE_PROGRAM)), mix(s.Reg(SET_SHADER_OTHER_STAGE_INPUT));
    mix(s.Reg(SET_SHADER_CLIP_PLANE_MODE));
    bool alpha_test = s.Reg(SET_ALPHA_TEST_ENABLE);
    mix(alpha_test ? s.Reg(SET_ALPHA_FUNC) : 0xFFFFFFFF);
    bool fog = s.Reg(SET_FOG_ENABLE);
    mix(fog ? s.Reg(SET_FOG_MODE) : 0xFFFFFFFF);
    mix(s.Reg(SET_SHADE_MODE)), mix(s.Reg(SET_TWO_SIDE_LIGHT_EN));
    mix(s.Reg(SET_CONTROL0) & kControl0WBuffer);
    mix(uint32_t(config.depth));  // performance: reversed depth
    uint32_t alpha_kill = 0;
    for (int i = 0; i < 4; i++) alpha_kill |= ((s.Reg(SET_TEXTURE_CONTROL0 + 0x40 * i) & kTextureAlphaKill) ? 1u : 0u) << i;
    mix(alpha_kill);
    bool shadow = false;
    for (int i = 0; i < 4; i++) {
        const Sampler k = config.sampler[i];
        mix(uint32_t(k));
        shadow |= k == Sampler::kShadow || k == Sampler::kSceneDepth || k == Sampler::kSceneDepthMS;
        if (k == Sampler::kSceneDepth || k == Sampler::kSceneDepthMS || k == Sampler::kTargetMS)  // point or linear
            mix((s.Reg(SET_TEXTURE_FILTER + 0x40 * i) >> 24) & 0xF);
    }
    if (shadow) mix(s.Reg(SET_SHADOW_COMPARE_FUNC));
    if (shadow && config.cascade) mix(0xCA5CADE0u | config.cascade);  // PC shadows: cascades (the stages)
    // signed textures (GitHub #14)
    const uint32_t signs = config.signs[0] | config.signs[1] << 4 | config.signs[2] << 8 | config.signs[3] << 12;
    if (signs) mix(0x5167u << 16 | signs);
    bool target_ms = false;  // GitHub #19
    for (int i = 0; i < 4; i++) target_ms |= config.sampler[i] == Sampler::kTargetMS;
    static const bool plain = GetEnvironmentVariableA("FCI_TARGET_MS_PLAIN", nullptr, 0) > 0;
    if (target_ms) mix(0x19u << 8 | config.target_ms_depth | (plain ? 0x80u : 0u));
    if (config.mirror_write || config.mirror_read)  // GitHub #19: the target's mirror
        mix(0x1919u << 16 | uint32_t(config.mirror_write) << 8 | uint32_t(config.mirror_resolved) << 12 |
            config.mirror_read);
    return h;
}

// ---- GitHub #19, the target's mirror ----
bool StageAlphaUsed(const State& s, int stage) {
    if (stage == 0) return true;  // (r0's alpha starts as t0's)
    const uint32_t reg = 8 + uint32_t(stage);
    auto alpha_of = [&](uint32_t byte) { return (byte & 0xF) == reg && (byte & 0x10); };  // .aaa / .a
    const uint32_t control = s.Reg(SET_COMBINER_CONTROL), stages = control & 0xFF;
    for (uint32_t i = 0; i < stages && i < 8; i++) {
        const uint32_t c = s.Reg(SET_COMBINER_COLOR_ICW + 4 * i), a = s.Reg(SET_COMBINER_ALPHA_ICW + 4 * i);
        for (int k = 0; k < 32; k += 8)
            if (alpha_of(c >> k) || alpha_of(a >> k)) return true;
    }
    const uint32_t cw0 = s.Reg(SET_COMBINER_SPECULAR_FOG_CW0), cw1 = s.Reg(SET_COMBINER_SPECULAR_FOG_CW1);
    for (int k = 0; k < 32; k += 8)
        if (alpha_of(cw0 >> k)) return true;
    if (alpha_of(cw1 >> 24) || alpha_of(cw1 >> 16) || alpha_of(cw1 >> 8)) return true;
    if (s.Reg(SET_TEXTURE_CONTROL0 + 0x40 * stage) & kTextureAlphaKill) return true;
    // a later stage that reads this one (dependent lookups): only those through blue and green leave the alpha alone
    const uint32_t programs = s.Reg(SET_SHADER_STAGE_PROGRAM), other = s.Reg(SET_SHADER_OTHER_STAGE_INPUT);
    for (int j = stage + 1; j < 4; j++) {
        const uint32_t program = (programs >> (5 * j)) & 0x1F;
        const int prev = j == 1 ? 0 : j == 2 ? int((other >> 16) & 0xF) : int((other >> 20) & 0xF);
        if (program >= 6 && prev == stage && program != 6 && program != 7 && program != 16) return true;
    }
    return false;
}
// ---- end GitHub #19 ----

std::string Generate(const State& s, const Config& config) {
    std::string out = kPrelude;
    static const char* const kTextureType[] = {"Texture2D",        "Texture2D", "TextureCube", "Texture3D",
                                               "Texture2D",        "Texture2D<float>", "Texture2DMS<float>",
                                               "Texture2DMS<float4>"};
    for (int i = 0; i < 4; i++) {
        char line[128];
        snprintf(line, sizeof line, "%s tex%d : register(t%d);\nSamplerState smp%d : register(s%d);\n",
                 kTextureType[int(config.sampler[i])], i, i, i, i);
        out += line;
    }
    // PC shadows: cascades, the stages that compare against the sun's map (projective stages on it)
    const uint32_t stage_programs = s.Reg(SET_SHADER_STAGE_PROGRAM);
    auto cascade_stage = [&](int i) {
        const uint32_t program = (stage_programs >> (5 * i)) & 0x1F;
        return ((config.cascade >> i) & 1) && config.sampler[i] == Sampler::kShadow && (program == 1 || program == 2);
    };
    bool cascade = false;
    for (int i = 0; i < 4; i++) cascade |= cascade_stage(i);
    bool target_ms_depth = false;  // GitHub #19: the depth target, for kTargetMS stages
    for (int i = 0; i < 4; i++) target_ms_depth |= config.target_ms_depth && config.sampler[i] == Sampler::kTargetMS;
    if (target_ms_depth) out += "Texture2DMS<float> target_depth : register(t7);\n";
    if (config.mirror_write || config.mirror_read)  // GitHub #19: the target's mirror (renderer.cpp)
        out += "cbuffer TargetMirror : register(b6) {\n"
               "    float4 mirror_id;  // x: the marker this draw writes into the mirror, y: the one its stages read\n"
               "};\n";
    if (config.mirror_read && config.mirror_resolved)  // the target's resolved copy for the filter (renderer.cpp)
        out += "Texture2D<float4> target_resolved : register(t6);\n";
    if (config.mirror_read && (g_target_ms_debug == 6 || g_target_ms_debug == 7))  // (diagnostics: the copy as well)
        out += "Texture2DMS<float4> target_copy_dbg : register(t8);\n";
    if (cascade)
        // the near cascade: a second map over part of the shadow map's square (renderer.cpp, "shadow cascades")
        out += "Texture2D tex_near : register(t4);\n"
               "Texture2D tex_far : register(t5);\n"
               "Texture2D tex_solid : register(t6);\n"
               "cbuffer Cascade : register(b5) {\n"
               "    float4 cascade_n[3];  // near map (u, v, depth) = cascade_n x the receiver's (u, v, depth, 1)\n"
               "    float4 cascade_f[3];  // far map's\n"
               "    float4 cascade_fade;  // x: 1 / the fading edge in near map coordinates, y: 1 with tex_solid\n"
               "    float4 cascade_s;     // tex_solid (the far pass's solid casters) at (far map uv - xy) * zw\n"
               "};\n";

    // the vertex shader's outputs, with flat colors when the shade mode asks for them
    std::string input = vsh::kOutputStruct;
    if (s.Reg(SET_SHADE_MODE) == 0x1D00) {
        for (const char* member : {"float4 d0", "float4 d1", "float4 b0", "float4 b1"}) {
            size_t at = input.find(member);
            if (at != std::string::npos) input.insert(at, "nointerpolation ");
        }
    }
    // the centroid copy of the W-buffer depth (below): as an interpolation mode, not EvaluateAttributeCentroid, which
    // Wine's / Proton's builtin HLSL compiler (vkd3d-shader) lacks
    input.insert(input.find("float4 wdepth_c"), "centroid ");
    out += input;
    // W-buffering: the depth is w (scaled to the depth range by D3D's projection), linear in eye space, which
    // keeps far geometry apart (z/w in 24 bits cannot: water and shore fought at a distance). w / zmax is tiny
    // up close, so the depth targets are float (renderer.cpp, GetDepth)
    // The depth is taken at the centroid, not the pixel centre: with multisampling a pixel the triangle covers only
    // partly (not its centre) still runs the shader, and the centre's value is extrapolated. The water's projected
    // grid has slivers along the horizon whose extrapolated w went to 0 there: they passed the depth test over walls
    // as a bright 1 pixel line across the view's middle (split screen, MSAA). Without MSAA both are the same.
    // performance: with the reversed depth (renderer.cpp) a W-buffered draw without polygon offset writes no depth:
    // the rasterizer's c / (w / zmax) is exact per sample, and early depth tests stay on (the scene's pass ran 40 %
    // shorter on the Steam Deck's GPU)
    char c_text[32];
    snprintf(c_text, sizeof c_text, "%.9g", kReversedDepth);
    const std::string c_value = c_text;
    // GitHub #19: a stage on the multisampled target being drawn reads the samples the fragment covers (the
    // rasterizer's coverage, an input: the shader still runs once per pixel)
    bool target_ms = false;
    for (int i = 0; i < 4; i++) target_ms |= config.sampler[i] == Sampler::kTargetMS;
    const std::string head = std::string("\nfloat4 main(VSOut input, bool front : SV_IsFrontFace") +
                             (target_ms ? ", uint coverage : SV_Coverage" : "") +
                             (config.mirror_write ? ", out float4 mirror_out : SV_Target1" : "");  // the mirror
    switch (config.depth) {
    case Depth::kW:
        out += head + ", out float depth : SV_Depth) : SV_Target {\n"
               "    depth = saturate(input.wdepth_c.x + depth_bias.x + depth_bias.y * "
               "max(abs(ddx(input.wdepth)), abs(ddy(input.wdepth))));\n";
        break;
    case Depth::kReversedW:
        out += head + ", out float depth : SV_Depth) : SV_Target {\n"
               "    depth = " + c_value + " / max(input.wdepth_c.x + depth_bias.x + depth_bias.y * "
               "max(abs(ddx(input.wdepth)), abs(ddy(input.wdepth))), " + c_value + ");\n";
        break;
    case Depth::kReversedZ:
        out += head + ", out float depth : SV_Depth) : SV_Target {\n"
               "    depth = " + c_value + " / max(input.pos.z + depth_bias.x + depth_bias.y * "
               "max(abs(ddx(input.pos.z)), abs(ddy(input.pos.z))), " + c_value + ");\n";
        break;
    default:
        out += head + ") : SV_Target {\n";
        break;
    }
    if (g_target_ms_debug) out += "    float4 target_ms_dbg = -1;\n";  // GitHub #19: FCI_TARGET_MS_DEBUG
    out += "    float4 T[4] = {input.t0, input.t1, input.t2, input.t3};\n";
    if (s.Reg(SET_TWO_SIDE_LIGHT_EN))
        out += "    float4 v0 = saturate(front ? input.d0 : input.b0), v1 = saturate(front ? input.d1 : input.b1);\n";
    else
        out += "    float4 v0 = saturate(input.d0), v1 = saturate(input.d1);\n";

    // fog factor from the fog coordinate the vertex shader wrote
    out += "    float fog_factor = 1;\n";
    static const bool no_fog = GetEnvironmentVariableA("FCI_NO_FOG", nullptr, 0) > 0;  // diagnostics
    if (s.Reg(SET_FOG_ENABLE) && !no_fog) {
        uint32_t mode = s.Reg(SET_FOG_MODE);
        bool abs_mode = mode == 0x802 || mode == 0x803 || mode == 0x804;
        out += abs_mode ? "    float fd = abs(input.fog.x);\n" : "    float fd = input.fog.x;\n";
        if (mode == 0x800 || mode == 0x802)
            out += "    fog_factor = fog_params.x + exp2(fd * fog_params.y * 16.0) - 1.5;\n";
        else if (mode == 0x801 || mode == 0x803)
            out += "    fog_factor = fog_params.x + exp2(-fd * fd * fog_params.y * fog_params.y * 32.0) - 1.5;\n";
        else
            out += "    fog_factor = fog_params.x + fd * fog_params.y - 1.0;\n";
        out += "    fog_factor = saturate(fog_factor);\n";
    }
    out += "    float4 fogr = float4(fog_color.rgb, fog_factor);\n";

    // texture shader stages
    uint32_t programs = s.Reg(SET_SHADER_STAGE_PROGRAM);
    uint32_t other = s.Reg(SET_SHADER_OTHER_STAGE_INPUT);
    uint32_t clip_mode = s.Reg(SET_SHADER_CLIP_PLANE_MODE);
    out += "    float4 t0 = 0, t1 = 0, t2 = 0, t3 = 0;\n";
    // ---- PC shadows: cascades, the taps (GitHub #16, flicker after 0.1.7). A receiver may compare against the sun's
    // map in several stages: the terrain does in two (the game's matrices for stage 0 and 3 differ only by +-0.35
    // guest texels in u and v about the pixel centre: a two tap filter in texels of the map). Each stage's
    // coordinates are the sun camera's for the point plus an offset of its own in the game's texels; in the other
    // cascade the same stage must read the same point with the same offset in that map's texels. Mapping every
    // stage's coordinates through the cameras' affine map (as 0.1.7 did) scales the offsets by the ratio of the
    // squares (k in the near map, 1/k in the far one): in every other frame the taps read other texels, the terrain's
    // pattern of leaf shadows changed from frame to frame at a low sun (stretched along the ground by 1 / sin of its
    // height). So the centroid of the stages' coordinates is mapped as the point, and each stage keeps its own offset
    // from it, taken from its own coordinates (no assumption about how many stages or how far apart). The renderer's
    // map takes the receivers' coordinates as the camera's plus the pixel centre (renderer.cpp), so this is exact
    // where the stages' offsets are centred on it: the game's single stage receivers (objects) address the map at
    // the pixel centre, the terrain's two taps symmetrically about it. One stage: the same as mapping it directly.
    // FCI_CASCADE_TAPS=0 (diagnostics) maps every stage on its own as before ----
    if (cascade && g_cascade_taps) {
        int taps = 0;
        out += "    float2 cascade_base = 0;\n";
        for (int i = 0; i < 4; i++) {
            if (!cascade_stage(i)) continue;
            char line[192];
            snprintf(line, sizeof line, "    cascade_base += T[%d].xy / (T[%d].w == 0 ? 1 : T[%d].w) * tex_scale[%d].xy;\n",
                     i, i, i, i);
            out += line;
            taps++;
        }
        if (taps > 1) out += "    cascade_base /= " + std::to_string(taps) + ";\n";
    }
    // ---- end PC shadows ----
    if (g_shadow_debug)  // PC shadows
        out += "    float shadow_dbg = 1, shadow_in = -1, shadow_ref = 0, shadow_near = 0; float3 shadow_uvn = -1, "
               "shadow_uvf = -1;\n";
    for (int i = 0; i < 4; i++) {
        uint32_t program = (programs >> (5 * i)) & 0x1F;
        if (!program) continue;
        Sampler sampler = config.sampler[i];
        int prev = i == 0 ? 0 : i == 1 ? 0 : i == 2 ? int((other >> 16) & 0xF) : int((other >> 20) & 0xF);
        char line[512];
        auto sample = [&](const char* coords) {
            if (sampler == Sampler::kCube)
                snprintf(line, sizeof line, "    t%d = tex%d.Sample(smp%d, (%s).xyz);\n", i, i, i, coords);
            else if (sampler == Sampler::k3D)
                snprintf(line, sizeof line, "    t%d = tex%d.Sample(smp%d, (%s).xyz);\n", i, i, i, coords);
            else if (sampler == Sampler::kRect)
                snprintf(line, sizeof line, "    t%d = tex%d.Sample(smp%d, (%s).xy * tex_scale[%d].xy);\n", i, i, i,
                         coords, i);
            else
                snprintf(line, sizeof line, "    t%d = tex%d.Sample(smp%d, (%s).xy);\n", i, i, i, coords);
            out += line;
        };
        char coords[160];
        if (sampler == Sampler::kSceneDepth || sampler == Sampler::kSceneDepthMS) {
            // a depth texture on the 3D view's depth target (the ocean's stage 2 reads the frame's depth buffer, with
            // r/q = zmax and GREATER). The host keeps that depth as float, maybe multisampled and reversed
            // (c / d), at a multiple of the guest's size: each of the four guest texels around the point is read at
            // its centre, turned back into the guest's depth and compared as the NV2A compares a depth texel with
            // r/q (texel <op> reference, as for the shadow maps below), weighted as its bilinear filter weights them;
            // with a point filter (the ocean's) only the host texel the point lies in (below)
            const bool ms = sampler == Sampler::kSceneDepthMS;
            const bool point = ((s.Reg(SET_TEXTURE_FILTER + 0x40 * i) >> 24) & 0xF) == 1;
            std::string op;
            switch (s.Reg(SET_SHADOW_COMPARE_FUNC) & 7) {
            case 0: op = "false"; break;
            case 1: op = "d < ref"; break;
            case 2: op = "d == ref"; break;
            case 3: op = "d <= ref"; break;
            case 4: op = "d > ref"; break;
            case 5: op = "d != ref"; break;
            case 6: op = "d >= ref"; break;
            default: op = "true"; break;
            }
            if (program != 1 && program != 2) op = "true";  // (not seen: no comparison, the depth itself)
            // GitHub #14: with the point filter the host texel the point lies in, not the centre of the guest texel:
            // the ocean's coordinates are its own pixel, so on the console each pixel compared its own depth; read
            // per guest texel that became blocks of scale x scale host pixels (the same texel at 1x)
            char point_base[160];
            snprintf(point_base, sizeof point_base,
                     "        base = (floor(p.xy * tex_scale[%d].xy) + 0.5) / tex_scale[%d].xy - 0.5, f = 0;\n", i, i);
            char depth[2048];
            snprintf(depth, sizeof depth,
                     "    {\n"
                     "        float3 p = T[%d].xyz / (T[%d].w == 0 ? 1 : T[%d].w);\n"
                     "        float ref = p.z / tex_scale[%d].z, w, h%s;\n"
                     "        tex%d.GetDimensions(w, h%s);\n"
                     "        float2 q = p.xy - 0.5, base = floor(q), f = q - base;\n"
                     "%s"
                     "        float4 lit = 0, dv = 0;\n"
                     "        [unroll] for (int k = 0; k < %d; k++) {\n"
                     "            int2 at = int2(clamp((base + float2(k & 1, k >> 1) + 0.5) * tex_scale[%d].xy, 0, float2(w, h) - 1));\n"
                     "            float stored = tex%d.Load(%s).x;\n"
                     "            float d = tex_scale[%d].w > 0 ? tex_scale[%d].w / max(stored, tex_scale[%d].w) : stored;\n"
                     "            dv[k] = d;\n"
                     "            lit[k] = (%s) ? 1 : 0;\n"
                     "        }\n"
                     "        t%d = %s;\n"
                     "    }\n",
                     i, i, i, i, ms ? ", n" : "", i, ms ? ", n" : "",
                     point ? point_base : "", point ? 1 : 4, i, i, ms ? "at, 0" : "int3(at, 0)", i, i, i,
                     op.c_str(), i,
                     (program == 1 || program == 2)
                         ? "dot(lit, float4((1 - f.x) * (1 - f.y), f.x * (1 - f.y), (1 - f.x) * f.y, f.x * f.y)).xxxx"
                         : "dv.xxxx");
            out += depth;
            continue;
        }
        std::string target_ms_code;  // GitHub #19: the projective stage's lookup (below)
        if (sampler == Sampler::kTargetMS) {
            // ---- GitHub #19: a pass that reads the multisampled target it draws into (the terrain's colour pass reads
            // its lighting pass back at the same pixel: renderer.cpp, TargetSnapshot). Where the coordinates are the
            // pixel's own position and the fragment covers only part of it (or something drawn before hides part of
            // it), the average of the samples it covers and its surface is visible at: exactly the samples its
            // output goes to. The average of all samples (the resolved copy) mixed in whatever covers the rest of an
            // edge pixel (the sky): bright dashed lines along the terrain's pieces. Everywhere else as the resolved
            // copy, filtered: a fragment that covers and sees the whole pixel (inside a surface, a full screen pass
            // such as the feral vision's tint) reads exactly what it read before, the sub-pixel offset of its
            // coordinates included (the bilinear filter blends a few percent of the neighbours in; read without it,
            // the feral tint changed along every edge). renderer.cpp passes only projective stages with clamped
            // addressing ----
            const bool point = ((s.Reg(SET_TEXTURE_FILTER + 0x40 * i) >> 24) & 0xF) == 1;
            // with the depth: the fragment's depth at each sample (the rasterizer's z on its plane, or the depth the
            // shader writes for the whole pixel) against the stored one; a covered sample whose stored depth is
            // nearer by more than about a pixel's depth step belongs to something in front (an object drawn before
            // the terrain) and fails the draw's depth test: not one the fragment writes
            std::string depth_head, depth_pick;
            if (config.target_ms_depth) {
                const bool written = config.depth != Depth::kNone;  // SV_Depth: one value for the pixel
                depth_head = std::string("        float e0 = ") + (written ? "depth" : "input.pos.z") + ";\n" +
                             (written ? "        float2 de = 0;\n" : "        float2 de = float2(ddx(e0), ddy(e0));\n") +
                             "        float tol = 1e-5 * abs(e0) + abs(de.x) + abs(de.y);\n";
                depth_pick = std::string(
                                 "            uint seen = 0;\n"
                                 "            [loop] for (uint m = 0; m < min(n, 8u); m++) {\n"
                                 "                if (((cv >> m) & 1) == 0) continue;\n"
                                 "                float e = e0 + dot(de, tex") + std::to_string(i) + ".GetSamplePosition(m));\n"
                                 "                float d = target_depth.Load(px, m);\n" +
                             (config.target_ms_depth == 2 ? "                if (d <= e + tol) seen |= 1u << m;\n"
                                                          : "                if (d >= e - tol) seen |= 1u << m;\n") +
                             "            }\n"
                             "            if (seen != 0) cv = seen;\n";  // none: all covered ones (as without depth)
            }
            const std::string dbg4 = "        float3 dv = abs(t" + std::to_string(i) + " - res).rgb;\n"
                                     "        target_ms_dbg = float4(own ? max(dv.r, max(dv.g, dv.b)) : 0, 0, own ? 0 : 1, 1);\n";
            // FCI_TARGET_MS_PLAIN=1 (diagnostics): a fragment that covers and sees the whole pixel reads the plain
            // average of its samples too (no bilinear filter: cheaper, but a few percent sharper than before)
            static const bool full_filter = GetEnvironmentVariableA("FCI_TARGET_MS_PLAIN", nullptr, 0) == 0;
            // the four texels of the filter, each the average of its samples as the resolve stores it
            const std::string tx = "tex" + std::to_string(i);
            std::string mirror_head, mirror_pick, tap_head, tap_end;
            std::string tap_code = "                float4 sum = 0;\n"
                                   "                [loop] for (uint m = 0; m < n; m++) sum += " + tx + ".Load(at, m);\n"
                                   "                float4 texel = round(sum / max(n, 1u) * 255) / 255;\n"
                                   "                res += texel * (((k & 1) != 0 ? f.x : 1 - f.x) * ((k >> 1) != 0 ? f.y : 1 - f.y));\n";
            if (config.mirror_read & (1u << i)) {
                // ---- GitHub #19, the target's mirror (renderer.cpp): the texture is the mirror the pass before wrote
                // (its colour, alpha its marker), not a copy of the frame. A sample with this draw's marker (mirror_id.y)
                // holds what the frame holds there: the pass before wrote both and nothing has drawn since. The own
                // pixel reads, of the samples the fragment covers and sees, those with the marker (the samples the
                // pass before wrote, which this draw writes; a sample within the depth tolerance that another surface
                // wrote is not one of them). The filter's texels (inside a surface, a few percent of the neighbours
                // for the coordinates' sub-pixel offset) come from the target's resolved copy (mirror_resolved: the
                // copy's exactly; the neighbours may hold what other surfaces drew, which the mirror lacks), or else
                // from the mirror's samples with the marker, a texel without any giving its weight to the others ----
                mirror_head = std::string("        uint mark = uint(mirror_id.y + 0.5);\n") +
                              (g_target_ms_debug == 7 ? "        uint cv_seen = 0;\n" : "");
                mirror_pick = std::string(g_target_ms_debug == 7 ? "            cv_seen = cv;\n" : "") +
                              "            uint mine = 0;\n"
                              "            [loop] for (uint mm = 0; mm < min(n, 8u); mm++)\n"
                              "                if (((cv >> mm) & 1) != 0 && uint(round(" + tx + ".Load(px, mm).a * 255)) == mark) "
                              "mine |= 1u << mm;\n" +
                              std::string(g_target_ms_debug == 5 ? "            target_ms_dbg = float4((cv & ~mine) != 0 ? 1 : 0, 0, "
                                                                   "mine == 0 ? 1 : 0, 1);\n" : "") +
                              std::string(g_target_ms_debug == 6
                                              ? "            float dmax = 0;\n"
                                                "            [loop] for (uint md = 0; md < min(n, 8u); md++)\n"
                                                "                if (((mine >> md) & 1) != 0) { float3 dv = abs(" + tx +
                                                    ".Load(px, md).rgb - target_copy_dbg.Load(px, md).rgb); "
                                                    "dmax = max(dmax, max(dv.r, max(dv.g, dv.b))); }\n"
                                                "            target_ms_dbg = float4(saturate(dmax * 16), 0, 0, 1);\n"
                                              : "") +
                              "            if (mine != 0) cv = mine;\n";
                tap_head = "            float4 raw = 0;\n"
                           "            float wsum = 0;\n"
                           "            uint partial = 0;\n" +
                           std::string(g_target_ms_debug == 6 ? "            float dtap = 0;\n" : "");
                tap_code = "                float4 sum = 0, every = 0;\n"
                           "                uint cnt = 0;\n"
                           "                [loop] for (uint m = 0; m < n; m++) {\n"
                           "                    float4 v = " + tx + ".Load(at, m);\n"
                           "                    every += v;\n"
                           "                    if (uint(round(v.a * 255)) == mark) sum += v, cnt++;\n" +
                           std::string(g_target_ms_debug == 6
                                           ? "                    if (uint(round(v.a * 255)) == mark) { float3 dv = "
                                             "abs(v.rgb - target_copy_dbg.Load(at, m).rgb); dtap = max(dtap, max(dv.r, "
                                             "max(dv.g, dv.b))); }\n"
                                           : "") +
                           "                }\n"
                           "                float wk = ((k & 1) != 0 ? f.x : 1 - f.x) * ((k >> 1) != 0 ? f.y : 1 - f.y);\n"
                           "                if (cnt != n && wk > 0) partial++;\n"
                           "                if (cnt != 0) res += round(sum / cnt * 255) / 255 * wk, wsum += wk;\n"
                           "                raw += round(every / max(n, 1u) * 255) / 255 * wk;\n";
                // (all texels whole: res as from the copy, unscaled; none with the marker: the mirror as it is)
                tap_end = std::string("            if (partial != 0) res = wsum > 0 ? res / wsum : raw;\n") +
                          (g_target_ms_debug == 5 ? "            if (partial != 0) target_ms_dbg = float4(max(target_ms_dbg.r, 0), 1, "
                                                    "max(target_ms_dbg.b, 0), 1);\n" : "") +
                          (g_target_ms_debug == 6 ? "            target_ms_dbg = float4(max(target_ms_dbg.r, 0), "
                                                    "saturate(dtap * 16), partial != 0 ? 1 : 0, 1);\n" : "");
                if (config.mirror_resolved) {
                    // the own pixel's texel (a fragment that covers and sees it all: all its samples have the marker,
                    // the frame's bytes) averaged from the mirror's samples as the copy's is; the neighbours' from the
                    // resolved copy. (The resolve may round halves otherwise than that average: under DXVK the own
                    // pixel's texel from the resolve, ~96 % of the value, moved 0.5 % of the pixels; at the neighbours'
                    // few percent it is nothing)
                    tap_head.clear(), tap_end.clear();
                    tap_code = "                float4 texel;\n"
                               "                [branch] if (own && all(at == px)) {\n"
                               "                    float4 sum = 0;\n"
                               "                    [loop] for (uint m = 0; m < n; m++) sum += " + tx + ".Load(at, m);\n"
                               "                    texel = round(sum / max(n, 1u) * 255) / 255;\n"
                               "                } else {\n"
                               "                    texel = target_resolved.Load(int3(at, 0));\n"
                               "                }\n"
                               "                res += texel * (((k & 1) != 0 ? f.x : 1 - f.x) * ((k >> 1) != 0 ? f.y : 1 - f.y));\n";
                }
                // ---- end the target's mirror ----
            }
            char ms[8192];
            snprintf(ms, sizeof ms,
                     "    {\n"
                     "        float2 hp = (T[%d].xy / (T[%d].w == 0 ? 1 : T[%d].w)) * tex_scale[%d].xy + float2(0, tex_scale[%d].z);\n"
                     "        uint w, h, n;\n"
                     "        tex%d.GetDimensions(w, h, n);\n"
                     "        int2 lo = int2(0, int(tex_scale[%d].z)), hi = int2(int(w), int(tex_scale[%d].w)) - 1;\n"
                     "        bool own = all(abs(hp - input.pos.xy) < 0.5);\n"
                     "%s"
                     // the samples read at the own position: those covered (none cannot happen for a shaded pixel:
                     // then all), maybe less those behind something nearer
                     "        uint all_samples = (1u << min(n, 8u)) - 1, cv = all_samples;\n"
                     "        int2 px = clamp(int2(floor(hp)), lo, hi);\n"
                     "        [branch] if (own) {\n"
                     "            cv = coverage & all_samples;\n"
                     "%s"
                     "            if (cv == 0) cv = all_samples;\n"
                     "%s"
                     "        }\n"
                     "        float4 res = 0;\n"  // as the resolved copy, filtered
                     "        [branch] if (!own || %s) {\n"
                     "            float2 q = hp - %s, base = floor(q), f = %s;\n"
                     "%s"
                     "            [unroll] for (int k = 0; k < %d; k++) {\n"
                     "                int2 at = clamp(int2(base) + int2(k & 1, k >> 1), lo, hi);\n"
                     "%s"
                     "            }\n"
                     "%s"
                     "        }\n"
                     "        [branch] if (own && !(%s)) {\n"
                     // their average, stored as the resolve stores it
                     "            float4 sum = 0;\n"
                     "            uint cnt = 0;\n"
                     "            [loop] for (uint m = 0; m < min(n, 8u); m++)\n"
                     "                if (((cv >> m) & 1) != 0) sum += tex%d.Load(px, m), cnt++;\n"
                     "            t%d = round(sum / max(cnt, 1u) * 255) / 255;\n"
                     "%s"
                     "        } else {\n"
                     "            t%d = res;\n"
                     "        }\n"
                     "%s"
                     "    }\n",
                     i, i, i, i, i, i, i, i, (depth_head + mirror_head).c_str(),
                     g_target_ms_debug == 3 ? "            target_ms_dbg.r = cv != all_samples ? 1 : 0;\n" : "",
                     (depth_pick + mirror_pick).c_str(),
                     g_target_ms_debug == 4 ? "true" : full_filter ? "cv == all_samples" : "false",
                     point ? "0" : "0.5", point ? "0" : "q - base", tap_head.c_str(), point ? 1 : 4, tap_code.c_str(),
                     tap_end.c_str(), full_filter ? "cv == all_samples" : "false", i, i,
                     g_target_ms_debug == 3
                         ? "            target_ms_dbg = float4(target_ms_dbg.r, float(cnt) / max(n, 1u), 0, 1);\n"
                         : "",
                     i,
                     g_target_ms_debug == 1   ? "        target_ms_dbg = own ? float4(0, 1, 0, 1) : float4(1, 0, 0, 1);\n"
                     : g_target_ms_debug == 2 ? "        target_ms_dbg = float4(saturate((hp - input.pos.xy) * 0.5 + 0.5), own ? 1 : 0, 1);\n"
                     : g_target_ms_debug == 3 ? "        if (!own) target_ms_dbg = float4(0, 0, 1, 1);\n"
                     : g_target_ms_debug == 4 ? dbg4.c_str()
                                              : "");
            target_ms_code = ms;
            // test of the fallback: FCI_TEST_TARGET_MS_FAILURE=1 makes these shaders fail to compile (renderer.cpp
            // then reads the resolved copy)
            static const bool fail = GetEnvironmentVariableA("FCI_TEST_TARGET_MS_FAILURE", nullptr, 0) > 0;
            if (fail) target_ms_code += "    target_ms_test_failure();\n";
            if (g_target_ms_debug == 7 && (config.mirror_read & (1u << i))) {
                // the target's mirror, diagnostics: the value the copy would have given (t6, renderer.cpp) against
                // the mirror's: red = the difference (16 levels: full), green = seen samples without the marker
                const std::string t = "t" + std::to_string(i);
                target_ms_code.insert(target_ms_code.size() - 6,
                    "        {\n"
                    "            float4 tc = 0;\n"
                    "            uint cvc = own ? cv_seen : all_samples;\n"
                    "            if (own && cvc != all_samples) {\n"
                    "                float4 s7 = 0; uint c7 = 0;\n"
                    "                [loop] for (uint q7 = 0; q7 < min(n, 8u); q7++)\n"
                    "                    if (((cvc >> q7) & 1) != 0) s7 += target_copy_dbg.Load(px, q7), c7++;\n"
                    "                tc = round(s7 / max(c7, 1u) * 255) / 255;\n"
                    "            } else {\n"
                    "                float2 q2 = hp - 0.5, b2 = floor(q2), f2 = q2 - b2;\n"
                    "                [unroll] for (int k2 = 0; k2 < 4; k2++) {\n"
                    "                    int2 at2 = clamp(int2(b2) + int2(k2 & 1, k2 >> 1), lo, hi);\n"
                    "                    float4 s7 = 0;\n"
                    "                    [loop] for (uint q7 = 0; q7 < n; q7++) s7 += target_copy_dbg.Load(at2, q7);\n"
                    "                    tc += round(s7 / max(n, 1u) * 255) / 255 * (((k2 & 1) != 0 ? f2.x : 1 - f2.x) * "
                    "((k2 >> 1) != 0 ? f2.y : 1 - f2.y));\n"
                    "                }\n"
                    "            }\n"
                    "            float3 dt = abs(" + t + ".rgb - tc.rgb);\n"
                    "            target_ms_dbg = float4(saturate(max(dt.r, max(dt.g, dt.b)) * 255 / 16), "
                    "(own && cv_seen != cv) ? 1 : 0, 0, 1);\n"
                    "        }\n");
            }
            // ---- end GitHub #19 ----
        }
        if (sampler == Sampler::kShadow && (program == 1 || program == 2)) {
            // shadow map: the share of the four nearest texels that pass the comparison, weighted as bilinear
            // filtering would (percentage closer filtering, as the NV2A does with a linear filter)
            // the texel (the occluder's depth) is compared with the reference, not the other way round: the
            // game sets GREATER and multiplies its lighting by the result (lit where the occluder is farther)
            std::string op;
            switch (s.Reg(SET_SHADOW_COMPARE_FUNC) & 7) {
            case 0: op = "false"; break;
            case 1: op = "d < ref"; break;
            case 2: op = "d == ref"; break;
            case 3: op = "d <= ref"; break;
            case 4: op = "d > ref"; break;
            case 5: op = "d != ref"; break;
            case 6: op = "d >= ref"; break;
            default: op = "true"; break;
            }
            char shadow[2048];
            // the lookup of one map (tex) at uv into result (a float declared before)
            auto lookup = [&](const char* tex, const char* uv, const char* result, const char* ref, bool soft = true) {
                std::string compare = op;  // its reference: ref, or the near map's
                for (size_t at = compare.find("ref"); at != std::string::npos; at = compare.find("ref", at + 1))
                    compare.replace(at, 3, ref), at += std::strlen(ref) - 1;
                if (g_shadow_filter == 1 && soft) {
                    // ---- PC shadows: soft, the average of 3x3 bilinear lookups one texel apart (a tent over the
                    // 4x4 texels base - 1 .. base + 2 around the point, weights (1 - f, 1, 1, f) per axis), from four
                    // gathers of 2x2 texels (gather order: x left bottom, y right bottom, z right top, w left top) ----
                    snprintf(shadow, sizeof shadow,
                             "        {\n"
                             "            float w, h;\n"
                             "            %s.GetDimensions(w, h);\n"
                             "            float2 size = float2(w, h), q = %s * size - 0.5, base = floor(q), f = q - base;\n"
                             "            float sum = 0;\n"
                             "            [unroll] for (int gy = 0; gy < 2; gy++) [unroll] for (int gx = 0; gx < 2; gx++) {\n"
                             "                float4 d = %s.Gather(smp%d, (base + 0.25 + 2 * float2(gx, gy)) / size);\n"
                             "                float4 lit = (%s) ? 1 : 0;\n"
                             "                float wl = gx ? 1 : 1 - f.x, wr = gx ? f.x : 1, wt = gy ? 1 : 1 - f.y, wb = gy ? f.y : 1;\n"
                             "                sum += dot(lit, float4(wl * wb, wr * wb, wr * wt, wl * wt));\n"
                             "            }\n"
                             "            %s = sum / 9;\n"
                             "        }\n",
                             tex, uv, tex, i, compare.c_str(), result);
                    // ---- end PC shadows ----
                } else {
                    // the 2x2 texels around the point and their weights from the same floor (the gather at the middle
                    // of that footprint): gathered at the point itself, the sampler's rounded choice of the footprint and
                    // frac() could disagree right at its edges, and a point the cascades map from the other camera (the same
                    // point in the last bits only) then read the neighbouring footprint in every other frame (GitHub #16)
                    snprintf(shadow, sizeof shadow,
                             "        {\n"
                             "            float w, h;\n"
                             "            %s.GetDimensions(w, h);\n"
                             "            float2 q = %s * float2(w, h) - 0.5, base = floor(q), f = q - base;\n"
                             "            float4 d = %s.Gather(smp%d, (base + 1) / float2(w, h));\n"
                             "            float4 lit = (%s) ? 1 : 0;\n"
                             "            %s = dot(lit, float4((1 - f.x) * f.y, f.x * f.y, f.x * (1 - f.y), (1 - f.x) * (1 - f.y)));\n"
                             "        }\n",
                             tex, uv, tex, i, compare.c_str(), result);
                }
                return std::string(shadow);
            };
            char head[512];
            snprintf(head, sizeof head,
                     "    {\n"
                     "        float3 p = T[%d].xyz / (T[%d].w == 0 ? 1 : T[%d].w);\n"
                     "        float2 uv = p.xy * tex_scale[%d].xy;\n"
                     "        float ref = p.z / tex_scale[%d].z, lit_far = 1, lit_near = 1;\n",
                     i, i, i, i, i);
            const std::string tex = "tex" + std::to_string(i);
            out += head;
            if (cascade_stage(i)) {
                // ---- PC shadows: cascades. Each map's coordinates and depth from the receiver's (affine maps: the
                // game's sun cameras are orthographic along the sun; the game's own map this frame is either one),
                // the point at the stages' centroid, this stage's tap at its own offset from it (above);
                // where the near map covers the point it is used, fading into the far map over its outer edge (no
                // seam); either map is read only where its weight is above 0 ----
                if (g_cascade_taps)
                    out += "        float4 pr = float4(cascade_base, ref, 1);\n"
                           "        float2 tap = uv - cascade_base;\n"
                           "        float2 uv_near = float2(dot(cascade_n[0], pr), dot(cascade_n[1], pr)) + tap;\n"
                           "        float ref_near = dot(cascade_n[2], pr);\n"
                           "        float2 uv_far = float2(dot(cascade_f[0], pr), dot(cascade_f[1], pr)) + tap;\n";
                else
                    out += "        float4 pr = float4(uv, ref, 1);\n"
                           "        float2 uv_near = float2(dot(cascade_n[0], pr), dot(cascade_n[1], pr));\n"
                           "        float ref_near = dot(cascade_n[2], pr);\n"
                           "        float2 uv_far = float2(dot(cascade_f[0], pr), dot(cascade_f[1], pr));\n";
                out += "        float ref_far = dot(cascade_f[2], pr);\n"
                       "        float2 e = min(uv_near, 1 - uv_near);\n"
                       "        float near_w = saturate(min(e.x, e.y) * cascade_fade.x);\n"
                       + std::string(g_cascade_force == 1 ? "        near_w = 1;\n" : g_cascade_force == 2 ? "        near_w = 0;\n" : "") +
                       "        [branch] if (near_w > 0) {\n";
                out += lookup("tex_near", "uv_near", "lit_near", "ref_near");
                // the far pass's solid casters the near pass may lack (renderer.cpp, kSolidsMargin), in the far
                // camera's depth: the darker of both; only where the near map leaves light, with one bilinear gather (its
                // texels are about as fine as the soft filter's footprint in the near map; the soft filter's four gathers
                // cost the Steam Deck ~0.7 ms a frame)
                out += "        float2 uv_s = (uv_far - cascade_s.xy) * cascade_s.zw;\n"
                       "        [branch] if (cascade_fade.y > 0 && lit_near > 0 && all(uv_s > 0) && all(uv_s < 1)) {\n"
                       "        float lit_solid = 1;\n";
                out += lookup("tex_solid", "uv_s", "lit_solid", "ref_far", g_solids_soft);
                out += "        lit_near = min(lit_near, lit_solid);\n"
                       "        }\n"
                       "        }\n";
                out += "        [branch] if (near_w < 1)\n";
                out += lookup("tex_far", "uv_far", "lit_far", "ref_far");
                snprintf(shadow, sizeof shadow, "        t%d = lerp(lit_far, lit_near, near_w).xxxx;\n", i);
                out += shadow;
                if (g_shadow_debug) out += "        shadow_near = near_w; shadow_uvn = float3(uv_near, ref_near); shadow_uvf = float3(uv_far, ref_far);\n";
                // ---- end PC shadows ----
            } else {
                out += lookup(tex.c_str(), "uv", "lit_far", "ref");
                snprintf(shadow, sizeof shadow, "        t%d = lit_far.xxxx;\n", i);
                out += shadow;
            }
            out += "    }\n";
            if (g_shadow_debug) {  // PC shadows: FCI_SHADOW_DEBUG=1
                snprintf(shadow, sizeof shadow,
                         "    { float3 p = T[%d].xyz / (T[%d].w == 0 ? 1 : T[%d].w); float2 uv = p.xy * tex_scale[%d].xy;\n"
                         "      shadow_dbg = t%d.x; shadow_in = all(uv > 0 && uv < 1) ? 1 : 0; shadow_ref = p.z / tex_scale[%d].z; }\n",
                         i, i, i, i, i, i);
                out += shadow;
            }
            continue;
        }
        switch (program) {
        case 1:  // 2D projective
        case 2:  // 3D projective
            if (!target_ms_code.empty()) {  // GitHub #19
                out += target_ms_code;
                break;
            }
            snprintf(coords, sizeof coords, "T[%d] / (T[%d].w == 0 ? 1 : T[%d].w)", i, i, i);
            if (sampler == Sampler::kCube) snprintf(coords, sizeof coords, "T[%d]", i);
            sample(coords);
            break;
        case 3:  // cube map
            snprintf(coords, sizeof coords, "T[%d]", i);
            sample(coords);
            break;
        case 4:  // pass through
            snprintf(line, sizeof line, "    t%d = saturate(T[%d]);\n", i, i);
            out += line;
            break;
        case 5:  // clip plane: kill the pixel where a coordinate is on the wrong side
            if (GetEnvironmentVariableA("FCI_NO_CLIPPLANE", nullptr, 0) > 0) break;  // diagnostics
            for (int c = 0; c < 4; c++) {
                bool ge = (clip_mode >> (4 * i + c)) & 1;
                snprintf(line, sizeof line, "    if (T[%d][%d] %s 0) discard;\n", i, c, ge ? ">=" : "<");
                out += line;
            }
            break;
        case 6:  // bump environment map, offsets from an earlier stage
        case 7:
            snprintf(line, sizeof line,
                     "    float2 d%d = %s(t%d.bg);\n"
                     "    float4 u%d = T[%d] + float4(d%d.x * bump[%d].x + d%d.y * bump[%d].z, "
                     "d%d.x * bump[%d].y + d%d.y * bump[%d].w, 0, 0);\n",
                     i, (config.signs[prev] & 0xC) ? "signed2f" : "signed2", prev, i, i, i, i, i, i, i, i, i, i);
            out += line;
            snprintf(coords, sizeof coords, "u%d", i);
            sample(coords);
            break;
        case 15:  // dependent alpha-red
            snprintf(coords, sizeof coords, "float4(t%d.a, t%d.r, 0, 1)", prev, prev);
            sample(coords);
            break;
        case 16:  // dependent green-blue
            snprintf(coords, sizeof coords, "float4(t%d.g, t%d.b, 0, 1)", prev, prev);
            sample(coords);
            break;
        default: {
            static uint32_t logged = 0;
            if (!(logged & (1u << program))) {
                logged |= 1u << program;
                rt::Log("psh: texture stage program %u not implemented", program);
            }
            break;
        }
        }
        // TEXTURE_CONTROL0 ALPHA_KILL: a texel with alpha 0 kills the pixel (cut-out foliage and fences)
        bool samples = program == 1 || program == 2 || program == 3 || program == 6 || program == 7 || program >= 15;
        // ---- signed textures (GitHub #14): the channels filtered as offset binary back to the two's complement
        // bytes the stage gave before (unchanged at texel centres, filtered in the signed values in between) ----
        if (samples && config.signs[i]) {
            const uint8_t m = config.signs[i];
            snprintf(line, sizeof line,
                     "    {\n"
                     "        float4 sv = t%d * 255 - 128;\n"
                     "        t%d = bool4(%s, %s, %s, %s) ? (sv < 0 ? sv + 256 : sv) / 255 : t%d;\n"
                     "    }\n",
                     i, i, m & 2 ? "true" : "false", m & 4 ? "true" : "false", m & 8 ? "true" : "false",
                     m & 1 ? "true" : "false", i);
            out += line;
        }
        // ---- end signed textures ----
        if (samples && (s.Reg(SET_TEXTURE_CONTROL0 + 0x40 * i) & kTextureAlphaKill)) {
            snprintf(line, sizeof line, "    if (t%d.a == 0) discard;\n", i);
            out += line;
        }
    }

    // general combiners
    uint32_t control = s.Reg(SET_COMBINER_CONTROL);
    uint32_t stages = control & 0xFF;
    bool mux_msb = control & 0x100;
    bool factor0_each = control & 0x1000, factor1_each = control & 0x10000;
    out += "    float4 r0 = float4(0, 0, 0, t0.a), r1 = 0;\n";
    for (uint32_t i = 0; i < stages && i < 8; i++) {
        uint32_t cicw = s.Reg(SET_COMBINER_COLOR_ICW + 4 * i), cocw = s.Reg(SET_COMBINER_COLOR_OCW + 4 * i);
        uint32_t aicw = s.Reg(SET_COMBINER_ALPHA_ICW + 4 * i), aocw = s.Reg(SET_COMBINER_ALPHA_OCW + 4 * i);
        char line[512];
        snprintf(line, sizeof line, "    {\n        float4 c0 = factor0[%u], c1 = factor1[%u];\n",
                 factor0_each ? i : 0, factor1_each ? i : 0);
        out += line;
        out += "        float3 cA = " + Input(cicw >> 24, false) + ", cB = " + Input((cicw >> 16) & 0xFF, false) +
               ", cC = " + Input((cicw >> 8) & 0xFF, false) + ", cD = " + Input(cicw & 0xFF, false) + ";\n";
        out += "        float aA = " + Input(aicw >> 24, true) + ", aB = " + Input((aicw >> 16) & 0xFF, true) +
               ", aC = " + Input((aicw >> 8) & 0xFF, true) + ", aD = " + Input(aicw & 0xFF, true) + ";\n";
        const char* select = mux_msb ? "r0.a >= 0.5" : "(uint(round(r0.a * 255)) & 1) != 0";
        out += (cocw & 0x2000) ? "        float3 cab = dot(cA, cB).xxx;\n" : "        float3 cab = cA * cB;\n";
        out += (cocw & 0x1000) ? "        float3 ccd = dot(cC, cD).xxx;\n" : "        float3 ccd = cC * cD;\n";
        snprintf(line, sizeof line, "        float3 csum = %s;\n",
                 (cocw & 0x4000) ? (std::string("(") + select + ") ? ccd : cab").c_str() : "cab + ccd");
        out += line;
        out += "        float aab = aA * aB, acd = aC * aD;\n";
        snprintf(line, sizeof line, "        float asum = %s;\n",
                 (aocw & 0x4000) ? (std::string("(") + select + ") ? acd : aab").c_str() : "aab + acd");
        out += line;
        const char *bias, *scale;
        Op((cocw >> 15) & 7, bias, scale);
        snprintf(line, sizeof line,
                 "        cab = clamp((cab + %s) * %s, -1, 1); ccd = clamp((ccd + %s) * %s, -1, 1); "
                 "csum = clamp((csum + %s) * %s, -1, 1);\n",
                 bias, scale, bias, scale, bias, scale);
        out += line;
        Op((aocw >> 15) & 7, bias, scale);
        snprintf(line, sizeof line,
                 "        aab = clamp((aab + %s) * %s, -1, 1); acd = clamp((acd + %s) * %s, -1, 1); "
                 "asum = clamp((asum + %s) * %s, -1, 1);\n",
                 bias, scale, bias, scale, bias, scale);
        out += line;
        auto write = [&](uint32_t dst, const char* value, const char* mask) {
            if (const char* reg = RegisterName(dst); reg && dst >= 4) out += std::string("        ") + reg + mask + " = " + value + ";\n";
        };
        write(cocw & 0xF, "ccd", ".rgb");
        write((cocw >> 4) & 0xF, "cab", ".rgb");
        write((cocw >> 8) & 0xF, "csum", ".rgb");
        write(aocw & 0xF, "acd", ".a");
        write((aocw >> 4) & 0xF, "aab", ".a");
        write((aocw >> 8) & 0xF, "asum", ".a");
        if (cocw & 0x40000) write(cocw & 0xF, "ccd.b", ".a");
        if (cocw & 0x80000) write((cocw >> 4) & 0xF, "cab.b", ".a");
        out += "    }\n";
    }

    // final combiner: A * B + (1 - A) * C + D, alpha from G
    uint32_t cw0 = s.Reg(SET_COMBINER_SPECULAR_FOG_CW0), cw1 = s.Reg(SET_COMBINER_SPECULAR_FOG_CW1);
    {
        char line[256];
        snprintf(line, sizeof line, "    float4 sum = float4(%s + %s, 0);\n",
                 (cw1 & 0x20) ? "(1 - r0.rgb)" : "r0.rgb", (cw1 & 0x40) ? "(1 - v1.rgb)" : "v1.rgb");
        out += line;
        if (cw1 & 0x80) out += "    sum = saturate(sum);\n";
        out += "    float4 c0 = final_factor[0], c1 = final_factor[1];\n";
        out += "    float4 ef = float4(" + FinalInput(cw1 >> 24, false) + " * " + FinalInput((cw1 >> 16) & 0xFF, false) +
               ", 0);\n";
        out += "    float3 fA = " + FinalInput(cw0 >> 24, false) + ";\n";
        out += "    float4 color = float4(fA * " + FinalInput((cw0 >> 16) & 0xFF, false) + " + (1 - fA) * " +
               FinalInput((cw0 >> 8) & 0xFF, false) + " + " + FinalInput(cw0 & 0xFF, false) + ", " +
               FinalInput((cw1 >> 8) & 0xFF, true) + ");\n";
        out += "    color = saturate(color);\n";
    }

    static const bool no_alpha_test = GetEnvironmentVariableA("FCI_NO_ALPHATEST", nullptr, 0) > 0;  // diagnostics
    if (s.Reg(SET_ALPHA_TEST_ENABLE) && !no_alpha_test) {
        out += "    float a = round(color.a * 255), ref = alpha_ref.x;\n";
        out += std::string("    if (!(") + Compare(s.Reg(SET_ALPHA_FUNC)) + ")) discard;\n";
    }
    // PC shadows: FCI_SHADOW_DEBUG=1 tints what a shadow map covers green and its shadow red, the near cascade's
    // share blue (2: red = outside the map, green = lit, blue = beyond its depth range)
    if (g_shadow_debug == 1)
        out += "    color.rgb = lerp(color.rgb, float3(0, 0.3, 1), 0.3 * shadow_near);\n";
    if (g_shadow_debug == 2)
        out += "    if (shadow_in >= 0) color.rgb = float3(1 - shadow_in, shadow_dbg, shadow_ref >= 1 ? 1 : 0);\n";
    if (g_shadow_debug == 1)
        out += "    if (shadow_in >= 0) color.rgb = lerp(lerp(color.rgb, float3(0, 0.6, 0), 0.25 * shadow_in), float3(1, 0, 0), "
               "0.7 * (1 - shadow_dbg));\n";
    if (g_target_ms_debug == 1) out += "    if (target_ms_dbg.a >= 0) color.rgb = lerp(color.rgb, target_ms_dbg.rgb, 0.6);\n";
    if (g_target_ms_debug == 2 || g_target_ms_debug == 3) out += "    if (target_ms_dbg.a >= 0) color = target_ms_dbg;\n";
    if (g_target_ms_debug == 4)
        out += "    if (target_ms_dbg.a >= 0) color.rgb = float3(max(color.r * 0.3, saturate(target_ms_dbg.r * 8)), "
               "color.g * 0.3, max(color.b * 0.3, target_ms_dbg.b));\n";
    if (g_target_ms_debug == 5)  // the target's mirror: red = seen samples without the marker, green = a filter texel
        // without all, blue = none with the marker; the rest of such draws dimmed
        out += "    if (target_ms_dbg.a >= 0 && any(target_ms_dbg.rgb > 0)) color.rgb = target_ms_dbg.rgb; "
               "else if (target_ms_dbg.a >= 0) color.rgb *= 0.3;\n";
    if (g_target_ms_debug == 7)  // the target's mirror against the copy, the value read (above)
        out += "    if (target_ms_dbg.a >= 0 && any(target_ms_dbg.rg > 0)) color.rgb = target_ms_dbg.rgb; "
               "else if (target_ms_dbg.a >= 0) color.rgb *= 0.3;\n";
    if (g_target_ms_debug == 6)  // the target's mirror against the copy: red = own samples (x16), green = the filter's
        // texels, blue = a texel without all samples; the rest of such draws dimmed
        out += "    if (target_ms_dbg.a >= 0 && any(target_ms_dbg.rgb > 0)) color.rgb = target_ms_dbg.rgb; "
               "else if (target_ms_dbg.a >= 0) color.rgb *= 0.3;\n";
    // 3: the near map's coordinates of a cascade receiver (fractions of 1/256 of the map in red and green, of 1/256 of
    // its depth range in blue): equal in near and far frames when both address the same texel (issue 16 analysis)
    if (g_shadow_debug == 3)
        out += "    if (shadow_uvn.x > -1) color.rgb = frac(shadow_uvn * 256);\n";
    // 4, 5: the near (4) or far (5) map's coordinates in fractions of 1/64 of the map
    if (g_shadow_debug == 4)
        out += "    if (shadow_uvn.x > -1) color.rgb = frac(shadow_uvn * 64);\n";
    if (g_shadow_debug == 5)
        out += "    if (shadow_uvf.x > -1) color.rgb = frac(shadow_uvf * 64);\n";
    // GitHub #19, the target's mirror: the colour as the frame gets it (the same float to the same format: the same
    // bytes), alpha the marker of this draw
    if (config.mirror_write) {
        out += "    mirror_out = float4(color.rgb, mirror_id.x);\n";
        static const bool fail = GetEnvironmentVariableA("FCI_TEST_TARGET_MIRROR_FAILURE", nullptr, 0) > 0;
        if (fail) out += "    target_mirror_test_failure();\n";  // test of the fallback (renderer.cpp)
    }
    out += "    return color;\n}\n";
    return out;
}

}  // namespace gpu::psh
