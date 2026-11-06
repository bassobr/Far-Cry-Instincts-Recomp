#include "gpu/texture.h"

#include <algorithm>
#include <cstring>

#include "gpu/nv2a.h"

namespace gpu::texture {

namespace {

enum class Kind : uint8_t {
    kNone, kY8, kAY8, kA1R5G5B5, kX1R5G5B5, kA4R4G4B4, kR5G6B5, kA8R8G8B8, kX8R8G8B8, kI8, kDxt1, kDxt3, kDxt5,
    kA8, kA8Y8, kYuy2, kUyvy, kR6G5B5, kG8B8, kR8B8, kDepth24, kDepth16, kA8B8G8R8, kB8G8R8A8, kR8G8B8A8,
};

struct Entry {
    Kind kind;
    uint8_t bytes;
    Layout layout;
};

Entry Find(uint32_t color) {
    constexpr auto S = Layout::kSwizzled, L = Layout::kLinear, C = Layout::kCompressed;
    switch (color) {
    case 0x00: return {Kind::kY8, 1, S};
    case 0x01: return {Kind::kAY8, 1, S};
    case 0x02: return {Kind::kA1R5G5B5, 2, S};
    case 0x03: return {Kind::kX1R5G5B5, 2, S};
    case 0x04: return {Kind::kA4R4G4B4, 2, S};
    case 0x05: return {Kind::kR5G6B5, 2, S};
    case 0x06: return {Kind::kA8R8G8B8, 4, S};
    case 0x07: return {Kind::kX8R8G8B8, 4, S};
    case 0x0B: return {Kind::kI8, 1, S};
    case 0x0C: return {Kind::kDxt1, 8, C};
    case 0x0E: return {Kind::kDxt3, 16, C};
    case 0x0F: return {Kind::kDxt5, 16, C};
    case 0x10: return {Kind::kA1R5G5B5, 2, L};
    case 0x11: return {Kind::kR5G6B5, 2, L};
    case 0x12: return {Kind::kA8R8G8B8, 4, L};
    case 0x13: return {Kind::kY8, 1, L};
    case 0x19: return {Kind::kA8, 1, S};
    case 0x1A: return {Kind::kA8Y8, 2, S};
    case 0x1B: return {Kind::kAY8, 1, L};
    case 0x1C: return {Kind::kX1R5G5B5, 2, L};
    case 0x1D: return {Kind::kA4R4G4B4, 2, L};
    case 0x1E: return {Kind::kX8R8G8B8, 4, L};
    case 0x1F: return {Kind::kA8, 1, L};
    case 0x20: return {Kind::kA8Y8, 2, L};
    case 0x24: return {Kind::kYuy2, 2, L};
    case 0x25: return {Kind::kUyvy, 2, L};
    case 0x27: return {Kind::kR6G5B5, 2, S};
    case 0x28: return {Kind::kG8B8, 2, S};
    case 0x29: return {Kind::kR8B8, 2, S};
    case 0x2A: return {Kind::kDepth24, 4, S};
    case 0x2C: return {Kind::kDepth16, 2, S};
    case 0x2E: return {Kind::kDepth24, 4, L};
    case 0x30: return {Kind::kDepth16, 2, L};
    case 0x3A: return {Kind::kA8B8G8R8, 4, S};
    case 0x3B: return {Kind::kB8G8R8A8, 4, S};
    case 0x3C: return {Kind::kR8G8B8A8, 4, S};
    case 0x3F: return {Kind::kA8B8G8R8, 4, L};
    case 0x40: return {Kind::kB8G8R8A8, 4, L};
    case 0x41: return {Kind::kR8G8B8A8, 4, L};
    default: return {Kind::kNone, 0, S};
    }
}

uint32_t Bgra(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return (a << 24) | (r << 16) | (g << 8) | b; }
uint32_t Expand5(uint32_t v) { return (v << 3) | (v >> 2); }
uint32_t Expand6(uint32_t v) { return (v << 2) | (v >> 4); }
uint32_t Expand4(uint32_t v) { return v * 17; }

uint32_t Pixel(Kind kind, const uint8_t* p, const uint32_t* palette) {
    uint32_t v16 = p[0] | (p[1] << 8);
    switch (kind) {
    case Kind::kY8: return Bgra(p[0], p[0], p[0], 255);
    case Kind::kAY8: return Bgra(p[0], p[0], p[0], p[0]);
    case Kind::kA8: return Bgra(255, 255, 255, p[0]);
    case Kind::kA8Y8: return Bgra(p[0], p[0], p[0], p[1]);
    case Kind::kA1R5G5B5:
        return Bgra(Expand5((v16 >> 10) & 31), Expand5((v16 >> 5) & 31), Expand5(v16 & 31), (v16 & 0x8000) ? 255 : 0);
    case Kind::kX1R5G5B5: return Bgra(Expand5((v16 >> 10) & 31), Expand5((v16 >> 5) & 31), Expand5(v16 & 31), 255);
    case Kind::kA4R4G4B4:
        return Bgra(Expand4((v16 >> 8) & 15), Expand4((v16 >> 4) & 15), Expand4(v16 & 15), Expand4(v16 >> 12));
    case Kind::kR5G6B5: return Bgra(Expand5(v16 >> 11), Expand6((v16 >> 5) & 63), Expand5(v16 & 31), 255);
    case Kind::kR6G5B5: return Bgra(Expand6(v16 >> 10), Expand5((v16 >> 5) & 31), Expand5(v16 & 31), 255);
    case Kind::kG8B8: return Bgra(0, p[1], p[0], 255);
    case Kind::kR8B8: return Bgra(p[1], 0, p[0], 255);
    case Kind::kA8R8G8B8: return Bgra(p[2], p[1], p[0], p[3]);
    case Kind::kX8R8G8B8: return Bgra(p[2], p[1], p[0], 255);
    case Kind::kA8B8G8R8: return Bgra(p[0], p[1], p[2], p[3]);
    case Kind::kB8G8R8A8: return Bgra(p[1], p[2], p[3], p[0]);
    case Kind::kR8G8B8A8: return Bgra(p[3], p[2], p[1], p[0]);
    case Kind::kI8: return palette ? palette[p[0]] : Bgra(p[0], p[0], p[0], 255);
    case Kind::kDepth24: return Bgra(p[3], p[3], p[3], 255);
    case Kind::kDepth16: return Bgra(p[1], p[1], p[1], 255);
    default: return 0;
    }
}

void Masks(uint32_t w, uint32_t h, uint32_t d, uint32_t& mx, uint32_t& my, uint32_t& mz) {
    mx = my = mz = 0;
    uint32_t bit = 1;
    for (uint32_t i = 1; i < w || i < h || i < d; i <<= 1) {
        if (i < w) mx |= bit, bit <<= 1;
        if (i < h) my |= bit, bit <<= 1;
        if (i < d) mz |= bit, bit <<= 1;
    }
}

// spreads the bits of v over the set bits of mask
uint32_t Deposit(uint32_t v, uint32_t mask) {
    uint32_t r = 0;
    for (uint32_t b = 1; mask; b <<= 1, mask &= mask - 1)
        if (v & b) r |= mask & (0u - mask);
    return r;
}

std::vector<uint32_t> Offsets(uint32_t n, uint32_t mask) {
    std::vector<uint32_t> table(n);
    for (uint32_t i = 0; i < n; i++) table[i] = Deposit(i, mask);
    return table;
}

void Dxt(Kind kind, const uint8_t* src, Image& img) {
    uint32_t bw = std::max(1u, (img.width + 3) / 4), bh = std::max(1u, (img.height + 3) / 4);
    uint32_t block_bytes = kind == Kind::kDxt1 ? 8 : 16;
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++) {
            const uint8_t* b = src + (size_t(by) * bw + bx) * block_bytes;
            const uint8_t* color = kind == Kind::kDxt1 ? b : b + 8;
            uint32_t c0 = color[0] | (color[1] << 8), c1 = color[2] | (color[3] << 8);
            uint32_t r[4], g[4], bl[4], a[4] = {255, 255, 255, 255};
            r[0] = Expand5(c0 >> 11), g[0] = Expand6((c0 >> 5) & 63), bl[0] = Expand5(c0 & 31);
            r[1] = Expand5(c1 >> 11), g[1] = Expand6((c1 >> 5) & 63), bl[1] = Expand5(c1 & 31);
            if (c0 > c1 || kind != Kind::kDxt1) {
                r[2] = (2 * r[0] + r[1]) / 3, g[2] = (2 * g[0] + g[1]) / 3, bl[2] = (2 * bl[0] + bl[1]) / 3;
                r[3] = (r[0] + 2 * r[1]) / 3, g[3] = (g[0] + 2 * g[1]) / 3, bl[3] = (bl[0] + 2 * bl[1]) / 3;
            } else {
                r[2] = (r[0] + r[1]) / 2, g[2] = (g[0] + g[1]) / 2, bl[2] = (bl[0] + bl[1]) / 2;
                r[3] = g[3] = bl[3] = 0, a[3] = 0;
            }
            uint32_t indices = color[4] | (color[5] << 8) | (color[6] << 16) | (uint32_t(color[7]) << 24);
            uint8_t alpha[8] = {};
            uint64_t alpha_bits = 0;
            if (kind == Kind::kDxt5) {
                alpha[0] = b[0], alpha[1] = b[1];
                if (alpha[0] > alpha[1]) {
                    for (int i = 1; i < 7; i++) alpha[i + 1] = uint8_t(((7 - i) * alpha[0] + i * alpha[1]) / 7);
                } else {
                    for (int i = 1; i < 5; i++) alpha[i + 1] = uint8_t(((5 - i) * alpha[0] + i * alpha[1]) / 5);
                    alpha[6] = 0, alpha[7] = 255;
                }
                for (int i = 0; i < 6; i++) alpha_bits |= uint64_t(b[2 + i]) << (8 * i);
            }
            for (uint32_t py = 0; py < 4; py++)
                for (uint32_t px = 0; px < 4; px++) {
                    uint32_t x = bx * 4 + px, y = by * 4 + py;
                    if (x >= img.width || y >= img.height) continue;
                    uint32_t i = py * 4 + px, ci = (indices >> (2 * i)) & 3, av = a[ci];
                    if (kind == Kind::kDxt3) av = Expand4((b[i / 2] >> (4 * (i & 1))) & 15);
                    if (kind == Kind::kDxt5) av = alpha[(alpha_bits >> (3 * i)) & 7];
                    img.pixels[size_t(y) * img.width + x] = Bgra(r[ci], g[ci], bl[ci], av);
                }
        }
}

uint32_t LevelSize(const Entry& e, uint32_t w, uint32_t h, uint32_t d) {
    if (e.layout == Layout::kCompressed) return std::max(1u, (w + 3) / 4) * std::max(1u, (h + 3) / 4) * d * e.bytes;
    return w * h * d * e.bytes;
}

}  // namespace

Format Lookup(uint32_t color) {
    Entry e = Find(color);
    return {e.bytes, e.layout, e.kind != Kind::kNone};
}

uint32_t GuestSize(const Desc& d) {
    Entry e = Find(d.color);
    if (e.kind == Kind::kNone) return 0;
    if (e.layout == Layout::kLinear) return d.pitch * d.height;
    uint32_t face = 0;
    for (uint32_t l = 0; l < d.levels; l++)
        face += LevelSize(e, std::max(1u, d.width >> l), std::max(1u, d.height >> l), std::max(1u, d.depth >> l));
    if (!d.cube) return face;
    return ((face + 127) & ~127u) * 6;
}

// ---- performance: DXT as BC ----
int BlockFormat(const Desc& d) {
    switch (Find(d.color).kind) {
    case Kind::kDxt1: return 1;
    case Kind::kDxt3: return 2;
    case Kind::kDxt5: return 3;
    default: return 0;
    }
}

bool CompressedLevels(const Desc& d, std::vector<Blocks>& levels) {
    Entry e = Find(d.color);
    if (!BlockFormat(d) || d.depth > 1 || !d.width || !d.height) return false;
    // D3D 11 wants the top level in whole blocks; the smaller levels may be partial
    if (d.width % 4 || d.height % 4) return false;
    uint32_t size = GuestSize(d);
    if (!size || d.address + uint64_t(size) > 0x04000000) return false;
    const uint8_t* base = Physical(d.address);
    uint32_t faces = d.cube ? 6 : 1, offset = 0;
    levels.clear();
    for (uint32_t f = 0; f < faces; f++) {
        for (uint32_t l = 0; l < d.levels; l++) {
            uint32_t w = std::max(1u, d.width >> l), h = std::max(1u, d.height >> l);
            uint32_t bytes = LevelSize(e, w, h, 1);
            levels.push_back({base + offset, std::max(1u, (w + 3) / 4) * e.bytes, bytes});
            offset += bytes;
        }
        if (d.cube) offset = (offset + 127) & ~127u;
    }
    return true;
}
// ---- end performance ----

std::vector<Image> Decode(const Desc& d) {
    Entry e = Find(d.color);
    std::vector<Image> out;
    uint32_t size = GuestSize(d);
    if (e.kind == Kind::kNone || !d.width || !d.height || !size || d.address + uint64_t(size) > 0x04000000) return out;
    const uint8_t* base = Physical(d.address);
    std::vector<uint32_t> palette;
    if (e.kind == Kind::kI8 && d.palette_entries) {
        palette.assign(256, 0);
        std::memcpy(palette.data(), Physical(d.palette), d.palette_entries * 4);
    }
    std::vector<uint8_t> linear;
    uint32_t faces = d.cube ? 6 : 1, levels = e.layout == Layout::kLinear ? 1 : d.levels;
    uint32_t offset = 0;
    for (uint32_t f = 0; f < faces; f++) {
        for (uint32_t l = 0; l < levels; l++) {
            Image img{std::max(1u, d.width >> l), std::max(1u, d.height >> l), std::max(1u, d.depth >> l), {}};
            img.pixels.resize(size_t(img.width) * img.height * img.depth);
            const uint8_t* src = base + offset;
            if (e.layout == Layout::kCompressed) {
                Dxt(e.kind, src, img);
            } else if (e.kind == Kind::kYuy2 || e.kind == Kind::kUyvy) {
                bool uyvy = e.kind == Kind::kUyvy;
                for (uint32_t y = 0; y < img.height; y++)
                    for (uint32_t x = 0; x < img.width; x++) {
                        const uint8_t* p = src + size_t(y) * d.pitch + (x & ~1u) * 2;
                        int luma = uyvy ? p[1 + 2 * (x & 1)] : p[2 * (x & 1)];
                        int u = uyvy ? p[0] : p[1], v = uyvy ? p[2] : p[3];
                        int c = luma - 16, du = u - 128, dv = v - 128;
                        auto clamp = [](int x) { return uint32_t(std::clamp(x, 0, 255)); };
                        img.pixels[size_t(y) * img.width + x] =
                            Bgra(clamp((298 * c + 409 * dv + 128) >> 8), clamp((298 * c - 100 * du - 208 * dv + 128) >> 8),
                                 clamp((298 * c + 516 * du + 128) >> 8), 255);
                    }
            } else {
                const uint8_t* rows = src;
                uint32_t pitch = d.pitch;
                if (e.layout == Layout::kSwizzled) {
                    pitch = img.width * e.bytes;
                    linear.resize(size_t(pitch) * img.height * img.depth);
                    Unswizzle(src, linear.data(), img.width, img.height, img.depth, e.bytes, pitch);
                    rows = linear.data();
                }
                const uint32_t* pal = palette.empty() ? nullptr : palette.data();
                for (uint32_t z = 0; z < img.depth; z++)
                    for (uint32_t y = 0; y < img.height; y++) {
                        const uint8_t* row = rows + (size_t(z) * img.height + y) * pitch;
                        uint32_t* dst = &img.pixels[(size_t(z) * img.height + y) * img.width];
                        for (uint32_t x = 0; x < img.width; x++) dst[x] = Pixel(e.kind, row + x * e.bytes, pal);
                    }
            }
            offset += LevelSize(e, img.width, img.height, img.depth);
            out.push_back(std::move(img));
        }
        if (d.cube) offset = (offset + 127) & ~127u;
    }
    return out;
}

void Unswizzle(const uint8_t* src, uint8_t* dst, uint32_t width, uint32_t height, uint32_t depth, uint32_t bytes,
               uint32_t dst_pitch) {
    uint32_t mx, my, mz;
    Masks(width, height, depth, mx, my, mz);
    auto xo = Offsets(width, mx), yo = Offsets(height, my), zo = Offsets(depth, mz);
    for (uint32_t z = 0; z < depth; z++)
        for (uint32_t y = 0; y < height; y++) {
            uint8_t* row = dst + (size_t(z) * height + y) * dst_pitch;
            uint32_t base = yo[y] | zo[z];
            if (bytes == 4) {
                for (uint32_t x = 0; x < width; x++)
                    std::memcpy(row + x * 4, src + size_t(base | xo[x]) * 4, 4);
            } else {
                for (uint32_t x = 0; x < width; x++)
                    std::memcpy(row + x * bytes, src + size_t(base | xo[x]) * bytes, bytes);
            }
        }
}

void Swizzle(const uint8_t* src, uint8_t* dst, uint32_t width, uint32_t height, uint32_t bytes, uint32_t src_pitch) {
    uint32_t mx, my, mz;
    Masks(width, height, 1, mx, my, mz);
    auto xo = Offsets(width, mx), yo = Offsets(height, my);
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* row = src + size_t(y) * src_pitch;
        for (uint32_t x = 0; x < width; x++) std::memcpy(dst + size_t(yo[y] | xo[x]) * bytes, row + x * bytes, bytes);
    }
}

}  // namespace gpu::texture
