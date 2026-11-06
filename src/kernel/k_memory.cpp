// Mm*, Nt*VirtualMemory and pool exports.
#include <windows.h>

#include <algorithm>

#include "kernel/kernel.h"
#include "runtime/memory.h"

namespace kernel {

namespace {

constexpr uint32_t kMemCommit = 0x1000, kMemReserve = 0x2000, kMemDecommit = 0x4000, kMemRelease = 0x8000;
constexpr uint32_t kPageReadWrite = 0x04;
constexpr uint32_t kPhysicalBytes = 64 * 1024 * 1024;

uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

}  // namespace

uint32_t AllocateVirtual(const Args& a);

KERNEL_EXPORT(184, NtAllocateVirtualMemory) {
    uint32_t base = R32(a[0]), size = R32(a[2]);
    uint32_t status = AllocateVirtual(a);
    rt::Log("kernel: NtAllocateVirtualMemory(%08X, %X, type %X) -> %08X+%X, %08X", base, size, a[3], R32(a[0]),
            R32(a[2]), status);
    return status;
}

uint32_t AllocateVirtual(const Args& a) {
    uint32_t base_ptr = a[0], size_ptr = a[2], type = a[3];
    uint32_t base = R32(base_ptr), size = R32(size_ptr);
    if (!size) return X_STATUS_INVALID_PARAMETER;
    auto& region = mem::Virtual();

    if (base) {
        uint32_t block, block_size;
        if (region.Find(base, &block, &block_size)) {
            // committing pages of an existing reservation
            if (!(type & kMemCommit)) return X_STATUS_CONFLICTING_ADDRESSES;
            uint32_t start = base & ~0xFFFu;
            uint32_t end = AlignUp(base + size, 0x1000);
            if (end > block + block_size) return X_STATUS_CONFLICTING_ADDRESSES;
            mem::Commit(start, end - start);
            W32(base_ptr, start);
            W32(size_ptr, end - start);
            return X_STATUS_SUCCESS;
        }
        uint32_t start = base & ~0xFFFFu;
        uint32_t len = AlignUp(base + size, 0x1000) - start;
        if (!region.AllocAt(start, len, type & kMemCommit)) return X_STATUS_CONFLICTING_ADDRESSES;
        W32(base_ptr, start);
        W32(size_ptr, AlignUp(len, 0x1000));
        return X_STATUS_SUCCESS;
    }
    size = AlignUp(size, 0x1000);
    uint32_t at = region.Alloc(size, 0, type & kMemCommit);
    if (!at) {
        rt::Log("kernel: NtAllocateVirtualMemory of %X bytes failed", size);
        return X_STATUS_NO_MEMORY;
    }
    W32(base_ptr, at);
    W32(size_ptr, size);
    return X_STATUS_SUCCESS;
}

KERNEL_EXPORT(199, NtFreeVirtualMemory) {
    uint32_t base_ptr = a[0], size_ptr = a[1], type = a[2];
    uint32_t base = R32(base_ptr), size = R32(size_ptr);
    rt::Log("kernel: NtFreeVirtualMemory(%08X, %X, type %X)", base, size, type);
    if (type & kMemRelease) {
        uint32_t freed = mem::Virtual().Free(base & ~0xFFFu);
        if (!freed) return X_STATUS_INVALID_PARAMETER;
        W32(size_ptr, freed);
        return X_STATUS_SUCCESS;
    }
    if (type & kMemDecommit) {
        uint32_t block, block_size;
        if (!mem::Virtual().Find(base, &block, &block_size)) return X_STATUS_INVALID_PARAMETER;
        if (!size) size = block + block_size - base;
        mem::Decommit(base, size);
        W32(base_ptr, base & ~0xFFFu);
        W32(size_ptr, AlignUp(size, 0x1000));
        return X_STATUS_SUCCESS;
    }
    return X_STATUS_INVALID_PARAMETER;
}

// MEMORY_BASIC_INFORMATION: BaseAddress, AllocationBase, AllocationProtect, RegionSize, State, Protect, Type
KERNEL_EXPORT(217, NtQueryVirtualMemory) {
    uint32_t addr = a[0], info = a[1];
    uint32_t block, block_size;
    uint32_t page = addr & ~0xFFFu;
    if (!mem::Virtual().Find(addr, &block, &block_size)) {
        W32(info + 0, page);
        W32(info + 4, 0);
        W32(info + 8, 0);
        W32(info + 12, 0x1000);
        W32(info + 16, 0x10000);  // MEM_FREE
        W32(info + 20, 1);        // PAGE_NOACCESS
        W32(info + 24, 0);
        return X_STATUS_SUCCESS;
    }
    // run of pages sharing the commit state of `page`
    bool committed = mem::IsCommitted(page);
    uint32_t end = page;
    while (end < block + block_size && mem::IsCommitted(end) == committed) end += 0x1000;
    W32(info + 0, page);
    W32(info + 4, block);
    W32(info + 8, kPageReadWrite);
    W32(info + 12, end - page);
    W32(info + 16, committed ? kMemCommit : kMemReserve);
    W32(info + 20, committed ? kPageReadWrite : 0);
    W32(info + 24, 0x20000);  // MEM_PRIVATE
    return X_STATUS_SUCCESS;
}

KERNEL_EXPORT(165, MmAllocateContiguousMemory) {
    uint32_t r = mem::Contiguous().Alloc(a[0]);
    rt::Log("kernel: MmAllocateContiguousMemory(%X) -> %08X", a[0], r);
    return r;
}

KERNEL_EXPORT(166, MmAllocateContiguousMemoryEx) {
    uint32_t align = a[3] ? a[3] : 0x1000;
    uint32_t r = mem::Contiguous().Alloc(a[0], align);
    rt::Log("kernel: MmAllocateContiguousMemoryEx(%X, %08X-%08X, align %X) -> %08X", a[0], a[1], a[2], a[3], r);
    return r;
}

KERNEL_EXPORT(171, MmFreeContiguousMemory) {
    mem::Contiguous().Free(a[0]);
    return 0;
}

// buffers outside the physical window get a device address that names them (see mem::Device)
KERNEL_EXPORT(173, MmGetPhysicalAddress) { return mem::DeviceAddress(a[0]); }

KERNEL_EXPORT(175, MmLockUnlockBufferPages) { return 0; }
KERNEL_EXPORT(176, MmLockUnlockPhysicalPage) { return 0; }
KERNEL_EXPORT(178, MmPersistContiguousMemory) { return 0; }
KERNEL_EXPORT(179, MmQueryAddressProtect) { return kPageReadWrite; }
KERNEL_EXPORT(182, MmSetAddressProtect) { return 0; }

KERNEL_EXPORT(180, MmQueryAllocationSize) {
    uint32_t base, size;
    if (mem::Contiguous().Find(a[0], &base, &size) || mem::Virtual().Find(a[0], &base, &size) ||
        mem::System().Find(a[0], &base, &size))
        return size;
    return 0;
}

// MM_STATISTICS
KERNEL_EXPORT(181, MmQueryStatistics) {
    uint32_t s = a[0];
    if (R32(s) != 0x24) return X_STATUS_INVALID_PARAMETER;
    uint32_t used = mem::Virtual().used() + mem::Contiguous().used();
    uint32_t total_pages = kPhysicalBytes / 0x1000;
    uint32_t used_pages = std::min(used / 0x1000, total_pages - 1);
    W32(s + 4, total_pages);
    W32(s + 8, total_pages - used_pages);
    W32(s + 12, used);
    W32(s + 16, used);
    for (int i = 20; i < 0x24; i += 4) W32(s + i, 0);
    return X_STATUS_SUCCESS;
}

// D3D takes the top of physical memory for the GPU's instance memory
KERNEL_EXPORT(168, MmClaimGpuInstanceMemory) {
    constexpr uint32_t kInstanceMemory = 0x10000;
    if (a[1]) W32(a[1], 0x10000);
    return 0x80000000u + kPhysicalBytes - kInstanceMemory;
}

KERNEL_EXPORT(169, MmCreateKernelStack) {
    uint32_t size = AlignUp(a[0], 0x1000);
    return mem::AllocSystem(size) + size;
}

KERNEL_EXPORT(170, MmDeleteKernelStack) {
    mem::System().Free(a[1]);
    return 0;
}

// pool blocks carry their size in a 16-byte header
KERNEL_EXPORT(15, ExAllocatePoolWithTag) {
    uint32_t block = mem::System().Alloc(a[0] + 16);
    if (!block) return 0;
    W32(block, a[0]);
    W32(block + 4, a[1]);
    return block + 16;
}

KERNEL_EXPORT(17, ExFreePool) {
    if (a[0]) mem::System().Free(a[0] - 16);
    return 0;
}

KERNEL_EXPORT(23, ExQueryPoolBlockSize) { return a[0] ? R32(a[0] - 16) : 0; }

}  // namespace kernel
