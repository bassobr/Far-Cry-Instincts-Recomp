#include "gpu/vsh.h"

#include <dxgiformat.h>

#include <cstdio>

#include "gpu/psh.h"  // performance: kReversedDepth

namespace gpu::vsh {

const char* const kOutputStruct = R"(
struct VSOut {
    float4 pos : SV_Position;
    float4 d0 : COLOR0;
    float4 d1 : COLOR1;
    float4 b0 : COLOR2;
    float4 b1 : COLOR3;
    float4 fog : FOG;
    float4 t0 : TEXCOORD0;
    float4 t1 : TEXCOORD1;
    float4 t2 : TEXCOORD2;
    float4 t3 : TEXCOORD3;
    float wdepth : WDEPTH;  // w / zmax: the depth W-buffering stores, linear in eye space
};
)";

namespace {

using namespace kelvin;

const char* const kPrelude = R"(
cbuffer Vertex : register(b0) {
    float4 defaults[16];
    float4 screen;        // 2 / width, 2 / height, 1 / zmax: guest screen space to NDC; w: W-buffering on (2: with
                          // the reversed depth, performance)
    float4 composite[4];  // fixed function transform, viewport included
    float4 hud;           // PC features (widescreen, fmv_scale): 2D vertices (w = 1) to xy * hud.xz + hud.yw
    // performance: last, so that a0-relative reads past either end read 0 (D3D11 bounds-checks constant buffer reads)
    // instead of a clamped index: clamping the index doubled the compile time of skinning programs (up to 97 ms)
    float4 C[192];
};

float4 rcc(float x) {
    float r = 1.0 / x;
    float m = clamp(abs(r), 5.42101e-20, 1.84467e19);
    return (r < 0 ? -m : m).xxxx;
}
float4 vexp(float x) { float f = floor(x); return float4(exp2(f), x - f, exp2(x), 1); }
float4 vlog(float x) {
    float a = abs(x);
    float e = floor(log2(a));
    return float4(e, a / exp2(e), log2(a), 1);
}
float4 vlit(float4 s) {
    float4 r = float4(1, 0, 0, 1);
    if (s.x > 0) {
        r.y = s.x;
        r.z = s.y > 0 ? pow(s.y, clamp(s.w, -127.9961, 127.9961)) : 0;
    }
    return r;
}
)";

uint32_t F(const uint32_t* w, int word, int shift, int bits) { return (w[word] >> shift) & ((1u << bits) - 1); }

std::string Swizzle(uint32_t swz) {
    static const char kComp[] = "xyzw";
    std::string s = ".";
    for (int i = 3; i >= 0; i--) s += kComp[(swz >> (2 * i)) & 3];
    return s == ".xyzw" ? "" : s;
}

std::string Mask(uint32_t mask) {
    std::string s = ".";
    if (mask & 8) s += 'x';
    if (mask & 4) s += 'y';
    if (mask & 2) s += 'z';
    if (mask & 1) s += 'w';
    return s;
}

std::string Operand(const uint32_t* w, int which, const char* constants) {
    uint32_t neg, swz, r, mux;
    if (which == 0) {
        neg = F(w, 1, 8, 1), swz = F(w, 1, 0, 8), r = F(w, 2, 28, 4), mux = F(w, 2, 26, 2);
    } else if (which == 1) {
        neg = F(w, 2, 25, 1), swz = F(w, 2, 17, 8), r = F(w, 2, 13, 4), mux = F(w, 2, 11, 2);
    } else {
        neg = F(w, 2, 10, 1), swz = F(w, 2, 2, 8), r = (F(w, 2, 0, 2) << 2) | F(w, 3, 30, 2), mux = F(w, 3, 28, 2);
    }
    char src[64];
    if (mux == 2) {
        snprintf(src, sizeof src, "v[%u]", F(w, 1, 9, 4));
    } else if (mux == 3) {
        uint32_t index = F(w, 1, 13, 8);
        if (F(w, 3, 1, 1) && constants[0] == 'C')  // performance: see the constant buffer
            snprintf(src, sizeof src, "C[a0 + %u]", index);
        else if (F(w, 3, 1, 1))  // a local copy (programs that write constants): no bounds check of its own
            snprintf(src, sizeof src, "%s[clamp(a0 + %u, 0, 191)]", constants, index);
        else
            snprintf(src, sizeof src, "%s[%u]", constants, index);
    } else {
        snprintf(src, sizeof src, "R[%u]", r > 12 ? 12 : r);
    }
    return std::string(neg ? "-" : "") + src + Swizzle(swz);
}

const uint32_t* Instruction(const State& s, uint32_t slot) { return s.program[slot]; }

bool ProgramMode(const State& s) { return (s.Reg(SET_TRANSFORM_EXECUTION_MODE) & 3) == 2; }

}  // namespace

uint64_t Key(const State& s, uint16_t enabled) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint32_t v) {
        for (int i = 0; i < 4; i++) h = (h ^ ((v >> (8 * i)) & 0xFF)) * 1099511628211ull;
    };
    // performance: the program's part only when it changed (State::program_generation), not at every draw
    static uint64_t generation = ~0ull, program = 0;
    if (s.program_generation != generation) {
        mix(ProgramMode(s));
        if (ProgramMode(s)) {
            for (uint32_t slot = s.Reg(SET_TRANSFORM_PROGRAM_START); slot < kProgramSlots; slot++) {
                const uint32_t* w = Instruction(s, slot);
                mix(w[1]), mix(w[2]), mix(w[3]);
                if (w[3] & 1) break;
            }
        }
        generation = s.program_generation, program = h;
    }
    h = program;
    mix(enabled);
    return h;
}

// ---- performance: raw draws ----
uint32_t FetchFormat(uint8_t format, int* kind) {
    uint32_t type = format & 0xF, size = (format >> 4) & 0xF;
    *kind = 0;
    switch (type) {
    case kFloat: {
        static const DXGI_FORMAT k[] = {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32B32_FLOAT,
                                        DXGI_FORMAT_R32G32B32A32_FLOAT};
        return k[(size - 1) & 3];
    }
    case kUbD3d: return DXGI_FORMAT_B8G8R8A8_UNORM;  // D3DCOLOR: bytes B G R A, x = R as the NV2A reads it
    case kUbOgl: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case kS1:
    case kS32k:
        *kind = 1;
        return size == 1 ? DXGI_FORMAT_R16_SINT : size == 2 ? DXGI_FORMAT_R16G16_SINT : DXGI_FORMAT_R16G16B16A16_SINT;
    case kCmp: *kind = 2; return DXGI_FORMAT_R32_UINT;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

namespace {
// the float4 an attribute of this format has (as kelvin's Convert makes it): missing components 0, 0, 0, 1
std::string Decode(int i, uint8_t format) {
    const std::string a = "input.a" + std::to_string(i), v = "v[" + std::to_string(i) + "]";
    uint32_t type = format & 0xF, size = (format >> 4) & 0xF;
    auto fill = [&](const std::string& f) {  // components past size from (0, 0, 0, 1)
        static const char* const kKeep[] = {"f", "float4(f.x, 0, 0, 1)", "float4(f.xy, 0, 1)", "float4(f.xyz, 1)", "f"};
        return "{ float4 f = " + f + "; " + v + " = " + kKeep[size > 4 ? 4 : size] + "; }";
    };
    switch (type) {
    case kFloat: return v + " = " + a + ";";  // the input assembler fills in 0, 0, 0, 1
    case kUbD3d:
    case kUbOgl: return fill(a);
    case kS1: return fill("(2.0 * float4(" + a + ") + 1.0) / 65535.0");
    case kS32k: return fill("float4(" + a + ")");
    case kCmp:
        return v + " = float4(float(asint(" + a + " << 21) >> 21) / 1023.0, float(asint(" + a +
               " << 10) >> 21) / 1023.0, float(asint(" + a + ") >> 22) / 511.0, 1);";
    default: return v + " = float4(0, 0, 0, 1);";
    }
}
}  // namespace
// ---- end performance ----

std::string Generate(const State& s, uint16_t enabled, const uint8_t* formats) {
    std::string out = kPrelude;
    {
        char c[64];
        snprintf(c, sizeof c, "static const float REVERSED_C = %.9g;\n", psh::kReversedDepth);
        out += c;
    }
    out += kOutputStruct;
    out += "struct VSIn {\n";
    for (int i = 0; i < kAttributes; i++)
        if (enabled & (1u << i)) {
            int kind = 0;
            if (formats) FetchFormat(formats[i], &kind);
            static const char* const kType[] = {"float4", "int4", "uint"};
            out += std::string("    ") + kType[kind] + " a" + std::to_string(i) + " : ATTR" + std::to_string(i) + ";\n";
        }
    out += "};\n\nVSOut main(VSIn input) {\n    float4 v[16];\n";
    for (int i = 0; i < kAttributes; i++) {
        if ((enabled & (1u << i)) && formats) {
            out += "    " + Decode(i, formats[i]) + "\n";
            continue;
        }
        out += "    v[" + std::to_string(i) + "] = ";
        out += (enabled & (1u << i)) ? "input.a" + std::to_string(i) : "defaults[" + std::to_string(i) + "]";
        out += ";\n";
    }
    out += "    float4 R[13];\n    [unroll] for (int k = 0; k < 13; k++) R[k] = 0;\n";
    out += "    float4 O[13];\n    [unroll] for (int j = 0; j < 13; j++) O[j] = float4(0, 0, 0, 1);\n";
    out += "    int a0 = 0;\n";

    if (ProgramMode(s)) {
        uint32_t start = s.Reg(SET_TRANSFORM_PROGRAM_START);
        // programs that write constants get a local copy
        bool writes_constants = false;
        for (uint32_t slot = start; slot < kProgramSlots; slot++) {
            const uint32_t* w = Instruction(s, slot);
            if (F(w, 3, 12, 4) && !F(w, 3, 11, 1)) writes_constants = true;
            if (w[3] & 1) break;
        }
        const char* constants = "C";
        if (writes_constants) {
            out += "    float4 K[192];\n    for (int c = 0; c < 192; c++) K[c] = C[c];\n";
            constants = "K";
        }
        static const char* const kMac[] = {
            nullptr, "a", "a * b", "a + c", "a * b + c", "dot(a.xyz, b.xyz).xxxx", "(dot(a.xyz, b.xyz) + b.w).xxxx",
            "dot(a, b).xxxx", "float4(1, a.y * b.y, a.z, b.w)", "min(a, b)", "max(a, b)", "(float4)(a < b)",
            "(float4)(a >= b)", nullptr, nullptr, nullptr};
        static const char* const kIlu[] = {nullptr, "c", "(1.0 / c.x).xxxx", "rcc(c.x)", "rsqrt(abs(c.x)).xxxx",
                                           "vexp(c.x)", "vlog(c.x)", "vlit(c)"};
        for (uint32_t slot = start; slot < kProgramSlots; slot++) {
            const uint32_t* w = Instruction(s, slot);
            uint32_t mac = F(w, 1, 21, 4), ilu = F(w, 1, 25, 3);
            uint32_t out_r = F(w, 3, 20, 4), mac_mask = F(w, 3, 24, 4), ilu_mask = F(w, 3, 16, 4);
            uint32_t o_mask = F(w, 3, 12, 4), orb = F(w, 3, 11, 1), address = F(w, 3, 3, 8), out_mux = F(w, 3, 2, 1);
            char line[256];
            out += "    {\n";
            out += "        float4 a = " + Operand(w, 0, constants) + ";\n";
            out += "        float4 b = " + Operand(w, 1, constants) + ";\n";
            out += "        float4 c = " + Operand(w, 2, constants) + ";\n";
            out += "        float4 m = 0, i = 0;\n";
            if (mac == 13) {
                out += "        a0 = int(floor(a.x + 0.001));\n";
            } else if (mac && kMac[mac]) {
                snprintf(line, sizeof line, "        m = %s;\n", kMac[mac]);
                out += line;
                if (mac_mask && out_r <= 12) {
                    snprintf(line, sizeof line, "        R[%u]%s = m%s;\n", out_r, Mask(mac_mask).c_str(),
                             Mask(mac_mask).c_str());
                    out += line;
                }
            }
            if (ilu) {
                snprintf(line, sizeof line, "        i = %s;\n", kIlu[ilu]);
                out += line;
                uint32_t r = mac ? 1 : out_r;  // paired with a MAC operation the ILU result goes to R1
                if (ilu_mask && r <= 12) {
                    snprintf(line, sizeof line, "        R[%u]%s = i%s;\n", r, Mask(ilu_mask).c_str(),
                             Mask(ilu_mask).c_str());
                    out += line;
                }
            }
            if (o_mask) {
                const char* value = out_mux ? "i" : "m";
                if (orb) {
                    uint32_t o = address & 0xF;
                    if (o == 0)
                        snprintf(line, sizeof line, "        R[12]%s = %s%s;\n", Mask(o_mask).c_str(), value,
                                 Mask(o_mask).c_str());
                    else if (o <= 12)
                        snprintf(line, sizeof line, "        O[%u]%s = %s%s;\n", o, Mask(o_mask).c_str(), value,
                                 Mask(o_mask).c_str());
                    else
                        line[0] = 0;
                } else {
                    snprintf(line, sizeof line, "        K[%u]%s = %s%s;\n", address % kConstants,
                             Mask(o_mask).c_str(), value, Mask(o_mask).c_str());
                }
                out += line;
            }
            out += "    }\n";
            if (w[3] & 1) break;
        }
    } else {
        // fixed function: transform by the composite matrix (it ends in screen space), colors and coordinates pass
        out += R"(    float4 fp = float4(dot(v[0], composite[0]), dot(v[0], composite[1]), dot(v[0], composite[2]),
                       dot(v[0], composite[3]));
    if (fp.w != 0) fp.xyz /= fp.w;
    R[12] = fp;
    O[3] = v[3];
    O[4] = v[4];
    O[9] = v[9];
    O[10] = v[10];
    O[11] = v[11];
    O[12] = v[12];
)";
    }

    out += R"(
    VSOut o;
    float4 p = R[12];
    float w = p.w;
    if (w == 0 || !(abs(w) <= 3.402823e38)) w = 1;  // also NaN; no isfinite (Wine's HLSL compiler lacks it)
    if (p.w == 1) p.xy = p.xy * hud.xz + hud.yw;
    o.pos = float4(p.x * screen.x - 1, 1 - p.y * screen.y, p.z * screen.z, 1);
    o.pos.xyz *= w;
    o.pos.w = w;
    // colours go to the rasterizer unclamped and are clamped per pixel (psh.cpp): FCI encodes the height above the
    // water as a colour, (z - 15) * 0.05 + 0.5, and the combiner switches at 0.5 – clamped per vertex, large terrain
    // triangles moved that edge off the waterline (turquoise slivers above the water at far shores)
    o.d0 = O[3];
    o.d1 = O[4];
    o.b0 = O[7];
    o.b1 = O[8];
    o.fog = O[5];
    o.t0 = O[9];
    o.t1 = O[10];
    o.t2 = O[11];
    o.t3 = O[12];
    o.wdepth = w * screen.z;
    // W-buffering stores and clips w, not z: the first person view's projection puts the hands in front of
    // its near plane (z < 0), which must not cut them away
    if (screen.w == 2) o.pos.z = REVERSED_C / screen.z;  // performance: reversed depth, c / (w / zmax) after the divide
    else if (screen.w != 0) o.pos.z = saturate(o.wdepth) * o.pos.w;
    return o;
}
)";
    return out;
}

}  // namespace gpu::vsh
