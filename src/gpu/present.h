// HLE graphics: the presented frame to the window (swap chain on video/display.cpp's window).
#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>

struct ImDrawData;  // settings menu (Dear ImGui)

namespace gpu::present {

// frame: the front buffer's host copy (render resolution w x h), may be null (black); aspect: the shape the
// picture is shown at (4:3, or the widescreen aspect for frames with the widened 3D view)
// performance, FCI_DUMP_LEFT_OUT: the next frame is dumped (as FCI_DUMP_FRAMES does) with tag in its name
void DumpNext(const char* tag);
void Frame(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11ShaderResourceView* frame, uint32_t w,
           uint32_t h, float aspect = 4.0f / 3.0f);
// PC features, vsync = 1: waits until the display takes the next frame (call with the guest CPU released, before
// the frame is presented); false without vsync (the caller paces the frames itself)
bool WaitForDisplay();
// D3DGAMMARAMP: 256 red, 256 green, 256 blue
void SetGamma(const uint8_t ramp[768]);

// ---- settings menu (app/menu.cpp) ----
// a picture was presented: the menu can be drawn over it (HLE graphics only)
bool MenuAvailable();
// one frame of the menu over the last picture, from the thread that holds the guest CPU (the game is held, so the
// device context is free). build makes the ImGui frame (ImGui::NewFrame ... ImGui::Render) and returns its draw
// data; it runs after the Direct3D 11 backend's NewFrame. Drawn with a device context state of its own: the
// renderer's state stays as the game left it. False when nothing was shown
bool MenuFrame(ImDrawData* (*build)());
// take effect with the next frame (the game's or the menu's)
void SetSmaa(bool on);
bool HdrActive();  // HDR10 output in use
// hdr_paper_white (0: Windows' SDR brightness), hdr_peak (0: the display's), hdr_boost
void SetHdrLevels(float paper_white, float peak, float boost);
// ---- end settings menu ----

}  // namespace gpu::present
