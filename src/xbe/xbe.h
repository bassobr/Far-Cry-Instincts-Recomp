// XBE (original Xbox executable) loader: header, sections, kernel imports and the mapped image.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace xbe {

enum SectionFlags : uint32_t {
    kWritable = 1,
    kPreload = 2,
    kExecutable = 4,
};

struct Section {
    std::string name;
    uint32_t flags = 0;
    uint32_t va = 0;
    uint32_t vsize = 0;
    uint32_t raw = 0;
    uint32_t rsize = 0;

    bool Contains(uint32_t addr) const { return addr >= va && addr < va + vsize; }
};

struct Library {
    std::string name;
    uint16_t major = 0, minor = 0, build = 0, flags = 0;
};

class Image {
public:
    bool Load(const std::string& path, std::string* error);
    // from the file's bytes (read from a disc image); path only names it in errors
    bool Load(std::vector<uint8_t> file, const std::string& path, std::string* error);

    uint32_t base = 0;
    uint32_t image_size = 0;
    uint32_t entry = 0;
    uint32_t kernel_thunk = 0;
    uint32_t tls = 0;
    uint32_t title_id = 0;
    bool retail = true;
    std::string title;
    std::vector<Section> sections;
    std::vector<Library> libraries;
    std::vector<uint32_t> kernel_imports;  // ordinals, in thunk table order

    // memory as the Xbox maps it, indexed from `base`
    std::vector<uint8_t> memory;

    bool Mapped(uint32_t va, uint32_t len = 1) const {
        return va >= base && uint64_t(va) - base + len <= memory.size();
    }
    const uint8_t* At(uint32_t va) const { return memory.data() + (va - base); }
    uint32_t Read32(uint32_t va) const {
        uint32_t v;
        std::memcpy(&v, At(va), 4);
        return v;
    }
    const Section* SectionAt(uint32_t va) const;
    const Section* FindSection(const std::string& name) const;

private:
    std::vector<uint8_t> file_;
};

}  // namespace xbe
