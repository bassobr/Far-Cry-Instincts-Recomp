// Button prompts for the input device in use (GitHub #15): the game's prompts ("Press {X} to reload", the menus'
// footers, the tutorial hints) are text in its font, and the font's icon glyphs (A, B, X, Y, white, black,
// triggers, sticks, D-pad, start, back) lie on one 256x128 DXT3 page of it. When the prompts are to show another
// device (hle/prompts.h), the renderer binds a page of our own instead: the game's page at the render scale, with
// each button glyph drawn anew in its place - a key cap with the bound key, a mouse with the bound button, or a
// PlayStation/Switch button. Everything is drawn at run time (shapes, and the key names with a system font); no
// button art is shipped. The glyphs keep their place and size, so the text's layout and the HUD's 4:3 narrowing
// stay the game's.
#pragma once

#include <d3d11.h>

#include <cstdint>

#include "gpu/texture.h"

namespace gpu::prompt_icons {

// the texture is the font page with the button glyphs (cheap for every other texture: shape test first)
bool IsGlyphPage(const texture::Desc& d);
// what to bind for that page now: our page for the current style (built when the style or the bindings changed),
// or null for the game's own (Xbox prompts). scale: the render scale (the page is drawn at that many texels per
// guest texel, up to 4)
ID3D11ShaderResourceView* Page(ID3D11Device* device, const texture::Desc& d, float scale);

}  // namespace gpu::prompt_icons
