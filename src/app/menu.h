// The settings menu: F10 (or Back + Start on a controller) opens and closes it, Escape / B / the Back button close
// it. While it is open the game is held as by the focus pause (app/focus_pause.h) and gets no input; changes are
// written to FarCryInstincts.ini at once, those that can apply while running do so, the others are marked for a
// restart. Dear ImGui on the Direct3D 11 presenter (gpu/present.h).
#pragma once

#include <windows.h>

namespace app {

bool MenuOpen();
// opens or closes it (F10, Escape, B, Back + Start, the Back button); opening needs a presented picture (HLE
// graphics)
void ToggleMenu();
// Back + Start on a pad (hle/xinput.cpp, from the guest's poll): opens it
void RequestMenu();

// display.cpp's window procedure asks first: true when the message was the menu's (result in *result)
bool MenuWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* result);

// from the focus pause's thread while it holds the game (app/focus_pause.cpp): one frame of the menu, and the
// menu has closed (input settles, settings are saved) before the game runs again
void MenuFrame();
void MenuClosed();

}  // namespace app
