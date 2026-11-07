// Button prompts that follow the input device (GitHub #15). The game writes its button prompts ("Press {X} to
// reload", the menus' footers) with icon glyphs of its font; gpu/prompt_icons.cpp draws those glyphs anew for the
// device in use: the bound key or mouse button with keyboard and mouse, PlayStation or Switch buttons for those
// pads. This part decides which: button_prompts = auto (the device used last), xbox, keyboard, playstation, switch.
#pragma once

#include <cstdint>

namespace hle::prompts {

enum class Style : uint8_t { kXbox, kKeyboard, kPlayStation, kSwitch };

// the controller family as SDL names it (host_pad.cpp)
enum class PadFamily : uint8_t { kXbox, kPlayStation, kSwitch, kGeneric };

// the style the prompts are drawn in now
Style Current();
const char* Name(Style s);  // "xbox", "keyboard", "playstation", "switch"
// PlayStation only: the pad's generation decides Share/Create/Select (3, 4 or 5; 4 when unknown)
int PlayStationGeneration();

// changes whenever what a style's glyphs show changes (a key or button binding, the PlayStation generation): the
// icons are drawn again. A change of style alone keeps it (each style's page stays ready)
uint32_t Revision();

// ---- what the player used last (auto) ----
// a key or mouse button went down (real, focused window, or FCI_KEY_SCRIPT)
void NoteKeyboard();
// relative mouse motion: only steady motion counts (a few counts of jitter do not switch)
void NoteMouseMotion(int dx, int dy);
// a controller's button, trigger or stick was used clearly (generation: PlayStation 3/4/5, else 0)
void NotePad(PadFamily family, int generation = 0);
// a controller was plugged in: its style until anything is used (without one: keyboard and mouse)
void NotePadConnected(PadFamily family, int generation = 0);

// ---- settings ----
void Reload();            // button_prompts again (settings menu)
void BindingsChanged();   // a keyboard or controller binding changed: the icons show the new keys

}  // namespace hle::prompts
