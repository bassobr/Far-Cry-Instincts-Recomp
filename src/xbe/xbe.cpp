#include "xbe/xbe.h"

#include <cstring>
#include <fstream>
#include <iterator>

namespace xbe {

namespace {

constexpr uint32_t kRetailEntryKey = 0xA8FC57AB;
constexpr uint32_t kDebugEntryKey = 0x94859D4B;
constexpr uint32_t kRetailThunkKey = 0x5B6D40B6;
constexpr uint32_t kDebugThunkKey = 0xEFB1F152;

template <typename T>
T Get(const std::vector<uint8_t>& b, size_t off) {
    T v{};
    if (off + sizeof(T) <= b.size()) std::memcpy(&v, b.data() + off, sizeof(T));
    return v;
}

}  // namespace

bool Image::Load(const std::string& path, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        *error = "cannot open " + path;
        return false;
    }
    return Load(std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {}), path, error);
}

bool Image::Load(std::vector<uint8_t> file, const std::string& path, std::string* error) {
    file_ = std::move(file);
    if (file_.size() < 0x178 || std::memcmp(file_.data(), "XBEH", 4) != 0) {
        *error = path + " is not an XBE";
        return false;
    }

    base = Get<uint32_t>(file_, 0x104);
    uint32_t header_size = Get<uint32_t>(file_, 0x108);
    image_size = Get<uint32_t>(file_, 0x10C);
    memory.assign(image_size, 0);
    std::memcpy(memory.data(), file_.data(), std::min<size_t>(header_size, std::min(file_.size(), memory.size())));

    // header pointers are virtual addresses inside the mapped headers
    auto header_off = [&](uint32_t va) { return size_t(va - base); };

    uint32_t section_count = Get<uint32_t>(file_, 0x11C);
    size_t sh = header_off(Get<uint32_t>(file_, 0x120));
    for (uint32_t i = 0; i < section_count; i++) {
        size_t s = sh + i * 56;
        Section sec;
        sec.flags = Get<uint32_t>(file_, s);
        sec.va = Get<uint32_t>(file_, s + 4);
        sec.vsize = Get<uint32_t>(file_, s + 8);
        sec.raw = Get<uint32_t>(file_, s + 12);
        sec.rsize = Get<uint32_t>(file_, s + 16);
        sec.name = reinterpret_cast<const char*>(file_.data() + header_off(Get<uint32_t>(file_, s + 20)));
        if (sec.va < base || uint64_t(sec.va) - base + sec.vsize > memory.size() ||
            uint64_t(sec.raw) + sec.rsize > file_.size()) {
            *error = "section " + sec.name + " out of bounds";
            return false;
        }
        std::memcpy(memory.data() + (sec.va - base), file_.data() + sec.raw, std::min(sec.rsize, sec.vsize));
        sections.push_back(std::move(sec));
    }

    uint32_t entry_raw = Get<uint32_t>(file_, 0x128);
    retail = ((entry_raw ^ kRetailEntryKey) & 0xF0000000) == 0;
    entry = entry_raw ^ (retail ? kRetailEntryKey : kDebugEntryKey);
    kernel_thunk = Get<uint32_t>(file_, 0x158) ^ (retail ? kRetailThunkKey : kDebugThunkKey);
    tls = Get<uint32_t>(file_, 0x12C);

    size_t cert = header_off(Get<uint32_t>(file_, 0x118));
    title_id = Get<uint32_t>(file_, cert + 8);
    for (int i = 0; i < 40; i++) {
        char16_t c = Get<char16_t>(file_, cert + 0xC + i * 2);
        if (!c) break;
        title.push_back(c < 0x80 ? char(c) : '?');
    }

    uint32_t lib_count = Get<uint32_t>(file_, 0x160);
    size_t lv = header_off(Get<uint32_t>(file_, 0x164));
    for (uint32_t i = 0; i < lib_count; i++) {
        size_t l = lv + i * 16;
        Library lib;
        lib.name.assign(reinterpret_cast<const char*>(file_.data() + l), strnlen(reinterpret_cast<const char*>(file_.data() + l), 8));
        lib.major = Get<uint16_t>(file_, l + 8);
        lib.minor = Get<uint16_t>(file_, l + 10);
        lib.build = Get<uint16_t>(file_, l + 12);
        lib.flags = Get<uint16_t>(file_, l + 14);
        libraries.push_back(std::move(lib));
    }

    if (!Mapped(kernel_thunk, 4)) {
        *error = "kernel thunk outside the image";
        return false;
    }
    for (uint32_t va = kernel_thunk; Mapped(va, 4); va += 4) {
        uint32_t v = Read32(va);
        if (!v) break;
        kernel_imports.push_back(v & 0x7FFFFFFF);
    }
    return true;
}

const Section* Image::SectionAt(uint32_t va) const {
    for (const auto& s : sections)
        if (s.Contains(va)) return &s;
    return nullptr;
}

const Section* Image::FindSection(const std::string& name) const {
    for (const auto& s : sections)
        if (s.name == name) return &s;
    return nullptr;
}

}  // namespace xbe
