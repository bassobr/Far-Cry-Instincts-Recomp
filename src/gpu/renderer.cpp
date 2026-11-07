#include "gpu/renderer.h"

#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <intrin.h>  // performance: __cpuid, crc32 (ContentHash)
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>  // PC shadows
#include <deque>
#include <map>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "gpu/ao.h"
#include "gpu/fmv.h"  // PC features: fmv_scale
#include "gpu/nv2a.h"
#include "gpu/present.h"
#include "gpu/psh.h"
#include "gpu/texture.h"
#include "gpu/texture_pack.h"  // texture dump/replacement
#include "app/output.h"  // display output options
#include "app/pcmode.h"
#include "app/settings.h"
#include "gpu/vsh.h"
#include "gpu/water.h"  // PC water
#include "runtime/hitch.h"  // performance: FCI_TRACE_HITCHES
#include "runtime/log.h"

namespace gpu::render {

namespace {

using Microsoft::WRL::ComPtr;
using namespace kelvin;

ComPtr<ID3D11Device> g_device;
ComPtr<ID3D11DeviceContext> g_context;
ComPtr<ID3D11InfoQueue> g_info;  // with FCI_D3D_DEBUG: the debug layer's messages go to the log

void DrainMessages() {
    if (!g_info) return;
    static int logged = 0;
    UINT64 n = g_info->GetNumStoredMessages();
    for (UINT64 i = 0; i < n && logged < 200; i++, logged++) {
        SIZE_T size = 0;
        g_info->GetMessage(i, nullptr, &size);
        std::vector<char> buffer(size);
        auto* message = reinterpret_cast<D3D11_MESSAGE*>(buffer.data());
        if (SUCCEEDED(g_info->GetMessage(i, message, &size))) rt::Log("d3d11: %s", message->pDescription);
    }
    g_info->ClearStoredMessages();
}
bool g_init_done = false, g_failed = false;
bool g_hle = false;  // HLE graphics: targets are not mirrored to guest memory
bool g_counting = false;  // pixels are counted for a visibility test (SET_ZPASS_PIXEL_COUNT_ENABLE)

// HLE graphics quality (FarCryInstincts.ini / FCI_<KEY>, see README): render targets at scale x the console's size,
// the shadow maps (16 bit depth) at shadow_scale x, the frame buffer sized ones with msaa samples, anisotropic
// filtering up to aniso. LLE keeps everything at 1. (PC shadows: shadow_scale is then also times shadow_distance.)
struct Quality {
    float scale = 1;  // performance: fractional (render_scale), e.g. 800 / 480 on the Steam Deck
    uint32_t shadow_scale = 1, msaa = 1, aniso = 1;
    float wide = 1.0f;  // PC features: frame buffer sized targets this much wider (pcmode::Widen)
};
Quality g_q;

// ---- performance: the pipeline state the draws set, set again only when it changes. Everything else that sets
// state (clears, scaling and aliasing passes, ambient occlusion, the present) calls Invalidate first ----
struct DrawState {
    bool valid = false;
    ID3D11BlendState* blend = nullptr;
    float factor[4] = {};
    ID3D11DepthStencilState* depth = nullptr;
    UINT ref = 0;
    ID3D11RasterizerState* raster = nullptr;
    ID3D11InputLayout* layout = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11Buffer* vcb = nullptr;
    ID3D11Buffer* pcb = nullptr;
    ID3D11SamplerState* samplers[4] = {};
    void Invalidate() { valid = false; }
};
DrawState g_ds;
const bool g_no_state_cache = GetEnvironmentVariableA("FCI_NO_STATE_CACHE", nullptr, 0) > 0;  // diagnostics
// performance: depth targets with 24 bit depth (float on the host) store c / d (psh::kReversedDepth) for the guest's
// depth d, with the depth tests reversed. W-buffered draws then need no depth from the pixel shader (the rasterizer
// interpolates c / (w / zmax) exactly), which keeps early depth tests; FCI_W_DEPTH=shader: d itself, the pixel shader
// writing w / zmax as before
bool g_reversed_depth = false;
// performance: 24 bit depth targets are D32_FLOAT (32 bits a sample) until a draw uses the stencil test, then
// D32_FLOAT_S8X24 (64 bits a sample) as before; FCI_DEPTH_STENCIL=1 from the start. The single-player run (intro to
// the wreck) never uses stencil: half the depth traffic at 4x MSAA
bool g_depth_stencil = GetEnvironmentVariableA("FCI_DEPTH_STENCIL", nullptr, 0) > 0;
uint64_t g_generation = 1;  // advances at every flush; caches re-check guest memory once per generation
uint64_t g_draws = 0, g_clears = 0;
uint64_t g_fast_clears = 0, g_resolves = 0;  // performance: clears through the clear calls, MSAA resolves
uint64_t g_writebacks = 0;  // render target write-backs into guest memory; texture caches must look again
uint64_t g_write_sequence = 0;  // orders host-side writes to targets that share memory

// performance: four independent lanes of 8 bytes (the single multiply chain ran at ~1 byte per cycle; textures
// are hashed once a frame)
uint64_t Hash(const uint8_t* p, size_t n) {
    constexpr uint64_t kMul = 0xFF51AFD7ED558CCDull;
    uint64_t h[4] = {0x9E3779B97F4A7C15ull ^ n, 0xC2B2AE3D27D4EB4Full, 0x165667B19E3779F9ull, 0x27D4EB2F165667C5ull};
    size_t blocks = n / 32;
    const uint8_t* q = p;
    for (size_t i = 0; i < blocks; i++, q += 32) {
        for (int k = 0; k < 4; k++) {
            uint64_t w;
            std::memcpy(&w, q + 8 * k, 8);
            h[k] = (h[k] ^ w) * kMul;
            h[k] ^= h[k] >> 32;
        }
    }
    uint64_t r = h[0];
    for (int k = 1; k < 4; k++) r = (r ^ (h[k] * kMul)) * kMul, r ^= r >> 29;
    for (size_t i = blocks * 32; i < n; i++) r = (r ^ p[i]) * 0x100000001B3ull;
    return r;
}

// performance: the once-a-frame check of texture and target memory, four CRC32C streams (the CPU's crc32 instruction,
// SSE 4.2: about 4x the multiply lanes above); Hash (stable, also the shader cache's file names) without SSE 4.2
__attribute__((target("sse4.2"))) uint64_t Crc4(const uint8_t* p, size_t n) {
    uint64_t c[4] = {0x9E3779B9u ^ n, 0x85EBCA6Bu, 0xC2B2AE35u, 0x27D4EB2Fu};
    size_t blocks = n / 32;
    const uint8_t* q = p;
    for (size_t i = 0; i < blocks; i++, q += 32) {
        uint64_t w[4];
        std::memcpy(w, q, 32);
        c[0] = _mm_crc32_u64(c[0], w[0]), c[1] = _mm_crc32_u64(c[1], w[1]);
        c[2] = _mm_crc32_u64(c[2], w[2]), c[3] = _mm_crc32_u64(c[3], w[3]);
    }
    for (size_t i = blocks * 32; i < n; i++) c[i & 3] = _mm_crc32_u8(uint32_t(c[i & 3]), p[i]);
    return (c[0] | c[1] << 32) ^ ((c[2] | c[3] << 32) * 0x9E3779B97F4A7C15ull);
}
bool HasSse42() {
    int info[4] = {};
    __cpuid(info, 1);
    return (info[2] >> 20) & 1;
}
uint64_t ContentHash(const uint8_t* p, size_t n) {
    static const bool crc = HasSse42();
    return crc ? Crc4(p, n) : Hash(p, n);
}

uint64_t Mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}


// ---- performance: FCI_GPU_TIMING=1: GPU time per frame by kind of pass (timestamp queries), every 300 frames ----
enum class Pass { kNone, kShadow, kScreen, kOffscreen, kAo, kPost, kScreen3D, kScreenBlend, kResolve, kWater, kCount };
struct GpuTiming {
    struct Mark {
        ComPtr<ID3D11Query> query;
        Pass pass;  // the kind of work from this timestamp to the next
    };
    struct Frame {
        ComPtr<ID3D11Query> disjoint;
        std::vector<Mark> marks;
        bool open = false;
        uint64_t number = 0;  // performance: FCI_TRACE_HITCHES, the frame as the hitch lines count it
    };
    double hitch_average = 0;
    bool enabled = GetEnvironmentVariableA("FCI_GPU_TIMING", nullptr, 0) > 0;
    Frame frames[4];
    uint32_t current = 0;
    Pass pass = Pass::kNone;
    std::vector<ComPtr<ID3D11Query>> spare;
    double total[int(Pass::kCount)] = {}, frame_total = 0;
    uint32_t counted = 0;

    ComPtr<ID3D11Query> Timestamp() {
        if (!spare.empty()) {
            ComPtr<ID3D11Query> q = std::move(spare.back());
            spare.pop_back();
            return q;
        }
        ComPtr<ID3D11Query> q;
        D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP, 0};
        g_device->CreateQuery(&desc, &q);
        return q;
    }
    void Begin(Pass p) {
        Frame& f = frames[current];
        if (!f.open) return;
        if (p == pass) return;
        ComPtr<ID3D11Query> q = Timestamp();
        if (!q) return;
        g_context->End(q.Get());
        f.marks.push_back({std::move(q), p});
        pass = p;
    }
    // the frames whose results are in; then this frame ends and the next begins
    void FrameEnd() {
        if (!enabled) return;
        Frame& f = frames[current];
        if (f.open) {
            Begin(Pass::kNone);
            g_context->End(f.disjoint.Get());
            f.open = false;
        }
        current = (current + 1) % 4;
        Collect(frames[current]);
        Frame& next = frames[current];
        if (!next.disjoint) {
            D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
            g_device->CreateQuery(&desc, &next.disjoint);
        }
        if (!next.disjoint) return;
        g_context->Begin(next.disjoint.Get());
        next.open = true;
        next.number = rt::hitch::FrameNumber() + 1;
        pass = Pass::kCount;  // the first Begin always marks
        Begin(Pass::kNone);
    }
    void Collect(Frame& f) {
        if (f.marks.empty()) return;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        // three frames later: waits only if the GPU is that far behind
        while (g_context->GetData(f.disjoint.Get(), &dj, sizeof dj, 0) == S_FALSE) {}
        std::vector<UINT64> t(f.marks.size());
        for (size_t i = 0; i < f.marks.size(); i++)
            while (g_context->GetData(f.marks[i].query.Get(), &t[i], sizeof t[i], 0) == S_FALSE) {}
        if (!dj.Disjoint && dj.Frequency) {
            for (size_t i = 0; i + 1 < f.marks.size(); i++)
                total[int(f.marks[i].pass)] += double(t[i + 1] - t[i]) * 1000.0 / double(dj.Frequency);
            const double ms = double(t.back() - t.front()) * 1000.0 / double(dj.Frequency);
            frame_total += ms;
            counted++;
            if (rt::hitch::Enabled()) {  // performance: FCI_TRACE_HITCHES, frames the GPU took long for
                if (hitch_average > 0 && ms > 1.25 * hitch_average && ms > hitch_average + 1.5) {
                    double by[int(Pass::kCount)] = {};
                    for (size_t i = 0; i + 1 < f.marks.size(); i++)
                        by[int(f.marks[i].pass)] += double(t[i + 1] - t[i]) * 1000.0 / double(dj.Frequency);
                    rt::Log("hitch gpu: frame %llu GPU %.2f ms (average %.2f): shadow %.2f, screen %.2f, resolves %.2f, "
                            "offscreen %.2f, AO %.2f, post %.2f, between %.2f",
                            (unsigned long long)f.number, ms, hitch_average, by[1], by[2] + by[6] + by[7], by[8], by[3],
                            by[4], by[5], by[0]);
                }
                hitch_average = hitch_average > 0 ? hitch_average + (std::min(ms, 2 * hitch_average) - hitch_average) / 30 : ms;
            }
        }
        for (auto& m : f.marks) spare.push_back(std::move(m.query));
        f.marks.clear();
        if (counted == 300) {
            rt::Log("render: GPU per frame %.2f ms: shadow maps %.2f, screen %.2f (3D opaque %.2f, 3D blended %.2f, "
                    "other %.2f), resolves %.2f, offscreen %.2f, ambient occlusion %.2f, post/present %.2f, between %.2f, "
                    "PC water %.3f (in screen) (average of 300)",
                    frame_total / counted, total[1] / counted, (total[2] + total[6] + total[7] + total[9]) / counted,
                    total[6] / counted, total[7] / counted, total[2] / counted, total[8] / counted, total[3] / counted,
                    total[4] / counted, total[5] / counted, total[0] / counted, total[9] / counted);
            for (double& v : total) v = 0;
            frame_total = 0, counted = 0;
        }
    }
};
GpuTiming g_gpu_timing;
// ---- end performance ----

// ---- render targets ----

// SET_SURFACE_FORMAT color: bytes per pixel
uint32_t ColorBytes(uint32_t format) {
    static const uint8_t kBytes[16] = {0, 2, 2, 2, 4, 4, 4, 4, 4, 1, 2};
    return kBytes[format & 0xF];
}

// ---- PC features: widescreen (app/pcmode.h) ----
// The game draws its widened 3D view (see xdk/game.cpp) into the console's 640x480 frame buffer; the host keeps
// frame buffer sized targets that much wider, so the picture is shown at its real shape with full resolution.
uint32_t HostWidthOf(uint32_t width, float scale, float wide) {
    return uint32_t(float(width) * scale * wide + 0.5f);
}
bool g_frame_3d = false;  // a W-buffered (3D) draw went into a widened target since the last present
// the post processing after the 3D view (the bloom chain into other targets) has begun: from then on the 2D
// draws into the widened target are the HUD. 2D before it belongs to the view (the sun's glare sprite, placed by
// the widened camera) and must not be narrowed or clipped to the 4:3 middle
bool g_post_3d = false;
// fmv_scale (gpu/fmv.h): a video texture went full screen into a widened target since the last present; the frame
// is shown at the widescreen aspect and the 2D over it stays in the 4:3 middle
bool g_frame_video = false;
bool g_last_video = false;  // so was the frame before: 2D before this frame's video quad is narrowed as well
float g_video_rect[4] = {};  // where the last video quad was drawn (guest pixels: x0, y0, x1, y1)
// 2D drawn before this frame's video outside its rectangle, drawn again after it (DrawPrimitives)
std::vector<std::pair<kelvin::State, kelvin::Draw>> g_before_video;
bool g_replay_now = false, g_replaying = false;
uint32_t g_stage_video[4] = {};  // per stage of the current draw: width << 16 | height of a linear guest texture
// ---- end PC features ----

// performance (fractional render scale): host rows of a guest height, the host row a guest row starts at
uint32_t HostHeightOf(uint32_t height, float scale) { return uint32_t(float(height) * scale + 0.5f); }

struct Target {
    uint32_t address = 0, format = 0, width = 0, height = 0, pitch = 0;  // guest surface
    bool swizzled = false;
    float scale = 1;  // HLE: host texture of width x height times scale (fractional allowed), multisampled
    uint32_t samples = 1;
    float wide = 1.0f;  // HLE widescreen: frame buffer sized targets are this much wider on the host (anamorphic)
    ComPtr<ID3D11Texture2D> texture, staging;  // staging: guest size
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;  // of texture itself, for GPU-side copies into other targets
    bool dirty = false;  // the host copy is newer than guest memory
    bool stale = false;  // guest memory is newer than the host copy (another target wrote it back)
    uint64_t hash = 0, checked = 0;
    uint64_t version = 0;     // advances with every draw or clear into the host copy
    uint64_t last_write = 0;  // g_write_sequence of the last draw, clear or copy into the host copy
    // a snapshot for draws that sample this target as a texture (D3D 11 cannot read and write one resource)
    ComPtr<ID3D11Texture2D> snapshot;
    ComPtr<ID3D11ShaderResourceView> snapshot_srv;
    uint64_t snapshot_version = ~0ull;
    // guest memory as of the last upload or write-back: a write-back stores only the pixels the host copy
    // changed since, as the GPU only writes the pixels it draws. Writing the whole rectangle put stale pixels
    // over whatever the game had placed in undrawn parts (objects in scratch surface memory, in splitscreen).
    std::vector<uint8_t> synced;
    // scaled or multisampled targets: a single sampled copy (resolved) and one at guest size (shrunk)
    ComPtr<ID3D11Texture2D> resolved, shrunk, upload;
    ComPtr<ID3D11ShaderResourceView> resolved_srv, upload_srv;
    ComPtr<ID3D11RenderTargetView> shrunk_rtv;
    uint64_t resolved_version = ~0ull;
    // HLE: copies for CPU reads that do not wait for the GPU (NOOVERWRITE locks), in submission order
    ComPtr<ID3D11Texture2D> late[3];
    uint32_t late_next = 0, late_pending = 0;
    // HLE: linear textures that start at a row inside the target (split screen's lower view): copies of those rows
    struct Rows {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> srv;
        uint64_t version = ~0ull;
    };
    std::map<std::pair<uint32_t, uint32_t>, Rows> rows;  // first row, row count

    uint32_t HostWidth() const { return HostWidthOf(width, scale, wide); }
    uint32_t HostHeight() const { return HostHeightOf(height, scale); }
    bool Native() const { return scale == 1 && samples == 1 && wide == 1.0f; }
    uint32_t Bytes() const { return ColorBytes(format); }
    uint32_t GuestSize() const { return swizzled ? width * height * Bytes() : pitch * height; }
    bool Overlaps(uint32_t a, uint32_t size) const { return a < address + GuestSize() && address < a + size; }
};

struct DepthTarget {
    uint32_t address = 0, width = 0, height = 0;  // guest size
    bool z16 = false;
    bool stencil = false;  // performance: 24 bit depth has a stencil plane only once the game has used stencil
    float scale = 1;
    uint32_t samples = 1;
    float wide = 1.0f;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11ShaderResourceView> srv;  // depth as R24 for shadow map lookups (guest memory never has it)
    uint64_t last_write = 0;
    ComPtr<ID3D11ShaderResourceView> ao_srv;  // ambient occlusion: the depth, multisampled or not
    ComPtr<ID3D11DepthStencilView> dsv_read;  // PC water: read-only, so that the water's shader can read the depth
};

// D3D reuses memory for differently shaped surfaces (a scratch buffer serves as a 484x240 bloom target and a
// 256x128 one), so a guest address can have several host targets; their contents meet in guest memory
std::vector<std::unique_ptr<Target>> g_targets;
std::vector<std::unique_ptr<DepthTarget>> g_depths;

// guest pixels <-> BGRA8
void ToHost(const Target& t, const uint8_t* guest, std::vector<uint32_t>& out) {
    out.resize(size_t(t.width) * t.height);
    std::vector<uint8_t> linear;
    uint32_t pitch = t.pitch, bytes = t.Bytes();
    if (t.swizzled) {
        pitch = t.width * bytes;
        linear.resize(size_t(pitch) * t.height);
        texture::Unswizzle(guest, linear.data(), t.width, t.height, 1, bytes, pitch);
        guest = linear.data();
    }
    for (uint32_t y = 0; y < t.height; y++) {
        const uint8_t* row = guest + size_t(y) * pitch;
        uint32_t* dst = &out[size_t(y) * t.width];
        switch (t.format) {
        case 1:
        case 2:
            for (uint32_t x = 0; x < t.width; x++) {
                uint32_t v = row[2 * x] | (row[2 * x + 1] << 8);
                uint32_t r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
                dst[x] = 0xFF000000u | (((r << 3) | (r >> 2)) << 16) | (((g << 3) | (g >> 2)) << 8) | ((b << 3) | (b >> 2));
            }
            break;
        case 3:
            for (uint32_t x = 0; x < t.width; x++) {
                uint32_t v = row[2 * x] | (row[2 * x + 1] << 8);
                uint32_t r = v >> 11, g = (v >> 5) & 63, b = v & 31;
                dst[x] = 0xFF000000u | (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
            }
            break;
        case 9:
            for (uint32_t x = 0; x < t.width; x++) dst[x] = 0xFF000000u | row[x];
            break;
        case 10:
            for (uint32_t x = 0; x < t.width; x++) dst[x] = 0xFF000000u | (row[2 * x + 1] << 8) | row[2 * x];
            break;
        default:
            std::memcpy(dst, row, size_t(t.width) * 4);
            break;
        }
    }
}

void ToGuest(const Target& t, const uint8_t* host, uint32_t host_pitch, uint8_t* guest) {
    uint32_t bytes = t.Bytes();
    std::vector<uint8_t> linear;
    uint8_t* dst_base = guest;
    uint32_t pitch = t.pitch;
    if (t.swizzled) {
        pitch = t.width * bytes;
        linear.resize(size_t(pitch) * t.height);
        dst_base = linear.data();
    }
    for (uint32_t y = 0; y < t.height; y++) {
        const uint32_t* src = reinterpret_cast<const uint32_t*>(host + size_t(y) * host_pitch);
        uint8_t* row = dst_base + size_t(y) * pitch;
        switch (t.format) {
        case 1:
        case 2: {
            uint32_t top = t.format == 2 ? 0x8000 : 0;
            for (uint32_t x = 0; x < t.width; x++) {
                uint32_t c = src[x];
                uint32_t v = top | (((c >> 19) & 31) << 10) | (((c >> 11) & 31) << 5) | ((c >> 3) & 31);
                row[2 * x] = uint8_t(v), row[2 * x + 1] = uint8_t(v >> 8);
            }
            break;
        }
        case 3:
            for (uint32_t x = 0; x < t.width; x++) {
                uint32_t c = src[x];
                uint32_t v = (((c >> 19) & 31) << 11) | (((c >> 10) & 63) << 5) | ((c >> 3) & 31);
                row[2 * x] = uint8_t(v), row[2 * x + 1] = uint8_t(v >> 8);
            }
            break;
        case 9:
            for (uint32_t x = 0; x < t.width; x++) row[x] = uint8_t(src[x]);
            break;
        case 10:
            for (uint32_t x = 0; x < t.width; x++) row[2 * x] = uint8_t(src[x]), row[2 * x + 1] = uint8_t(src[x] >> 8);
            break;
        default:
            std::memcpy(row, src, size_t(t.width) * 4);
            break;
        }
    }
    if (t.swizzled) texture::Swizzle(linear.data(), guest, t.width, t.height, bytes, pitch);
}

void Grow(Target& t);

void Upload(Target& t) {
    const int64_t hitch_t0 = rt::hitch::Enabled() ? rt::hitch::Now() : 0;
    struct Noted {
        int64_t t0;
        uint64_t bytes;
        ~Noted() {
            if (t0) rt::hitch::Note(rt::hitch::kUpload, rt::hitch::Now() - t0, bytes);
        }
    } noted{hitch_t0, uint64_t(t.width) * t.height * 4};
    const uint8_t* guest = Physical(t.address);
    std::vector<uint32_t> pixels;
    ToHost(t, guest, pixels);
    if (t.Native()) {
        g_context->UpdateSubresource(t.texture.Get(), 0, nullptr, pixels.data(), t.width * 4, 0);
    } else {
        // guest pixels into a texture of their own, drawn into the scaled or multisampled target
        if (!t.upload) {
            D3D11_TEXTURE2D_DESC desc{t.width, t.height, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                      D3D11_BIND_SHADER_RESOURCE, 0, 0};
            g_device->CreateTexture2D(&desc, nullptr, &t.upload);
            g_device->CreateShaderResourceView(t.upload.Get(), nullptr, &t.upload_srv);
        }
        g_context->UpdateSubresource(t.upload.Get(), 0, nullptr, pixels.data(), t.width * 4, 0);
        Grow(t);
    }
    t.version++;
    t.synced.assign(guest, guest + t.GuestSize());
    t.hash = ContentHash(guest, t.GuestSize());
    t.checked = g_generation;
    t.stale = false;
}

// writes mapped staging contents back; other targets on the same memory now lag behind it
void WriteBack(Target& t, const D3D11_MAPPED_SUBRESOURCE& map) {
    uint8_t* guest = Physical(t.address);
    const size_t size = t.GuestSize();
    if (t.synced.size() != size) t.synced.assign(guest, guest + size);
    // the host copy in guest layout, over the last synced bytes (pitch padding stays as it was)
    static std::vector<uint8_t> fresh;
    fresh = t.synced;
    ToGuest(t, static_cast<const uint8_t*>(map.pData), map.RowPitch, fresh.data());
    const uint32_t bytes = t.Bytes();
    for (size_t block = 0; block < size; block += 64) {
        size_t end = std::min(size, block + 64);
        if (!std::memcmp(&fresh[block], &t.synced[block], end - block)) continue;
        for (size_t i = block; i + bytes <= end; i += bytes)
            if (std::memcmp(&fresh[i], &t.synced[i], bytes)) std::memcpy(guest + i, &fresh[i], bytes);
    }
    t.synced.swap(fresh);
    t.dirty = false;
    g_writebacks++;
    t.hash = ContentHash(guest, t.GuestSize());
    t.checked = g_generation;
    for (auto& other : g_targets)
        if (other.get() != &t && other->Overlaps(t.address, t.GuestSize())) other->stale = true;
}

void ToStaging(Target& t);

void Download(Target& t) {
    if (!t.dirty) return;
    ToStaging(t);
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(g_context->Map(t.staging.Get(), 0, D3D11_MAP_READ, 0, &map))) return;
    WriteBack(t, map);
    g_context->Unmap(t.staging.Get(), 0);
}

void MarkWritten(Target& t) {
    t.dirty = true;
    t.version++;
    t.last_write = ++g_write_sequence;
}

// two targets on the same memory with the same pixel layout: one's host copy can go into the other on the GPU
bool SameLayout(const Target& a, const Target& b) {
    return a.address == b.address && a.format == b.format && !a.swizzled && !b.swizzled && a.pitch == b.pitch &&
           a.scale == b.scale && a.samples == b.samples && a.wide == b.wide;
}

// overlapping 32-bit linear targets with different shapes: dst takes src's bytes, reinterpreted
bool CanReinterpret(const Target& src, const Target& dst) {
    return src.format == dst.format && src.Bytes() == 4 && !src.swizzled && !dst.swizzled && src.Native() &&
           dst.Native();
}
void Reinterpret(Target& src, Target& dst);

void CopyInto(Target& src, Target& dst) {
    D3D11_BOX box{0, 0, 0, std::min(src.HostWidth(), dst.HostWidth()), std::min(src.HostHeight(), dst.HostHeight()), 1};
    g_context->CopySubresourceRegion(dst.texture.Get(), 0, 0, 0, 0, src.texture.Get(), 0, &box);
    MarkWritten(dst);
}

// render targets the GPU drew into that overlap a range the guest is about to read through a texture
// GPU -> CPU read-backs in the middle of a frame stall the pipeline; counted per cause, logged periodically
std::unordered_map<std::string, uint64_t> g_download_causes;

void CountDownload(const Target& t, const char* why, uint32_t address) {
    char key[160];
    snprintf(key, sizeof key, "target %08X %ux%u format %u%s for %s at %08X", t.address, t.width, t.height, t.format,
             t.swizzled ? " swizzled" : "", why, address);
    g_download_causes[key]++;
}

void DownloadOverlapping(uint32_t address, uint32_t size, const char* why = "") {
    for (auto& t : g_targets)
        if (t->dirty && t->Overlaps(address, size)) {
            CountDownload(*t, why, address);
            Download(*t);
        }
}

// brings a target up to date with guest memory before it is drawn into
void Refresh(Target& t) {
    std::vector<Target*> copies;  // newer host copies of the same memory, laid over t on the GPU
    for (auto& other : g_targets) {
        if (other.get() == &t || !other->dirty || !other->Overlaps(t.address, t.GuestSize())) continue;
        if (SameLayout(*other, t) || CanReinterpret(*other, t)) {
            if (other->last_write > t.last_write) copies.push_back(other.get());
            continue;
        }
        if (g_hle) continue;  // HLE: no round trips through guest memory between differently shaped targets
        char why[64];
        snprintf(why, sizeof why, "target %ux%u format %u", t.width, t.height, t.format);
        CountDownload(*other, why, t.address);
        Download(*other);
    }
    if (!t.dirty) {
        if (t.stale) {
            Upload(t);
        } else if (t.checked != g_generation) {
            // the CPU may have written the surface since we last saw it
            if (ContentHash(Physical(t.address), t.GuestSize()) != t.hash) Upload(t);
            t.checked = g_generation;
        }
    }
    std::sort(copies.begin(), copies.end(), [](Target* a, Target* b) { return a->last_write < b->last_write; });
    for (Target* other : copies) {
        if (SameLayout(*other, t)) CopyInto(*other, t);
        else Reinterpret(*other, t);
    }
}

// refresh = false: the caller neither reads nor writes the host copy (a draw with color writes off), so it is not
// brought up to date with guest memory now (the next draw that writes it does that)
Target* GetTarget(uint32_t address, uint32_t format, uint32_t width, uint32_t height, uint32_t pitch, bool swizzled,
                  float scale, uint32_t samples, float wide = 1.0f, bool refresh = true) {
    if (!width || !height || !ColorBytes(format)) return nullptr;
    Target* best = nullptr;
    for (auto& t : g_targets)
        if (t->address == address && t->format == format && t->swizzled == swizzled && t->pitch == pitch &&
            t->scale == scale && t->samples == samples && t->wide == wide &&
            t->width >= width && t->height >= height && (!best || t->width * t->height < best->width * best->height))
            best = t.get();
    if (best) {
        if (refresh) Refresh(*best);
        return best;
    }
    if (!g_hle) DownloadOverlapping(address, swizzled ? width * height * ColorBytes(format) : pitch * height, "new target");
    rt::hitch::Note(rt::hitch::kTarget, 0, uint64_t(width) * height * 4);
    auto t = std::make_unique<Target>();
    t->address = address, t->format = format, t->width = width, t->height = height, t->pitch = pitch;
    t->swizzled = swizzled;
    t->scale = scale, t->samples = samples, t->wide = wide;
    if (address + uint64_t(t->GuestSize()) > 0x04000000 || (!swizzled && pitch < width * ColorBytes(format))) return nullptr;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = t->HostWidth(), desc.Height = t->HostHeight(), desc.MipLevels = 1, desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = samples;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | (samples == 1 ? D3D11_BIND_SHADER_RESOURCE : 0);
    if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &t->texture))) return nullptr;
    desc.Width = width, desc.Height = height, desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING, desc.BindFlags = 0, desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &t->staging))) return nullptr;
    g_device->CreateRenderTargetView(t->texture.Get(), nullptr, &t->rtv);
    Upload(*t);
    rt::Log("render: target %08X %ux%u format %u pitch %u%s, host %ux%u x%u samples", address, width, height, format,
            pitch, swizzled ? " swizzled" : "", t->HostWidth(), t->HostHeight(), samples);
    Target* raw = t.get();
    g_targets.push_back(std::move(t));
    return raw;
}

// 24 bit depth is kept as float: W-buffered draws store w / zmax (see psh.cpp), which for the first person
// hands and weapon (w below one) is far below one step of a 24 bit fixed point buffer, so they fought each
// other. Float keeps the same relative precision at every distance and reads the same [0, 1].
// 16 bit depth (the shadow maps) stays 16 bit: polygon offset units and comparisons then match the guest.
DepthTarget* GetDepth(uint32_t address, uint32_t width, uint32_t height, bool z16, float scale, uint32_t samples,
                      float wide = 1.0f) {
    for (auto& d : g_depths)
        if (d->address == address && d->width == width && d->height == height && d->z16 == z16 && d->scale == scale &&
            d->samples == samples && d->wide == wide)
            return d.get();
    rt::hitch::Note(rt::hitch::kTarget, 0, uint64_t(width) * height * 4);
    auto slot = std::make_unique<DepthTarget>();
    slot->address = address, slot->width = width, slot->height = height, slot->z16 = z16;
    slot->scale = scale, slot->samples = samples, slot->wide = wide;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = HostWidthOf(width, scale, wide), desc.Height = HostHeightOf(height, scale), desc.MipLevels = 1,
    desc.ArraySize = 1;
    slot->stencil = !z16 && g_depth_stencil;
    desc.Format = z16 ? DXGI_FORMAT_R16_TYPELESS : slot->stencil ? DXGI_FORMAT_R32G8X24_TYPELESS : DXGI_FORMAT_R32_TYPELESS;
    desc.SampleDesc.Count = samples;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | (samples == 1 ? D3D11_BIND_SHADER_RESOURCE : 0);
    // ambient occlusion and the PC water read it
    if (!z16 && g_hle && (ao::BootMode() || water::BootQuality())) desc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &slot->texture))) return nullptr;
    D3D11_DEPTH_STENCIL_VIEW_DESC dsv{z16 ? DXGI_FORMAT_D16_UNORM
                                      : slot->stencil ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D32_FLOAT,
                                      samples > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D};
    g_device->CreateDepthStencilView(slot->texture.Get(), &dsv, &slot->dsv);
    if (samples == 1) {
        D3D11_SHADER_RESOURCE_VIEW_DESC srv{z16 ? DXGI_FORMAT_R16_UNORM
                                            : slot->stencil ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT_R32_FLOAT,
                                            D3D11_SRV_DIMENSION_TEXTURE2D};
        srv.Texture2D.MipLevels = 1;
        g_device->CreateShaderResourceView(slot->texture.Get(), &srv, &slot->srv);
    }
    // (performance: the reversed depth's far is 0)
    g_context->ClearDepthStencilView(slot->dsv.Get(), D3D11_CLEAR_DEPTH | (slot->stencil ? D3D11_CLEAR_STENCIL : 0),
                                     g_reversed_depth && !z16 ? 0.0f : 1.0f, 0);
    rt::Log("render: depth target %08X %ux%u%s, host x%g, %u samples", address, width, height, z16 ? " (16 bit)" : "",
            scale, samples);
    g_depths.push_back(std::move(slot));
    return g_depths.back().get();
}

struct Surfaces {
    Target* color = nullptr;
    DepthTarget* depth = nullptr;
    uint32_t width = 0, height = 0;  // host pixels
    D3D11_RECT clip{};
    uint32_t scale_x = 1, scale_y = 1;
    float scale = 1;  // HLE: host pixels per guest pixel (fractional allowed)
    uint32_t samples = 1;  // samples per pixel
    float wide = 1.0f;                // HLE widescreen: x host pixels per guest pixel are scale x wide
    float zmax = 16777215.0f;
};

Surfaces BindSurfaces(const State& s, bool want_color, bool want_depth, bool refresh_color = true) {
    Surfaces out;
    uint32_t format = s.Reg(SET_SURFACE_FORMAT);
    uint32_t color = format & 0xF, zeta = (format >> 4) & 0xF, type = (format >> 8) & 0xF, aa = (format >> 12) & 0xF;
    out.scale_x = (aa == 1 || aa == 2) ? 2 : 1;
    out.scale_y = aa == 2 ? 2 : 1;
    uint32_t ch = s.Reg(SET_SURFACE_CLIP_HORIZONTAL), cv = s.Reg(SET_SURFACE_CLIP_VERTICAL);
    uint32_t clip_x = ch & 0xFFFF, clip_w = ch >> 16, clip_y = cv & 0xFFFF, clip_h = cv >> 16;
    bool swizzled = type == 2;
    if (swizzled) {
        out.width = 1u << ((format >> 16) & 0xFF);
        out.height = 1u << ((format >> 24) & 0xFF);
    } else {
        out.width = (clip_x + clip_w) * out.scale_x;
        out.height = (clip_y + clip_h) * out.scale_y;
    }
    out.width = std::min(out.width, 4096u), out.height = std::min(out.height, 4096u);
    out.clip = {LONG(clip_x * out.scale_x), LONG(clip_y * out.scale_y), LONG((clip_x + clip_w) * out.scale_x),
                LONG((clip_y + clip_h) * out.scale_y)};
    if (zeta == 1) out.zmax = 65535.0f;
    // HLE: 16 bit depth passes are the shadow maps; frame buffer sized targets are multisampled
    if (g_hle) {
        bool shadow = zeta == 1;
        out.scale = shadow ? float(g_q.shadow_scale) : g_q.scale;
        // by the pitch, not the clip: partial clears and draws must land in the same target as whole ones
        uint32_t row = color ? (s.Reg(SET_SURFACE_PITCH) & 0xFFFF) / std::max(ColorBytes(color), 1u)
                             : (s.Reg(SET_SURFACE_PITCH) >> 16) / (zeta == 1 ? 2 : 4);
        if (!shadow && !swizzled && row >= 640) out.samples = g_q.msaa;
        if (!shadow && !swizzled && row >= 640) out.wide = g_q.wide;  // PC features: widescreen
    }
    uint32_t pitch = s.Reg(SET_SURFACE_PITCH);
    if (want_color && color) {
        uint32_t address = ResolveDma(s.Reg(SET_CONTEXT_DMA_COLOR)) + s.Reg(SET_SURFACE_COLOR_OFFSET);
        out.color = GetTarget(address, color, out.width, out.height, pitch & 0xFFFF, swizzled, out.scale, out.samples,
                              out.wide, refresh_color);
        if (out.color) out.width = out.color->width, out.height = out.color->height;
    }
    if (want_depth && zeta) {
        uint32_t address = ResolveDma(s.Reg(SET_CONTEXT_DMA_ZETA)) + s.Reg(SET_SURFACE_ZETA_OFFSET);
        out.depth = GetDepth(address, out.width, out.height, zeta == 1, out.scale, out.samples, out.wide);
    }
    const float sx = float(out.scale) * out.wide;
    out.clip.left = LONG(out.clip.left * sx + 0.5f), out.clip.right = LONG(out.clip.right * sx + 0.5f);
    out.clip.top = LONG(out.clip.top * out.scale + 0.5f), out.clip.bottom = LONG(out.clip.bottom * out.scale + 0.5f);
    ID3D11RenderTargetView* rtv = out.color ? out.color->rtv.Get() : nullptr;
    g_context->OMSetRenderTargets(rtv ? 1 : 0, rtv ? &rtv : nullptr, out.depth ? out.depth->dsv.Get() : nullptr);
    if (g_gpu_timing.enabled)  // performance: FCI_GPU_TIMING
        g_gpu_timing.Begin(out.depth && out.depth->z16 ? Pass::kShadow
                           : out.color && (out.color->samples > 1 || out.color->wide != 1.0f) ? Pass::kScreen
                                                                                                : Pass::kOffscreen);
    D3D11_VIEWPORT vp{0, 0, float(HostWidthOf(out.width, out.scale, out.wide)), float(HostHeightOf(out.height, out.scale)),
                      0, 1};
    g_context->RSSetViewports(1, &vp);
    return out;
}

// ---- shaders ----

struct ShaderJob;  // performance: shaders off the render thread
struct VertexShader {
    ComPtr<ID3D11VertexShader> shader;
    ComPtr<ID3D11InputLayout> layout;
    std::shared_ptr<ShaderJob> job;  // performance: not made yet
};
struct PixelShader {
    ComPtr<ID3D11PixelShader> shader;
    std::shared_ptr<ShaderJob> job;  // performance: not made yet
};
std::unordered_map<uint64_t, VertexShader> g_vertex_shaders;
std::unordered_map<uint64_t, PixelShader> g_pixel_shaders;

const bool g_dump_shaders = GetEnvironmentVariableA("FCI_DUMP_SHADERS", nullptr, 0) > 0;
// FCI_TRACE_DRAWS=<count>[@<seconds>]: log that many draws, from that many seconds after start
ULONGLONG g_trace_from = 0;
int g_trace_draws = [] {
    char value[32] = {};
    if (!GetEnvironmentVariableA("FCI_TRACE_DRAWS", value, sizeof value)) return 0;
    if (const char* at = std::strchr(value, '@')) g_trace_from = GetTickCount64() + ULONGLONG(std::atof(at + 1) * 1000);
    return std::atoi(value);
}();
// FCI_TRACE_PS=<key>: only trace draws with this pixel shader key (hex)
const uint64_t g_trace_ps = [] {
    char value[32] = {};
    return GetEnvironmentVariableA("FCI_TRACE_PS", value, sizeof value) ? std::strtoull(value, nullptr, 16) : 0ull;
}();

// ---- performance: compiled shaders on disk (shader_cache\ beside the exe, by a hash of the HLSL), so a shader
// compiles once and not again in every session: a compile takes milliseconds (a hitch at each new effect) ----
struct ShaderCache {
    std::wstring dir;
    bool enabled = GetEnvironmentVariableA("FCI_NO_SHADER_CACHE", nullptr, 0) == 0;
    uint64_t hits = 0, compiles = 0;
    double compile_ms = 0;

    ShaderCache() {
        wchar_t path[MAX_PATH];
        DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
        if (!n || n >= MAX_PATH) {
            enabled = false;
            return;
        }
        std::wstring exe(path, n);
        dir = exe.substr(0, exe.find_last_of(L"\\/") + 1) + L"shader_cache\\";
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    std::wstring File(uint64_t hash) const {
        wchar_t name[40];
        swprintf(name, 40, L"%016llx.dxbc", (unsigned long long)hash);
        return dir + name;
    }
    ComPtr<ID3DBlob> Load(uint64_t hash) const {
        if (!enabled) return nullptr;
        HANDLE f = CreateFileW(File(hash).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) return nullptr;
        ComPtr<ID3DBlob> blob;
        LARGE_INTEGER size{};
        DWORD got = 0;
        if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (16 << 20) &&
            SUCCEEDED(D3DCreateBlob(SIZE_T(size.QuadPart), &blob)) &&
            (!ReadFile(f, blob->GetBufferPointer(), DWORD(size.QuadPart), &got, nullptr) || got != size.QuadPart))
            blob.Reset();
        CloseHandle(f);
        return blob;
    }
    void Store(uint64_t hash, ID3DBlob* blob) const {
        if (!enabled) return;
        std::wstring final_name = File(hash), temp = final_name + L".tmp";
        HANDLE f = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) return;
        DWORD put = 0;
        bool ok = WriteFile(f, blob->GetBufferPointer(), DWORD(blob->GetBufferSize()), &put, nullptr) &&
                  put == blob->GetBufferSize();
        CloseHandle(f);
        if (!ok || !MoveFileExW(temp.c_str(), final_name.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(temp.c_str());
    }
};
ShaderCache& Cache() {
    static ShaderCache cache;
    return cache;
}
// ---- end performance ----

// the cache's name of a shader (the version changes with the compile options or the HLSL interface)
uint64_t SourceHash(const std::string& source, const char* profile) {
    constexpr uint64_t kCacheVersion = 1;
    return Mix(Mix(Hash(reinterpret_cast<const uint8_t*>(source.data()), source.size()),
                   Hash(reinterpret_cast<const uint8_t*>(profile), std::strlen(profile))),
               kCacheVersion);
}

// (performance: also on the shader workers, at the same time)
ComPtr<ID3DBlob> Compile(const std::string& source, const char* profile, uint64_t key = 0) {
    static std::mutex lock;  // the counters, the dump names
    if (g_dump_shaders) {
        static int dumped = 0;
        char name[64];
        {
            std::lock_guard guard(lock);
            snprintf(name, sizeof name, "shader_%03d_%s.hlsl", dumped++, profile);
        }
        rt::Log("render: %s %016llX in %s", profile, (unsigned long long)key, name);
        if (FILE* f = std::fopen(name, "w")) {
            std::fwrite(source.data(), 1, source.size(), f);
            std::fclose(f);
        }
    }
    // performance: the shader cache
    const uint64_t hash = SourceHash(source, profile);
    if (ComPtr<ID3DBlob> cached = Cache().Load(hash)) {
        std::lock_guard guard(lock);
        Cache().hits++;
        return cached;
    }
    ComPtr<ID3DBlob> code, errors;
    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceCounter(&t0);
    HRESULT hr = CompileHlsl(source.data(), source.size(), profile, nullptr, nullptr, "main", profile,
                             D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &errors);
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&freq);
    {
        std::lock_guard guard(lock);
        ShaderCache& cache = Cache();
        cache.compiles++;
        cache.compile_ms += double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(freq.QuadPart);
        rt::hitch::Note(rt::hitch::kShader, t1.QuadPart - t0.QuadPart);
        if (cache.compiles % 50 == 0)
            rt::Log("render: %llu shaders compiled (%.0f ms of compiler time), %llu from the shader cache",
                    (unsigned long long)cache.compiles, cache.compile_ms, (unsigned long long)cache.hits);
    }
    if (SUCCEEDED(hr)) Cache().Store(hash, code.Get());
    if (FAILED(hr)) {
        static int failures = 0;
        rt::Log("render: %s compile failed: %s", profile,
                errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        int n;
        {
            std::lock_guard guard(lock);
            n = failures < 8 ? failures++ : 8;
        }
        if (n < 8) {
            char name[64];
            snprintf(name, sizeof name, "shader_fail_%d.hlsl", n);
            if (FILE* f = std::fopen(name, "w")) {
                std::fwrite(source.data(), 1, source.size(), f);
                std::fclose(f);
            }
        }
        return nullptr;
    }
    return code;
}

// ---- performance: shaders off the render thread ----
// A shader missing from the cache took D3DCompile 10-100 ms on the PC (3-4x that on the Steam Deck), and its first
// draw waited for it on the render thread: a hitch at each new effect, 0.7-1.8 s at a level's first frame. Now
// worker threads compile (D3DCompile is thread-safe), load from the cache and create the D3D11 objects
// (ID3D11Device is free-threaded), many shaders at once. The render thread generates the HLSL and then:
// - a frame that is already long (a loading screen: > 40 ms since the last present, or the last frame took that
//   long), and every frame in the 1.5 s after such a frame was held back (a level's first moments), does not wait:
//   its draws whose shaders are not ready are left out, and at the present it waits for all of them and is not shown
//   (the screen keeps the frame before), so no picture with missing objects appears;
// - in play the draw waits for its shader up to shader_wait ms (all waits of a frame together, default 4); a shader
//   still not ready leaves its draws out of this frame only. A frame that would miss a fifth of its draws or more (a
//   scene's first frame after a loading screen whose own frames were short, a cut to a new place) is held back too.
// shader_async = 0 waits for every shader as before (but they still compile on the workers).
// At start a thread reads shader_cache\ and creates its shaders, so cached shaders are ready when first drawn.
struct ShaderJob {
    bool vertex = false;
    std::string source;
    uint64_t hash = 0;
    std::vector<D3D11_INPUT_ELEMENT_DESC> elements;
    std::atomic<bool> done{false};
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11PixelShader> ps;
};

struct ShaderWorkers {
    std::mutex lock;
    std::condition_variable work_cv, done_cv;
    std::deque<std::shared_ptr<ShaderJob>> queue;
    int busy = 0;
    bool started = false;
    // threads in all, and how many compile at once in play: on a CPU with few cores (Steam Deck: 4 cores, 8
    // threads) all of them took the game thread's CPU time (frames of 15-19 ms); a held frame lets all compile
    int threads = 2, in_play = 1;
    bool boost = false;

    void Run() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);  // (the game thread first; Wine lowers the nice value too)
        for (;;) {
            std::shared_ptr<ShaderJob> job;
            {
                std::unique_lock guard(lock);
                work_cv.wait(guard, [&] { return !queue.empty() && busy < (boost ? threads : in_play); });
                job = std::move(queue.front());
                queue.pop_front();
                busy++;
            }
            Make(*job);
            {
                std::lock_guard guard(lock);
                job->done = true;
                busy--;
            }
            done_cv.notify_all();
            work_cv.notify_one();
        }
    }
    static void Make(ShaderJob& job) {
        ComPtr<ID3DBlob> code = Compile(job.source, job.vertex ? "vs_5_0" : "ps_5_0");
        if (!code) return;
        if (job.vertex) {
            g_device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &job.vs);
            if (!job.elements.empty())
                g_device->CreateInputLayout(job.elements.data(), UINT(job.elements.size()), code->GetBufferPointer(),
                                            code->GetBufferSize(), &job.layout);
        } else {
            g_device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &job.ps);
        }
        job.source.clear();
        job.source.shrink_to_fit();
    }
    void Add(std::shared_ptr<ShaderJob> job) {
        {
            std::lock_guard guard(lock);
            if (!started) {
                started = true;
                const int hw = int(std::thread::hardware_concurrency());
                threads = std::clamp(hw / 2, 2, 6);
                in_play = std::clamp(hw - 6, 1, threads);
                for (int i = 0; i < threads; i++) std::thread([this] { Run(); }).detach();
                rt::Log("render: %d shader compile threads (%d at once in play)", threads, in_play);
            }
            queue.push_back(std::move(job));
        }
        work_cv.notify_one();
    }
    // until the job is done or ms passed (< 0: no limit); true when done
    bool Wait(ShaderJob& job, double ms) {
        std::unique_lock guard(lock);
        auto ready = [&] { return job.done.load(); };
        if (ms < 0) {
            done_cv.wait(guard, ready);
            return true;
        }
        return done_cv.wait_for(guard, std::chrono::microseconds(int64_t(ms * 1000)), ready);
    }
    void WaitAll() {
        std::unique_lock guard(lock);
        boost = true;
        work_cv.notify_all();
        done_cv.wait(guard, [&] { return queue.empty() && busy == 0; });
        boost = false;
    }
};
ShaderWorkers& Workers() {
    static ShaderWorkers workers;
    return workers;
}

double ShaderWait() {
    static const double ms = std::clamp(settings::Float("shader_wait", 4.0f), 0.0f, 1000.0f);
    return ms;
}
bool ShaderAsync() {
    static const bool on = settings::Bool("shader_async", true);
    return on;
}
LARGE_INTEGER g_present_at{};    // the last present (QueryPerformanceCounter)
double g_present_interval = 0;   // ms between the last two presents
double g_frame_waited = 0;       // ms this frame's draws waited for shaders
LARGE_INTEGER g_held_at{};       // the last frame held back
uint64_t g_frame_left_out = 0;   // draws left out of this frame: in play
uint64_t g_frame_held = 0;       // in a long frame (it is not shown)
uint64_t g_left_out = 0, g_left_frames = 0, g_held_frames = 0;  // totals for the log
uint64_t g_left_vs = 0, g_left_ps = 0;  // draws left out (in shown frames) for a vertex / a pixel shader
uint64_t g_frame_left_vs = 0;           // of this frame's left out: for a vertex shader

// the shader cache read ahead: blobs by name, their shaders made in the background
std::mutex g_prefetch_lock;
std::unordered_map<uint64_t, ComPtr<ID3DBlob>> g_prefetched;
std::unordered_map<uint64_t, ComPtr<ID3D11DeviceChild>> g_prefetched_shaders;

void PrefetchShaderCache() {
    std::thread([] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        ShaderCache& cache = Cache();
        if (!cache.enabled) return;
        LARGE_INTEGER t0, t1, freq;
        QueryPerformanceCounter(&t0);
        WIN32_FIND_DATAW fd;
        HANDLE find = FindFirstFileW((cache.dir + L"*.dxbc").c_str(), &fd);
        if (find == INVALID_HANDLE_VALUE) return;
        int vs = 0, ps = 0;
        do {
            const uint64_t hash = std::wcstoull(fd.cFileName, nullptr, 16);
            ComPtr<ID3DBlob> blob = cache.Load(hash);
            if (!blob || blob->GetBufferSize() < 32) continue;
            // the program type of the shader chunk's version token: 0 pixel, 1 vertex
            const uint8_t* p = static_cast<const uint8_t*>(blob->GetBufferPointer());
            const size_t size = blob->GetBufferSize();
            int type = -1;
            const uint32_t chunks = *reinterpret_cast<const uint32_t*>(p + 28);
            for (uint32_t i = 0; i < chunks && 32 + 4 * i + 4 <= size; i++) {
                const uint32_t at = *reinterpret_cast<const uint32_t*>(p + 32 + 4 * i);
                if (at + 12 > size) break;
                if (!std::memcmp(p + at, "SHEX", 4) || !std::memcmp(p + at, "SHDR", 4))
                    type = int(*reinterpret_cast<const uint32_t*>(p + at + 8) >> 16);
            }
            ComPtr<ID3D11DeviceChild> made;
            if (type == 1) {
                ComPtr<ID3D11VertexShader> v;
                if (SUCCEEDED(g_device->CreateVertexShader(p, size, nullptr, &v))) made = v, vs++;
            } else if (type == 0) {
                ComPtr<ID3D11PixelShader> x;
                if (SUCCEEDED(g_device->CreatePixelShader(p, size, nullptr, &x))) made = x, ps++;
            }
            if (!made) continue;
            std::lock_guard guard(g_prefetch_lock);
            g_prefetched[hash] = blob;
            g_prefetched_shaders[hash] = made;
        } while (FindNextFileW(find, &fd));
        FindClose(find);
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&freq);
        rt::Log("render: shader cache read ahead: %d vertex and %d pixel shaders in %.0f ms", vs, ps,
                double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(freq.QuadPart));
    }).detach();
}

// the job's shader from the read-ahead cache, at once; false when it is not there (yet)
bool FromPrefetch(ShaderJob& job) {
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3D11DeviceChild> made;
    {
        std::lock_guard guard(g_prefetch_lock);
        auto it = g_prefetched.find(job.hash);
        if (it == g_prefetched.end()) return false;
        blob = it->second;
        made = g_prefetched_shaders[job.hash];
    }
    if (job.vertex) {
        if (FAILED(made.As(&job.vs))) return false;
        if (!job.elements.empty())
            g_device->CreateInputLayout(job.elements.data(), UINT(job.elements.size()), blob->GetBufferPointer(),
                                        blob->GetBufferSize(), &job.layout);
    } else if (FAILED(made.As(&job.ps))) {
        return false;
    }
    std::lock_guard guard(Workers().lock);
    Cache().hits++;
    job.done = true;
    return true;
}

double MsSince(const LARGE_INTEGER& t) {
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    return double(now.QuadPart - t.QuadPart) * 1000.0 / double(freq.QuadPart);
}

// whether a draw may use the job's shader now (waits as the comment above says); false: leave the draw out
bool ShaderReady(ShaderJob& job) {
    if (job.done) return true;
    if (!ShaderAsync()) return Workers().Wait(job, -1);
    if ((g_present_at.QuadPart && (g_present_interval > 40 || MsSince(g_present_at) > 40)) ||
        (g_held_at.QuadPart && MsSince(g_held_at) < 1500)) {
        g_frame_held++;
        return false;
    }
    if (const double left = ShaderWait() - g_frame_waited; left > 0) {
        LARGE_INTEGER t0;
        QueryPerformanceCounter(&t0);
        const bool done = Workers().Wait(job, left);
        g_frame_waited += MsSince(t0);
        if (done) return true;
    }
    g_frame_left_out++;
    if (job.vertex) g_frame_left_vs++;
    rt::hitch::Note(rt::hitch::kSkippedDraw, 0);
    return false;
}

// at a present: true when the frame is to be shown (not one whose draws were left out because it was long)
bool ShadersPresent() {
    static uint64_t draws_before = 0;
    const uint64_t drawn = g_draws - draws_before;
    draws_before = g_draws;
    if (g_frame_left_out >= 20 && g_frame_left_out * 4 > drawn)  // a fifth or more of the frame is missing
        g_frame_held += g_frame_left_out, g_frame_left_out = 0;
    const bool held = g_frame_held != 0;
    if (held) Workers().WaitAll();
    if (held) QueryPerformanceCounter(&g_held_at);
    if (g_frame_left_out) g_left_frames++;
    if (g_frame_left_out) g_left_vs += g_frame_left_vs, g_left_ps += g_frame_left_out - g_frame_left_vs;
    // FCI_DUMP_LEFT_OUT=<n>: the first n shown frames with draws left out, each with the next complete frame
    static int dump_left = [] {
        char v[16] = {};
        return GetEnvironmentVariableA("FCI_DUMP_LEFT_OUT", v, sizeof v) ? std::atoi(v) : 0;
    }();
    static bool dump_complete = false;
    if (dump_left > 0 && g_frame_left_out && !held) {
        present::DumpNext("missing"), dump_left--, dump_complete = true;
        rt::Log("render: frame dumped with %llu draws left out", (unsigned long long)g_frame_left_out);
    }
    else if (dump_complete && !g_frame_left_out && !held) present::DumpNext("complete"), dump_complete = false;
    if (held) g_held_frames++;
    g_left_out += g_frame_left_out + g_frame_held;
    g_frame_left_out = g_frame_held = g_frame_left_vs = 0;
    g_frame_waited = 0;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_present_at.QuadPart) g_present_interval = MsSince(g_present_at);
    g_present_at = now;
    return !held;
}
// ---- end performance ----

// formats (performance: raw draws): attribute i in input slot i as the guest stores it, else float4s in slot 0
// water (PC water): the ocean's vertex program also hands the world position (its R0: the grid point through
// c171..c173) to the pixel shader
VertexShader* GetVertexShader(const State& s, uint16_t enabled, const uint8_t* formats = nullptr, bool water = false) {
    uint64_t key = vsh::Key(s, enabled);
    if (water) key = Mix(key, 0x5741544552ull);
    if (formats) {
        uint64_t f = 0x52415721;  // the formats of the enabled attributes
        for (int i = 0; i < kAttributes; i++)
            if (enabled & (1u << i)) f = Mix(f, uint64_t(i) << 8 | formats[i]);
        key = Mix(key, f);
    }
    auto it = g_vertex_shaders.find(key);
    if (it == g_vertex_shaders.end()) {
        rt::hitch::Timed timed(rt::hitch::kShaderMade);  // performance: FCI_TRACE_HITCHES
        VertexShader& vs = g_vertex_shaders[key];
        auto job = std::make_shared<ShaderJob>();  // performance: shaders off the render thread
        job->vertex = true;
        job->source = vsh::Generate(s, enabled, formats);
        if (water) {  // PC water
            std::string& src = job->source;
            src.insert(src.find("};", src.find("struct VSOut")), "    float4 wpos : WPOS;\n");
            src.insert(src.rfind("    return o;"),
                       "    o.wpos = float4(dot(v[0].xyz, C[171].xyz) + C[171].w, dot(v[0].xyz, C[172].xyz) + C[172].w, "
                       "dot(v[0].xyz, C[173].xyz) + C[173].w, 1);\n");
        }
        job->hash = SourceHash(job->source, "vs_5_0");
        UINT n = 0;
        for (UINT i = 0; i < kAttributes; i++)
            if (enabled & (1u << i)) {
                if (formats) {
                    int kind = 0;
                    DXGI_FORMAT f = DXGI_FORMAT(vsh::FetchFormat(formats[i], &kind));
                    job->elements.push_back({"ATTR", i, f, i, 0, D3D11_INPUT_PER_VERTEX_DATA, 0}), n++;
                } else {
                    job->elements.push_back(
                        {"ATTR", i, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, n * 16, D3D11_INPUT_PER_VERTEX_DATA, 0}), n++;
                }
            }
        if (g_dump_shaders || !FromPrefetch(*job)) Workers().Add(job);
        vs.job = std::move(job);
        it = g_vertex_shaders.find(key);
    }
    VertexShader& vs = it->second;
    if (vs.job) {
        if (!ShaderReady(*vs.job)) return nullptr;
        vs.shader = std::move(vs.job->vs), vs.layout = std::move(vs.job->layout);
        vs.job.reset();
    }
    return vs.shader ? &vs : nullptr;
}

ID3D11PixelShader* GetPixelShader(const State& s, const psh::Config& config) {
    uint64_t key = psh::Key(s, config);
    auto it = g_pixel_shaders.find(key);
    if (it == g_pixel_shaders.end()) {
        rt::hitch::Timed timed(rt::hitch::kShaderMade);  // performance: FCI_TRACE_HITCHES
        auto& entry = g_pixel_shaders[key];
        std::string source = psh::Generate(s, config);
        // FCI_RENDER_SOLID=1: every pixel shader draws magenta; FCI_RENDER_SOLID_PS=<key> (hex): only that one
        static const uint64_t solid_key = [] {
            char value[32] = {};
            return GetEnvironmentVariableA("FCI_RENDER_SOLID_PS", value, sizeof value) ? std::strtoull(value, nullptr, 16)
                                                                                       : 0ull;
        }();
        static const bool solid = GetEnvironmentVariableA("FCI_RENDER_SOLID", nullptr, 0) > 0;
        if (solid || (solid_key && key == solid_key)) {
            size_t at = source.find("float4 T[4]");
            if (at != std::string::npos) source.insert(at, "return float4(1, 0, 1, 1);\n    ");
        }
        auto job = std::make_shared<ShaderJob>();  // performance: shaders off the render thread
        job->source = std::move(source);
        job->hash = SourceHash(job->source, "ps_5_0");
        if (g_dump_shaders || !FromPrefetch(*job)) Workers().Add(job);
        entry.job = std::move(job);
        it = g_pixel_shaders.find(key);
    }
    PixelShader& ps = it->second;
    if (ps.job) {
        if (!ShaderReady(*ps.job)) return nullptr;
        ps.shader = std::move(ps.job->ps);
        ps.job.reset();
    }
    return ps.shader.Get();
}

// ---- textures ----

struct TextureEntry {
    ComPtr<ID3D11ShaderResourceView> srv;
    uint64_t hash = 0, checked = 0, writebacks = 0;
    // ---- texture dump/replacement (gpu/texture_pack.h) ----
    uint64_t pack_key = 0;           // the content's name while replacing applies, else 0
    uint32_t pack_revision = 0;      // texpack::Revision() the replacement was looked up at
    bool pack_pending = false;       // the replacement is still loading: srv is the game's own for now
    uint32_t pack_rewrites = 0;      // content changes soon after the previous one: the game draws or streams here
    uint64_t pack_generation = 0;    // when the content last changed
    ComPtr<ID3D11ShaderResourceView> pack_original;  // the game's texture while srv is a replacement
    uint64_t pack_dump_key = 0;      // dumped once the content stayed for a while (not a video frame)
    texture::Desc pack_dump_desc;
    std::vector<texture::Image> pack_dump_images;
    // ---- end texture dump/replacement ----
};

bool DirtyOverlap(uint32_t address, uint32_t size) {
    for (auto& t : g_targets)
        if (t->dirty && t->Overlaps(address, size)) return true;
    return false;
}

// a linear texture that is exactly a render target's surface: sample a GPU-side snapshot of the host copy
// instead of a round trip through guest memory
ID3D11ShaderResourceView* Resolved(Target& t);

ID3D11ShaderResourceView* TargetSnapshot(const texture::Desc& d, float scale[2]) {
    bool linear = d.color == 0x12, swizzled = d.color == 0x06;  // LIN_A8R8G8B8, SZ_A8R8G8B8
    if (!linear && !swizzled) return nullptr;
    Target* found = nullptr;
    for (auto& t : g_targets) {
        // only while the host copy is newer: otherwise guest memory is current and the texture cache serves it
        if (!t->dirty || t->address != d.address || t->format != 8 || t->swizzled != swizzled) continue;
        if (linear && (t->pitch != d.pitch || t->width < d.width || t->height < d.height)) continue;
        if (swizzled && (t->width != d.width || t->height != d.height || d.depth > 1 || d.cube)) continue;
        if (!found || t->last_write > found->last_write) found = t.get();
    }
    // HLE: a linear texture over rows further down a target (no guest copy to fall back on at another scale)
    if (!found && linear && g_hle) {
        for (auto& t : g_targets) {
            if (!t->dirty || t->format != 8 || t->swizzled || t->pitch != d.pitch || d.address <= t->address) continue;
            uint32_t offset = d.address - t->address;
            if (offset % d.pitch || offset / d.pitch + d.height > t->height || d.width > t->width) continue;
            if (!found || t->last_write > found->last_write) found = t.get();
        }
        if (Target* t = found) {
            uint32_t first = (d.address - t->address) / d.pitch;
            auto& r = t->rows[{first, d.height}];
            const uint32_t row0 = HostHeightOf(first, t->scale), row1 = HostHeightOf(first + d.height, t->scale);
            if (!r.texture) {
                D3D11_TEXTURE2D_DESC desc{t->HostWidth(), row1 - row0, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0},
                                          D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
                if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &r.texture))) return nullptr;
                g_device->CreateShaderResourceView(r.texture.Get(), nullptr, &r.srv);
            }
            if (r.version != t->version) {
                ID3D11ShaderResourceView* whole = Resolved(*t);
                ComPtr<ID3D11Resource> src;
                whole->GetResource(&src);
                D3D11_BOX box{0, row0, 0, t->HostWidth(), row1, 1};
                g_context->CopySubresourceRegion(r.texture.Get(), 0, 0, 0, 0, src.Get(), 0, &box);
                r.version = t->version;
            }
            scale[0] = 1.0f / t->width, scale[1] = 1.0f / d.height;
            return r.srv.Get();
        }
        return nullptr;
    }
    if (Target* t = found; t && t->samples > 1) {  // performance: one resolve serves snapshots and the present
        ID3D11ShaderResourceView* r = Resolved(*t);
        if (r && linear) scale[0] = 1.0f / t->width, scale[1] = 1.0f / t->height;
        return r;
    }
    if (Target* t = found) {
        if (!t->snapshot) {
            D3D11_TEXTURE2D_DESC desc;
            t->texture->GetDesc(&desc);
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            desc.SampleDesc.Count = 1;
            if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &t->snapshot))) return nullptr;
            g_device->CreateShaderResourceView(t->snapshot.Get(), nullptr, &t->snapshot_srv);
        }
        if (t->snapshot_version != t->version) {
            if (t->samples > 1)
                g_context->ResolveSubresource(t->snapshot.Get(), 0, t->texture.Get(), 0, DXGI_FORMAT_B8G8R8A8_UNORM);
            else
                g_context->CopyResource(t->snapshot.Get(), t->texture.Get());
            t->snapshot_version = t->version;
        }
        if (linear) scale[0] = 1.0f / t->width, scale[1] = 1.0f / t->height;
        return t->snapshot_srv.Get();
    }
    return nullptr;
}
std::unordered_map<uint64_t, TextureEntry> g_textures;

// ---- texture dump/replacement (gpu/texture_pack.h) ----
// textures on render target memory are the GPU's output, not game assets: neither dumped nor replaced
bool OnTarget(uint32_t address, uint32_t size) {
    for (auto& t : g_targets)
        if (t->Overlaps(address, size)) return true;
    for (auto& t : g_depths)
        if (address < t->address + t->width * t->height * (t->z16 ? 2 : 4) && t->address < address + size) return true;
    return false;
}

// the replacement once it is loaded (or again after the folder changed), else the game's own texture
void PackLookup(TextureEntry& e, psh::Sampler kind) {
    e.pack_revision = texpack::Revision();
    ComPtr<ID3D11ShaderResourceView> replacement;
    texpack::Lookup found = texpack::Find(g_device.Get(), e.pack_key, kind == psh::Sampler::kCube, replacement);
    e.pack_pending = found == texpack::Lookup::kLoading;
    e.srv = found == texpack::Lookup::kReady ? replacement : e.pack_original;
}

// a texture is dumped once its content stayed this long (a generation is a frame or half of one), so the
// frames of videos and other textures the game rewrites all the time are not
constexpr uint64_t kPackStable = 20;

// a texture was used: dumped once it has been stable
void PackUsed(TextureEntry& e) {
    if (g_generation - e.pack_generation < kPackStable) return;
    texpack::Dump(e.pack_dump_key, e.pack_dump_desc, std::move(e.pack_dump_images));
    e.pack_dump_key = 0;
    e.pack_dump_images.clear();
}

// a texture was (re)created from guest memory: keep it for the dump, look for its replacement
void PackCreated(TextureEntry& e, const texture::Desc& d, psh::Sampler kind, uint32_t size,
                 std::vector<texture::Image>& images) {
    // changed twice within a few frames of the previous version: rewritten all the time, as videos and
    // procedural textures are; such an entry stays the game's
    if (e.pack_generation && g_generation - e.pack_generation < kPackStable) e.pack_rewrites++;
    e.pack_generation = g_generation;
    e.pack_key = 0, e.pack_pending = false, e.pack_original.Reset();
    e.pack_dump_key = 0, e.pack_dump_images.clear();
    if (!e.srv || e.pack_rewrites >= 2 || OnTarget(d.address, size)) return;
    if (kind != psh::Sampler::k2D && kind != psh::Sampler::kRect && kind != psh::Sampler::kCube) return;
    uint64_t key = texpack::Key(d);
    if (!key) return;
    if (texpack::WantsDump(key)) {
        e.pack_dump_key = key, e.pack_dump_desc = d;
        e.pack_dump_images = std::move(images);
        if (!d.cube) e.pack_dump_images.resize(1);  // PNG: level 0
    }
    if (!texpack::Replacing()) return;
    // rect textures stay addressed in texels of the guest size (tex_scale), so a bigger image fits as it is
    e.pack_key = key;
    e.pack_original = e.srv;
    PackLookup(e, kind);
}
// ---- end texture dump/replacement ----
std::map<std::array<uint32_t, 4>, ComPtr<ID3D11SamplerState>> g_samplers;  // address, filter, border, control

// ---- performance: DXT textures go to the GPU as BC1/BC2/BC3 blocks (a quarter to an eighth of the BGRA8 bytes
// to read when drawing, no decode on the CPU); null when that cannot be (3D textures, levels not in whole blocks) ----
ComPtr<ID3D11ShaderResourceView> CreateBlockTexture(const texture::Desc& d, psh::Sampler kind) {
    static const bool off = GetEnvironmentVariableA("FCI_NO_BC", nullptr, 0) > 0;  // diagnostics: decode as before
    const int bc = texture::BlockFormat(d);
    if (off || !bc || kind == psh::Sampler::k3D) return nullptr;
    static std::vector<texture::Blocks> levels;
    if (!texture::CompressedLevels(d, levels)) return nullptr;
    std::vector<D3D11_SUBRESOURCE_DATA> data(levels.size());
    for (size_t i = 0; i < levels.size(); i++) data[i] = {levels[i].data, levels[i].row_pitch, levels[i].bytes};
    static const DXGI_FORMAT kFormat[] = {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC2_UNORM,
                                          DXGI_FORMAT_BC3_UNORM};
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = d.width, desc.Height = d.height, desc.MipLevels = d.levels, desc.ArraySize = d.cube ? 6 : 1;
    desc.Format = kFormat[bc];
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = d.cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    if (SUCCEEDED(g_device->CreateTexture2D(&desc, data.data(), &tex)))
        g_device->CreateShaderResourceView(tex.Get(), nullptr, &srv);
    return srv;
}
// ---- end performance ----

ComPtr<ID3D11ShaderResourceView> CreateTexture(const texture::Desc& d, psh::Sampler kind,
                                               const std::vector<texture::Image>& images) {
    uint32_t faces = d.cube ? 6 : 1;
    UINT levels = UINT(images.size() / faces);
    std::vector<D3D11_SUBRESOURCE_DATA> data(images.size());
    for (size_t i = 0; i < images.size(); i++)
        data[i] = {images[i].pixels.data(), images[i].width * 4, images[i].width * images[i].height * 4};
    ComPtr<ID3D11ShaderResourceView> srv;
    if (kind == psh::Sampler::k3D) {
        D3D11_TEXTURE3D_DESC desc{d.width, d.height, d.depth, levels, DXGI_FORMAT_B8G8R8A8_UNORM,
                                  D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0};
        ComPtr<ID3D11Texture3D> tex;
        if (SUCCEEDED(g_device->CreateTexture3D(&desc, data.data(), &tex)))
            g_device->CreateShaderResourceView(tex.Get(), nullptr, &srv);
    } else {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = d.width, desc.Height = d.height, desc.MipLevels = levels, desc.ArraySize = faces;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = d.cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
        ComPtr<ID3D11Texture2D> tex;
        if (SUCCEEDED(g_device->CreateTexture2D(&desc, data.data(), &tex)))
            g_device->CreateShaderResourceView(tex.Get(), nullptr, &srv);
    }
    return srv;
}

const bool g_no_snapshot = GetEnvironmentVariableA("FCI_NO_TARGET_SNAPSHOT", nullptr, 0) > 0;  // diagnostics

// a depth texture on a depth target's memory (a shadow map the GPU just rendered): the host depth itself
ID3D11ShaderResourceView* DepthTexture(const texture::Desc& d, float* scale) {
    DepthTarget* found = nullptr;
    for (auto& t : g_depths)
        if (t->address == d.address && t->srv && t->width >= d.width && t->height >= d.height &&
            (!found || t->last_write > found->last_write))
            found = t.get();
    if (!found) return nullptr;
    bool z16 = d.color == 0x2C || d.color == 0x30;
    scale[2] = z16 ? 65535.0f : 16777215.0f;
    // linear (rect) textures address in texels of the guest size; the host target may be larger
    if (d.color == 0x2E || d.color == 0x30) scale[0] = 1.0f / found->width, scale[1] = 1.0f / found->height;
    return found->srv.Get();
}

// the texture of a stage, or null; kind and scale (xy: coordinate scale, z: depth range of shadow maps)
// describe how the shader addresses it
ID3D11ShaderResourceView* GetTexture(const State& s, int stage, psh::Sampler& kind, float* scale) {
    uint32_t base = 0x40 * stage;
    g_stage_video[stage] = 0;  // PC features: fmv_scale
    uint32_t control0 = s.Reg(SET_TEXTURE_CONTROL0 + base);
    if (!(control0 & 0x40000000)) return nullptr;
    uint32_t format = s.Reg(SET_TEXTURE_FORMAT + base);
    uint32_t dma = (format & 3) == 2 ? SET_CONTEXT_DMA_B : SET_CONTEXT_DMA_A;
    texture::Desc d;
    d.address = ResolveDma(s.Reg(dma)) + s.Reg(SET_TEXTURE_OFFSET + base);
    d.color = (format >> 8) & 0xFF;
    texture::Format info = texture::Lookup(d.color);
    if (!info.supported) {
        static uint64_t logged[4] = {};
        if (!(logged[d.color >> 6] & (1ull << (d.color & 63)))) {
            logged[d.color >> 6] |= 1ull << (d.color & 63);
            rt::Log("render: texture format %02X not supported", d.color);
        }
        return nullptr;
    }
    uint32_t dimensionality = (format >> 4) & 0xF;
    d.cube = format & 4;
    if (info.layout == texture::Layout::kLinear) {
        uint32_t rect = s.Reg(SET_TEXTURE_IMAGE_RECT + base);
        d.width = rect >> 16, d.height = rect & 0xFFFF;
        d.pitch = s.Reg(SET_TEXTURE_CONTROL1 + base) >> 16;
        d.levels = 1;
        d.cube = false;
        kind = psh::Sampler::kRect;
        scale[0] = d.width ? 1.0f / d.width : 1.0f, scale[1] = d.height ? 1.0f / d.height : 1.0f;
    } else {
        d.width = 1u << ((format >> 20) & 0xF);
        d.height = 1u << ((format >> 24) & 0xF);
        d.depth = dimensionality == 3 ? 1u << ((format >> 28) & 0xF) : 1;
        uint32_t max_levels = 1;
        while ((std::max(d.width, d.height) >> max_levels) && max_levels < 13) max_levels++;
        d.levels = std::clamp((format >> 16) & 0xF, 1u, max_levels);
        kind = d.cube ? psh::Sampler::kCube : dimensionality == 3 ? psh::Sampler::k3D : psh::Sampler::k2D;
    }
    if (d.color == 0x0B) {
        uint32_t palette = s.Reg(SET_TEXTURE_PALETTE + base);
        d.palette = ResolveDma(s.Reg((palette & 1) ? SET_CONTEXT_DMA_B : SET_CONTEXT_DMA_A)) + (palette & ~0x3Fu);
        d.palette_entries = 256 >> ((palette >> 2) & 3);
    }
    uint32_t size = texture::GuestSize(d);
    if (!d.width || !d.height || !size || d.address + uint64_t(size) > 0x04000000) return nullptr;

    if (d.color == 0x2A || d.color == 0x2C || d.color == 0x2E || d.color == 0x30)
        if (ID3D11ShaderResourceView* srv = DepthTexture(d, scale)) {
            kind = psh::Sampler::kShadow;
            return srv;
        }
    if (!g_no_snapshot)
        if (ID3D11ShaderResourceView* srv = TargetSnapshot(d, scale)) return srv;

    uint64_t key = Mix(Mix(Mix(d.address, d.color), Mix(d.width, d.height)), Mix(Mix(d.depth, d.levels),
                                                                                  Mix(d.cube, Mix(d.pitch, d.palette))));
    TextureEntry& entry = g_textures[key];
    // PC features, fmv_scale: Bink fills the title's and menus' videos into linear textures (not render targets)
    if (info.layout == texture::Layout::kLinear) g_stage_video[stage] = d.width << 16 | d.height;
    // ---- texture dump/replacement: a replacement that finished loading, or a changed replacement folder ----
    if (entry.pack_key && (entry.pack_pending || entry.pack_revision != texpack::Revision())) PackLookup(entry, kind);
    if (entry.pack_dump_key) PackUsed(entry);
    // once per generation, unless the GPU has drawn into this memory since
    if (entry.srv && entry.checked == g_generation && entry.writebacks == g_writebacks && !DirtyOverlap(d.address, size))
        return entry.srv.Get();
    char why[64];
    snprintf(why, sizeof why, "texture %ux%u color %02X pitch %u", d.width, d.height, d.color, d.pitch);
    DownloadOverlapping(d.address, size, why);
    uint64_t hash = ContentHash(Physical(d.address), size);
    if (d.palette_entries) hash = Mix(hash, ContentHash(Physical(d.palette), d.palette_entries * 4));
    entry.checked = g_generation;
    entry.writebacks = g_writebacks;
    if (entry.srv && entry.hash == hash) return entry.srv.Get();
    entry.hash = hash;
    const int64_t made_at = rt::hitch::Enabled() ? rt::hitch::Now() : 0;  // performance: FCI_TRACE_HITCHES
    if (!texpack::Active()) {  // performance: DXT as BC (texture dump/replacement wants the decoded pixels)
        if (ComPtr<ID3D11ShaderResourceView> srv = CreateBlockTexture(d, kind)) {
            entry.srv = std::move(srv);
            if (made_at) rt::hitch::Note(rt::hitch::kTexture, rt::hitch::Now() - made_at, size);
            return entry.srv.Get();
        }
    }
    auto images = texture::Decode(d);
    entry.srv = images.empty() ? nullptr : CreateTexture(d, kind, images);
    if (made_at) rt::hitch::Note(rt::hitch::kTexture, rt::hitch::Now() - made_at, size);
    if (texpack::Active()) PackCreated(entry, d, kind, size, images);  // ---- texture dump/replacement ----
    return entry.srv.Get();
}

D3D11_TEXTURE_ADDRESS_MODE AddressMode(uint32_t v) {
    switch (v) {
    case 2: return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 3:
    case 5: return D3D11_TEXTURE_ADDRESS_CLAMP;
    case 4: return D3D11_TEXTURE_ADDRESS_BORDER;
    default: return D3D11_TEXTURE_ADDRESS_WRAP;
    }
}

ID3D11SamplerState* GetSampler(const State& s, int stage) {
    uint32_t base = 0x40 * stage;
    uint32_t address = s.Reg(SET_TEXTURE_ADDRESS + base), filter = s.Reg(SET_TEXTURE_FILTER + base);
    uint32_t border = s.Reg(SET_TEXTURE_BORDER_COLOR + base);
    uint32_t control = s.Reg(SET_TEXTURE_CONTROL0 + base);
    std::array<uint32_t, 4> key{address & 0xFFFFF, filter & 0x0FFF1FFF, border, control & 0x3FFFFFC0};
    auto& sampler = g_samplers[key];
    if (sampler) return sampler.Get();
    uint32_t min = (filter >> 16) & 0xFF, mag = (filter >> 24) & 0xF;
    bool min_linear = min == 2 || min == 4 || min == 6 || min == 7, mag_linear = mag != 1;
    bool mip = min >= 3 && min <= 6, mip_linear = min == 5 || min == 6;
    D3D11_SAMPLER_DESC desc{};
    int f = (min_linear ? 0x10 : 0) | (mag_linear ? 0x4 : 0) | (mip_linear ? 0x1 : 0);
    desc.Filter = D3D11_FILTER(f);
    desc.MaxAnisotropy = 1;
    if (g_q.aniso > 1 && min_linear && mag_linear) {
        desc.Filter = D3D11_FILTER_ANISOTROPIC;
        desc.MaxAnisotropy = g_q.aniso;
    }
    desc.AddressU = AddressMode(address & 0xF);
    desc.AddressV = AddressMode((address >> 8) & 0xF);
    desc.AddressW = AddressMode((address >> 16) & 0xF);
    desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    desc.BorderColor[0] = ((border >> 16) & 0xFF) / 255.0f;
    desc.BorderColor[1] = ((border >> 8) & 0xFF) / 255.0f;
    desc.BorderColor[2] = (border & 0xFF) / 255.0f;
    desc.BorderColor[3] = (border >> 24) / 255.0f;
    // MIPMAP_LOD_BIAS (signed, 1/256 steps) and the LOD clamps of TEXTURE_CONTROL0 (4.8 fixed point)
    desc.MipLODBias = float(int32_t(filter << 19) >> 19) / 256.0f;
    desc.MinLOD = mip ? float((control >> 18) & 0xFFF) / 256.0f : 0;
    desc.MaxLOD = mip ? float((control >> 6) & 0xFFF) / 256.0f : 0;
    g_device->CreateSamplerState(&desc, &sampler);
    return sampler.Get();
}

// ---- fixed function state ----

D3D11_BLEND BlendFactor(uint32_t gl, bool alpha) {
    switch (gl) {
    case 0: return D3D11_BLEND_ZERO;
    case 1: return D3D11_BLEND_ONE;
    case 0x300: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case 0x301: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case 0x302: return D3D11_BLEND_SRC_ALPHA;
    case 0x303: return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x304: return D3D11_BLEND_DEST_ALPHA;
    case 0x305: return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x306: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case 0x307: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case 0x308: return alpha ? D3D11_BLEND_ONE : D3D11_BLEND_SRC_ALPHA_SAT;
    case 0x8001:
    case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
    case 0x8002:
    case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
    default: return D3D11_BLEND_ONE;
    }
}

D3D11_BLEND_OP BlendOp(uint32_t gl) {
    switch (gl) {
    case 0x800A: return D3D11_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D11_BLEND_OP_REV_SUBTRACT;
    case 0x8007: return D3D11_BLEND_OP_MIN;
    case 0x8008: return D3D11_BLEND_OP_MAX;
    default: return D3D11_BLEND_OP_ADD;
    }
}

D3D11_STENCIL_OP StencilOp(uint32_t gl) {
    switch (gl) {
    case 0: return D3D11_STENCIL_OP_ZERO;
    case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150A: return D3D11_STENCIL_OP_INVERT;
    case 0x8507: return D3D11_STENCIL_OP_INCR;
    case 0x8508: return D3D11_STENCIL_OP_DECR;
    default: return D3D11_STENCIL_OP_KEEP;
    }
}

std::unordered_map<uint64_t, ComPtr<ID3D11BlendState>> g_blend_states;
std::unordered_map<uint64_t, ComPtr<ID3D11DepthStencilState>> g_depth_states;
std::unordered_map<uint64_t, ComPtr<ID3D11RasterizerState>> g_raster_states;

UINT8 WriteMask(uint32_t color_mask) {
    return UINT8(((color_mask >> 16) & 1 ? 1 : 0) | ((color_mask >> 8) & 1 ? 2 : 0) | (color_mask & 1 ? 4 : 0) |
                 ((color_mask >> 24) & 1 ? 8 : 0));
}

ID3D11BlendState* GetBlendState(bool enable, uint32_t src, uint32_t dst, uint32_t equation, UINT8 mask) {
    uint64_t key = Mix(Mix(enable, src), Mix(dst, Mix(equation, mask)));
    auto& state = g_blend_states[key];
    if (state) return state.Get();
    D3D11_BLEND_DESC desc{};
    auto& rt = desc.RenderTarget[0];
    rt.BlendEnable = enable;
    rt.SrcBlend = BlendFactor(src, false), rt.DestBlend = BlendFactor(dst, false), rt.BlendOp = BlendOp(equation);
    rt.SrcBlendAlpha = BlendFactor(src, true), rt.DestBlendAlpha = BlendFactor(dst, true);
    rt.BlendOpAlpha = BlendOp(equation);
    rt.RenderTargetWriteMask = mask;
    g_device->CreateBlendState(&desc, &state);
    return state.Get();
}

ID3D11DepthStencilState* GetDepthState(const D3D11_DEPTH_STENCIL_DESC& desc) {
    uint64_t key = Hash(reinterpret_cast<const uint8_t*>(&desc), sizeof desc);
    auto& state = g_depth_states[key];
    if (!state) g_device->CreateDepthStencilState(&desc, &state);
    return state.Get();
}

ID3D11RasterizerState* GetRasterState(const D3D11_RASTERIZER_DESC& desc) {
    uint64_t key = Hash(reinterpret_cast<const uint8_t*>(&desc), sizeof desc);
    auto& state = g_raster_states[key];
    if (!state) g_device->CreateRasterizerState(&desc, &state);
    return state.Get();
}

// ---- buffers ----

struct VertexConstants {
    float defaults[kAttributes][4];
    float screen[4];
    float composite[4][4];
    float hud[4];  // PC features: 2D vertices to x * hud[0] + hud[1], y * hud[2] + hud[3] (widescreen, fmv_scale)
    float c[kConstants][4];  // performance: last (vsh.cpp: reads past it are 0 without clamping the index)
};

struct PixelConstants {
    float factor0[8][4];
    float factor1[8][4];
    float fog_color[4];
    float fog_params[4];
    float alpha_ref[4];
    float tex_scale[4][4];
    float bump[4][4];
    float final_factor[2][4];
    float depth_bias[4];
};

ComPtr<ID3D11Buffer> g_clear_constants;
ComPtr<ID3D11VertexShader> g_clear_vs;
ComPtr<ID3D11PixelShader> g_clear_ps;

const char* const kClearShader = R"(
cbuffer Clear : register(b2) { float4 clear_color; float4 clear_depth; };
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * float2(2, -2) + float2(-1, 1), clear_depth.x, 1);
}
float4 ps() : SV_Target { return clear_color; }
)";

ComPtr<ID3D11Buffer> MakeConstantBuffer(UINT size) {
    D3D11_BUFFER_DESC desc{(size + 15) & ~15u, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER,
                           D3D11_CPU_ACCESS_WRITE, 0, 0};
    ComPtr<ID3D11Buffer> buffer;
    g_device->CreateBuffer(&desc, nullptr, &buffer);
    return buffer;
}

void Fill(ID3D11Buffer* buffer, const void* data, size_t size) {
    D3D11_MAPPED_SUBRESOURCE map;
    if (SUCCEEDED(g_context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        std::memcpy(map.pData, data, size);
        g_context->Unmap(buffer, 0);
    }
}

// a constant buffer that is only written when its contents change: thousands of draws a frame share most
// of their constants, and each WRITE_DISCARD costs the driver a new buffer version
template <typename T>
struct Constants {
    ComPtr<ID3D11Buffer> buffer;
    T last{};
    bool valid = false;

    ID3D11Buffer* Update(const T& value) {
        if (!buffer) buffer = MakeConstantBuffer(sizeof(T));
        if (!valid || std::memcmp(&value, &last, sizeof(T)) != 0) {
            Fill(buffer.Get(), &value, sizeof(T));
            last = value;
            valid = true;
        }
        return buffer.Get();
    }
};

// vertex and index data go into a ring: NO_OVERWRITE appends, a DISCARD only when it wraps
struct Ring {
    ComPtr<ID3D11Buffer> buffer;
    UINT capacity = 0, offset = 0;
    UINT bind = 0;

    explicit Ring(UINT bind_flags) : bind(bind_flags) {}

    // the byte offset the data landed at
    UINT Push(const void* data, UINT size) {
        UINT at = 0;
        PushMany(&data, &size, 1, &at);
        return at;
    }

    // several pieces in one map (one buffer version: a wrap cannot separate them), each 256 byte aligned
    void PushMany(const void* const* data, const UINT* size, int n, UINT* at) {
        UINT total = 0;
        for (int k = 0; k < n; k++) total += (size[k] + 255) & ~255u;
        D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
        if (total > capacity) {
            capacity = std::max(std::max(total * 2, capacity * 2), 8u << 20);
            D3D11_BUFFER_DESC desc{capacity, D3D11_USAGE_DYNAMIC, bind, D3D11_CPU_ACCESS_WRITE, 0, 0};
            buffer.Reset();
            g_device->CreateBuffer(&desc, nullptr, &buffer);
            mode = D3D11_MAP_WRITE_DISCARD, offset = 0;
        } else if (offset + total > capacity) {
            mode = D3D11_MAP_WRITE_DISCARD, offset = 0;
        }
        D3D11_MAPPED_SUBRESOURCE map;
        if (FAILED(g_context->Map(buffer.Get(), 0, mode, 0, &map))) {
            for (int k = 0; k < n; k++) at[k] = 0;
            return;
        }
        for (int k = 0; k < n; k++) {
            std::memcpy(static_cast<uint8_t*>(map.pData) + offset, data[k], size[k]);
            at[k] = offset;
            offset = (offset + size[k] + 255) & ~255u;
        }
        g_context->Unmap(buffer.Get(), 0);
    }
};

// Safety net (CompileHlsl): the first failed HLSL compile of a run shows one message box, from a thread of its own so
// that neither the render thread nor the background shader workers wait for it
std::atomic<bool> g_hlsl_failure_shown{false};

bool UnderWine() {
    return GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
}

void ShowHlslFailure() {
    if (g_hlsl_failure_shown.exchange(true)) return;
    const bool wine = UnderWine();
    rt::Log("render: HLSL compile failed, showing a message box once (%s)", wine ? "Wine/Proton" : "Windows");
    std::thread([wine] {
        const wchar_t* text =
            wine ? L"A shader could not be compiled. Proton's own HLSL compiler (d3dcompiler_47) does not handle "
                   L"everything this game generates, so some objects may be invisible.\n\n"
                   L"Fix: install Microsoft's compiler into the game's Proton prefix with protontricks:\n"
                   L"    protontricks -l\n"
                   L"(lists the app ID of the Far Cry Instincts shortcut), then\n"
                   L"    protontricks <app ID> d3dcompiler_47\n\n"
                   L"Details are in fci.log next to FarCryInstincts.exe. The game continues."
                 : L"A shader could not be compiled by the system's HLSL compiler (d3dcompiler_47.dll), so some "
                   L"objects may be invisible.\n\n"
                   L"Details are in fci.log next to FarCryInstincts.exe. The game continues.";
        MessageBoxW(nullptr, text, L"Far Cry Instincts", MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
    }).detach();
}

// The HLSL compiler in use. A release ships none: Windows' own in System32, under Wine/Proton the builtin
// d3dcompiler_47 (vkd3d-shader, which lacks parts of HLSL: no isfinite, no EvaluateAttributeCentroid; the generated
// HLSL avoids them). A d3dcompiler_47.dll next to the exe (the developer build copies the Windows SDK's) is taken first
// (application directory) unless a DLL override says otherwise.
void LogShaderCompiler() {
    HMODULE module = GetModuleHandleW(L"d3dcompiler_47.dll");
    wchar_t path[MAX_PATH] = L"(not loaded)";
    if (module) GetModuleFileNameW(module, path, MAX_PATH);
    unsigned v[4] = {};
    DWORD handle = 0, size = module ? GetFileVersionInfoSizeW(path, &handle) : 0;
    if (size) {
        std::vector<uint8_t> info(size);
        VS_FIXEDFILEINFO* fixed = nullptr;
        UINT len = 0;
        if (GetFileVersionInfoW(path, 0, size, info.data()) &&
            VerQueryValueW(info.data(), L"\\", reinterpret_cast<void**>(&fixed), &len) && fixed)
            v[0] = HIWORD(fixed->dwFileVersionMS), v[1] = LOWORD(fixed->dwFileVersionMS),
            v[2] = HIWORD(fixed->dwFileVersionLS), v[3] = LOWORD(fixed->dwFileVersionLS);
    }
    using WineVersion = const char* (*)();
    auto wine = reinterpret_cast<WineVersion>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version"));
    rt::Log("render: HLSL compiler %ls (version %u.%u.%u.%u)%s%s", path, v[0], v[1], v[2], v[3], wine ? ", Wine " : "",
            wine ? wine() : "");
    // test of the safety net: FCI_TEST_SHADER_FAILURE=1 compiles a broken shader once
    char test[4] = {};
    if (GetEnvironmentVariableA("FCI_TEST_SHADER_FAILURE", test, sizeof test) && test[0] == '1') {
        static const char kBroken[] = "float4 main() : SV_Target { return no_such_function(); }";
        ComPtr<ID3DBlob> code, errors;
        if (FAILED(CompileHlsl(kBroken, sizeof kBroken - 1, "test", nullptr, nullptr, "main", "ps_5_0", 0, 0, &code,
                               &errors)))
            rt::Log("render: FCI_TEST_SHADER_FAILURE: %s",
                    errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
    }
}

bool Ready() {
    if (g_init_done) return !g_failed;
    g_init_done = true;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    char debug[8] = {};
    if (GetEnvironmentVariableA("FCI_D3D_DEBUG", debug, sizeof debug)) flags |= D3D11_CREATE_DEVICE_DEBUG;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, &level, 1, D3D11_SDK_VERSION,
                                   &g_device, nullptr, &g_context);
    if (FAILED(hr)) {
        rt::Log("render: D3D11CreateDevice failed %08X", unsigned(hr));
        g_failed = true;
        return false;
    }
    if (flags & D3D11_CREATE_DEVICE_DEBUG) g_device.As(&g_info);
    LogShaderCompiler();
    PrefetchShaderCache();  // performance: shaders off the render thread
    if (g_hle) ao::Prepare(g_device.Get());  // ambient occlusion shaders compile in the background
    g_clear_constants = MakeConstantBuffer(32);
    ComPtr<ID3DBlob> code, errors;
    if (SUCCEEDED(CompileHlsl(kClearShader, std::strlen(kClearShader), "clear", nullptr, nullptr, "vs", "vs_5_0", 0, 0,
                              &code, &errors)))
        g_device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_clear_vs);
    if (SUCCEEDED(CompileHlsl(kClearShader, std::strlen(kClearShader), "clear", nullptr, nullptr, "ps", "ps_5_0", 0, 0,
                              &code, &errors)))
        g_device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_clear_ps);
    rt::Log("render: Direct3D 11 device ready");
    return true;
}

const char* const kReinterpretShader = R"(
Texture2D<float4> src : register(t0);
cbuffer Alias : register(b3) { uint dst_pitch, src_pitch, src_width, src_height; int delta; };
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps(float4 pos : SV_Position) : SV_Target {
    uint2 p = uint2(pos.xy);
    int signed_offset = delta + int(p.y * dst_pitch + p.x * 4);  // bytes from src's start
    if (signed_offset < 0) discard;
    uint offset = uint(signed_offset);
    uint2 q = uint2((offset % src_pitch) / 4, offset / src_pitch);
    if (q.x >= src_width || q.y >= src_height) discard;
    return src.Load(int3(q, 0));
}
)";

void Reinterpret(Target& src, Target& dst) {
    static ComPtr<ID3D11VertexShader> vs;
    static ComPtr<ID3D11PixelShader> ps;
    static ComPtr<ID3D11Buffer> constants;
    if (!vs) {
        ComPtr<ID3DBlob> code, errors;
        if (SUCCEEDED(CompileHlsl(kReinterpretShader, std::strlen(kReinterpretShader), "alias", nullptr, nullptr, "vs",
                                  "vs_5_0", 0, 0, &code, &errors)))
            g_device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vs);
        if (SUCCEEDED(CompileHlsl(kReinterpretShader, std::strlen(kReinterpretShader), "alias", nullptr, nullptr, "ps",
                                  "ps_5_0", 0, 0, &code, &errors)))
            g_device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &ps);
        constants = MakeConstantBuffer(32);
    }
    if (!src.srv) g_device->CreateShaderResourceView(src.texture.Get(), nullptr, &src.srv);
    g_ds.Invalidate();  // performance: state cache
    uint32_t values[8] = {dst.pitch, src.pitch, src.width, src.height, dst.address - src.address};
    Fill(constants.Get(), values, sizeof values);
    ID3D11RenderTargetView* rtv = dst.rtv.Get();
    g_context->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp{0, 0, float(dst.width), float(dst.height), 0, 1};
    g_context->RSSetViewports(1, &vp);
    g_context->OMSetBlendState(GetBlendState(false, 1, 0, 0x8006, 0xF), nullptr, 0xFFFFFFFF);
    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
    ds.FrontFace.StencilFunc = ds.BackFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
    g_context->OMSetDepthStencilState(GetDepthState(ds), 0);
    D3D11_RASTERIZER_DESC rs{};
    rs.FillMode = D3D11_FILL_SOLID, rs.CullMode = D3D11_CULL_NONE;
    g_context->RSSetState(GetRasterState(rs));
    g_context->IASetInputLayout(nullptr);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->VSSetShader(vs.Get(), nullptr, 0);
    g_context->PSSetShader(ps.Get(), nullptr, 0);
    ID3D11Buffer* cb = constants.Get();
    g_context->PSSetConstantBuffers(3, 1, &cb);
    ID3D11ShaderResourceView* srv = src.srv.Get();
    g_context->PSSetShaderResources(0, 1, &srv);
    g_context->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    g_context->PSSetShaderResources(0, 1, &none);
    MarkWritten(dst);
}

void UnpackArgb(uint32_t v, float out[4]) {
    out[0] = ((v >> 16) & 0xFF) / 255.0f;
    out[1] = ((v >> 8) & 0xFF) / 255.0f;
    out[2] = (v & 0xFF) / 255.0f;
    out[3] = (v >> 24) / 255.0f;
}

// primitive -> D3D topology, with index lists for the shapes D3D 11 lacks
D3D11_PRIMITIVE_TOPOLOGY Topology(uint32_t primitive, uint32_t count, std::vector<uint32_t>& indices) {
    indices.clear();
    switch (primitive) {
    case kPoints: return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
    case kLines: return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
    case kLineStrip: return D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
    case kLineLoop:
        for (uint32_t i = 0; i < count; i++) indices.push_back(i);
        if (count) indices.push_back(0);
        return D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
    case kTriangles: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case kTriangleStrip: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    case kTriangleFan:
    case kPolygon:
        for (uint32_t i = 1; i + 1 < count; i++) indices.insert(indices.end(), {0, i, i + 1});
        return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case kQuads:
        for (uint32_t i = 0; i + 3 < count; i += 4) indices.insert(indices.end(), {i, i + 1, i + 2, i, i + 2, i + 3});
        return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case kQuadStrip:
        for (uint32_t i = 0; i + 3 < count; i += 2) indices.insert(indices.end(), {i, i + 1, i + 3, i, i + 3, i + 2});
        return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    default: return D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    }
}

int g_dumped = 0;  // draw_NNNN.bmp files written; the draw trace names the file of each draw

// FCI_DUMP_DRAWS=<seconds>: from then on, the render target after each of the next 400 draws, shrunk to
// 160x120 (FCI_DUMP_DRAWS_FULL=1: full size), as draw_NNNN.bmp
const ULONGLONG g_dump_from = [] {
    char value[16] = {};
    return GetEnvironmentVariableA("FCI_DUMP_DRAWS", value, sizeof value)
               ? GetTickCount64() + ULONGLONG(std::atof(value) * 1000)
               : ~0ull;
}();

void DumpDrawStep(Target* t) {
    const ULONGLONG from = g_dump_from;
    static int limit = [] {
        char value[16] = {};
        return GetEnvironmentVariableA("FCI_DUMP_DRAWS_COUNT", value, sizeof value) ? std::atoi(value) : 400;
    }();
    int& dumped = g_dumped;
    if (!t || dumped >= limit || GetTickCount64() < from) return;
    ToStaging(*t);
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(g_context->Map(t->staging.Get(), 0, D3D11_MAP_READ, 0, &map))) return;
    static const bool full = GetEnvironmentVariableA("FCI_DUMP_DRAWS_FULL", nullptr, 0) != 0;
    const int kW = full ? int(t->width) : 160, kH = full ? int(t->height) : 120;
    std::vector<uint32_t> shrunk(size_t(kW) * kH);
    for (int y = 0; y < kH; y++)
        for (int x = 0; x < kW; x++)
            shrunk[y * kW + x] = reinterpret_cast<const uint32_t*>(
                static_cast<const uint8_t*>(map.pData) + size_t(y * t->height / kH) * map.RowPitch)[x * t->width / kW];
    g_context->Unmap(t->staging.Get(), 0);
    // FCI_DUMP_DRAWS_PROBE=<fx>,<fy>[,<fx>,<fy>…] (fractions of the target): instead of pictures, the colour of
    // these pixels after each draw, logged when it changes (which draw wrote a pixel; FCI_TRACE_DRAWS names the
    // draws by the same number)
    static const std::vector<float> probe = [] {
        std::vector<float> v;
        char value[256] = {};
        if (GetEnvironmentVariableA("FCI_DUMP_DRAWS_PROBE", value, sizeof value))
            for (char* p = value; *p;) {
                v.push_back(float(std::strtod(p, &p)));
                while (*p == ',' || *p == ' ') p++;
            }
        return v;
    }();
    if (!probe.empty()) {
        static std::vector<uint32_t> last(probe.size() / 2, 0x12345678);
        D3D11_TEXTURE2D_DESC sd;
        t->staging->GetDesc(&sd);
        if (FAILED(g_context->Map(t->staging.Get(), 0, D3D11_MAP_READ, 0, &map))) return;
        for (size_t i = 0; i + 1 < probe.size(); i += 2) {
            uint32_t x = std::min(uint32_t(probe[i] * sd.Width), sd.Width - 1);
            uint32_t y = std::min(uint32_t(probe[i + 1] * sd.Height), sd.Height - 1);
            uint32_t c = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(map.pData) +
                                                          size_t(y) * map.RowPitch)[x];
            if (c != last[i / 2])
                rt::Log("probe: draw %d target %08X %ux%u pixel %u,%u = %08X", dumped, t->address, sd.Width, sd.Height,
                        x, y, c);
            last[i / 2] = c;
        }
        g_context->Unmap(t->staging.Get(), 0);
        dumped++;
        return;
    }
    char name[32];
    snprintf(name, sizeof name, "draw_%04d.bmp", dumped++);
    if (FILE* f = std::fopen(name, "wb")) {
        BITMAPFILEHEADER fh{};
        BITMAPINFOHEADER ih{};
        ih.biSize = sizeof ih, ih.biWidth = kW, ih.biHeight = -kH, ih.biPlanes = 1, ih.biBitCount = 32;
        fh.bfType = 0x4D42, fh.bfOffBits = sizeof fh + sizeof ih, fh.bfSize = fh.bfOffBits + kW * kH * 4;
        std::fwrite(&fh, sizeof fh, 1, f);
        std::fwrite(&ih, sizeof ih, 1, f);
        std::fwrite(shrunk.data(), 4, shrunk.size(), f);
        std::fclose(f);
    }
}

// ---- scaled and multisampled targets ----

const char* const kScaleShader = R"(
Texture2D src : register(t0);
cbuffer Scale : register(b3) { float scale_y; float scale_x; uint2 pad; };  // host pixels per guest pixel (y, x)
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
}
// a guest pixel as the average of its host pixels (fractional scales: the rows and columns its span starts in)
float4 shrink(float4 pos : SV_Position) : SV_Target {
    uint cols = uint(ceil(scale_x - 0.001)), rows = uint(ceil(scale_y - 0.001));
    int2 at = int2(int(floor(pos.x - 0.5) * scale_x + 0.001), int(floor(pos.y - 0.5) * scale_y + 0.001));
    float4 sum = 0;
    for (uint y = 0; y < rows; y++)
        for (uint x = 0; x < cols; x++) sum += src.Load(int3(at + int2(x, y), 0));
    return sum / (rows * cols);
}
// host pixels from guest ones
float4 grow(float4 pos : SV_Position) : SV_Target {
    return src.Load(int3(int(floor(pos.x) / scale_x), int(floor(pos.y) / scale_y), 0));
}
)";

struct ScalePass {
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> shrink, grow;
    ComPtr<ID3D11Buffer> constants;
};

ScalePass& Scaler() {
    static ScalePass p;
    if (!p.vs) {
        ComPtr<ID3DBlob> code, errors;
        if (SUCCEEDED(CompileHlsl(kScaleShader, std::strlen(kScaleShader), "scale", nullptr, nullptr, "vs", "vs_5_0", 0,
                                  0, &code, &errors)))
            g_device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &p.vs);
        if (SUCCEEDED(CompileHlsl(kScaleShader, std::strlen(kScaleShader), "scale", nullptr, nullptr, "shrink",
                                  "ps_5_0", 0, 0, &code, &errors)))
            g_device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &p.shrink);
        if (SUCCEEDED(CompileHlsl(kScaleShader, std::strlen(kScaleShader), "scale", nullptr, nullptr, "grow", "ps_5_0",
                                  0, 0, &code, &errors)))
            g_device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &p.grow);
        p.constants = MakeConstantBuffer(16);
    }
    return p;
}

void ScaleDraw(ID3D11RenderTargetView* rtv, uint32_t w, uint32_t h, ID3D11ShaderResourceView* src,
               ID3D11PixelShader* ps, float scale_y, float scale_x) {
    ScalePass& p = Scaler();
    g_ds.Invalidate();  // performance: state cache
    float values[4] = {scale_y, scale_x, 0, 0};
    Fill(p.constants.Get(), values, sizeof values);
    g_context->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
    g_context->RSSetViewports(1, &vp);
    g_context->OMSetBlendState(GetBlendState(false, 1, 0, 0x8006, 0xF), nullptr, 0xFFFFFFFF);
    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
    ds.FrontFace.StencilFunc = ds.BackFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
    g_context->OMSetDepthStencilState(GetDepthState(ds), 0);
    D3D11_RASTERIZER_DESC rs{};
    rs.FillMode = D3D11_FILL_SOLID, rs.CullMode = D3D11_CULL_NONE;
    g_context->RSSetState(GetRasterState(rs));
    g_context->IASetInputLayout(nullptr);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->VSSetShader(p.vs.Get(), nullptr, 0);
    g_context->PSSetShader(ps, nullptr, 0);
    ID3D11Buffer* cb = p.constants.Get();
    g_context->PSSetConstantBuffers(3, 1, &cb);
    g_context->PSSetShaderResources(0, 1, &src);
    g_context->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    g_context->PSSetShaderResources(0, 1, &none);
    g_context->OMSetRenderTargets(0, nullptr, nullptr);
}

// a single sampled view of the target's current contents at host size
ID3D11ShaderResourceView* Resolved(Target& t) {
    if (t.samples == 1) {
        if (!t.srv) g_device->CreateShaderResourceView(t.texture.Get(), nullptr, &t.srv);
        return t.srv.Get();
    }
    if (!t.resolved) {
        D3D11_TEXTURE2D_DESC desc{t.HostWidth(), t.HostHeight(), 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0},
                                  D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
        if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &t.resolved))) return nullptr;
        g_device->CreateShaderResourceView(t.resolved.Get(), nullptr, &t.resolved_srv);
    }
    if (t.resolved_version != t.version) {
        g_context->OMSetRenderTargets(0, nullptr, nullptr);
        // performance: FCI_GPU_TIMING (the PC water's copy of the scene counts as the water's)
        if (g_gpu_timing.enabled && g_gpu_timing.pass != Pass::kWater) g_gpu_timing.Begin(Pass::kResolve);
        g_context->ResolveSubresource(t.resolved.Get(), 0, t.texture.Get(), 0, DXGI_FORMAT_B8G8R8A8_UNORM);
        t.resolved_version = t.version;
        g_resolves++;
    }
    return t.resolved_srv.Get();
}

void ToStagingTexture(Target& t, ID3D11Texture2D* staging);

// the target at guest size into its staging texture
void ToStaging(Target& t) { ToStagingTexture(t, t.staging.Get()); }

void ToStagingTexture(Target& t, ID3D11Texture2D* staging) {
    if (t.Native()) {
        g_context->CopyResource(staging, t.texture.Get());
        return;
    }
    if (!t.shrunk) {
        D3D11_TEXTURE2D_DESC desc{t.width, t.height, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                  D3D11_BIND_RENDER_TARGET, 0, 0};
        if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &t.shrunk))) return;
        g_device->CreateRenderTargetView(t.shrunk.Get(), nullptr, &t.shrunk_rtv);
    }
    ID3D11ShaderResourceView* src = Resolved(t);
    g_context->OMSetRenderTargets(0, nullptr, nullptr);
    ScaleDraw(t.shrunk_rtv.Get(), t.width, t.height, src, Scaler().shrink.Get(), float(t.HostHeight()) / float(t.height),
              float(t.HostWidth()) / float(t.width));
    g_context->CopyResource(staging, t.shrunk.Get());
}

// a CPU read that does not wait for the GPU: the newest copy the GPU has finished goes to guest memory (a frame or
// two old, as on the console, whose GPU runs behind), and a copy of the current contents is started for later.
// The target stays the newer side: its state is left alone.
void LateSync(Target& t) {
    if (!t.late[0]) {
        D3D11_TEXTURE2D_DESC desc{t.width, t.height, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_STAGING, 0,
                                  D3D11_CPU_ACCESS_READ, 0};
        for (auto& slot : t.late)
            if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &slot))) return;
    }
    const uint32_t n = 3;
    while (t.late_pending) {  // every finished copy in order, the newest last
        uint32_t oldest = (t.late_next + n - t.late_pending) % n;
        D3D11_MAPPED_SUBRESOURCE map;
        if (g_context->Map(t.late[oldest].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &map) != S_OK) break;
        ToGuest(t, static_cast<const uint8_t*>(map.pData), map.RowPitch, Physical(t.address));
        g_context->Unmap(t.late[oldest].Get(), 0);
        t.late_pending--;
    }
    if (t.late_pending < n) {
        ToStagingTexture(t, t.late[t.late_next].Get());
        t.late_next = (t.late_next + 1) % n;
        t.late_pending++;
    }
}

// the upload texture (guest pixels) drawn into the target
void Grow(Target& t) {
    ScaleDraw(t.rtv.Get(), t.HostWidth(), t.HostHeight(), t.upload_srv.Get(), Scaler().grow.Get(),
              float(t.HostHeight()) / float(t.height), float(t.HostWidth()) / float(t.width));
}

// ---- ambient occlusion (gpu/ao.h) ----
// FCI draws a 3D view as: opaque W-buffered meshes into the frame buffer sized target (depth written, no blending),
// each followed by its additive light passes (ONE/ONE, no depth write); then the water's occlusion queries (no
// colour), the water and other alpha blended or additive 3D draws, then 2D (bloom, HUD) and other targets. The
// occlusion goes in once per view, before the first draw that is neither of the first kind nor a light pass: the
// first alpha blended or 2D draw into that target, another target, another view (split screen) or a clear of it.
struct AoView {
    Target* color = nullptr;
    DepthTarget* depth = nullptr;
    D3D11_RECT rect{};  // the view, host pixels
    bool pending = false;
    std::vector<D3D11_RECT> done;  // views that got their occlusion this frame
    float fog[3] = {};             // the latest fog of a 3D draw at stored depth d: saturate(fog[0] * d + fog[1])
};
AoView g_ao;

bool SameRect(const D3D11_RECT& a, const D3D11_RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

// the depth as a shader resource, multisampled or not (AO, PC water); null when the target was made without the bind flag
ID3D11ShaderResourceView* DepthView(DepthTarget& d) {
    if (!d.ao_srv) {
        D3D11_TEXTURE2D_DESC td;
        d.texture->GetDesc(&td);
        if (!(td.BindFlags & D3D11_BIND_SHADER_RESOURCE)) return nullptr;
        D3D11_SHADER_RESOURCE_VIEW_DESC srv{d.stencil ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT_R32_FLOAT,
                                            D3D11_SRV_DIMENSION_TEXTURE2D};
        if (d.samples > 1) srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
        srv.Texture2D.MipLevels = 1;
        if (FAILED(g_device->CreateShaderResourceView(d.texture.Get(), &srv, &d.ao_srv))) return nullptr;
    }
    return d.ao_srv.Get();
}

void AoFlush() {
    g_ds.Invalidate();  // performance: state cache
    if (!g_ao.pending) return;
    g_ao.pending = false;
    if (g_gpu_timing.enabled) g_gpu_timing.Begin(Pass::kAo);  // performance: FCI_GPU_TIMING
    g_ao.done.push_back(g_ao.rect);
    Target& t = *g_ao.color;
    DepthTarget& d = *g_ao.depth;
    if (!DepthView(d)) return;
    ao::Pass p;
    p.depth = d.ao_srv.Get(), p.samples = d.samples;
    p.color = t.rtv.Get(), p.width = t.HostWidth(), p.height = t.HostHeight();
    p.left = g_ao.rect.left, p.top = g_ao.rect.top, p.right = g_ao.rect.right, p.bottom = g_ao.rect.bottom;
    std::memcpy(p.fog, g_ao.fog, sizeof p.fog);
    p.reversed = g_reversed_depth && !d.z16 ? psh::kReversedDepth : 0.0f;  // performance: reversed depth
    ao::Apply(g_device.Get(), g_context.Get(), p);
    MarkWritten(t);
}

// before a draw into surf; true when the occlusion went in and surf's targets must be bound again
bool AoBeforeDraw(const State& s, const Surfaces& surf, bool depth_write, UINT8 mask) {
    if (!g_hle || !ao::Mode()) return false;
    const bool w_buffer = s.Reg(SET_CONTROL0) & kControl0WBuffer;
    const bool blend = s.Reg(SET_BLEND_ENABLE) != 0;
    const bool program = (s.Reg(SET_TRANSFORM_EXECUTION_MODE) & 3) == 2;
    const bool screen = surf.color && surf.depth && !surf.depth->z16 && !surf.color->swizzled &&
                        surf.color->width >= 640 && surf.depth->samples == surf.color->samples;
    const bool opaque = screen && w_buffer && program && mask && !blend && depth_write && s.Reg(SET_DEPTH_TEST_ENABLE);
    D3D11_RECT rect{};
    if (opaque && s.Reg(SET_FOG_ENABLE) && s.RegF(SET_FOG_PARAMS) == 1.0f && s.RegF(SET_FOG_PARAMS + 4) == 1.0f) {
        // FCI's vertex shaders write the fog visibility themselves: min(w, c184.z) * c184.x + c184.y (linear fog,
        // the fog factor passes through), so the fog amount at stored depth d (w / zmax) is
        // saturate(-c184.x * zmax * d + 1 - c184.y)
        const float* f = s.constants[184];
        if (f[0] < 0) g_ao.fog[0] = -f[0] * 16777215.0f, g_ao.fog[1] = 1.0f - f[1], g_ao.fog[2] = 1.0f;
    }
    if (opaque) {
        // the viewport from the XDK's screen transform: c58 scale (w / 2, -h / 2), c59 offset (centre + 0.53125)
        const float sx = float(surf.scale) * surf.wide, sy = float(surf.scale);
        const float* scale = s.constants[58];
        const float* offset = s.constants[59];
        float x0 = offset[0] - 0.53125f - scale[0], y0 = offset[1] - 0.53125f + scale[1];
        float x1 = x0 + 2 * scale[0], y1 = y0 - 2 * scale[1];
        rect.left = std::max<LONG>(LONG(std::lround(x0 * sx)), surf.clip.left);
        rect.top = std::max<LONG>(LONG(std::lround(y0 * sy)), surf.clip.top);
        rect.right = std::min<LONG>(LONG(std::lround(x1 * sx)), surf.clip.right);
        rect.bottom = std::min<LONG>(LONG(std::lround(y1 * sy)), surf.clip.bottom);
    }
    bool flushed = false;
    if (g_ao.pending) {
        const bool same = surf.color == g_ao.color && surf.depth == g_ao.depth;
        const bool light = w_buffer && blend && s.Reg(SET_BLEND_FUNC_SFACTOR) == 1 &&
                           s.Reg(SET_BLEND_FUNC_DFACTOR) == 1 && !depth_write;
        const bool query = mask == 0 && !depth_write;  // occlusion tests draw nothing
        const bool overdraw = w_buffer && program && !blend;  // second unblended passes over opaque meshes
        bool keep = same && (opaque ? SameRect(rect, g_ao.rect) : (light || query || overdraw));
        if (!keep) {
            AoFlush();
            flushed = true;
        }
    }
    if (opaque && !g_ao.pending && rect.right > rect.left && rect.bottom > rect.top) {
        bool done = false;
        for (const D3D11_RECT& r : g_ao.done) done |= SameRect(r, rect);
        if (!done) {
            g_ao.pending = true;
            g_ao.color = surf.color, g_ao.depth = surf.depth, g_ao.rect = rect;
        }
    }
    return flushed;
}

// a clear: the pending view's occlusion goes in first if it touches its targets
bool AoBeforeClear(const Surfaces& surf) {
    if (!g_ao.pending || (surf.color != g_ao.color && surf.depth != g_ao.depth)) return false;
    AoFlush();
    return true;
}

void AoPresent() {
    g_ao.pending = false;  // never reached its end before the frame was shown: no place left for it
    g_ao.done.clear();
}

void Rebind(const Surfaces& surf) {
    ID3D11RenderTargetView* rtv = surf.color ? surf.color->rtv.Get() : nullptr;
    g_context->OMSetRenderTargets(rtv ? 1 : 0, rtv ? &rtv : nullptr, surf.depth ? surf.depth->dsv.Get() : nullptr);
    const float w = float(HostWidthOf(surf.width, surf.scale, surf.wide)), h = float(HostHeightOf(surf.height, surf.scale));
    D3D11_VIEWPORT vp{0, 0, w, h, 0, 1};
    g_context->RSSetViewports(1, &vp);
}
// ---- end ambient occlusion ----

// ---- PC water (gpu/water.h) ----
// The ocean (FCI's projected grid, vertex program "OceanOver"): a triangle strip with position and colour, W-buffered,
// blended SRC_ALPHA / ONE_MINUS_SRC_ALPHA without depth writes, stage 0 the du/dv wave texture (G8B8, projective 2D at
// world xy x c104), stage 1 the reflection target (bump environment mapped by stage 0), stage 2 the frame's depth buffer
// as a projective depth texture (r/q = zmax: whether anything was drawn behind; our HLE reads guest memory there, as
// before). With water_quality medium/high its pixel shader is ours (gpu/water.cpp), with the world position from the
// vertex program, the scene's depth (a read-only depth view, the water writes none) and for high a resolved copy of the
// scene. Under water the game draws the surface with another program (OceanUnder), which stays the game's.
struct WaterConstants {
    float eye[4];
    float refl_s[4], refl_t[4], refl_q[4];
    float screen_px[4];
    float view_uv[4];
    float depth_c[4];
    float tune[4];
    float tune2[4];
    float sun[4];
};

// the sun's sprite in the 3D view (the game draws it right after the sky: a pre-transformed additive quad at z 0.99,
// also when the sun is behind the camera): its centre in guest screen pixels and its rhw, for the water's sun highlight
struct SunSprite {
    float x = 0, y = 0, rhw = 0;
    uint64_t present = ~0ull;  // g_water_presents when seen
};
SunSprite g_sun;

// presents so far, and the one at which our shader last drew the ocean: the ground under water keeps the game's tint
// where no ocean of ours lies over it (WaterGround)
uint64_t g_water_presents = 0, g_water_drawn = ~0ull;

struct WaterPass {
    bool active = false;
    ID3D11ShaderResourceView* srvs[3] = {};  // t4 scene depth, t5 scene colour (high), t6 the wave map
    ID3D11DepthStencilView* dsv = nullptr;  // read-only
};

bool IsOcean(const State& s, const Draw& d, const Surfaces& surf) {
    if (!surf.color || !surf.depth || surf.depth->z16 || surf.color->swizzled || surf.color->width < 640) return false;
    if (d.enabled != 0x0009 || surf.depth->samples != surf.color->samples) return false;
    if (!(s.Reg(SET_CONTROL0) & kControl0WBuffer) || (s.Reg(SET_TRANSFORM_EXECUTION_MODE) & 3) != 2) return false;
    if (!s.Reg(SET_BLEND_ENABLE) || s.Reg(SET_BLEND_FUNC_SFACTOR) != 0x302 || s.Reg(SET_BLEND_FUNC_DFACTOR) != 0x303)
        return false;
    if (s.Reg(SET_DEPTH_MASK) || !s.Reg(SET_DEPTH_TEST_ENABLE)) return false;
    if ((s.Reg(SET_SHADER_STAGE_PROGRAM) & 0x7FFF) != 0x08C1) return false;  // projective 2D, bump env, projective 3D
    return ((s.Reg(SET_TEXTURE_FORMAT) >> 8) & 0xFF) == 0x28;               // the du/dv wave texture
}

ID3D11PixelShader* GetWaterPixelShader(int quality, uint32_t samples, uint32_t fog_mode) {
    static std::map<uint64_t, PixelShader> shaders;
    const uint64_t key = uint64_t(quality) << 40 | uint64_t(samples) << 32 | fog_mode;
    PixelShader& ps = shaders[key];
    if (!ps.shader && !ps.job) {
        auto job = std::make_shared<ShaderJob>();
        job->source = water::PixelShader(quality, samples, fog_mode);
        job->hash = SourceHash(job->source, "ps_5_0");
        if (g_dump_shaders || !FromPrefetch(*job)) Workers().Add(job);
        ps.job = std::move(job);
    }
    if (ps.job) {
        if (!ps.job->done) return nullptr;  // the game's water until ours is ready (no waiting, nothing left out)
        ps.shader = std::move(ps.job->ps);
        ps.job.reset();
    }
    return ps.shader.Get();
}

// tuning (diagnostics): FCI_WATER_TUNE=<wave strength>,<reflection bend>,<refraction bend>,<shore m>,<opacity per m>,
// <foam>,<sun>,<reflection scale>
const std::array<float, 8>& WaterTune() {
    static const std::array<float, 8> tune = [] {
        std::array<float, 8> t = {1.0f, 0.04f, 0.6f, 0.2f, 0.3f, 0.35f, 1.0f, 1.0f};
        char v[256] = {};
        if (GetEnvironmentVariableA("FCI_WATER_TUNE", v, sizeof v)) {
            char* p = v;
            for (float& f : t) {
                char* e = nullptr;
                double x = std::strtod(p, &e);
                if (e == p) break;
                f = float(x);
                p = *e == ',' ? e + 1 : e;
            }
        }
        return t;
    }();
    return tune;
}

// the camera: the point the view projection (x, y and w rows) sends to x = y = w = 0
bool CameraPosition(const float* x, const float* y, const float* w, float out[3]) {
    const double* r[3];
    double rows[3][4];
    for (int k = 0; k < 4; k++) rows[0][k] = x[k], rows[1][k] = y[k], rows[2][k] = w[k];
    for (int i = 0; i < 3; i++) r[i] = rows[i];
    double e[4];
    for (int c = 0; c < 4; c++) {
        int col[3], n = 0;
        for (int k = 0; k < 4; k++)
            if (k != c) col[n++] = k;
        double det = r[0][col[0]] * (r[1][col[1]] * r[2][col[2]] - r[1][col[2]] * r[2][col[1]]) -
                     r[0][col[1]] * (r[1][col[0]] * r[2][col[2]] - r[1][col[2]] * r[2][col[0]]) +
                     r[0][col[2]] * (r[1][col[0]] * r[2][col[1]] - r[1][col[1]] * r[2][col[0]]);
        e[c] = (c & 1) ? -det : det;
    }
    if (std::fabs(e[3]) < 1e-30) return false;
    for (int k = 0; k < 3; k++) out[k] = float(e[k] / e[3]);
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

// makes the draw the PC water's: its shaders, constants and views (bound by WaterBind); false: the game's as before
bool PrepareWater(const State& s, const Draw& d, const Surfaces& surf, WaterPass& w, VertexShader*& vs,
                  ID3D11PixelShader*& ps) {
    const int quality = water::Current();
    DepthTarget& depth = *surf.depth;
    if (!g_reversed_depth) return false;  // (the shader writes no depth; FCI_W_DEPTH=shader keeps the game's water)
    ID3D11ShaderResourceView* depth_srv = DepthView(depth);
    if (!depth_srv) return false;  // made without the bind flag (water and AO off at start): needs a restart
    float eye[3];
    if (!CameraPosition(s.constants[167], s.constants[168], s.constants[170], eye)) return false;
    // under the surface the game draws OceanUnder; should this one be seen from below, it stays the game's
    if (eye[2] < s.constants[173][3]) return false;
    // the wave map (made on a thread at start): the game's water until it is there
    water::Prepare();
    static ComPtr<ID3D11ShaderResourceView> waves;
    if (!waves) {
        const water::WaveMap* map = water::Waves();
        if (!map) return false;
        D3D11_TEXTURE2D_DESC td{map->size, map->size, UINT(map->levels.size()), 1, DXGI_FORMAT_R16G16_SNORM, {1, 0},
                                D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0};
        std::vector<D3D11_SUBRESOURCE_DATA> data;
        for (size_t i = 0; i < map->levels.size(); i++) {
            const UINT side = std::max(map->size >> i, 1u);
            data.push_back({map->levels[i].data(), side * 4, side * side * 4});
        }
        ComPtr<ID3D11Texture2D> tex;
        if (FAILED(g_device->CreateTexture2D(&td, data.data(), &tex))) return false;
        g_device->CreateShaderResourceView(tex.Get(), nullptr, &waves);
        if (!waves) return false;
    }
    const uint32_t fog_mode = s.Reg(SET_FOG_ENABLE) ? s.Reg(SET_FOG_MODE) : 0;
    ID3D11PixelShader* wps = GetWaterPixelShader(quality, surf.samples, fog_mode);
    VertexShader* wvs = wps ? GetVertexShader(s, d.enabled, d.raw ? d.format : nullptr, true) : nullptr;
    if (!wps || !wvs || !wvs->layout) return false;
    if (!depth.dsv_read) {
        D3D11_DEPTH_STENCIL_VIEW_DESC desc;
        depth.dsv->GetDesc(&desc);
        desc.Flags = D3D11_DSV_READ_ONLY_DEPTH | (depth.stencil ? D3D11_DSV_READ_ONLY_STENCIL : 0);
        if (FAILED(g_device->CreateDepthStencilView(depth.texture.Get(), &desc, &depth.dsv_read))) return false;
    }
    if (g_gpu_timing.enabled) g_gpu_timing.Begin(Pass::kWater);  // performance: FCI_GPU_TIMING
    ID3D11ShaderResourceView* scene = nullptr;
    if (quality >= water::kHigh) {
        Target& t = *surf.color;
        if (t.samples > 1) {
            scene = Resolved(t);  // (unbinds the targets when it resolves)
        } else {
            // without MSAA Resolved is the target itself, which the draw writes: a copy (the snapshot, as for textures)
            if (!t.snapshot) {
                D3D11_TEXTURE2D_DESC desc;
                t.texture->GetDesc(&desc);
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &t.snapshot))) return false;
                g_device->CreateShaderResourceView(t.snapshot.Get(), nullptr, &t.snapshot_srv);
            }
            if (t.snapshot_version != t.version) {
                g_context->CopyResource(t.snapshot.Get(), t.texture.Get());
                t.snapshot_version = t.version;
            }
            scene = t.snapshot_srv.Get();
        }
        if (!scene) return false;
        Rebind(surf);
    }
    WaterConstants c{};
    c.eye[0] = eye[0], c.eye[1] = eye[1], c.eye[2] = eye[2];
    {
        // seconds since start for the waves' scroll (wrapping after 4000 s: whole periods of every layer's scroll)
        static LARGE_INTEGER t0{}, freq{};
        if (!t0.QuadPart) QueryPerformanceCounter(&t0), QueryPerformanceFrequency(&freq);
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        c.eye[3] = float(std::fmod(double(now.QuadPart - t0.QuadPart) / double(freq.QuadPart), 4000.0));
    }
    // the sun's direction: its sprite's screen point back through this view's projection (x, y, w rows: a direction)
    if (g_sun.present != ~0ull && g_water_presents - g_sun.present <= 1 && g_sun.rhw != 0) {
        const float* sc = s.constants[58];
        const float* of = s.constants[59];
        const double w = 1.0 / g_sun.rhw, nx = (g_sun.x - of[0]) / sc[0] * w, ny = (g_sun.y - of[1]) / sc[1] * w;
        const float* rx = s.constants[167];
        const float* ry = s.constants[168];
        const float* rw = s.constants[170];
        auto det = [](double a, double b, double c, double d, double e, double f, double g, double h, double i) {
            return a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
        };
        const double D = det(rx[0], rx[1], rx[2], ry[0], ry[1], ry[2], rw[0], rw[1], rw[2]);
        if (std::fabs(D) > 1e-30) {
            double v[3] = {det(nx, rx[1], rx[2], ny, ry[1], ry[2], w, rw[1], rw[2]) / D,
                           det(rx[0], nx, rx[2], ry[0], ny, ry[2], rw[0], w, rw[2]) / D,
                           det(rx[0], rx[1], nx, ry[0], ry[1], ny, rw[0], rw[1], w) / D};
            const double len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (len > 0 && std::isfinite(len)) {
                for (int k = 0; k < 3; k++) c.sun[k] = float(v[k] / len);
                c.sun[3] = 1;
            }
        }
        static int logged = 0;
        if (logged < 3 && c.sun[3] != 0) {
            logged++;
            rt::Log("water: sun direction %.3f %.3f %.3f (sprite at %.1f, %.1f, rhw %g)", c.sun[0], c.sun[1], c.sun[2],
                    g_sun.x, g_sun.y, g_sun.rhw);
        }
    }
    std::memcpy(c.refl_s, s.constants[100], 16);
    std::memcpy(c.refl_t, s.constants[101], 16);
    std::memcpy(c.refl_q, s.constants[103], 16);
    const float host_w = float(surf.color->HostWidth()), host_h = float(surf.color->HostHeight());
    c.screen_px[0] = 1.0f / host_w, c.screen_px[1] = 1.0f / host_h;
    {
        // the view (split screen) from the XDK's screen transform, as for the ambient occlusion
        const float sx = float(surf.scale) * surf.wide, sy = float(surf.scale);
        const float* scale = s.constants[58];
        const float* offset = s.constants[59];
        float x0 = offset[0] - 0.53125f - scale[0], y0 = offset[1] - 0.53125f + scale[1];
        float x1 = x0 + 2 * scale[0], y1 = y0 - 2 * scale[1];
        c.view_uv[0] = std::clamp(x0 * sx / host_w, 0.0f, 1.0f), c.view_uv[1] = std::clamp(y0 * sy / host_h, 0.0f, 1.0f);
        c.view_uv[2] = std::clamp(x1 * sx / host_w, 0.0f, 1.0f), c.view_uv[3] = std::clamp(y1 * sy / host_h, 0.0f, 1.0f);
    }
    c.depth_c[0] = psh::kReversedDepth, c.depth_c[1] = float(surf.samples);
    const std::array<float, 8>& t = WaterTune();
    std::memcpy(c.tune, t.data(), 16);
    std::memcpy(c.tune2, t.data() + 4, 16);
    static Constants<WaterConstants> constants;
    ID3D11Buffer* cb = constants.Update(c);
    g_context->PSSetConstantBuffers(4, 1, &cb);
    static ComPtr<ID3D11SamplerState> clamp;
    if (!clamp) {
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        g_device->CreateSamplerState(&sd, &clamp);
    }
    static ComPtr<ID3D11SamplerState> wrap;
    if (!wrap) {
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_ANISOTROPIC;
        sd.MaxAnisotropy = std::max(g_q.aniso, 1u);
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        g_device->CreateSamplerState(&sd, &wrap);
    }
    ID3D11SamplerState* smp[2] = {clamp.Get(), wrap.Get()};
    g_context->PSSetSamplers(4, 2, smp);
    w.active = true;
    g_water_drawn = g_water_presents;
    w.srvs[0] = depth_srv, w.srvs[1] = scene, w.srvs[2] = waves.Get();
    w.dsv = depth.dsv_read.Get();
    vs = wvs, ps = wps;
    static bool logged = false;
    if (!logged) {
        logged = true;
        rt::Log("water: the ocean with our %s shader (eye %.1f %.1f %.1f, water level %.2f, %u samples)",
                quality >= water::kHigh ? "high" : "medium", eye[0], eye[1], eye[2], s.constants[173][3], surf.samples);
    }
    return true;
}

// The game's terrain colour pass: after the terrain's lighting it draws the terrain again (no blending, no depth
// writes, LEQUAL, the frame buffer itself as stage 3: textures x lighting) and fogs it, above the water line (d1.b =
// (z - c187.w) * c185.w + c185.z >= 0.5) towards the fog colour (factor0[7]) by the distance fog (d1.g, c184), below it
// towards a turquoise (factor1[7]) by a much denser fog (d1.a, c153): the underwater look of the ground, which the
// console's nearly opaque water hardly shows. Our water shows the ground and colours it by the depth itself; with the
// turquoise fog on top shallow water was a flat turquoise with a bright rim at every shore. While the camera is above
// the water (its own view projection, c167/c168/c170, against c187.w) the ground under water gets the fog above water
// (factor1[7] = factor0[7], c153 = c184), as long as our shader draws the ocean; under water (diving) it stays the
// game's.
bool WaterGround(const State& s) {
    // only while our shader draws the ocean (it comes after the ground: this frame or the one before)
    if (g_water_drawn == ~0ull || g_water_presents - g_water_drawn > 1) return false;
    if (!(s.Reg(SET_CONTROL0) & kControl0WBuffer) || (s.Reg(SET_TRANSFORM_EXECUTION_MODE) & 3) != 2) return false;
    if (s.Reg(SET_BLEND_ENABLE) || s.Reg(SET_DEPTH_MASK) || !s.Reg(SET_FOG_ENABLE)) return false;
    // stage 3: the colour target being drawn (a linear A8R8G8B8 texture on its memory), projective; the other stages
    // (detail textures) vary with the sector
    const uint32_t stage3 = 0x40 * 3, program3 = (s.Reg(SET_SHADER_STAGE_PROGRAM) >> 15) & 0x1F;
    if (program3 != 1 && program3 != 2) return false;
    if (((s.Reg(SET_TEXTURE_FORMAT + stage3) >> 8) & 0xFF) != 0x12) return false;
    const uint32_t tex = ResolveDma(s.Reg((s.Reg(SET_TEXTURE_FORMAT + stage3) & 3) == 2 ? SET_CONTEXT_DMA_B : SET_CONTEXT_DMA_A)) +
                         s.Reg(SET_TEXTURE_OFFSET + stage3);
    const uint32_t color = ResolveDma(s.Reg(SET_CONTEXT_DMA_COLOR)) + s.Reg(SET_SURFACE_COLOR_OFFSET);
    if (tex != color) return false;
    // the eight combiner stages switching on r0.a (the height over the water) between two colours
    const uint32_t control = s.Reg(SET_COMBINER_CONTROL);
    if ((control & 0xFF) != 8 || !(control & 0x100)) return false;
    float eye[3];
    if (!CameraPosition(s.constants[167], s.constants[168], s.constants[170], eye)) return false;
    const bool above = eye[2] > s.constants[187][3];
    static bool logged = false;
    if (!logged && above) {
        logged = true;
        rt::Log("water: the ground under water fogged as above it (eye %.1f %.1f %.1f, water level %.2f)", eye[0], eye[1],
                eye[2], s.constants[187][3]);
    }
    return above;
}

// the sun's sprite: an additive quad of four pre-transformed vertices (x, y in guest pixels, z 0.99, rhw), no depth test
void WaterSun(const State& s, const Draw& d) {
    if (d.primitive != kTriangleFan || d.count != 4 || !(d.enabled & 1) || s.Reg(SET_DEPTH_TEST_ENABLE)) return;
    if (!s.Reg(SET_BLEND_ENABLE) || s.Reg(SET_BLEND_FUNC_SFACTOR) != 1 || s.Reg(SET_BLEND_FUNC_DFACTOR) != 1) return;
    if ((s.Reg(SET_SURFACE_CLIP_HORIZONTAL) >> 16) < 640) return;  // the screen's, not the reflection target's
    const std::vector<float>& v = kelvin::Vertices(d);
    if (v.size() < size_t(d.stride) * 4 || d.stride < 4) return;
    float x = 0, y = 0, rhw = 0;
    for (int k = 0; k < 4; k++) {
        const float* p = &v[size_t(k) * d.stride];
        if (std::fabs(p[2] - 0.99f) > 1e-4f || p[3] == 1.0f || p[3] == 0.0f) return;
        x += p[0] * 0.25f, y += p[1] * 0.25f, rhw += p[3] * 0.25f;
    }
    g_sun.x = x, g_sun.y = y, g_sun.rhw = rhw, g_sun.present = g_water_presents;
}

// just before the draw: the read-only depth with the depth and the scene as textures
void WaterBind(const WaterPass& w, const Surfaces& surf) {
    ID3D11RenderTargetView* rtv = surf.color->rtv.Get();
    g_context->OMSetRenderTargets(1, &rtv, w.dsv);
    g_context->PSSetShaderResources(4, 3, w.srvs);
}

void WaterUnbind(const Surfaces& surf) {
    ID3D11ShaderResourceView* none[3] = {};
    g_context->PSSetShaderResources(4, 3, none);
    ID3D11RenderTargetView* rtv = surf.color->rtv.Get();
    g_context->OMSetRenderTargets(1, &rtv, surf.depth->dsv.Get());
}
// ---- end PC water ----

bool StageSamples(uint32_t program) {
    return program == 1 || program == 2 || program == 3 || program == 6 || program == 7 || program == 15 ||
           program == 16;
}

}  // namespace

// performance: the draw's shaders, constant buffers and samplers through the state cache (the textures always: binding
// a render target unbinds a view of the same resource behind our back)
void BindPipeline(VertexShader* vs, D3D11_PRIMITIVE_TOPOLOGY topology, ID3D11PixelShader* ps, ID3D11Buffer* vcb,
                  ID3D11Buffer* pcb, ID3D11ShaderResourceView* const* srvs, ID3D11SamplerState* const* samplers) {
    const bool all = !g_ds.valid;
    if (all || vs->layout.Get() != g_ds.layout) g_context->IASetInputLayout(g_ds.layout = vs->layout.Get());
    if (all || topology != g_ds.topology) g_context->IASetPrimitiveTopology(g_ds.topology = topology);
    if (all || vs->shader.Get() != g_ds.vs) g_context->VSSetShader(g_ds.vs = vs->shader.Get(), nullptr, 0);
    if (all || ps != g_ds.ps) g_context->PSSetShader(g_ds.ps = ps, nullptr, 0);
    if (all || vcb != g_ds.vcb) g_context->VSSetConstantBuffers(0, 1, &vcb), g_ds.vcb = vcb;
    if (all || pcb != g_ds.pcb) g_context->PSSetConstantBuffers(1, 1, &pcb), g_ds.pcb = pcb;
    g_context->PSSetShaderResources(0, 4, srvs);
    if (all || std::memcmp(samplers, g_ds.samplers, sizeof g_ds.samplers)) {
        g_context->PSSetSamplers(0, 4, samplers);
        std::memcpy(g_ds.samplers, samplers, sizeof g_ds.samplers);
    }
    g_ds.valid = true;
}

void DrawOne(const State& s, const Draw& d) {  // (DrawPrimitives, below, adds fmv_scale's second pass)
    if (!Ready() || !d.count || !d.stride) return;
    std::vector<uint32_t> indices;
    D3D11_PRIMITIVE_TOPOLOGY topology = Topology(d.primitive, d.count, indices);
    if (topology == D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED) return;

    bool depth_test = s.Reg(SET_DEPTH_TEST_ENABLE), stencil_test = s.Reg(SET_STENCIL_TEST_ENABLE);
    if (stencil_test && !g_depth_stencil && g_hle) {
        // performance: the game uses stencil after all: 24 bit depth targets again with a stencil plane (made anew,
        // their depth is lost once)
        g_depth_stencil = true;
        g_ao.pending = false;
        std::erase_if(g_depths, [](const std::unique_ptr<DepthTarget>& t) { return !t->z16; });
        rt::Log("render: the game uses the stencil test: depth targets with stencil from now on");
    }
    const bool water_ground = g_hle && water::Current() && WaterGround(s);  // PC water
    if (g_hle && water::Current()) WaterSun(s, d);  // PC water

    // textures first: reading one may write a render target back
    psh::Config config;
    ID3D11ShaderResourceView* srvs[4] = {};
    ID3D11SamplerState* samplers[4] = {};
    PixelConstants pc{};
    uint32_t programs = s.Reg(SET_SHADER_STAGE_PROGRAM);
    for (int i = 0; i < 4; i++) {
        pc.tex_scale[i][0] = pc.tex_scale[i][1] = 1.0f;
        if (!StageSamples((programs >> (5 * i)) & 0x1F)) continue;
        psh::Sampler kind = psh::Sampler::k2D;
        srvs[i] = GetTexture(s, i, kind, pc.tex_scale[i]);
        config.sampler[i] = kind;
        samplers[i] = GetSampler(s, i);
    }
    // (with color writes off, as in the shadow map passes over a dummy color target at physical 0 and the
    // visibility tests, the color target is bound but not refreshed from guest memory: that was a 4 MB upload and
    // hash every frame, as the CPU's data at physical 0 changes all the time)
    Surfaces surf = BindSurfaces(s, true, depth_test || stencil_test, WriteMask(s.Reg(SET_COLOR_MASK)) != 0);
    if (!surf.color && !surf.depth) return;
    // ---- ambient occlusion ----
    if (AoBeforeDraw(s, surf, surf.depth && depth_test && s.Reg(SET_DEPTH_MASK),
                     surf.color ? WriteMask(s.Reg(SET_COLOR_MASK)) : UINT8(0)))
        Rebind(surf);
    // ---- end ambient occlusion ----

    // ---- performance: reversed depth ----
    const bool w_buffer = s.Reg(SET_CONTROL0) & kControl0WBuffer;
    const bool reversed = g_reversed_depth && surf.depth && !surf.depth->z16;
    const bool offset = s.Reg(SET_POLY_OFFSET_FILL_ENABLE) != 0;
    if (!reversed) config.depth = w_buffer ? psh::Depth::kW : psh::Depth::kNone;
    else if (w_buffer) config.depth = offset ? psh::Depth::kReversedW : psh::Depth::kNone;
    else config.depth = (depth_test || s.Reg(SET_DEPTH_MASK)) ? psh::Depth::kReversedZ : psh::Depth::kNone;
    // ---- end performance ----
    if (g_gpu_timing.enabled && w_buffer && surf.color && (surf.color->samples > 1 || surf.color->wide != 1.0f))
        g_gpu_timing.Begin(s.Reg(SET_BLEND_ENABLE) ? Pass::kScreenBlend : Pass::kScreen3D);  // performance: FCI_GPU_TIMING
    else if (g_gpu_timing.enabled && surf.color && (surf.color->samples > 1 || surf.color->wide != 1.0f))
        g_gpu_timing.Begin(Pass::kScreen);
    VertexShader* vs = GetVertexShader(s, d.enabled, d.raw ? d.format : nullptr);
    ID3D11PixelShader* ps = GetPixelShader(s, config);
    if (!vs || !vs->layout || !ps) return;
    // ---- PC water ----
    WaterPass wpass;
    if (g_hle && water::Current() && IsOcean(s, d, surf)) PrepareWater(s, d, surf, wpass, vs, ps);
    // ---- end PC water ----

    // constants
    static VertexConstants vc;
    std::memcpy(vc.c, s.constants, sizeof vc.c);
    if (water_ground) std::memcpy(vc.c[153], vc.c[184], sizeof vc.c[153]);  // PC water: the fog above the water
    std::memcpy(vc.defaults, s.attribute_value, sizeof vc.defaults);
    vc.screen[0] = 2.0f / surf.width, vc.screen[1] = 2.0f / surf.height, vc.screen[2] = 1.0f / surf.zmax;
    vc.screen[3] = w_buffer ? (reversed ? 2.0f : 1.0f) : 0.0f;
    for (int i = 0; i < 16; i++) vc.composite[i / 4][i % 4] = s.RegF(SET_COMPOSITE_MATRIX + 4 * i);
    // ---- PC features: widescreen ----
    // Over a widened 3D view, 2D draws (not W-buffered; the shader moves only vertices with w = 1, which
    // pre-transformed screen space vertices have) keep the console's 4:3 shape in the middle of the screen. What
    // spans the whole width stays (full screen effects: bloom, tints, letterbox bars). 3D draws are left alone:
    // the game's own projection is wider (xdk/game.cpp).
    // hud: x' = x * hud[0] + hud[1], y' = y * hud[2] + hud[3] for vertices with w = 1 (identity: as drawn)
    vc.hud[0] = 1.0f, vc.hud[1] = 0.0f, vc.hud[2] = 1.0f, vc.hud[3] = 0.0f;
    bool narrowed = false;
    if (g_frame_3d && surf.color && surf.color->wide == 1.0f) g_post_3d = true;
    // fmv_scale: frames of a video (a video texture drawn, or the overlay showing one) are shown wide too
    const bool video_frame = fmv::Fill() && (g_frame_video || g_last_video || gpu::OverlayActive());
    if (surf.color && surf.color->wide != 1.0f) {
        if (vc.screen[3] != 0) {
            g_frame_3d = true;
            g_post_3d = false;  // another 3D view (split screen)
        } else if (((g_frame_3d && g_post_3d) || fmv::Fill()) && (d.enabled & 1) && d.stride >= 4 && !g_counting &&
                   s.Reg(SET_COLOR_MASK) != 0) {
            // (visibility tests are no HUD: FCI decides per water patch from a pre-transformed test polygon whether to
            // draw it; narrowed into the middle, the patches at the sides counted no pixels and the water was missing)
            float lo = 1e30f, hi = -1e30f, top = 1e30f, bottom = -1e30f;
            const std::vector<float>& vertices = kelvin::Vertices(d);  // performance: raw draws convert here
            for (size_t v = 0; v + 4 <= vertices.size(); v += d.stride) {
                lo = std::min(lo, vertices[v]), hi = std::max(hi, vertices[v]);
                top = std::min(top, vertices[v + 1]), bottom = std::max(bottom, vertices[v + 1]);
            }
            // full screen: a quad across the whole width (letterbox bars), or anything covering all of it; a HUD
            // batch whose parts slide out at both sides spans the width too, but neither the height nor one quad
            bool across = lo <= 1.0f && hi >= surf.width - 1.0f;
            bool covers = across && top <= 1.0f && bottom >= surf.height - 1.0f;
            // ---- fmv_scale: a video texture (title, menus) scaled to fill, from then on the frame is a video's ----
            uint32_t video = g_stage_video[0];
            if (fmv::Fill() && !g_frame_3d && video && StageSamples(s.Reg(SET_SHADER_STAGE_PROGRAM) & 0x1F) &&
                fmv::IsVideo(video >> 16, video & 0xFFFF)) {
                float z = fmv::Transform(lo, top, hi, bottom, video & 0xFFFF, vc.hud);
                static uint64_t logged = 0;
                uint64_t what = uint64_t(video) << 32 ^ uint64_t(z * 1000);
                if (what != logged)
                    rt::Log("render: fmv: video texture %ux%u at %.1f,%.1f..%.1f,%.1f shown x%.3f", video >> 16,
                            video & 0xFFFF, lo, top, hi, bottom, z);
                logged = what;
                g_frame_video = true;
                g_video_rect[0] = lo, g_video_rect[1] = top, g_video_rect[2] = hi, g_video_rect[3] = bottom;
                g_replay_now = !g_replaying && !g_before_video.empty();
            } else if ((g_frame_3d && g_post_3d && !(covers || (across && d.count <= 6))) ||
                       (video_frame && !g_frame_3d && !covers)) {
                // over a video all 2D but what covers the screen (fades) goes to the middle, bands across the width
                // too (the main menu draws a still of its video's band under the video)
                vc.hud[0] = 1.0f / surf.color->wide, vc.hud[1] = surf.width * 0.5f * (1.0f - vc.hud[0]);
                narrowed = true;
                // before this frame's video: 2D beside where the video went last frame is drawn again after it.
                // Per triangle of a list (one draw may hold a glyph above and one below the video), else the box
                if (video_frame && !g_frame_video && !g_replaying && !across && g_before_video.size() < 64) {
                    const float* r = g_video_rect;
                    auto beside = [&](float x0, float y0, float x1, float y1) {
                        return y1 <= r[1] + 1 || y0 >= r[3] - 1 || x1 <= r[0] + 1 || x0 >= r[2] - 1;
                    };
                    bool outside = beside(lo, top, hi, bottom);
                    if (!outside && d.primitive == kTriangles && d.count % 3 == 0) {
                        outside = true;
                        for (size_t v = 0; outside && v + 3 * d.stride <= vertices.size(); v += 3 * d.stride) {
                            float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
                            for (int k = 0; k < 3; k++) {
                                const float* p = &vertices[v + k * d.stride];
                                x0 = std::min(x0, p[0]), x1 = std::max(x1, p[0]);
                                y0 = std::min(y0, p[1]), y1 = std::max(y1, p[1]);
                            }
                            outside = beside(x0, y0, x1, y1);
                        }
                    }
                    if (outside) g_before_video.emplace_back(s, d);
                }
            }
            // ---- end fmv_scale ----
        }
    }
    // ---- end PC features ----
    static Constants<VertexConstants> vertex_constants;
    ID3D11Buffer* vcb = vertex_constants.Update(vc);
    bool factor0_each = s.Reg(SET_COMBINER_CONTROL) & 0x1000, factor1_each = s.Reg(SET_COMBINER_CONTROL) & 0x10000;
    for (int i = 0; i < 8; i++) {
        UnpackArgb(s.Reg(SET_COMBINER_FACTOR0 + 4 * (factor0_each ? i : 0)), pc.factor0[i]);
        UnpackArgb(s.Reg(SET_COMBINER_FACTOR1 + 4 * (factor1_each ? i : 0)), pc.factor1[i]);
    }
    if (water_ground) std::memcpy(pc.factor1[7], pc.factor0[7], sizeof pc.factor1[7]);  // PC water: its colour too
    uint32_t fog = s.Reg(SET_FOG_COLOR);
    for (int i = 0; i < 4; i++) pc.fog_color[i] = ((fog >> (8 * i)) & 0xFF) / 255.0f;
    for (int i = 0; i < 3; i++) pc.fog_params[i] = s.RegF(SET_FOG_PARAMS + 4 * i);
    pc.alpha_ref[0] = float(s.Reg(SET_ALPHA_REF) & 0xFF);
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) pc.bump[i][j] = s.RegF(SET_TEXTURE_BUMP_ENV_MAT + 0x40 * i + 4 * j);
    UnpackArgb(s.Reg(SET_SPECULAR_FOG_FACTOR), pc.final_factor[0]);
    UnpackArgb(s.Reg(SET_SPECULAR_FOG_FACTOR + 4), pc.final_factor[1]);
    // a shader that writes the depth gets no rasterizer bias: W-buffered draws add it themselves
    static const bool no_offset = GetEnvironmentVariableA("FCI_NO_POLY_OFFSET", nullptr, 0) > 0;  // diagnostics
    if (s.Reg(SET_POLY_OFFSET_FILL_ENABLE) && !no_offset) {
        pc.depth_bias[0] = s.RegF(SET_POLYGON_OFFSET_BIAS) / 16777216.0f;
        pc.depth_bias[1] = s.RegF(SET_POLYGON_OFFSET_SCALE_FACTOR) * surf.scale;  // slopes per guest pixel
        static int logged = 0;
        static const bool trace_offset = GetEnvironmentVariableA("FCI_TRACE_POLY_OFFSET", nullptr, 0) > 0;
        if (logged < 8 && trace_offset)
            logged++, rt::Log("render: polygon offset bias %g scale %g (ps %016llX)", s.RegF(SET_POLYGON_OFFSET_BIAS),
                              s.RegF(SET_POLYGON_OFFSET_SCALE_FACTOR), (unsigned long long)psh::Key(s, config));
    }
    static Constants<PixelConstants> pixel_constants;
    ID3D11Buffer* pcb = pixel_constants.Update(pc);

    // fixed function state
    UINT8 mask = surf.color ? WriteMask(s.Reg(SET_COLOR_MASK)) : 0;
    float blend_color[4];
    UnpackArgb(s.Reg(SET_BLEND_COLOR), blend_color);
    if (g_no_state_cache) g_ds.Invalidate();
    {
        ID3D11BlendState* b = GetBlendState(s.Reg(SET_BLEND_ENABLE) != 0, s.Reg(SET_BLEND_FUNC_SFACTOR),
                                            s.Reg(SET_BLEND_FUNC_DFACTOR), s.Reg(SET_BLEND_EQUATION), mask);
        if (!g_ds.valid || b != g_ds.blend || std::memcmp(blend_color, g_ds.factor, sizeof blend_color)) {
            g_context->OMSetBlendState(b, blend_color, 0xFFFFFFFF);
            g_ds.blend = b;
            std::memcpy(g_ds.factor, blend_color, sizeof blend_color);
        }
    }
    D3D11_DEPTH_STENCIL_DESC ds{};
    if (surf.depth) {
        ds.DepthEnable = depth_test;
        ds.DepthWriteMask = s.Reg(SET_DEPTH_MASK) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        ds.DepthFunc = D3D11_COMPARISON_FUNC((s.Reg(SET_DEPTH_FUNC) & 7) + 1);
        if (reversed) {  // performance: larger is nearer
            static const D3D11_COMPARISON_FUNC kFlip[9] = {
                D3D11_COMPARISON_NEVER, D3D11_COMPARISON_NEVER, D3D11_COMPARISON_GREATER, D3D11_COMPARISON_EQUAL,
                D3D11_COMPARISON_GREATER_EQUAL, D3D11_COMPARISON_LESS, D3D11_COMPARISON_NOT_EQUAL,
                D3D11_COMPARISON_LESS_EQUAL, D3D11_COMPARISON_ALWAYS};
            ds.DepthFunc = kFlip[ds.DepthFunc];
        }
        ds.StencilEnable = stencil_test;
        ds.StencilReadMask = UINT8(s.Reg(SET_STENCIL_FUNC_MASK));
        ds.StencilWriteMask = UINT8(s.Reg(SET_STENCIL_MASK));
        ds.FrontFace.StencilFunc = D3D11_COMPARISON_FUNC((s.Reg(SET_STENCIL_FUNC) & 7) + 1);
        ds.FrontFace.StencilFailOp = StencilOp(s.Reg(SET_STENCIL_OP_FAIL));
        ds.FrontFace.StencilDepthFailOp = StencilOp(s.Reg(SET_STENCIL_OP_ZFAIL));
        ds.FrontFace.StencilPassOp = StencilOp(s.Reg(SET_STENCIL_OP_ZPASS));
        ds.BackFace = ds.FrontFace;
    } else {
        ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
        ds.FrontFace.StencilFunc = ds.BackFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
    }
    static const bool no_depth = GetEnvironmentVariableA("FCI_NO_DEPTH", nullptr, 0) > 0;  // diagnostics
    if (no_depth) ds.DepthEnable = FALSE, ds.StencilEnable = FALSE;
    {
        ID3D11DepthStencilState* d = GetDepthState(ds);
        UINT ref = s.Reg(SET_STENCIL_FUNC_REF) & 0xFF;
        if (!g_ds.valid || d != g_ds.depth || ref != g_ds.ref) g_context->OMSetDepthStencilState(d, ref);
        g_ds.depth = d, g_ds.ref = ref;
    }
    D3D11_RASTERIZER_DESC rs{};
    rs.FillMode = s.Reg(SET_FRONT_POLYGON_MODE) == 0x1B01 ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_NONE;
    if (s.Reg(SET_CULL_FACE_ENABLE)) {
        uint32_t face = s.Reg(SET_CULL_FACE);
        if (face == 0x404) rs.CullMode = D3D11_CULL_FRONT;
        if (face == 0x405) rs.CullMode = D3D11_CULL_BACK;
    }
    // (performance: the diagnostics' switches read once; GetEnvironmentVariable at every draw was 5 % of the render
    // thread)
    static const bool no_cull = GetEnvironmentVariableA("FCI_NO_CULL", nullptr, 0) > 0;
    static const bool no_depth_clip = GetEnvironmentVariableA("FCI_NO_DEPTH_CLIP", nullptr, 0) > 0;
    if (no_cull) rs.CullMode = D3D11_CULL_NONE;  // diagnostics
    // SET_FRONT_FACE (GL_CW 0x900, GL_CCW 0x901) counts winding in screen space as D3D 11 does
    rs.FrontCounterClockwise = s.Reg(SET_FRONT_FACE) == 0x901;
    if (s.Reg(SET_POLY_OFFSET_FILL_ENABLE)) {
        rs.DepthBias = INT(s.RegF(SET_POLYGON_OFFSET_BIAS));
        rs.SlopeScaledDepthBias = s.RegF(SET_POLYGON_OFFSET_SCALE_FACTOR) * surf.scale;
    }
    // geometry in front of the near plane must go, or a close mesh covers the screen
    rs.DepthClipEnable = !no_depth_clip;
    rs.ScissorEnable = TRUE;
    rs.MultisampleEnable = surf.samples > 1;
    {
        ID3D11RasterizerState* r = GetRasterState(rs);
        if (!g_ds.valid || r != g_ds.raster) g_context->RSSetState(r);
        g_ds.raster = r;
    }
    D3D11_RECT scissor = surf.clip;
    // ---- PC features: widescreen and fov ----
    // FCI draws the light of each dynamic light as an additive pass over the objects it reaches, cut to a scissor
    // rectangle (a surface clip smaller than the target, renderer light list +0x54..+0x60 -> 0x002425F0 ->
    // D3DDevice_SetScissors) that the engine works out from the light's sphere. The rectangle follows the projection
    // (16:9 -> 21:9 scales it as the picture) but is too narrow towards the sides: lit surfaces beyond it end in
    // straight vertical lines. On the console that error lies outside the 4:3 picture; Hor+ (pcmode::Widen) and
    // fov (pcmode::FovScale) show the sides at angles where it is plain (GitHub #8: grates and light in the
    // tutorial's wreck at 16:9, fov 1). Wherever the picture is not the console's, the 3D draws (W-buffered, vertex
    // program) on the frame buffer keep their view's rectangle instead (viewport constants c58/c59: scale, offset;
    // a split screen's view stays inside its half); the light's own falloff ends it. 4:3 with fov 1: unchanged.
    // Diagnostics: FCI_ENGINE_LIGHT_SCISSOR=1 keeps the engine's rectangles, =2 alternates every 0.5 s (A/B in
    // frame dumps); FCI_TRACE_LIGHT_SCISSOR=1 logs the first 3000 such draws with their rectangle.
    static const int engine_scissor = [] {
        char v[8] = {};
        return GetEnvironmentVariableA("FCI_ENGINE_LIGHT_SCISSOR", v, sizeof v) ? std::atoi(v) : 0;
    }();
    static const bool trace_scissor = GetEnvironmentVariableA("FCI_TRACE_LIGHT_SCISSOR", nullptr, 0) > 0;
    if (trace_scissor && w_buffer && surf.width >= 640 && s.constants[59][0] > 2.0f) {
        static int logged = 0;
        const uint32_t ch = s.Reg(SET_SURFACE_CLIP_HORIZONTAL), cv = s.Reg(SET_SURFACE_CLIP_VERTICAL);
        if (logged < 3000 && ((ch >> 16) < 640 || (cv >> 16) < 480)) {
            logged++;
            rt::Log("render: light scissor, draw %llu x %u..%u y %u..%u ps %016llX blend %u %X/%X c58 %g %g c59 %g %g",
                    (unsigned long long)g_draws, ch & 0xFFFF, (ch & 0xFFFF) + (ch >> 16), cv & 0xFFFF,
                    (cv & 0xFFFF) + (cv >> 16), (unsigned long long)psh::Key(s, config), s.Reg(SET_BLEND_ENABLE),
                    s.Reg(SET_BLEND_FUNC_SFACTOR), s.Reg(SET_BLEND_FUNC_DFACTOR), s.constants[58][0],
                    s.constants[58][1], s.constants[59][0], s.constants[59][1]);
        }
    }
    const bool keep_engine = engine_scissor == 1 || (engine_scissor == 2 && ((GetTickCount64() / 500) & 1));
    if (w_buffer && surf.color && surf.width >= 640 && (s.Reg(SET_TRANSFORM_EXECUTION_MODE) & 3) == 2 &&
        (pcmode::Widen() != 1.0f || pcmode::FovScale() != 1.0f) && !keep_engine) {
        const float* scale = s.constants[58];
        const float* offset = s.constants[59];
        const float hw = std::fabs(scale[0]), hh = std::fabs(scale[1]);
        const float vx = offset[0] - 0.53125f, vy = offset[1] - 0.53125f;  // the view's centre (see below)
        if (hw >= 16.0f && hh >= 16.0f && vx - hw >= -1.0f && vy - hh >= -1.0f && vx + hw <= float(surf.width) + 1.0f &&
            vy + hh <= float(surf.height) + 1.0f) {  // constants that describe a view on this target
            // guest pixels to host pixels as the surface clip goes (BindSurfaces)
            const float sx = surf.scale * surf.wide * float(surf.scale_x), sy = surf.scale * float(surf.scale_y);
            // the offset holds the XDK's pixel centre bias (+0.53125): the view is offset - 0.53125 +- scale
            const LONG x0 = LONG(std::floor((vx - hw) * sx + 0.01f)), x1 = LONG(std::ceil((vx + hw) * sx - 0.01f));
            const LONG y0 = LONG(std::floor((vy - hh) * sy + 0.01f)), y1 = LONG(std::ceil((vy + hh) * sy - 0.01f));
            const LONG w = LONG(HostWidthOf(surf.width, surf.scale, surf.wide));
            const LONG h = LONG(HostHeightOf(surf.height, surf.scale));
            scissor = {std::max<LONG>(x0, 0), std::max<LONG>(y0, 0), std::min(x1, w), std::min(y1, h)};
            if (scissor.left >= scissor.right || scissor.top >= scissor.bottom) scissor = surf.clip;
        }
    }
    // ---- end PC features: widescreen and fov ----
    if (narrowed) {
        // PC features: a narrowed 2D draw stays inside the 4:3 middle, as it stayed inside the console's screen
        // (HUD parts the game slides off screen must not show up beside it)
        float center = HostWidthOf(surf.width, surf.scale, surf.wide) * 0.5f;
        scissor.left = LONG(center + (scissor.left - center) * vc.hud[0] + 0.5f);
        scissor.right = LONG(center + (scissor.right - center) * vc.hud[0] + 0.5f);
    }
    g_context->RSSetScissorRects(1, &scissor);

    if (wpass.active) WaterBind(wpass, surf);  // PC water

    // geometry
    static Ring vertex_ring(D3D11_BIND_VERTEX_BUFFER), index_ring(D3D11_BIND_INDEX_BUFFER);
    // performance: raw draws put vertices and indices into one buffer with one map
    static Ring geometry_ring(D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER);
    if (d.raw) {
        // ---- performance: raw draws. The guest's vertex memory the draw uses, an input slot per attribute;
        // interleaved attributes (same stride, overlapping bytes) share one copy ----
        struct Stream {
            const uint8_t *lo, *hi;
            uint32_t stride;
            UINT offset;
        };
        Stream streams[kAttributes];
        int of[kAttributes] = {}, n = 0;
        for (int i = 0; i < kAttributes; i++) {
            if (!(d.enabled & (1u << i))) continue;
            const uint8_t* a = d.data[i];
            const uint8_t* e = a + size_t(d.span - 1) * d.data_stride[i] + kelvin::RawBytes(d.format[i]);
            int k = 0;
            while (k < n && !(streams[k].stride == d.data_stride[i] && a < streams[k].hi + 64 && e + 64 > streams[k].lo)) k++;
            if (k == n) streams[n++] = {a, e, d.data_stride[i], 0};
            else streams[k].lo = std::min(streams[k].lo, a), streams[k].hi = std::max(streams[k].hi, e);
            of[i] = k;
        }
        // the indices: the vertices' numbers, through the topology's own list (fans, quads) where it has one
        const bool short_ids = d.span <= 0x10000;
        static std::vector<uint16_t> ids16;
        static std::vector<uint32_t> ids32;
        const size_t count = indices.empty() ? d.ids.size() : indices.size();
        const void* index_data;
        UINT index_bytes;
        if (short_ids) {
            ids16.resize(count);
            if (indices.empty())
                for (size_t k = 0; k < count; k++) ids16[k] = uint16_t(d.ids[k]);
            else
                for (size_t k = 0; k < count; k++) ids16[k] = uint16_t(d.ids[indices[k]]);
            index_data = ids16.data(), index_bytes = UINT(count * 2);
        } else {
            const uint32_t* src = d.ids.data();
            if (!indices.empty()) {
                ids32.resize(count);
                for (size_t k = 0; k < count; k++) ids32[k] = d.ids[indices[k]];
                src = ids32.data();
            }
            index_data = src, index_bytes = UINT(count * 4);
        }
        const void* pieces[kAttributes + 1];
        UINT sizes[kAttributes + 1], at_piece[kAttributes + 1];
        for (int k = 0; k < n; k++) pieces[k] = streams[k].lo, sizes[k] = UINT(streams[k].hi - streams[k].lo);
        pieces[n] = index_data, sizes[n] = index_bytes;
        geometry_ring.PushMany(pieces, sizes, n + 1, at_piece);
        for (int k = 0; k < n; k++) streams[k].offset = at_piece[k];
        ID3D11Buffer* vb = geometry_ring.buffer.Get();
        ID3D11Buffer* vbs[kAttributes];
        UINT strides[kAttributes], offsets[kAttributes];
        int last = 0;
        for (int i = 0; i < kAttributes; i++) {
            vbs[i] = vb, strides[i] = 0, offsets[i] = 0;
            if (!(d.enabled & (1u << i))) continue;
            const Stream& st = streams[of[i]];
            strides[i] = d.data_stride[i];
            offsets[i] = st.offset + UINT(d.data[i] - st.lo);
            last = i + 1;
        }
        g_context->IASetVertexBuffers(0, last, vbs, strides, offsets);
        BindPipeline(vs, topology, ps, vcb, pcb, srvs, samplers);
        g_context->IASetIndexBuffer(vb, short_ids ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT, at_piece[n]);
        g_context->DrawIndexed(UINT(count), 0, 0);
        // ---- end performance ----
    } else {
    UINT stride = UINT(d.stride * 4);
    UINT offset = vertex_ring.Push(d.vertices.data(), UINT(d.vertices.size() * 4));
    ID3D11Buffer* vb = vertex_ring.buffer.Get();
    g_context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    BindPipeline(vs, topology, ps, vcb, pcb, srvs, samplers);
    if (indices.empty()) {
        g_context->Draw(d.count, 0);
    } else {
        UINT at = index_ring.Push(indices.data(), UINT(indices.size() * 4));
        g_context->IASetIndexBuffer(index_ring.buffer.Get(), DXGI_FORMAT_R32_UINT, at);
        g_context->DrawIndexed(UINT(indices.size()), 0, 0);
    }
    }
    if (wpass.active) WaterUnbind(surf);  // PC water
    if (surf.color && mask) MarkWritten(*surf.color);
    g_draws++;
    if (surf.depth && depth_test && s.Reg(SET_DEPTH_MASK)) surf.depth->last_write = ++g_write_sequence;
    DrainMessages();
    DumpDrawStep(surf.color);
    if (g_trace_draws > 0 && GetTickCount64() >= std::min(g_trace_from, g_dump_from) &&
        (!g_trace_ps || psh::Key(s, config) == g_trace_ps)) {
        g_trace_draws--;
        rt::Log("draw %llu (file %d): prim %u, %u vertices, attrs %04X, target %08X %ux%u, mask %X, blend %u %X/%X, depth %u, "
                "cull %u, vs %016llX ps %016llX, tex0 %s, fog %u mode %X gen %X color %08X params %g %g %g, "
                "final %08X %08X, mode %X, zfunc %X zmask %u stencil %u func %X ref %X fmask %X ops %X/%X/%X, "
                "clip %g..%g control0 %08X surface %08X, cullface %X front %X stages %05X clipmode %X, "
                "c58 %g %g %g %g c59 %g %g %g %g",
                (unsigned long long)g_draws, g_dumped - 1, d.primitive, d.count, d.enabled, surf.color ? surf.color->address : 0,
                surf.width, surf.height, mask, s.Reg(SET_BLEND_ENABLE), s.Reg(SET_BLEND_FUNC_SFACTOR),
                s.Reg(SET_BLEND_FUNC_DFACTOR), depth_test, s.Reg(SET_CULL_FACE_ENABLE),
                (unsigned long long)vsh::Key(s, d.enabled), (unsigned long long)psh::Key(s, config),
                srvs[0] ? "yes" : "no", s.Reg(SET_FOG_ENABLE), s.Reg(SET_FOG_MODE), s.Reg(SET_FOG_GEN_MODE),
                s.Reg(SET_FOG_COLOR), s.RegF(SET_FOG_PARAMS), s.RegF(SET_FOG_PARAMS + 4), s.RegF(SET_FOG_PARAMS + 8),
                s.Reg(SET_COMBINER_SPECULAR_FOG_CW0), s.Reg(SET_COMBINER_SPECULAR_FOG_CW1),
                s.Reg(SET_TRANSFORM_EXECUTION_MODE), s.Reg(SET_DEPTH_FUNC), s.Reg(SET_DEPTH_MASK),
                s.Reg(SET_STENCIL_TEST_ENABLE), s.Reg(SET_STENCIL_FUNC), s.Reg(SET_STENCIL_FUNC_REF),
                s.Reg(SET_STENCIL_FUNC_MASK), s.Reg(SET_STENCIL_OP_FAIL), s.Reg(SET_STENCIL_OP_ZFAIL),
                s.Reg(SET_STENCIL_OP_ZPASS), s.RegF(SET_CLIP_MIN), s.RegF(SET_CLIP_MAX), s.Reg(SET_CONTROL0),
                s.Reg(SET_SURFACE_FORMAT), s.Reg(SET_CULL_FACE), s.Reg(SET_FRONT_FACE),
                s.Reg(SET_SHADER_STAGE_PROGRAM), s.Reg(SET_SHADER_CLIP_PLANE_MODE), s.constants[58][0], s.constants[58][1], s.constants[58][2],
                s.constants[58][3], s.constants[59][0], s.constants[59][1], s.constants[59][2], s.constants[59][3]);
        for (int i = 0; i < 4; i++) {
            uint32_t base = 0x40 * i;
            rt::Log("    stage %d: program %u srv %s kind %d format %08X offset %08X control0 %08X control1 %08X "
                    "address %08X border %08X filter %08X rect %08X, factor0 %08X factor1 %08X",
                    i, (programs >> (5 * i)) & 0x1F, srvs[i] ? "yes" : "no", int(config.sampler[i]),
                    s.Reg(SET_TEXTURE_FORMAT + base), s.Reg(SET_TEXTURE_OFFSET + base),
                    s.Reg(SET_TEXTURE_CONTROL0 + base), s.Reg(SET_TEXTURE_CONTROL1 + base),
                    s.Reg(SET_TEXTURE_ADDRESS + base), s.Reg(SET_TEXTURE_BORDER_COLOR + base),
                    s.Reg(SET_TEXTURE_FILTER + base),
                    s.Reg(SET_TEXTURE_IMAGE_RECT + base), s.Reg(SET_COMBINER_FACTOR0 + 4 * i),
                    s.Reg(SET_COMBINER_FACTOR1 + 4 * i));
        }
        rt::Log("    factor0[4..7] %08X %08X %08X %08X factor1[4..7] %08X %08X %08X %08X",
                s.Reg(SET_COMBINER_FACTOR0 + 16), s.Reg(SET_COMBINER_FACTOR0 + 20), s.Reg(SET_COMBINER_FACTOR0 + 24),
                s.Reg(SET_COMBINER_FACTOR0 + 28), s.Reg(SET_COMBINER_FACTOR1 + 16), s.Reg(SET_COMBINER_FACTOR1 + 20),
                s.Reg(SET_COMBINER_FACTOR1 + 24), s.Reg(SET_COMBINER_FACTOR1 + 28));
        rt::Log("    alpha test %u func %X ref %u", s.Reg(SET_ALPHA_TEST_ENABLE), s.Reg(SET_ALPHA_FUNC),
                s.Reg(SET_ALPHA_REF) & 0xFF);
        rt::Log("    surface clip %08X %08X window clip type %X %08X %08X", s.Reg(SET_SURFACE_CLIP_HORIZONTAL),
                s.Reg(SET_SURFACE_CLIP_VERTICAL), s.Reg(SET_WINDOW_CLIP_TYPE), s.Reg(SET_WINDOW_CLIP_HORIZONTAL),
                s.Reg(SET_WINDOW_CLIP_VERTICAL));
        static const std::string trace_consts = [] {
            char value[256] = {};
            GetEnvironmentVariableA("FCI_TRACE_CONSTS", value, sizeof value);
            return std::string(value);
        }();
        for (size_t at = 0; at < trace_consts.size();) {
            int k = std::atoi(trace_consts.c_str() + at);
            if (k >= 0 && k < int(kConstants))
                rt::Log("    c%d = %g %g %g %g", k, s.constants[k][0], s.constants[k][1], s.constants[k][2],
                        s.constants[k][3]);
            size_t comma = trace_consts.find(',', at);
            at = comma == std::string::npos ? trace_consts.size() : comma + 1;
        }
        const std::vector<float>& vertices = kelvin::Vertices(d);  // performance: raw draws convert here
        for (uint32_t v = 0; v < std::min(d.count, 4u); v++) {
            std::string line = "    ";
            for (int k = 0; k < d.stride; k += 4) {
                char item[96];
                snprintf(item, sizeof item, "(%g %g %g %g) ", vertices[v * d.stride + k],
                         vertices[v * d.stride + k + 1], vertices[v * d.stride + k + 2],
                         vertices[v * d.stride + k + 3]);
                line += item;
            }
            rt::Log("%s", line.c_str());
        }
    }
}

// ---- PC features: fmv_scale ----
// The menus draw some 2D before their video (the profile line at the top, button hints at the bottom, in the bars);
// the video scaled to fill would cover it, so those draws are kept and drawn again over the video
void DrawPrimitives(const State& s, const Draw& d) {
    DrawOne(s, d);
    if (!g_replay_now) return;
    g_replay_now = false;
    std::vector<std::pair<State, Draw>> again = std::move(g_before_video);
    g_before_video.clear();
    g_replaying = true;
    for (const auto& [state, draw] : again) DrawOne(state, draw);
    g_replaying = false;
}
// ---- end fmv_scale ----

void Clear(const State& s, uint32_t flags) {
    if (!Ready()) return;
    bool color = flags & 0xF0, depth = flags & 3;
    Surfaces surf = BindSurfaces(s, color, depth);
    if (!surf.color && !surf.depth) return;
    if (AoBeforeClear(surf)) Rebind(surf);  // ambient occlusion
    uint32_t h = s.Reg(SET_CLEAR_RECT_HORIZONTAL), v = s.Reg(SET_CLEAR_RECT_VERTICAL);
    D3D11_RECT rect{LONG((h & 0xFFFF) * surf.scale_x), LONG((v & 0xFFFF) * surf.scale_y),
                    LONG(((h >> 16) + 1) * surf.scale_x), LONG(((v >> 16) + 1) * surf.scale_y)};
    rect.right = std::min<LONG>(rect.right, surf.width), rect.bottom = std::min<LONG>(rect.bottom, surf.height);
    const float sx = float(surf.scale) * surf.wide;
    rect.left = LONG(rect.left * sx + 0.5f), rect.right = LONG(rect.right * sx + 0.5f);
    rect.top = LONG(rect.top * surf.scale + 0.5f), rect.bottom = LONG(rect.bottom * surf.scale + 0.5f);
    if (rect.left >= rect.right || rect.top >= rect.bottom) return;

    float constants[8] = {};
    uint32_t value = s.Reg(SET_COLOR_CLEAR_VALUE);
    uint32_t format = surf.color ? surf.color->format : 0;
    if (format == 3) {
        constants[0] = ((value >> 11) & 31) / 31.0f, constants[1] = ((value >> 5) & 63) / 63.0f;
        constants[2] = (value & 31) / 31.0f, constants[3] = 1;
    } else if (format == 1 || format == 2) {
        constants[0] = ((value >> 10) & 31) / 31.0f, constants[1] = ((value >> 5) & 31) / 31.0f;
        constants[2] = (value & 31) / 31.0f, constants[3] = 1;
    } else {
        UnpackArgb(value, constants);
    }
    uint32_t zeta = s.Reg(SET_ZSTENCIL_CLEAR_VALUE);
    bool z16 = surf.zmax == 65535.0f;
    constants[4] = float(z16 ? (zeta & 0xFFFF) : (zeta >> 8)) / surf.zmax;
    if (g_reversed_depth && surf.depth && !z16)  // performance: reversed depth
        constants[4] = psh::kReversedDepth / std::max(constants[4], psh::kReversedDepth);
    Fill(g_clear_constants.Get(), constants, sizeof constants);

    UINT8 mask = 0;
    if (surf.color) mask = UINT8((flags & 0x10 ? 1 : 0) | (flags & 0x20 ? 2 : 0) | (flags & 0x40 ? 4 : 0) |
                                 (flags & 0x80 ? 8 : 0));
    // ---- performance: a clear of whole targets (all channels, depth and stencil as asked) through the clear calls,
    // which the GPU does as fast clears (compression kept); before every clear was a full screen draw, at 4x MSAA
    // one write of every sample ----
    {
        auto whole = [&](uint32_t w, uint32_t h) {
            return rect.left == 0 && rect.top == 0 && rect.right >= LONG(w) && rect.bottom >= LONG(h);
        };
        const bool want_color = surf.color && mask, want_depth = surf.depth && (flags & 3);
        const bool color_ok = !want_color || (mask == 0xF && whole(surf.color->HostWidth(), surf.color->HostHeight()));
        const bool depth_ok =
            !want_depth || (whole(HostWidthOf(surf.depth->width, surf.depth->scale, surf.depth->wide),
                                  HostHeightOf(surf.depth->height, surf.depth->scale)) &&
                            (surf.depth->z16 || (flags & 3) == 3 || !(flags & 2)));
        if ((want_color || want_depth) && color_ok && depth_ok) {
            if (want_color) g_context->ClearRenderTargetView(surf.color->rtv.Get(), constants);
            if (want_depth) {
                UINT what = (flags & 1 ? D3D11_CLEAR_DEPTH : 0) | (flags & 2 && surf.depth->stencil ? D3D11_CLEAR_STENCIL : 0);
                if (what) g_context->ClearDepthStencilView(surf.depth->dsv.Get(), what, constants[4], UINT8(zeta & 0xFF));
            }
            if (surf.color && mask) MarkWritten(*surf.color);
            g_clears++, g_fast_clears++;
            DumpDrawStep(surf.color);
            return;
        }
    }
    // ---- end performance ----
    g_ds.Invalidate();  // performance: state cache
    g_context->OMSetBlendState(GetBlendState(false, 1, 0, 0x8006, mask), nullptr, 0xFFFFFFFF);
    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = surf.depth && (flags & 1);
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
    ds.StencilEnable = surf.depth && surf.depth->stencil && (flags & 2);
    ds.StencilReadMask = 0xFF, ds.StencilWriteMask = 0xFF;
    ds.FrontFace = {D3D11_STENCIL_OP_REPLACE, D3D11_STENCIL_OP_REPLACE, D3D11_STENCIL_OP_REPLACE,
                    D3D11_COMPARISON_ALWAYS};
    ds.BackFace = ds.FrontFace;
    g_context->OMSetDepthStencilState(GetDepthState(ds), z16 ? 0 : (zeta & 0xFF));
    D3D11_RASTERIZER_DESC rs{};
    rs.FillMode = D3D11_FILL_SOLID, rs.CullMode = D3D11_CULL_NONE, rs.DepthClipEnable = FALSE;
    rs.ScissorEnable = TRUE;
    g_context->RSSetState(GetRasterState(rs));
    g_context->RSSetScissorRects(1, &rect);
    g_context->IASetInputLayout(nullptr);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->VSSetShader(g_clear_vs.Get(), nullptr, 0);
    g_context->PSSetShader(g_clear_ps.Get(), nullptr, 0);
    ID3D11Buffer* cb = g_clear_constants.Get();
    g_context->VSSetConstantBuffers(2, 1, &cb);
    g_context->PSSetConstantBuffers(2, 1, &cb);
    g_context->Draw(3, 0);
    if (surf.color && mask) MarkWritten(*surf.color);
    g_clears++;
    DumpDrawStep(surf.color);
    if (g_trace_draws > 0 && GetTickCount64() >= g_trace_from)
        rt::Log("clear flags %X color %08X zeta %08X rect %08X %08X target %08X", flags, value, zeta, h, v,
                surf.color ? surf.color->address : 0);
}

// ---- occlusion counting: D3D 11 occlusion queries cover the spans counting was on ----

namespace {
// the queries since the last clear; a report covers a prefix of them
struct CountEpoch {
    std::vector<ComPtr<ID3D11Query>> queries;
    std::vector<uint64_t> samples;  // results of the first queries, in order
};
struct PendingReport {
    uint8_t* report;
    std::shared_ptr<CountEpoch> epoch;
    size_t queries;
};
ComPtr<ID3D11Query> g_active_query;
std::shared_ptr<CountEpoch> g_epoch = std::make_shared<CountEpoch>();
std::deque<PendingReport> g_reports;

void BeginQuery() {
    D3D11_QUERY_DESC desc{D3D11_QUERY_OCCLUSION, 0};
    if (SUCCEEDED(g_device->CreateQuery(&desc, &g_active_query))) g_context->Begin(g_active_query.Get());
}

void EndQuery() {
    if (!g_active_query) return;
    g_context->End(g_active_query.Get());
    g_epoch->queries.push_back(std::move(g_active_query));
}

uint64_t g_count_divisor = 1;  // HLE: samples per console pixel in the scaled, multisampled frame buffer

void WriteReport(uint8_t* report, uint64_t count) {
    count = (count + g_count_divisor - 1) / g_count_divisor;
    // FCI_REPORT_VISIBLE=1 (diagnostics): every visibility test passes (all of the game's water patches drawn)
    static const bool all_visible = GetEnvironmentVariableA("FCI_REPORT_VISIBLE", nullptr, 0) > 0;
    if (all_visible) count = std::max<uint64_t>(count, 100000);
    uint64_t timestamp = GetTickCount64() * 1000000;
    uint32_t value = uint32_t(std::min<uint64_t>(count, 0xFFFFFFFF)), status = 0;
    std::memcpy(report, &timestamp, 8);
    std::memcpy(report + 8, &value, 4);
    std::memcpy(report + 12, &status, 4);
    static int traced = 0;
    static const bool trace = GetEnvironmentVariableA("FCI_TRACE_REPORTS", nullptr, 0) > 0;
    if (trace && traced < 200) traced++, rt::Log("gpu: report = %u pixels", value);
}
}  // namespace

void SetPixelCounting(bool enable) {
    if (!Ready() || enable == g_counting) return;
    g_counting = enable;
    if (enable)
        BeginQuery();
    else
        EndQuery();
}

void ClearPixelCount() {
    if (!Ready()) return;
    EndQuery();
    g_epoch = std::make_shared<CountEpoch>();  // pending reports keep the old one
    if (g_counting) BeginQuery();
}

void QueueReport(uint8_t* report) {
    if (!Ready()) return WriteReport(report, 0);
    if (g_counting) EndQuery(), BeginQuery();
    g_reports.push_back({report, g_epoch, g_epoch->queries.size()});
    PollReports();
}

void PollReports() {
    while (!g_reports.empty()) {
        PendingReport& r = g_reports.front();
        CountEpoch& e = *r.epoch;
        while (e.samples.size() < r.queries) {
            UINT64 samples = 0;
            if (g_context->GetData(e.queries[e.samples.size()].Get(), &samples, sizeof samples, 0) != S_OK) return;
            e.queries[e.samples.size()].Reset();
            e.samples.push_back(samples);
        }
        uint64_t count = 0;
        for (size_t i = 0; i < r.queries; i++) count += e.samples[i];
        WriteReport(r.report, count);
        g_reports.pop_front();
    }
}

void Flush() {
    if (!g_init_done || g_failed) return;
    // copy every dirty target first, then map them: one wait for the GPU instead of one per target
    std::vector<Target*> dirty;
    for (auto& t : g_targets)
        if (t->dirty && !g_hle) {
            ToStaging(*t);
            dirty.push_back(t.get());
        }
    // oldest first: where targets share memory, the latest drawing must land last
    std::sort(dirty.begin(), dirty.end(), [](Target* a, Target* b) { return a->last_write < b->last_write; });
    for (Target* t : dirty) {
        D3D11_MAPPED_SUBRESOURCE map;
        if (FAILED(g_context->Map(t->staging.Get(), 0, D3D11_MAP_READ, 0, &map))) continue;
        WriteBack(*t, map);
        g_context->Unmap(t->staging.Get(), 0);
    }
    g_generation++;
    if (g_generation % 600 == 0)
        for (auto& [cause, n] : g_download_causes) rt::Log("render: %llu downloads of %s", (unsigned long long)n, cause.c_str());
    if (g_generation % 600 == 0) g_download_causes.clear();
    if (g_generation % 600 == 0)
        rt::Log("render: generation %llu, %llu draws, %llu clears (%llu fast), %llu resolves, %zu targets, %zu textures, "
                "%zu+%zu shaders, display %08X",
                (unsigned long long)g_generation, (unsigned long long)g_draws, (unsigned long long)g_clears,
                (unsigned long long)g_fast_clears, (unsigned long long)g_resolves,
                g_targets.size(), g_textures.size(), g_vertex_shaders.size(), g_pixel_shaders.size(),
                DisplayAddress());
    if (g_generation % 600 == 0 && (g_left_out || g_held_frames))  // performance: shaders off the render thread
        rt::Log("render: draws left out until their shaders were ready: %llu, in %llu shown frames (%llu for a vertex "
                "shader, %llu for a pixel shader); %llu long frames held back",
                (unsigned long long)g_left_out, (unsigned long long)g_left_frames, (unsigned long long)g_left_vs,
                (unsigned long long)g_left_ps, (unsigned long long)g_held_frames);
}

}  // namespace gpu::render

namespace gpu::render {

void SetHle(bool on) {
    g_hle = on;
    if (!on) return;
    if (g_device) ao::Prepare(g_device.Get());  // else when the device is made (Ready)
    if (water::BootQuality()) water::Prepare();  // PC water: the wave map on a thread
    // scale: "auto" (or 0) renders at the output's height (display output options; performance: fractional, 800 / 480
    // on the Steam Deck, 1080 / 480 at 1080p, before rounded up to whole multiples of 480), "<n>%" that share of the
    // output's height (20-200 %), a number that many times the console's 480 lines (1-6, fractions allowed)
    std::string scale = settings::String("scale", "auto");
    const float output_lines = float(std::max(output::OutputHeight(), 240));
    float s = float(std::atof(scale.c_str()));
    if (!scale.empty() && scale.back() == '%') s = output_lines * std::clamp(s, 20.0f, 200.0f) / 100.0f / 480.0f;
    if (s <= 0) s = output_lines / 480.0f;
    g_q.scale = std::clamp(s, 0.5f, 6.0f);
    g_q.shadow_scale = uint32_t(std::clamp(settings::Int("shadow_scale", 2), 1, 8));
    // ---- PC shadows: the sun's shadow map covers shadow_distance x the console's square (xdk/shadows.cpp); its
    // resolution grows alike so a texel stays as large as at shadow_scale (up to 8192x8192: 8x the console's) ----
    if (float k = pcmode::ShadowDistance(); k != 1.0f) {
        const uint32_t density = g_q.shadow_scale;
        g_q.shadow_scale = uint32_t(std::clamp(std::lround(density * k), 1l, 8l));
        rt::Log("render: shadow distance x%.2f: shadow maps %ux (texels %.2fx the size at shadow_scale %u)", k,
                g_q.shadow_scale, density * k / g_q.shadow_scale, density);
    }
    {
        std::string filter = settings::String("shadow_filter", "soft");
        psh::SetShadowFilter(filter == "soft" || filter == "1" ? 1 : 0);
        rt::Log("render: shadow filter %s", filter == "soft" || filter == "1" ? "soft (4x4 texels)" : "sharp (2x2 texels, as the NV2A)");
    }
    // ---- end PC shadows ----
    g_q.aniso = uint32_t(std::clamp(settings::Int("aniso", 16), 1, 16));
    g_reversed_depth = settings::String("w_depth", "reversed") != "shader";  // performance
    rt::Log("render: depth %s", g_reversed_depth ? "reversed (c / d, the rasterizer's for W-buffering)"
                                                 : "as the guest's, W-buffering from the pixel shader");
    uint32_t msaa = uint32_t(std::clamp(settings::Int("msaa", 4), 1, 8));
    while (msaa > 1) {  // the most the GPU does at or below the setting, a power of two
        UINT quality = 0;
        if (Ready() && (msaa & (msaa - 1)) == 0 &&
            SUCCEEDED(g_device->CheckMultisampleQualityLevels(DXGI_FORMAT_B8G8R8A8_UNORM, msaa, &quality)) && quality &&
            SUCCEEDED(g_device->CheckMultisampleQualityLevels(DXGI_FORMAT_R32G8X24_TYPELESS, msaa, &quality)) && quality)
            break;
        msaa--;
    }
    g_q.msaa = msaa;
    g_q.wide = pcmode::Widen();  // PC features: widescreen
    g_count_divisor = uint64_t(double(g_q.scale) * g_q.scale * g_q.msaa * g_q.wide + 0.5);
    rt::Log("render: HLE graphics at %.4gx the console's resolution (%u lines), %ux MSAA, %ux anisotropic filtering, "
            "shadow maps %ux",
            g_q.scale, HostHeightOf(480, g_q.scale), g_q.msaa, g_q.aniso, g_q.shadow_scale);
    if (g_q.wide != 1.0f)
        rt::Log("render: widescreen: frame buffer sized targets %.3fx wider (%ux%u for 640x480)", g_q.wide,
                HostWidthOf(640, g_q.scale, g_q.wide), HostHeightOf(480, g_q.scale));
}


void Present(uint32_t front) {
    if (!Ready()) return;
    Target* shown = nullptr;
    for (auto& t : g_targets)
        if (front && t->address == front && !t->swizzled && (!shown || t->last_write > shown->last_write))
            shown = t.get();
    ID3D11ShaderResourceView* srv = nullptr;
    uint32_t w = 0, h = 0;
    if (shown) {
        srv = Resolved(*shown);
        w = shown->HostWidth(), h = shown->HostHeight();
    }
    g_context->OMSetRenderTargets(0, nullptr, nullptr);
    // PC features: a frame with the widened 3D view is shown at its shape, others (menus, videos) at 4:3
    // fmv_scale (gpu/fmv.h): so are frames of a full screen video in fill mode (the 2D over it narrowed)
    bool video = fmv::Fill() && (g_frame_video || g_last_video || gpu::OverlayActive()) &&
                 (!shown || shown->wide != 1.0f);
    g_last_video = g_frame_video;
    g_before_video.clear();
    float aspect = (shown && shown->wide != 1.0f && g_frame_3d) || video ? pcmode::Aspect() : 4.0f / 3.0f;
    g_frame_3d = false;
    g_frame_video = false;
    g_post_3d = false;
    AoPresent();  // ambient occlusion
    g_water_presents++;  // PC water
    if (g_gpu_timing.enabled) g_gpu_timing.Begin(Pass::kPost);  // performance: FCI_GPU_TIMING
    g_ds.Invalidate();  // performance: state cache
    // performance: shaders off the render thread; a long frame with draws left out is not shown
    if (ShadersPresent()) present::Frame(g_device.Get(), g_context.Get(), srv, w, h, aspect);
    g_gpu_timing.FrameEnd();
    texpack::Frame();  // ---- texture dump/replacement: notices changes to the replacement folder ----
    Flush();  // a new generation: the texture caches look at guest memory again
}

bool SyncToGuest(uint32_t address, bool late) {
    if (!g_init_done || g_failed) return false;
    bool any = false;
    for (auto& t : g_targets)
        if (t->dirty && t->Overlaps(address, 1)) {
            if (late) {
                LateSync(*t);
            } else {
                CountDownload(*t, "cpu", address);
                Download(*t);
            }
            any = true;
        }
    return any;
}

void SetGammaRamp(const uint8_t ramp[768]) { present::SetGamma(ramp); }

std::recursive_mutex& ContextLock() {
    static std::recursive_mutex lock;
    return lock;
}


long CompileHlsl(const void* source, size_t size, const char* name, const void* defines, void* include,
                 const char* entry, const char* target, unsigned flags1, unsigned flags2, ID3D10Blob** code,
                 ID3D10Blob** errors) {
    HRESULT hr = D3DCompile(source, size, name, static_cast<const D3D_SHADER_MACRO*>(defines),
                            static_cast<ID3DInclude*>(include), entry, target, flags1, flags2, code, errors);
    if (FAILED(hr)) {
        rt::Log("render: HLSL %s %s %s: compile failed (%08X)", name ? name : "?", entry ? entry : "?",
                target ? target : "?", unsigned(hr));
        ShowHlslFailure();
    }
    return hr;
}

}  // namespace gpu::render
