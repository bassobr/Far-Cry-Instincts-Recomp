// Host side of the runtime: dispatch setup and constants shared with the kernel.
#pragma once

#include <cstdint>

#include "runtime/guest.h"

namespace rt {

// kernel thunk slots of function exports hold kKernelThunkBase + ordinal * 4, so indirect calls through
// a loaded slot reach KernelCall as well
constexpr uint32_t kKernelThunkBase = 0xFE000000;

void InitDispatch(uint32_t image_base, uint32_t image_size);
GuestFunction Lookup(uint32_t address);

}  // namespace rt
