// The window and what the TV would show: the scanned out frame buffer with the video overlay on top.
#pragma once

#include <string>

namespace display {

void Init();
// wakes the display thread to compose and show the current frame (called once per vblank)
void Vblank();
// mouse movement since the last call while the window holds the mouse (a click into it captures the mouse,
// losing the focus releases it); false when not captured
bool TakeMouseMotion(int* dx, int* dy);
// ---- PC comfort features: wheel notches since the last call (up positive), while the window has the focus ----
int TakeWheelNotches();
bool MouseCaptured();
// ---- end PC comfort features ----
// GitHub #28: whether a held mouse button (VK_LBUTTON ... VK_XBUTTON2) may reach the game: only a press that began in
// the client area of the window in front, not the click that brought it there, not on the title bar or the frame,
// not in a move/size loop or the settings menu. True for every other key code
bool ButtonForGame(int vk);
// ---- PC comfort features ----
// settings menu: opened (the mouse is let go, the pointer shows) or closed (borderless takes the mouse again)
void MenuShown(bool open);
// ---- end PC comfort features ----

// HLE graphics: the renderer presents into the window (HWND) itself
void* Window();
// where a 4:3 picture goes in a client area of this size (letterboxed)
void OutputRect(int width, int height, int* x, int* y, int* w, int* h);
int DumpEvery();  // FCI_DUMP_FRAMES: every n-th frame as BMP, 0 for none
// relaunch handover (kernel::Relaunch): a relaunched instance opens its window hidden; the presenter calls this once
// the first frames are out, which shows the window and lets the previous instance end
void FinishHandover();
// relaunch: the window as it is now (mode, monitor, rectangle, maximized, windowed placement) for the next process
// (FCI_WINDOW_STATE, app/output.h); empty when the settings menu changed display_mode, monitor or resolution (the
// next process follows the settings then)
std::string RelaunchWindowState();

// ---- display output options (app/output.h): exclusive fullscreen, driven by gpu/present.cpp ----
// display_mode = fullscreen: whether the window wants exclusive fullscreen now (F11/Alt+Enter toggle it, closing
// the window drops it)
bool WantExclusive();
// present.cpp: the swap chain is (true) or is no longer (false) in exclusive fullscreen; from the render thread
void ExclusiveChanged(bool on);
// ---- end display output options ----

}  // namespace display
