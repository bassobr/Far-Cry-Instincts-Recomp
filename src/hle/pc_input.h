// Keyboard and mouse on player 1's pad: bindings from FarCryInstincts.ini (bind_<button> = <keys>), mouse look
// with sensitivity and inversion, and the test scripts that inject host input (FCI_KEY_SCRIPT). Used by
// hle/xinput.cpp, which owns the pads.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hle::pc {

// Xbox gamepad: digital buttons in wButtons, the eight analog buttons by index
enum : uint16_t { kDpadUp = 1, kDpadDown = 2, kDpadLeft = 4, kDpadRight = 8, kStart = 0x10, kBack = 0x20,
                  kLeftThumb = 0x40, kRightThumb = 0x80 };
enum { kA, kB, kX, kY, kBlack, kWhite, kLeftTrigger, kRightTrigger };

struct Pad {
    uint16_t buttons = 0;
    uint8_t analog[8] = {};
    int16_t thumb[4] = {};  // LX LY RX RY
    bool operator==(const Pad&) const = default;
};

// the clock of the input scripts: seconds since the first controller poll, in guest time (a focus pause
// does not move it)
double ScriptSeconds();

// keyboard, mouse buttons, wheel and mouse look onto player 1's pad (real devices only while the window has
// the focus; scripted ones always)
void ApplyKeyboardMouse(Pad& p, bool focused);

// FCI_TRACE_PAD=1: player 1's pad state whenever it changes, with the script time; polls per second
void TracePad(const Pad& p);

// ---- settings menu (app/menu.cpp; called while the guest is held, so no poll runs meanwhile) ----
int BindingCount();
const char* BindingName(int target);     // "a", "lup", ... (bind_<name>)
std::vector<int> BindingKeys(int target);  // the keys in effect
std::string KeyName(int code);           // as the ini writes it: "space", "mouse1", "wheelup"
int WheelCode(bool up);                  // the wheel's two bindable codes
// binds code to the target's slot-th key (0: the first; past the end: added) and takes it from every other
// target; code 0 removes the slot-th key. Written as bind_<name> (settings::Set) and in effect at once
void SetBinding(int target, int slot, int code);
void ResetBindings();
void ReloadBindings();
void ReloadMouse();  // mouse_sensitivity*, mouse_invert_y, mouse_acceleration again
// the menu closed: keys held now are ignored until released, motion and wheel since are dropped
void SettleAfterMenu();
// ---- end settings menu ----

}  // namespace hle::pc
