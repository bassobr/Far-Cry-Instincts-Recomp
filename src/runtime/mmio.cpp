// Hardware above 0xFD000000: the NV2A has a register model, everything else (APU, USB, NIC, flash)
// behaves as plain memory until it needs more.
#include <atomic>
#include <cstring>

#include "audio/apu.h"
#include "gpu/nv2a.h"
#include "runtime/guest.h"
#include "runtime/log.h"
#include "runtime/memory.h"

namespace rt {

namespace {

std::atomic<int> g_reports{0};

void Report(const char* what, uint32_t a, uint32_t v) {
    if (g_reports++ < 64) Log("mmio: %s %08X = %08X", what, a, v);
}

}  // namespace

// AC'97 controller: global status reports the primary codec ready
constexpr uint32_t kAc97GlobalStatus = 0xFEC00130;

extern "C" uint32_t FCI_MMIO_CC MmioRead(uint32_t a, unsigned size) {
    if (a - gpu::kMmioBase < gpu::kMmioSize) return gpu::Read(a - gpu::kMmioBase, size);
    if (a - apu::kMmioBase < apu::kMmioSize) return apu::Read(a - apu::kMmioBase, size);
    if (a == kAc97GlobalStatus) return 0x100;
    uint32_t v = 0;
    std::memcpy(&v, Host(a), size);
    Report("read", a, v);
    return v;
}

extern "C" uint32_t FCI_MMIO_CC GuestRead(uint32_t a, unsigned size) {
    if (a >= kMmioBase) return MmioRead(a, size);
    uint32_t v = 0;
    std::memcpy(&v, Host(a), size);
    return v;
}

extern "C" void FCI_MMIO_CC GuestWrite(uint32_t a, uint32_t v, unsigned size) {
    if (a >= kMmioBase) return MmioWrite(a, v, size);
    std::memcpy(Host(a), &v, size);
}

extern "C" void FCI_MMIO_CC MmioWrite(uint32_t a, uint32_t v, unsigned size) {
    if (a - gpu::kMmioBase < gpu::kMmioSize) return gpu::Write(a - gpu::kMmioBase, v, size);
    if (a - apu::kMmioBase < apu::kMmioSize) return apu::Write(a - apu::kMmioBase, v, size);
    // AC'97 bus master control registers (0x..B): the reset bit clears itself once the channel reset
    if (a >= 0xFEC00100 && a < 0xFEC00200 && (a & 0xF) == 0xB && size == 1) v &= ~2u;
    Report("write", a, v);
    std::memcpy(Host(a), &v, size);
}

}  // namespace rt
