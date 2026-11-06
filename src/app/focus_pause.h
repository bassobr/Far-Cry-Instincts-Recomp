// Pause on focus loss (setting pause_on_focus_loss): while the window is in the background or minimized the
// guest CPU is held, the guest clocks stand still and the sound output stops.
// The settings menu (app/menu.h) pauses the game the same way while it is open, and draws its frames from the
// thread that holds the guest CPU.
#pragma once

namespace app {

// the window became active (true) or inactive/minimized (false); from the window procedure
void WindowActivated(bool active);
bool FocusPaused();

// ---- settings menu ----
// the menu opened (true) or closed (false): the game is held while it is open
void MenuPause(bool open);
// pause_on_focus_loss changed in the menu
void SetPauseOnFocusLoss(bool on);
// ---- end settings menu ----

}  // namespace app
