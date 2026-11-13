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

// the program ends (window closed, the menu's Exit Game, the session ends): the settings menu closes (its settings
// saved), a pause ends and none starts again, so a save the pause held between two of its steps can finish
// (kernel::FinishSaves). From the window procedure: waits at most max_ms for the game to run again, handling the
// window's messages meanwhile (the menu's last frame may wait on them)
void ResumeForExit(unsigned max_ms);

}  // namespace app
