#include "gpu/psh.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>

#include "gpu/vsh.h"
#include "runtime/log.h"

namespace gpu::psh {

namespace {

using namespace kelvin;

int g_shadow_filter = 0;  // PC shadows (SetShadowFilter)
const int g_shadow_debug = [] {  // PC shadows: FCI_SHADOW_DEBUG (diagnostics)
    char v[8] = {};
    return GetEnvironmentVariableA("FCI_SHADOW_DEBUG", v, sizeof v) ? atoi(v) : 0;
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
    for (int i = 0; i < 4; i++) mix(uint32_t(config.sampler[i])), shadow |= config.sampler[i] == Sampler::kShadow;
    if (shadow) mix(s.Reg(SET_SHADOW_COMPARE_FUNC));
    return h;
}

std::string Generate(const State& s, const Config& config) {
    std::string out = kPrelude;
    static const char* const kTextureType[] = {"Texture2D", "Texture2D", "TextureCube", "Texture3D", "Texture2D"};
    for (int i = 0; i < 4; i++) {
        char line[128];
        snprintf(line, sizeof line, "%s tex%d : register(t%d);\nSamplerState smp%d : register(s%d);\n",
                 kTextureType[int(config.sampler[i])], i, i, i, i);
        out += line;
    }

    // the vertex shader's outputs, with flat colors when the shade mode asks for them
    std::string input = vsh::kOutputStruct;
    if (s.Reg(SET_SHADE_MODE) == 0x1D00) {
        for (const char* member : {"float4 d0", "float4 d1", "float4 b0", "float4 b1"}) {
            size_t at = input.find(member);
            if (at != std::string::npos) input.insert(at, "nointerpolation ");
        }
    }
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
    switch (config.depth) {
    case Depth::kW:
        out += "\nfloat4 main(VSOut input, bool front : SV_IsFrontFace, out float depth : SV_Depth) : SV_Target {\n"
               "    depth = saturate(EvaluateAttributeCentroid(input.wdepth) + depth_bias.x + depth_bias.y * "
               "max(abs(ddx(input.wdepth)), abs(ddy(input.wdepth))));\n";
        break;
    case Depth::kReversedW:
        out += "\nfloat4 main(VSOut input, bool front : SV_IsFrontFace, out float depth : SV_Depth) : SV_Target {\n"
               "    depth = " + c_value + " / max(EvaluateAttributeCentroid(input.wdepth) + depth_bias.x + depth_bias.y * "
               "max(abs(ddx(input.wdepth)), abs(ddy(input.wdepth))), " + c_value + ");\n";
        break;
    case Depth::kReversedZ:
        out += "\nfloat4 main(VSOut input, bool front : SV_IsFrontFace, out float depth : SV_Depth) : SV_Target {\n"
               "    depth = " + c_value + " / max(input.pos.z + depth_bias.x + depth_bias.y * "
               "max(abs(ddx(input.pos.z)), abs(ddy(input.pos.z))), " + c_value + ");\n";
        break;
    default:
        out += "\nfloat4 main(VSOut input, bool front : SV_IsFrontFace) : SV_Target {\n";
        break;
    }
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
    if (g_shadow_debug) out += "    float shadow_dbg = 1, shadow_in = -1, shadow_ref = 0;\n";  // PC shadows
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
            if (g_shadow_filter == 1) {
                // ---- PC shadows: soft, the average of 3x3 bilinear lookups one texel apart (a tent over the
                // 4x4 texels base - 1 .. base + 2 around the point, weights (1 - f, 1, 1, f) per axis), from four
                // gathers of 2x2 texels (gather order: x left bottom, y right bottom, z right top, w left top) ----
                snprintf(shadow, sizeof shadow,
                         "    {\n"
                         "        float3 p = T[%d].xyz / (T[%d].w == 0 ? 1 : T[%d].w);\n"
                         "        float2 uv = p.xy * tex_scale[%d].xy;\n"
                         "        float ref = p.z / tex_scale[%d].z, w, h;\n"
                         "        tex%d.GetDimensions(w, h);\n"
                         "        float2 size = float2(w, h), q = uv * size - 0.5, base = floor(q), f = q - base;\n"
                         "        float sum = 0;\n"
                         "        [unroll] for (int gy = 0; gy < 2; gy++) [unroll] for (int gx = 0; gx < 2; gx++) {\n"
                         "            float4 d = tex%d.Gather(smp%d, (base + 0.25 + 2 * float2(gx, gy)) / size);\n"
                         "            float4 lit = (%s) ? 1 : 0;\n"
                         "            float wl = gx ? 1 : 1 - f.x, wr = gx ? f.x : 1, wt = gy ? 1 : 1 - f.y, wb = gy ? f.y : 1;\n"
                         "            sum += dot(lit, float4(wl * wb, wr * wb, wr * wt, wl * wt));\n"
                         "        }\n"
                         "        t%d = (sum / 9).xxxx;\n"
                         "    }\n",
                         i, i, i, i, i, i, i, i, op.c_str(), i);
                // ---- end PC shadows ----
            } else {
                snprintf(shadow, sizeof shadow,
                         "    {\n"
                         "        float3 p = T[%d].xyz / (T[%d].w == 0 ? 1 : T[%d].w);\n"
                         "        float2 uv = p.xy * tex_scale[%d].xy;\n"
                         "        float ref = p.z / tex_scale[%d].z, w, h;\n"
                         "        tex%d.GetDimensions(w, h);\n"
                         "        float2 f = frac(uv * float2(w, h) - 0.5);\n"
                         "        float4 d = tex%d.Gather(smp%d, uv);\n"
                         "        float4 lit = (%s) ? 1 : 0;\n"
                         "        t%d = dot(lit, float4((1 - f.x) * f.y, f.x * f.y, f.x * (1 - f.y), (1 - f.x) * (1 - f.y))).xxxx;\n"
                         "    }\n",
                         i, i, i, i, i, i, i, i, op.c_str(), i);
            }
            out += shadow;
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
                     "    float2 d%d = signed2(t%d.bg);\n"
                     "    float4 u%d = T[%d] + float4(d%d.x * bump[%d].x + d%d.y * bump[%d].z, "
                     "d%d.x * bump[%d].y + d%d.y * bump[%d].w, 0, 0);\n",
                     i, prev, i, i, i, i, i, i, i, i, i, i);
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
    // PC shadows: FCI_SHADOW_DEBUG=1 tints what a shadow map covers green and its shadow red
    // (2: red = outside the map, green = lit, blue = beyond its depth range)
    if (g_shadow_debug == 2)
        out += "    if (shadow_in >= 0) color.rgb = float3(1 - shadow_in, shadow_dbg, shadow_ref >= 1 ? 1 : 0);\n";
    if (g_shadow_debug == 1)
        out += "    if (shadow_in >= 0) color.rgb = lerp(lerp(color.rgb, float3(0, 0.6, 0), 0.25 * shadow_in), float3(1, 0, 0), "
               "0.7 * (1 - shadow_dbg));\n";
    out += "    return color;\n}\n";
    return out;
}

}  // namespace gpu::psh
