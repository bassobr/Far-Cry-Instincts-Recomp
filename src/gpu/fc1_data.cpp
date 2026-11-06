#include "gpu/fc1_data.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>

#include "app/settings.h"
#include "runtime/log.h"

namespace gpu::fc1 {

namespace {

bool IsFolder(const std::string& path) {
    DWORD a = GetFileAttributesA(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool IsFile(const std::string& path) {
    DWORD a = GetFileAttributesA(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string ExeFolder() {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string p(path);
    return p.substr(0, p.find_last_of("\\/"));
}

// ---- inflate (RFC 1951), written for the .pak reader ----
struct BitReader {
    const uint8_t* p;
    size_t n, at = 0;
    uint32_t bits = 0;
    int count = 0;
    bool bad = false;
    uint32_t Get(int k) {
        while (count < k) {
            if (at >= n) {
                bad = true;
                return 0;
            }
            bits |= uint32_t(p[at++]) << count;
            count += 8;
        }
        uint32_t v = bits & ((1u << k) - 1);
        bits >>= k, count -= k;
        return v;
    }
};

// canonical Huffman code: counts per length, symbols in order
struct Huffman {
    uint16_t count[16] = {};
    uint16_t symbol[320] = {};
    void Build(const uint8_t* lengths, int n) {
        std::memset(count, 0, sizeof count);
        for (int i = 0; i < n; i++) count[lengths[i]]++;
        count[0] = 0;
        uint16_t offset[16] = {};
        for (int i = 1; i < 16; i++) offset[i] = offset[i - 1] + count[i - 1];
        for (int i = 0; i < n; i++)
            if (lengths[i]) symbol[offset[lengths[i]]++] = uint16_t(i);
    }
    int Decode(BitReader& in) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; len++) {
            code |= int(in.Get(1));
            int c = count[len];
            if (code - c < first) return symbol[index + (code - first)];
            index += c, first += c, first <<= 1, code <<= 1;
            if (in.bad) return -1;
        }
        return -1;
    }
};

const uint16_t kLenBase[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                             35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLenExtra[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                              257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool Inflate(const uint8_t* src, size_t n, std::vector<uint8_t>& out, size_t expected) {
    BitReader in{src, n};
    out.clear();
    out.reserve(expected);
    for (bool last = false; !last;) {
        last = in.Get(1);
        uint32_t type = in.Get(2);
        if (in.bad) return false;
        if (type == 0) {
            in.bits = 0, in.count = 0;  // to the byte boundary
            if (in.at + 4 > n) return false;
            uint32_t len = src[in.at] | src[in.at + 1] << 8;
            in.at += 4;
            if (in.at + len > n) return false;
            out.insert(out.end(), src + in.at, src + in.at + len);
            in.at += len;
            continue;
        }
        if (type == 3) return false;
        Huffman lit, dist;
        uint8_t lengths[320] = {};
        if (type == 1) {
            for (int i = 0; i < 288; i++) lengths[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
            lit.Build(lengths, 288);
            for (int i = 0; i < 30; i++) lengths[i] = 5;
            dist.Build(lengths, 30);
        } else {
            int nlen = int(in.Get(5)) + 257, ndist = int(in.Get(5)) + 1, ncode = int(in.Get(4)) + 4;
            static const uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            uint8_t cl[19] = {};
            for (int i = 0; i < ncode; i++) cl[kOrder[i]] = uint8_t(in.Get(3));
            Huffman code;
            code.Build(cl, 19);
            int i = 0;
            while (i < nlen + ndist) {
                int sym = code.Decode(in);
                if (sym < 0) return false;
                if (sym < 16) {
                    lengths[i++] = uint8_t(sym);
                } else {
                    int repeat = 0;
                    uint8_t value = 0;
                    if (sym == 16) {
                        if (!i) return false;
                        value = lengths[i - 1], repeat = 3 + int(in.Get(2));
                    } else if (sym == 17) {
                        repeat = 3 + int(in.Get(3));
                    } else {
                        repeat = 11 + int(in.Get(7));
                    }
                    if (i + repeat > nlen + ndist) return false;
                    while (repeat--) lengths[i++] = value;
                }
            }
            lit.Build(lengths, nlen);
            dist.Build(lengths + nlen, ndist);
        }
        for (;;) {
            int sym = lit.Decode(in);
            if (sym < 0) return false;
            if (sym < 256) {
                out.push_back(uint8_t(sym));
            } else if (sym == 256) {
                break;
            } else {
                sym -= 257;
                if (sym >= 29) return false;
                size_t len = kLenBase[sym] + in.Get(kLenExtra[sym]);
                int d = dist.Decode(in);
                if (d < 0 || d >= 30) return false;
                size_t back = kDistBase[d] + in.Get(kDistExtra[d]);
                if (back > out.size() || in.bad) return false;
                size_t from = out.size() - back;
                for (size_t k = 0; k < len; k++) out.push_back(out[from + k]);
            }
        }
    }
    return true;
}
// ---- end inflate ----

uint32_t U16(const uint8_t* p) { return p[0] | p[1] << 8; }
uint32_t U32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }

bool SameName(const std::string& a, const char* b) {
    if (a.size() != std::strlen(b)) return false;
    for (size_t i = 0; i < a.size(); i++) {
        char x = a[i] == '\\' ? '/' : a[i], y = b[i] == '\\' ? '/' : b[i];
        if (std::tolower(static_cast<unsigned char>(x)) != std::tolower(static_cast<unsigned char>(y))) return false;
    }
    return true;
}

}  // namespace

const std::string& DataFolder() {
    static const std::string folder = [] {
        std::vector<std::string> tries;
        std::string set = settings::String("fc1_path", "");
        if (!set.empty()) {
            std::string p = set;
            if (!(p.size() > 1 && (p[1] == ':' || p[0] == '\\' || p[0] == '/'))) p = ExeFolder() + "\\" + p;
            tries.push_back(p + "\\FCData");
            tries.push_back(p);
        } else {
            // beside the game's assets\: the exe searches assets\default.xbe upwards, so do we
            std::string dir = ExeFolder();
            for (int up = 0; up < 8 && !dir.empty(); up++) {
                tries.push_back(dir + "\\asset_fc1\\Far Cry\\FCData");
                size_t cut = dir.find_last_of("\\/");
                if (cut == std::string::npos) break;
                dir = dir.substr(0, cut);
            }
        }
        for (const std::string& t : tries)
            if (IsFile(t + "\\Textures.pak") || IsFolder(t + "\\Textures")) {
                rt::Log("fc1: Far Cry (PC) data found in %s", t.c_str());
                return t;
            }
        rt::Log("fc1: no Far Cry (PC) data (%s%s): the water uses its own wave normals",
                set.empty() ? "looked for asset_fc1\\Far Cry beside assets" : "fc1_path = ", set.c_str());
        return std::string();
    }();
    return folder;
}

bool ReadPakFile(const char* pak, const char* name, std::vector<uint8_t>& out) {
    const std::string& folder = DataFolder();
    if (folder.empty()) return false;
    {
        // a loose file beside the paks wins, as in Far Cry itself (FCData\Textures\...)
        std::string loose = folder + "\\" + name;
        std::replace(loose.begin(), loose.end(), '/', '\\');
        std::ifstream l(loose, std::ios::binary);
        if (l) {
            out.assign(std::istreambuf_iterator<char>(l), std::istreambuf_iterator<char>());
            return !out.empty();
        }
    }
    std::ifstream f(folder + "\\" + pak, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const int64_t size = f.tellg();
    // the end of central directory record (no zip64: the paks are below 4 GB)
    const int64_t tail = std::min<int64_t>(size, 65536 + 22);
    std::vector<uint8_t> end(static_cast<size_t>(tail));
    f.seekg(size - tail);
    f.read(reinterpret_cast<char*>(end.data()), tail);
    int64_t eocd = -1;
    for (int64_t i = tail - 22; i >= 0; i--)
        if (U32(&end[size_t(i)]) == 0x06054B50) {
            eocd = i;
            break;
        }
    if (eocd < 0) return false;
    const uint32_t entries = U16(&end[size_t(eocd) + 10]), dir_size = U32(&end[size_t(eocd) + 12]);
    const uint32_t dir_at = U32(&end[size_t(eocd) + 16]);
    std::vector<uint8_t> dir(dir_size);
    f.seekg(dir_at);
    f.read(reinterpret_cast<char*>(dir.data()), dir_size);
    if (!f) return false;
    size_t at = 0;
    for (uint32_t e = 0; e < entries && at + 46 <= dir.size(); e++) {
        const uint8_t* h = &dir[at];
        if (U32(h) != 0x02014B50) return false;
        const uint32_t method = U16(h + 10), csize = U32(h + 20), usize = U32(h + 24);
        const uint32_t nlen = U16(h + 28), xlen = U16(h + 30), clen = U16(h + 32), local = U32(h + 42);
        std::string entry(reinterpret_cast<const char*>(h + 46), std::min<size_t>(nlen, dir.size() - at - 46));
        at += 46 + nlen + xlen + clen;
        if (!SameName(entry, name)) continue;
        uint8_t lh[30];
        f.seekg(local);
        f.read(reinterpret_cast<char*>(lh), 30);
        if (!f || U32(lh) != 0x04034B50) return false;
        f.seekg(int64_t(local) + 30 + U16(lh + 26) + U16(lh + 28));
        std::vector<uint8_t> data(csize);
        f.read(reinterpret_cast<char*>(data.data()), csize);
        if (!f) return false;
        if (method == 0) {
            out = std::move(data);
            return true;
        }
        if (method != 8 || !Inflate(data.data(), data.size(), out, usize) || out.size() != usize) {
            rt::Log("fc1: %s in %s: could not unpack (method %u)", name, pak, method);
            return false;
        }
        return true;
    }
    return false;
}

bool DdsGray(const std::vector<uint8_t>& dds, uint32_t& width, uint32_t& height, std::vector<uint8_t>& gray) {
    if (dds.size() < 128 || U32(dds.data()) != 0x20534444) return false;  // "DDS "
    height = U32(&dds[12]), width = U32(&dds[16]);
    const uint32_t flags = U32(&dds[80]), bits = U32(&dds[88]), rmask = U32(&dds[92]);
    if (!(flags & 0x40) || (bits != 24 && bits != 32) || !width || !height || width > 4096 || height > 4096) return false;
    const size_t bpp = bits / 8, need = 128 + size_t(width) * height * bpp;
    if (dds.size() < need) return false;
    const int shift = rmask == 0xFF0000 ? 2 : rmask == 0xFF ? 0 : rmask == 0xFF00 ? 1 : 2;  // bytes are B G R (A)
    gray.resize(size_t(width) * height);
    for (size_t i = 0; i < gray.size(); i++) gray[i] = dds[128 + i * bpp + size_t(shift)];
    return true;
}

}  // namespace gpu::fc1
