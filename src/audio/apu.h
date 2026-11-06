// The MCPX audio processing unit (APU) at 0xFE800000.
#pragma once

#include <cstdint>

namespace apu {

constexpr uint32_t kMmioBase = 0xFE800000;
constexpr uint32_t kMmioSize = 0x80000;

void Init();
uint32_t Read(uint32_t offset, unsigned size);
void Write(uint32_t offset, uint32_t value, unsigned size);

}  // namespace apu
