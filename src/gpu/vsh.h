// NV2A vertex programs (and the fixed function transform) as HLSL vertex shaders.
#pragma once

#include <cstdint>
#include <string>

#include "gpu/kelvin.h"

namespace gpu::vsh {

// identifies the shader the current state needs; equal keys give equal HLSL
uint64_t Key(const kelvin::State& s, uint16_t enabled);
// formats (performance: raw draws, kelvin::Draw::format): the enabled attributes arrive as the guest stores them
// (FetchFormat) and are decoded to float4 here; nullptr: as float4
std::string Generate(const kelvin::State& s, uint16_t enabled, const uint8_t* formats = nullptr);
// raw draws: the input assembler format of an attribute (a DXGI_FORMAT) and whether the shader reads it as int4
// (1), uint (2) or float4 (0)
uint32_t FetchFormat(uint8_t format, int* kind);

// HLSL interface shared with the pixel shader
extern const char* const kOutputStruct;

}  // namespace gpu::vsh
