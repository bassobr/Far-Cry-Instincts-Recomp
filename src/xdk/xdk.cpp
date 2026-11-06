#include "xdk/hle.h"

#include <windows.h>

namespace xdk {

bool HleAudio() {
    static const bool on = GetEnvironmentVariableA("FCI_LLE_AUDIO", nullptr, 0) == 0;
    return on;
}

bool HleGraphics() {
    static const bool on = GetEnvironmentVariableA("FCI_LLE_GFX", nullptr, 0) == 0;
    return on;
}

}  // namespace xdk
