// The host's controllers for the game's four controller ports: SDL3 (any pad SDL knows: Xbox, PlayStation,
// Switch, generic) or, with pad_backend = xinput, XInput alone. A controller takes the lowest free slot when it is
// plugged in and keeps it until it is unplugged; slot n is the game's port n. The buttons go through the
// bindings of FarCryInstincts.ini (pad_bind_<button> = <controller buttons>). Used by hle/xinput.cpp.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "hle/pc_input.h"

namespace hle::host {

constexpr int kSlots = 4;

// a controller in that slot (0-3)
bool Connected(int slot);
// that slot's controller onto the Xbox pad, through the bindings; false (p untouched) for an empty slot
bool Read(int slot, pc::Pad& p);
// the game's motor speeds for that slot (they run until set again, as on the Xbox)
void Rumble(int slot, uint16_t low, uint16_t high);
// motors off while the game is paused (app/focus_pause.cpp), the game's speeds again after
void PauseRumble(bool paused);
// motors off before the process ends (window closed, the title quits or relaunches): a timed SDL rumble and an
// XInput motor would otherwise keep running after the exit
void StopRumble();

// ---- settings menu (app/menu.cpp, CONTROLLER page) ----
int BindingCount();                        // the Xbox buttons (pad_bind_<name>)
const char* BindingName(int target);       // "a", "white", "lt", ...
std::vector<int> BindingButtons(int target);  // the controller buttons bound now
const char* ButtonName(int button);        // as the ini writes them: "a", "lb", "lt", "ls", "guide", ...
// binds the controller button to the target's slot-th entry (past the end: added) and takes it from every other
// target; button -1 removes the slot-th entry. Written as pad_bind_<name> (settings::Set), in effect at once
void SetBinding(int target, int slot, int button);
void ResetBindings();
void ReloadOptions();       // pad_dead_zone and pad_rumble again
int PressedButton();        // a controller button held on any controller now (raw, no bindings), -1 for none
std::string Name(int slot);  // the controller in that slot, empty for none
const char* BackendName();  // "SDL3", "XInput" or "off"
// ---- end settings menu ----

// FCI_VIRTUAL_PADS=<n>[:<plug in>[:<unplug>]]: n virtual SDL controllers for self tests without a physical pad,
// optionally plugged in and unplugged at those script seconds. The scripted input of a port (FCI_INPUT_SCRIPT) whose
// slot holds one goes into it and comes back through SDL like a real controller's.
bool Virtual(int slot);
void FeedVirtual(int slot, const pc::Pad& p);

}  // namespace hle::host
