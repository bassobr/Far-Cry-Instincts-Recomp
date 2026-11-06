// The NV2A pixel pipeline as HLSL: texture shader stages, the register combiners, the final combiner,
// fog and the alpha test.
#pragma once

#include <cstdint>
#include <string>

#include "gpu/kelvin.h"

namespace gpu::psh {

// kShadow: a depth texture (a host depth target) that projective stages compare against r/q, with
// tex_scale.xy addressing it and tex_scale.z its depth range (65535 or 16777215)
enum class Sampler : uint8_t { k2D, kRect, kCube, k3D, kShadow };

// performance: what the pixel shader writes as depth (renderer.cpp decides by target and draw)
enum class Depth : uint8_t {
    kNone,         // nothing: the rasterizer's depth
    kW,            // W-buffering: w / zmax (+ polygon offset) at the centroid, as before
    kReversedW,    // reversed depth target, a W-buffered draw with polygon offset: c / (w / zmax + offset)
    kReversedZ,    // reversed depth target, a draw without W-buffering: c / (z + offset)
};

struct Config {
    Sampler sampler[4] = {};
    Depth depth = Depth::kNone;
};

// performance: reversed depth stores c / d for the guest's depth d (w / zmax or z / zmax)
constexpr float kReversedDepth = 1.0f / 67108864.0f;  // 2^-26

// ---- PC shadows: shadow map filter, set once before the first shader (renderer.cpp, HLE only) ----
// 0: the NV2A's (bilinear PCF over 2x2 texels), 1: soft (3x3 bilinear taps, a tent over 4x4 texels)
void SetShadowFilter(int filter);
// ---- end PC shadows ----

uint64_t Key(const kelvin::State& s, const Config& config);
std::string Generate(const kelvin::State& s, const Config& config);

}  // namespace gpu::psh
