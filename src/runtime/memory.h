// Guest address space: 4 GB reserved at rt::kGuestBase, handed out by region.
#pragma once

#include <cstdint>
#include <map>
#include <mutex>

namespace xbe {
class Image;
}

namespace mem {

// Xbox address ranges the allocators hand out
constexpr uint32_t kVirtualBegin = 0x00010000, kVirtualEnd = 0x7FFF0000;        // NtAllocateVirtualMemory, the XBE
// Physical memory is visible at 0x80000000 + physical address. MmAllocateContiguousMemory hands out
// that range; it ends where the GPU's instance memory (RAMIN, the last MB of the 64 MB) begins.
constexpr uint32_t kPhysicalBase = 0x80000000, kPhysicalSize = 0x04000000;
constexpr uint32_t kRaminBase = kPhysicalBase + kPhysicalSize - 0x100000;
constexpr uint32_t kContiguousBegin = 0x80100000, kContiguousEnd = kRaminBase;
constexpr uint32_t kSystemBegin = 0xD0000000, kSystemEnd = 0xE0000000;          // pool, stacks, kernel objects

// first-fit allocator over one address range; blocks are reserved, pages committed on demand
class Region {
public:
    Region(uint32_t begin, uint32_t end, uint32_t granularity) : begin_(begin), end_(end), granularity_(granularity) {}

    uint32_t Alloc(uint32_t size, uint32_t align = 0, bool commit = true);  // 0 when full
    bool AllocAt(uint32_t addr, uint32_t size, bool commit);
    uint32_t Free(uint32_t addr);  // returns the block size, 0 if `addr` starts no block
    bool Find(uint32_t addr, uint32_t* base, uint32_t* size) const;  // block containing addr
    uint32_t used() const;

private:
    uint32_t begin_, end_, granularity_;
    std::map<uint32_t, uint32_t> blocks_;  // base -> size
    mutable std::mutex lock_;
};

bool Init();
void LoadImage(const xbe::Image& image);
void Commit(uint32_t addr, uint32_t size);
void Decommit(uint32_t addr, uint32_t size);
bool IsCommitted(uint32_t addr);

Region& Virtual();
Region& Contiguous();
Region& System();

// zeroed system memory for kernel structures
uint32_t AllocSystem(uint32_t size);

// host pointer for a physical address (as the GPU and APU see memory)
uint8_t* Physical(uint32_t address);

// Bus master addresses. Only the 64 MB at kPhysicalBase are physical memory here; for buffers elsewhere
// MmGetPhysicalAddress hands out a device address that names the guest memory itself: guest addresses at
// or above 64 MB as they are, lower ones (the XBE image) at kLowAlias + address. Device() resolves both.
constexpr uint32_t kLowAlias = 0xE0000000;
uint32_t DeviceAddress(uint32_t guest);
uint8_t* Device(uint32_t address);

}  // namespace mem
