// Direct mouse look (hle/mouse_look.cpp): the frame's camera of the first local player shows the mouse's turn of
// this frame already (the player's angles take it only for the next frame).
#pragma once

#include <cstdint>

namespace hle {

// from Game_CameraUpdate (xdk/game.cpp) for the client's view camera (built at 0x000F0C50), with its field of view
// as the game set it (radians, before the widening; the zoom of the next look input): adds the mouse's degrees the
// look input took this frame to its angles, unless the game held the view back last time
void PredictView(uint32_t camera, float game_fov);

}  // namespace hle
