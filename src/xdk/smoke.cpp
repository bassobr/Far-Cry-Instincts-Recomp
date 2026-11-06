// The smallest HLE replacement, to keep the path from config/hle.txt to hle::<name> tested: the TV flicker
// filter means nothing on a PC monitor.
#include "xdk/hle.h"

namespace {
bool HleOff() { return !xdk::HleGraphics(); }
}  // namespace

HLE_STDCALL(D3DDevice_SetFlickerFilter, 1) {
    LOG_FIRST(4, "xdk: D3DDevice_SetFlickerFilter(%u) ignored", a[0]);
    return 0;
}
