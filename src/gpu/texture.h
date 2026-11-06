// NV2A texture memory layouts: swizzled (Morton order), linear (pitch) and DXT, decoded to BGRA8.
#pragma once

#include <cstdint>
#include <vector>

namespace gpu::texture {

enum class Layout : uint8_t { kSwizzled, kLinear, kCompressed };

struct Format {
    uint8_t bytes = 0;  // per pixel; per 4x4 block for DXT
    Layout layout = Layout::kSwizzled;
    bool supported = false;
};

// SET_TEXTURE_FORMAT COLOR field
Format Lookup(uint32_t color);

struct Desc {
    uint32_t address = 0;  // physical
    uint32_t color = 0;
    uint32_t width = 0, height = 0, depth = 1;
    uint32_t levels = 1;
    bool cube = false;
    uint32_t pitch = 0;    // linear layouts
    uint32_t palette = 0;  // physical address of the palette, 256 or fewer A8R8G8B8 entries
    uint32_t palette_entries = 0;
};

struct Image {
    uint32_t width, height, depth;
    std::vector<uint32_t> pixels;  // BGRA8
};

// bytes the texture occupies in guest memory, all levels and faces
uint32_t GuestSize(const Desc& d);
// every face (cube: 6) times every level, face major; empty for unsupported formats
std::vector<Image> Decode(const Desc& d);

// ---- performance: DXT textures as the GPU's own block formats (BC1/BC2/BC3), not decoded ----
struct Blocks {
    const uint8_t* data;  // the level's blocks in guest memory
    uint32_t row_pitch;   // bytes per row of blocks
    uint32_t bytes;       // bytes of the level
};
// 1, 2 or 3 for DXT1, DXT3, DXT5 (BC1..BC3), 0 for other formats; with levels: every face times every level, face
// major, as Decode; false when the texture cannot go as blocks (a level that is not whole blocks wide and high)
int BlockFormat(const Desc& d);
bool CompressedLevels(const Desc& d, std::vector<Blocks>& levels);

// Morton order addressing as the NV2A swizzles surfaces and textures
void Unswizzle(const uint8_t* src, uint8_t* dst, uint32_t width, uint32_t height, uint32_t depth, uint32_t bytes,
               uint32_t dst_pitch);
void Swizzle(const uint8_t* src, uint8_t* dst, uint32_t width, uint32_t height, uint32_t bytes, uint32_t src_pitch);

}  // namespace gpu::texture
