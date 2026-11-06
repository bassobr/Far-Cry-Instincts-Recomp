// fps_counter = 1: frame rate and frame time, top left over the presented picture (HLE graphics).
// The idea follows SCDA-XBERecomp's src/app/fps_counter.cpp (MIT, Copyright (c) 2026 TGP482, Joshhhuaaa); drawn here
// with its own small bitmap font instead of the game's.
#pragma once

#include <d3d11.h>

#include <cstdint>

namespace gpu::fps {

bool Enabled();
void SetEnabled(bool on);  // settings menu: from the next frame
// once per presented frame: counts it and draws the counter into target (w x h, the composed frame)
void Draw(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11RenderTargetView* target, uint32_t w, uint32_t h);

}  // namespace gpu::fps
