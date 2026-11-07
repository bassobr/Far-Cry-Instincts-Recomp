// Texture dumping and replacement, see texture_pack.h.
// Adapted from SCDA-XBERecomp (MIT, Copyright (c) 2026 TGP482, Joshhhuaaa), src/gpu/texturedumper.cpp: names by
// hash, the worker, DDS reading/writing, PNG through WIC, box filtered mipmaps and the folder watch. Here on
// Direct3D 11 (the worker creates the replacement textures itself, the device is free threaded), with cube maps and
// rect textures, and keyed from the NV2A guest layout.
#include "gpu/texture_pack.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "app/settings.h"
#include "gpu/nv2a.h"
#include "runtime/log.h"

namespace gpu::texpack {

namespace {

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

constexpr uint32_t kMaxSide = 16384;                       // D3D 11's 2D texture limit
constexpr size_t kMaxQueuedDumpBytes = 512u << 20;         // dumps waiting for the worker; more are skipped
constexpr auto kSettle = std::chrono::milliseconds(500);  // files still being copied in
constexpr DWORD kChanges = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE |
                           FILE_NOTIFY_CHANGE_SIZE;

// a loaded replacement: rows tightly packed (block rows for BC), face major
struct Level {
    std::vector<uint8_t> px;
    uint32_t pitch = 0;
};
struct Image {
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0, height = 0, faces = 1, levels = 0;
    std::vector<Level> data;  // faces x levels
};

struct Ready {
    ComPtr<ID3D11ShaderResourceView> srv;  // null: the file did not load
    bool cube = false;
};

struct State {
    bool init = false, dumping = false, replacing = false;
    fs::path dump_dir, replace_dir;
    ID3D11Device* device = nullptr;
    // the worker
    std::once_flag worker;
    std::mutex lock;  // everything below
    std::condition_variable wake;
    std::deque<std::function<void()>> loads, dumps;  // loads first
    size_t queued_dump_bytes = 0;
    std::unordered_set<uint64_t> dumped;               // dumped in this or an earlier run
    std::unordered_map<uint64_t, fs::path> files;      // replacements by the key in their names
    std::unordered_set<uint64_t> asked;
    std::unordered_map<uint64_t, Ready> ready;
    std::atomic<uint32_t> revision{1};  // read by the render thread without the lock
    // the render thread's: the replacement folder's change notification
    HANDLE watch = INVALID_HANDLE_VALUE;
    uint32_t watch_tries = 0;
    std::optional<Clock::time_point> changed;
    // the render thread's: time spent hashing and handing over dumps, in this frame and the worst in 10 s
    double cost = 0, worst = 0;
    uint32_t posted = 0;
};


State g;

// adds the time of its scope to g.cost
struct Cost {
    Clock::time_point start = Clock::now();
    ~Cost() { g.cost += std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }
};

std::string Utf8(const fs::path& p) {
    std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

// ---- worker ----

void Work() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock l(g.lock);
            g.wake.wait(l, [] { return !g.loads.empty() || !g.dumps.empty(); });
            auto& queue = g.loads.empty() ? g.dumps : g.loads;
            job = std::move(queue.front());
            queue.pop_front();
        }
        job();
    }
}

// lock held by the caller
void PostLocked(std::function<void()> job, bool load) {
    std::call_once(g.worker, [] { std::thread(Work).detach(); });
    (load ? g.loads : g.dumps).push_back(std::move(job));
    g.wake.notify_one();
}

// ---- names ----

// a 64 bit hash (one lane of XXH64), stable across runs and machines
constexpr uint64_t kP1 = 0x9E3779B185EBCA87ull, kP2 = 0xC2B2AE3D27D4EB4Full, kP3 = 0x165667B19E3779F9ull,
                   kP4 = 0x85EBCA77C2B2AE63ull, kP5 = 0x27D4EB2F165667C5ull;
uint64_t Rotl(uint64_t v, int r) { return (v << r) | (v >> (64 - r)); }
uint64_t HashBytes(const uint8_t* p, size_t n, uint64_t h) {
    h += kP5 + n;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        h ^= Rotl(w * kP2, 31) * kP1;
        h = Rotl(h, 27) * kP1 + kP4;
    }
    for (; i < n; i++) h = Rotl(h ^ (p[i] * kP5), 11) * kP1;
    h ^= h >> 33, h *= kP2, h ^= h >> 29, h *= kP3, h ^= h >> 32;
    return h;
}

// the 16 hex digits in a name such as 0123456789abcdef or 0123456789abcdef_sky, 0 without
uint64_t KeyInName(const std::wstring& stem) {
    size_t start = 0;
    while (start < stem.size()) {
        size_t end = std::min(stem.find_first_of(L"_-. ", start), stem.size());
        std::wstring part = stem.substr(start, end - start);
        if (part.size() == 16 && part.find_first_not_of(L"0123456789abcdefABCDEF") == std::wstring::npos)
            return std::wcstoull(part.c_str(), nullptr, 16);
        start = end + 1;
    }
    return 0;
}

bool IsImage(const fs::path& p) {
    return _wcsicmp(p.extension().c_str(), L".png") == 0 || _wcsicmp(p.extension().c_str(), L".dds") == 0;
}

std::unordered_map<uint64_t, fs::path> List(const fs::path& dir) {
    std::unordered_map<uint64_t, fs::path> files;
    std::error_code ec;
    fs::recursive_directory_iterator it(dir, ec);
    for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const fs::path& p = it->path();
        if (!IsImage(p)) continue;
        if (uint64_t key = KeyInName(p.stem().wstring())) files.emplace(key, p);
    }
    return files;
}

void Init() {
    if (g.init) return;
    g.init = true;
    g.dumping = settings::Bool("texture_dump", false);
    g.replacing = settings::Bool("texture_replace", false);
    if (!g.dumping && !g.replacing) return;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    fs::path root = fs::path(exe).parent_path() / "textures";
    g.dump_dir = root / "dump";
    g.replace_dir = root / "replace";
    std::error_code ec;
    if (g.dumping) {
        fs::create_directories(g.dump_dir, ec);
        for (auto& [key, path] : List(g.dump_dir)) g.dumped.insert(key);
        rt::Log("texture pack: dumping into %s (%zu there already)", Utf8(g.dump_dir).c_str(), g.dumped.size());
    }
    if (g.replacing) {
        fs::create_directories(g.replace_dir, ec);
        g.files = List(g.replace_dir);
        rt::Log("texture pack: %zu replacement images in %s", g.files.size(), Utf8(g.replace_dir).c_str());
    }
}

// ---- DDS ----

constexpr uint32_t FourCC(const char (&s)[5]) {
    return uint32_t(uint8_t(s[0])) | uint32_t(uint8_t(s[1])) << 8 | uint32_t(uint8_t(s[2])) << 16 |
           uint32_t(uint8_t(s[3])) << 24;
}
// "DDS " then DDS_HEADER (byte offsets from the file start); DDS_HEADER_DXT10 follows for FourCC DX10
constexpr size_t kDdsFlags = 8, kDdsHeight = 12, kDdsWidth = 16, kDdsPitch = 20, kDdsMipCount = 28,
                 kDdsPixelSize = 76, kDdsPixelFlags = 80, kDdsFourCC = 84, kDdsBitCount = 88, kDdsRedMask = 92,
                 kDdsGreenMask = 96, kDdsBlueMask = 100, kDdsAlphaMask = 104, kDdsCaps = 108, kDdsCaps2 = 112,
                 kDdsHeaderBytes = 128, kDx10Format = 128, kDx10Misc = 136, kDx10ArraySize = 140,
                 kDx10HeaderBytes = 148;
constexpr uint32_t kDdpfAlphaPixels = 0x1, kDdpfFourCC = 0x4, kDdpfRgb = 0x40;
constexpr uint32_t kCaps2Cube = 0x200, kCaps2AllFaces = 0xFC00;

uint32_t Read32(const std::vector<uint8_t>& b, size_t at) {
    uint32_t v;
    std::memcpy(&v, &b[at], 4);
    return v;
}
void Write32(std::vector<uint8_t>& b, size_t at, uint32_t v) { std::memcpy(&b[at], &v, 4); }

bool Uncompressed(DXGI_FORMAT f) { return f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM; }

// bytes per row (block row) and rows; pitch 0 for a format not supported
void Shape(DXGI_FORMAT f, uint32_t w, uint32_t h, uint32_t& pitch, uint32_t& rows) {
    uint32_t bx = std::max(1u, (w + 3) / 4), by = std::max(1u, (h + 3) / 4);
    switch (f) {
    case DXGI_FORMAT_BC1_UNORM: pitch = bx * 8, rows = by; return;
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC7_UNORM: pitch = bx * 16, rows = by; return;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM: pitch = w * 4, rows = h; return;
    default: pitch = rows = 0; return;
    }
}

uint32_t FullChain(uint32_t w, uint32_t h) {
    uint32_t n = 1;
    while ((std::max(w, h) >> n) > 0) n++;
    return n;
}

bool ReadDds(const fs::path& path, Image& img) {
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> f{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (f.size() < kDdsHeaderBytes || Read32(f, 0) != FourCC("DDS ")) return false;
    size_t at = kDdsHeaderBytes;
    uint32_t flags = Read32(f, kDdsPixelFlags), cc = Read32(f, kDdsFourCC);
    img.faces = (Read32(f, kDdsCaps2) & kCaps2Cube) ? 6 : 1;
    if (img.faces == 6 && (Read32(f, kDdsCaps2) & kCaps2AllFaces) != kCaps2AllFaces) return false;
    if (flags & kDdpfFourCC) {
        if (cc == FourCC("DXT1")) img.format = DXGI_FORMAT_BC1_UNORM;
        else if (cc == FourCC("DXT2") || cc == FourCC("DXT3")) img.format = DXGI_FORMAT_BC2_UNORM;
        else if (cc == FourCC("DXT4") || cc == FourCC("DXT5")) img.format = DXGI_FORMAT_BC3_UNORM;
        else if (cc == FourCC("DX10") && f.size() >= kDx10HeaderBytes) {
            at = kDx10HeaderBytes;
            img.format = DXGI_FORMAT(Read32(f, kDx10Format));
            if (img.format == DXGI_FORMAT_BC1_UNORM_SRGB) img.format = DXGI_FORMAT_BC1_UNORM;
            if (img.format == DXGI_FORMAT_BC2_UNORM_SRGB) img.format = DXGI_FORMAT_BC2_UNORM;
            if (img.format == DXGI_FORMAT_BC3_UNORM_SRGB) img.format = DXGI_FORMAT_BC3_UNORM;
            if (img.format == DXGI_FORMAT_BC7_UNORM_SRGB) img.format = DXGI_FORMAT_BC7_UNORM;
            if (img.format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) img.format = DXGI_FORMAT_B8G8R8A8_UNORM;
            if (img.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) img.format = DXGI_FORMAT_R8G8B8A8_UNORM;
            if (Read32(f, kDx10Misc) & 4) img.faces = 6 * std::max(1u, Read32(f, kDx10ArraySize));  // TEXTURECUBE
            if (img.faces != 1 && img.faces != 6) return false;
        }
    } else if ((flags & kDdpfRgb) && Read32(f, kDdsBitCount) == 32) {
        uint32_t red = Read32(f, kDdsRedMask);
        img.format = red == 0x00FF0000 ? DXGI_FORMAT_B8G8R8A8_UNORM
                     : red == 0x000000FF ? DXGI_FORMAT_R8G8B8A8_UNORM
                                         : DXGI_FORMAT_UNKNOWN;
    }
    img.width = Read32(f, kDdsWidth), img.height = Read32(f, kDdsHeight);
    uint32_t pitch, rows;
    Shape(img.format, 1, 1, pitch, rows);
    if (!pitch || !img.width || !img.height || img.width > kMaxSide || img.height > kMaxSide) return false;
    uint32_t mips = Read32(f, kDdsFlags) & 0x20000 ? Read32(f, kDdsMipCount) : 1;
    img.levels = std::clamp(mips, 1u, FullChain(img.width, img.height));
    for (uint32_t face = 0; face < img.faces; face++)
        for (uint32_t l = 0; l < img.levels; l++) {
            Shape(img.format, std::max(img.width >> l, 1u), std::max(img.height >> l, 1u), pitch, rows);
            size_t bytes = size_t(pitch) * rows;
            if (at + bytes > f.size()) return false;
            img.data.push_back({std::vector<uint8_t>(f.begin() + at, f.begin() + at + bytes), pitch});
            at += bytes;
        }
    // X8R8G8B8 and the like: alpha left undefined
    if (Uncompressed(img.format) && !(flags & kDdpfFourCC) && !(flags & kDdpfAlphaPixels))
        for (Level& l : img.data)
            for (size_t i = 3; i < l.px.size(); i += 4) l.px[i] = 0xFF;
    return true;
}

// BGRA8, faces x levels of texture::Image
bool WriteDds(const fs::path& path, const std::vector<texture::Image>& images, uint32_t faces) {
    const texture::Image& top = images[0];
    uint32_t levels = uint32_t(images.size() / faces);
    std::vector<uint8_t> h(kDdsHeaderBytes);
    Write32(h, 0, FourCC("DDS "));
    Write32(h, 4, 124);
    Write32(h, kDdsFlags, 0x1007 | 0x8 | (levels > 1 ? 0x20000 : 0));  // caps, size, format, pitch, mip count
    Write32(h, kDdsHeight, top.height);
    Write32(h, kDdsWidth, top.width);
    Write32(h, kDdsPitch, top.width * 4);
    Write32(h, kDdsMipCount, levels);
    Write32(h, kDdsPixelSize, 32);
    Write32(h, kDdsPixelFlags, kDdpfRgb | kDdpfAlphaPixels);
    Write32(h, kDdsBitCount, 32);
    Write32(h, kDdsRedMask, 0x00FF0000);
    Write32(h, kDdsGreenMask, 0x0000FF00);
    Write32(h, kDdsBlueMask, 0x000000FF);
    Write32(h, kDdsAlphaMask, 0xFF000000);
    Write32(h, kDdsCaps, 0x1000 | (levels > 1 || faces > 1 ? 0x8 : 0) | (levels > 1 ? 0x400000 : 0));
    Write32(h, kDdsCaps2, faces == 6 ? kCaps2Cube | kCaps2AllFaces : 0);
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(h.data()), std::streamsize(h.size()));
    for (const texture::Image& img : images)
        out.write(reinterpret_cast<const char*>(img.pixels.data()), std::streamsize(img.pixels.size() * 4));
    return bool(out);
}

// ---- PNG through WIC ----

IWICImagingFactory* Wic() {
    static ComPtr<IWICImagingFactory> wic;  // the worker's
    if (!wic) CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    return wic.Get();
}

bool WritePng(const fs::path& path, const texture::Image& img) {
    IWICImagingFactory* wic = Wic();
    ComPtr<IWICBitmap> bitmap;
    ComPtr<IWICStream> file;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    HRESULT hr = wic ? S_OK : E_FAIL;
    if (SUCCEEDED(hr))
        hr = wic->CreateBitmapFromMemory(img.width, img.height, GUID_WICPixelFormat32bppBGRA, img.width * 4,
                                         UINT(img.pixels.size() * 4),
                                         reinterpret_cast<BYTE*>(const_cast<uint32_t*>(img.pixels.data())), &bitmap);
    if (SUCCEEDED(hr)) hr = wic->CreateStream(&file);
    if (SUCCEEDED(hr)) hr = file->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(file.Get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
    if (SUCCEEDED(hr)) hr = frame->SetSize(img.width, img.height);
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
    if (SUCCEEDED(hr)) hr = frame->WriteSource(bitmap.Get(), nullptr);
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    return SUCCEEDED(hr);
}

bool ReadPng(const fs::path& path, Image& img) {
    IWICImagingFactory* wic = Wic();
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapSource> bgra;
    HRESULT hr = wic ? S_OK : E_FAIL;
    if (SUCCEEDED(hr))
        hr = wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                            &decoder);
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(hr)) hr = WICConvertBitmapSource(GUID_WICPixelFormat32bppBGRA, frame.Get(), &bgra);
    UINT w = 0, h = 0;
    if (SUCCEEDED(hr)) hr = bgra->GetSize(&w, &h);
    if (FAILED(hr) || !w || !h || w > kMaxSide || h > kMaxSide) return false;
    Level top{std::vector<uint8_t>(size_t(w) * h * 4), w * 4};
    if (FAILED(bgra->CopyPixels(nullptr, top.pitch, UINT(top.px.size()), top.px.data()))) return false;
    img.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    img.width = w, img.height = h, img.faces = 1, img.levels = 1;
    img.data.push_back(std::move(top));
    return true;
}

// ---- mipmaps ----

// one 2x2 box of src, its far row and column clamped to the level's edge
void AverageBox(const Level& src, uint32_t w, uint32_t h, uint32_t x, uint32_t y, uint8_t* out) {
    const uint8_t* row0 = &src.px[size_t(y * 2) * src.pitch];
    const uint8_t* row1 = &src.px[size_t(std::min(y * 2 + 1, h - 1)) * src.pitch];
    uint32_t x0 = x * 2 * 4, x1 = std::min(x * 2 + 1, w - 1) * 4;
    for (uint32_t c = 0; c < 4; c++) out[c] = uint8_t((row0[x0 + c] + row0[x1 + c] + row1[x0 + c] + row1[x1 + c] + 2) / 4);
}

// an image with level 0 alone gets a box filtered chain, so a bigger image does not shimmer at a distance
void AddMipmaps(Image& img) {
    if (img.levels > 1 || img.faces != 1 || !Uncompressed(img.format)) return;
    uint32_t w = img.width, h = img.height;
    while (w > 1 || h > 1) {
        uint32_t hw = std::max(w / 2, 1u), hh = std::max(h / 2, 1u);
        Level next{std::vector<uint8_t>(size_t(hw) * hh * 4), hw * 4};
        const Level& prev = img.data.back();
        for (uint32_t y = 0; y < hh; y++)
            for (uint32_t x = 0; x < hw; x++) AverageBox(prev, w, h, x, y, &next.px[size_t(y) * next.pitch + x * 4]);
        img.data.push_back(std::move(next));
        img.levels++;
        w = hw, h = hh;
    }
}

// ---- loading ----

ComPtr<ID3D11ShaderResourceView> Upload(const Image& img) {
    std::vector<D3D11_SUBRESOURCE_DATA> data(img.data.size());
    for (size_t i = 0; i < img.data.size(); i++)
        data[i] = {img.data[i].px.data(), img.data[i].pitch, UINT(img.data[i].px.size())};
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = img.width, desc.Height = img.height, desc.MipLevels = img.levels, desc.ArraySize = img.faces;
    desc.Format = img.format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = img.faces == 6 ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    if (SUCCEEDED(g.device->CreateTexture2D(&desc, data.data(), &tex)))
        g.device->CreateShaderResourceView(tex.Get(), nullptr, &srv);
    return srv;
}

void Load(uint64_t key, const fs::path& path, uint32_t revision) {
    auto start = Clock::now();
    Image img;
    bool dds = _wcsicmp(path.extension().c_str(), L".dds") == 0;
    bool ok = dds ? ReadDds(path, img) : ReadPng(path, img);
    // a BC texture's top level is whole blocks
    if (ok && !Uncompressed(img.format) && ((img.width | img.height) & 3)) ok = false;
    Ready r;
    if (ok) {
        AddMipmaps(img);
        r.srv = Upload(img);
        r.cube = img.faces == 6;
    }
    double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    if (r.srv)
        rt::Log("texture pack: %016llx replaced by %s (%ux%u%s, %u levels, %.1f ms)", (unsigned long long)key,
                Utf8(path.filename()).c_str(), img.width, img.height, r.cube ? " cube" : "", img.levels, ms);
    else
        rt::Log("texture pack: %s did not load", Utf8(path).c_str());
    std::lock_guard l(g.lock);
    if (revision == g.revision) g.ready[key] = std::move(r);
}

}  // namespace

bool Dumping() {
    Init();
    return g.dumping;
}

bool Replacing() {
    Init();
    return g.replacing;
}

uint64_t Key(const texture::Desc& d) {
    if (!Active()) return 0;
    Cost cost;
    return ContentKey(d);
}

uint64_t ContentKey(const texture::Desc& d) {
    switch (d.color) {
    case 0x24: case 0x25:                        // YUY2/UYVY: video frames
    case 0x2A: case 0x2C: case 0x2E: case 0x30:  // depth
        return 0;
    }
    texture::Format f = texture::Lookup(d.color);
    if (!f.supported || d.depth > 1 || !d.width || !d.height) return 0;
    if (f.layout == texture::Layout::kLinear && d.pitch < d.width * f.bytes) return 0;
    const uint32_t shape[4] = {d.color, d.width, d.height, d.cube ? 1u : 0u};
    uint64_t key = HashBytes(reinterpret_cast<const uint8_t*>(shape), sizeof shape, 0);
    const uint8_t* base = Physical(d.address);
    if (f.layout == texture::Layout::kLinear) {
        // the rows' texels only: what lies in the pitch beyond the width is not the texture's
        for (uint32_t y = 0; y < d.height; y++) key = HashBytes(base + size_t(y) * d.pitch, size_t(d.width) * f.bytes, key);
    } else {
        uint32_t level0 = f.layout == texture::Layout::kCompressed
                              ? std::max(1u, (d.width + 3) / 4) * std::max(1u, (d.height + 3) / 4) * f.bytes
                              : d.width * d.height * f.bytes;
        texture::Desc face = d;
        face.cube = false;
        uint32_t stride = (texture::GuestSize(face) + 127) & ~127u;  // cube faces start 128 byte aligned
        for (uint32_t i = 0; i < (d.cube ? 6u : 1u); i++) key = HashBytes(base + size_t(i) * stride, level0, key);
    }
    if (d.palette_entries) key = HashBytes(Physical(d.palette), d.palette_entries * 4, key);
    return key ? key : 1;
}

bool WantsDump(uint64_t key) {
    if (!key || !Dumping()) return false;
    std::lock_guard l(g.lock);
    return !g.files.count(key) && !g.dumped.count(key);
}

void Dump(uint64_t key, const texture::Desc& d, std::vector<texture::Image>&& images) {
    if (!key || !Dumping() || images.empty()) return;
    Cost cost;
    std::lock_guard l(g.lock);
    if (g.files.count(key) || g.dumped.count(key)) return;
    uint32_t faces = d.cube ? 6 : 1;
    if (!d.cube) images.resize(1);  // PNG: level 0
    size_t bytes = 0;
    for (auto& img : images) bytes += img.pixels.size() * 4;
    if (g.queued_dump_bytes + bytes > kMaxQueuedDumpBytes) return;  // tried again when the texture comes back
    g.dumped.insert(key);
    g.queued_dump_bytes += bytes;
    g.posted++;
    char name[32];
    std::snprintf(name, sizeof name, "%016llx.%s", (unsigned long long)key, d.cube ? "dds" : "png");
    fs::path path = g.dump_dir / name;
    // what the texture is on the console, for textures\dump\index.txt
    texture::Format f = texture::Lookup(d.color);
    char info[160];
    std::snprintf(info, sizeof info, "%016llx  %ux%u  format %02X %s  %u level%s%s%s\n", (unsigned long long)key,
                  d.width, d.height, d.color,
                  f.layout == texture::Layout::kLinear       ? "linear (rect)"
                  : f.layout == texture::Layout::kCompressed ? "DXT"
                                                             : "swizzled",
                  d.levels, d.levels == 1 ? "" : "s", d.cube ? "  cube" : "", d.palette_entries ? "  palette" : "");
    PostLocked(
        [path, faces, bytes, info = std::string(info), images = std::move(images)] {
            // written under a temporary name: a dump cut off by quitting is not taken for a whole one
            fs::path part = path;
            part += L".part";
            bool ok = faces == 6 ? WriteDds(part, images, faces) : WritePng(part, images[0]);
            std::error_code ec;
            if (ok) fs::rename(part, path, ec);
            if (!ok || ec) {
                fs::remove(part, ec);
                rt::Log("texture pack: %s not written", Utf8(path).c_str());
            } else {
                std::ofstream index(path.parent_path() / "index.txt", std::ios::app);
                index << info;
            }
            std::lock_guard l(g.lock);
            g.queued_dump_bytes -= bytes;
        },
        false);
}

Lookup Find(ID3D11Device* device, uint64_t key, bool cube, ComPtr<ID3D11ShaderResourceView>& out) {
    if (!key || !Replacing()) return Lookup::kNone;
    std::lock_guard l(g.lock);
    auto file = g.files.find(key);
    if (file == g.files.end()) return Lookup::kNone;
    auto it = g.ready.find(key);
    if (it != g.ready.end()) {
        if (!it->second.srv) return Lookup::kNone;
        if (it->second.cube != cube) {
            static int logged = 0;
            if (logged++ < 8)
                rt::Log("texture pack: %016llx is sampled as a %s, the replacement is not one", (unsigned long long)key,
                        cube ? "cube map" : "2D texture");
            return Lookup::kNone;
        }
        out = it->second.srv;
        return Lookup::kReady;
    }
    if (!g.asked.insert(key).second) return Lookup::kLoading;
    g.device = device;
    PostLocked([key, path = file->second, revision = g.revision.load()] { Load(key, path, revision); }, true);
    return Lookup::kLoading;
}

uint32_t Revision() { return g.revision; }

void Frame() {
    if (!Active()) return;
    g.worst = std::max(g.worst, g.cost);
    g.cost = 0;
    static Clock::time_point next = Clock::now() + std::chrono::seconds(10);
    if (Clock::now() >= next) {
        next = Clock::now() + std::chrono::seconds(10);
        size_t queued, ready;
        {
            std::lock_guard l(g.lock);
            queued = g.queued_dump_bytes, ready = g.ready.size();
        }
        rt::Log("texture pack: %u textures handed to the dump, %zu KB waiting, %zu replacements ready; render "
                "thread cost at most %.2f ms in a frame", g.posted, queued >> 10, ready, g.worst);
        g.worst = 0;
    }
    if (!Replacing()) return;
    // the folder's change handle, made again now and then while it fails
    if (g.watch == INVALID_HANDLE_VALUE) {
        if (g.watch_tries++ % 120) return;
        g.watch = FindFirstChangeNotificationW(g.replace_dir.c_str(), TRUE, kChanges);
        return;
    }
    Clock::time_point now = Clock::now();
    if (WaitForSingleObject(g.watch, 0) == WAIT_OBJECT_0) {
        g.changed = now;
        if (!FindNextChangeNotification(g.watch)) {
            FindCloseChangeNotification(g.watch);
            g.watch = INVALID_HANDLE_VALUE;
        }
    }
    if (!g.changed || now - *g.changed < kSettle) return;
    g.changed.reset();
    // listed on the worker; the new revision makes the renderer look its replacements up again
    std::lock_guard l(g.lock);
    PostLocked(
        [] {
            auto files = List(g.replace_dir);
            std::lock_guard l(g.lock);
            g.files = std::move(files);
            g.asked.clear();
            g.ready.clear();
            g.revision++;
            rt::Log("texture pack: replacement folder changed, %zu images", g.files.size());
        },
        true);
}

}  // namespace gpu::texpack
