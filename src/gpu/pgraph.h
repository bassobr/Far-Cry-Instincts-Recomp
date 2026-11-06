// PGRAPH: executes the methods the DMA pusher reads from the push buffer.
#pragma once

#include <cstdint>

namespace gpu {

// shared with the pusher (nv2a.cpp)
uint32_t Ramin32(uint32_t offset);
uint32_t DmaBase(uint32_t instance);
uint32_t RamhtLookup(uint32_t handle);  // object handle -> instance offset in RAMIN, ~0 if unknown
// raises a PGRAPH interrupt for a method and stalls until the guest's service routine cleared it
void RaisePgraphInterrupt(uint32_t bits, uint32_t method, uint32_t subchannel, uint32_t data);

void SemaphoreReleased(uint32_t value);

// 3D flip counters: the push buffer advances WRITE, the vblank DPC advances READ through PGRAPH_INCREMENT,
// and FLIP_STALL holds the pusher while all buffers wait for display
enum class Flip { kRead, kWrite, kModulo };
void SetFlip(Flip which, uint32_t value);
void FlipIncrementWrite();
void FlipStall();
void LogTimings();  // vblank service latency and flip stall statistics since the last call

namespace pgraph {

constexpr uint32_t kIntrNotify = 1u << 0, kIntrError = 1u << 20;

void Method(uint32_t subchannel, uint32_t method, uint32_t data);
void DumpStatistics();

}  // namespace pgraph
}  // namespace gpu
