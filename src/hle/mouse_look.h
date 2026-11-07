// Direct mouse look (hle/mouse_look.cpp): the frame's camera of the first local player shows the mouse's turn of
// this frame already (the player's angles take it only for the next frame).
#pragma once

#include <cstdint>

namespace hle {

// from Game_CameraUpdate (xdk/game.cpp) for the client's view camera (built at 0x000F0C50), with its field of view
// as the game set it (radians, before the widening; the zoom of the next look input): adds the mouse's degrees the
// look input took this frame to its angles, unless the game held the view back last time
void PredictView(uint32_t camera, float game_fov);

// from Game_FirstPersonProjection (xdk/game.cpp), before the original: the hands and the weapon were placed from the
// player's angles before this frame's turn (in the entities' update, before the client's), so the first person
// items are drawn with the view of those angles: puts it into the renderer's view matrix (the old one in saved) and
// returns true; FirstPersonViewDone puts the old one back after the original built the first person matrices
bool FirstPersonView(uint32_t renderer, float saved[16]);
void FirstPersonViewDone(uint32_t renderer, const float saved[16]);

}  // namespace hle
