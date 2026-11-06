// The host replacements the executable exports to the game code DLL (hle_hooks.h, generated from config/hle.txt)
// and the recompiled originals they replace, found in the DLL by address once per call site.
#pragma once

#include <cstdint>

#include "hle_hooks.h"
#include "runtime/runtime.h"

namespace hle {

template <uint32_t address>
rt::GuestFunction Original() {
    static const rt::GuestFunction f = rt::Original(address);
    return f;
}

}  // namespace hle

// the recompiled body of the replaced function at a guest address (o_<address> in the generated code)
#define GUEST_ORIG_AT(address) (::hle::Original<(address)>())
