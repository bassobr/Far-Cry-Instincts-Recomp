// The NV2A pixel pipeline as HLSL: texture shader stages, the register combiners, the final combiner,
// fog and the alpha test.
#pragma once

#include <cstdint>
#include <string>

#include "gpu/kelvin.h"

namespace gpu::psh {

// kShadow: a depth texture (a host depth target) that projective stages compare against r/q, with
// tex_scale.xy addressing it and tex_scale.z its depth range (65535 or 16777215)
// kSceneDepth / kSceneDepthMS: a linear depth texture on a 24 bit depth target (the 3D view's, float on the host,
// multisampled or not, maybe reversed): compared like kShadow, per texel of the guest's size (a linear filter) or of
// the host's (the point filter: the pixel's own, GitHub #14), with tex_scale.xy the host
// pixels per guest texel, tex_scale.z the depth range and tex_scale.w the reversed depth's constant (0: not reversed)
// kTargetMS (GitHub #19): a linear texture on the multisampled colour target the draw itself renders into, read at
// projected coordinates (a pass that reads back what an earlier pass drew at the same pixel): a multisampled copy of
// the target, read where a pixel's coordinates are its own position as the average of the samples the fragment covers
// (and its surface is visible at: the samples its output goes to; the earlier pass's value for this fragment's own
// part of the pixel, not the average with whatever covers the rest), elsewhere filtered from the samples' average as
// the resolved copy would be. tex_scale.xy: host pixels per guest texel, tex_scale.zw: the host rows the texture
// covers (split screen's lower view starts further down). With Config::mirror_read the texture is the target's mirror
// instead of a copy (Config, below)
enum class Sampler : uint8_t { k2D, kRect, kCube, k3D, kShadow, kSceneDepth, kSceneDepthMS, kTargetMS };

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
    // PC shadows: the stages (bit i: stage i) whose shadow map has a near cascade (textures t4-t6, cbuffer b5:
    // renderer.cpp, "shadow cascades"); all of them sample the same map with the same sun camera
    uint8_t cascade = 0;
    // signed textures (GitHub #14): per stage, the channels (bit 0 A, 1 R, 2 G, 3 B) its texture holds as offset
    // binary (renderer.cpp, g_stage_signs); the shader turns them back into two's complement after filtering
    uint8_t signs[4] = {};
    // GitHub #19: kTargetMS stages read, of the samples the fragment covers, those whose depth (the depth target as t7,
    // bound read-only: the draw writes no depth) is not nearer than the fragment's own there, i.e. the samples the
    // fragment's own surface is visible at. 0: no depth to read (all covered samples), 1: depth, 2: reversed depth
    uint8_t target_ms_depth = 0;
    // ---- GitHub #19, the target's mirror (renderer.cpp, "target mirror"): instead of a multisampled copy per read, the
    // pass a self-read follows (the terrain's lighting pass) writes its colour into a second multisampled target as
    // well (SV_Target1: rgb as the frame's, alpha its marker, cbuffer b6 mirror_id.x); the reading pass's kTargetMS
    // stages in mirror_read (bit per stage) read that target and take a sample only where the marker is that pass's
    // (mirror_id.y), i.e. where it is the pass's own output, the same as the frame's there ----
    bool mirror_write = false;
    uint8_t mirror_read = 0;
    // the filter's texels (inside a surface, or away from the own pixel) from the target's resolved copy (t6), exactly
    // as from the multisampled copy; false: from the mirror's samples with the marker (no resolve, a texel beyond the
    // surface's edge gives its weight to the others)
    bool mirror_resolved = false;
};

// GitHub #19, the target's mirror: whether the shader uses the alpha of a stage's texture (combiner inputs, the
// final combiner, the alpha kill, dependent stages after it); the mirror keeps its marker there
bool StageAlphaUsed(const kelvin::State& s, int stage);

// performance: reversed depth stores c / d for the guest's depth d (w / zmax or z / zmax)
constexpr float kReversedDepth = 1.0f / 67108864.0f;  // 2^-26

// ---- PC shadows: shadow map filter, set once before the first shader (renderer.cpp, HLE only) ----
// 0: the NV2A's (bilinear PCF over 2x2 texels), 1: soft (3x3 bilinear taps, a tent over 4x4 texels)
void SetShadowFilter(int filter);
// ---- end PC shadows ----

uint64_t Key(const kelvin::State& s, const Config& config);
std::string Generate(const kelvin::State& s, const Config& config);

}  // namespace gpu::psh
