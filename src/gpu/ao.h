// Screen space ambient occlusion over the opaque 3D view of the HLE renderer (FarCryInstincts.ini / FCI_<KEY>,
// see README): ao = off | ssao | gtao, ao_strength, ao_resolution = auto | full | half.
//
// The renderer (gpu/renderer.cpp) decides where it goes in (once per 3D viewport, where the opaque pass ends) and
// hands over the scene's depth and colour; this pass reconstructs view space positions from the W-buffered depth
// (w / zmax = view depth / far plane) and the game camera's projection (xdk/game.cpp), computes the occlusion, blurs it
// depth aware and multiplies it into the colour, faded out with distance and fog.
#pragma once

#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;
struct ID3D11RenderTargetView;

namespace gpu::ao {

// 0 off, 1 SSAO (the default), 2 GTAO (setting ao); read once (the LLE graphics path never uses it)
int BootMode();
// ---- settings menu: the mode in use now (BootMode until changed), SetMode false when the pass cannot go on
// without a restart (it was off at start: the depth targets have no view for it); strength 0..4 ----
int Mode();
bool SetMode(int mode);
void SetStrength(float strength);
// ---- end settings menu ----

// the projection of the game's screen camera (D3DXMatrixPerspectiveFovRH): view x = ndc x * depth / x_scale, view
// y = ndc y * depth / y_scale; far_plane: its far plane, where the XDK's W-buffering puts w = zmax (stored depth 1).
// The latest one counts
void SetProjection(float x_scale, float y_scale, float far_plane);

struct Pass {
    // the scene's depth, w / zmax (= view depth / far plane); multisampled when samples > 1
    ID3D11ShaderResourceView* depth = nullptr;
    uint32_t samples = 1;
    ID3D11RenderTargetView* color = nullptr;     // the scene's colour, multiplied by the occlusion
    uint32_t width = 0, height = 0;              // of both, host pixels
    int32_t left = 0, top = 0, right = 0, bottom = 0;  // the 3D view (viewport), host pixels
    float fog[3] = {};  // fog the scene has at stored depth d: fog[2] * saturate(fog[0] * d + fog[1])
    // performance (renderer.cpp, reversed depth): the target holds c / d instead of d; 0: d itself
    float reversed = 0;
};

// compiles the shaders on a background thread right after the device exists, so the first AO pass (at
// the first level) doesn't stall the frame on shader compilation; Apply waits for it if it isn't done yet
void Prepare(ID3D11Device* device);

// draws the occlusion into p.color; leaves render targets, shaders, resources and states to the caller to set again
void Apply(ID3D11Device* device, ID3D11DeviceContext* context, const Pass& p);

}  // namespace gpu::ao
