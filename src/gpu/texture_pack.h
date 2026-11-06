// Texture dumping and replacement (texture packs), HLE and LLE alike.
// A game texture is named by a 64 bit hash of its level 0 guest bytes (every face of a cube map), its palette and
// its shape (format, width, height), written as 16 hex digits. It does not depend on where the texture lies in
// memory, so the name is the same in every run.
//  - texture_dump = 1: textures\dump\<hash>.png beside the exe (2D and rect textures, level 0) or <hash>.dds (cube
//    maps: BGRA8, every face and level), each once, written on a worker thread.
//  - texture_replace = 1: an image in textures\replace\ (any subfolder) whose name contains <hash> (PNG, or DDS:
//    BGRA8/RGBA8, DXT1/3/5, BC7, cube maps) is drawn instead, at any resolution. Loaded and uploaded on the worker;
//    until it is ready the game's own texture is used. Changes to the folder are picked up while the game runs.
// Adapted from SCDA-XBERecomp (MIT, Copyright (c) 2026 TGP482, Joshhhuaaa), src/gpu/texturedumper.cpp.
#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

#include "gpu/texture.h"

namespace gpu::texpack {

bool Dumping();
bool Replacing();
inline bool Active() { return Dumping() || Replacing(); }

// the texture's name, 0 when it cannot be dumped or replaced (3D, depth, video formats)
uint64_t Key(const texture::Desc& d);

// dumping is on and key has neither been dumped (in this or an earlier run) nor has a replacement
bool WantsDump(uint64_t key);
// once per key: hands the decoded images (face major, as texture::Decode returns them) to the worker, which
// writes the image and a line in textures\dump\index.txt (hash, size, guest format, layout, levels)
void Dump(uint64_t key, const texture::Desc& d, std::vector<texture::Image>&& images);

enum class Lookup { kNone, kLoading, kReady };
// the replacement for key; cube: the texture is sampled as a cube map (only a cube DDS fits), else 2D.
// kLoading: the first ask starts loading it, ask again later
Lookup Find(ID3D11Device* device, uint64_t key, bool cube, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& out);
// advances whenever the replacement folder was listed again: replacements are looked up anew
uint32_t Revision();

// once per presented frame: notices changes in textures\replace
void Frame();

}  // namespace gpu::texpack
