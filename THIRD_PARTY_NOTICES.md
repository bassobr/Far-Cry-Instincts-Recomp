# Third-party notices

Far Cry Instincts Recomp is MIT licensed (see [LICENSE](LICENSE)). It builds on the open-source projects below; their
licences and copyright notices follow. Licence headers inside individual source files stay in place.

| Project | Licence | How it is used |
|---|---|---|
| [SCDA-XBERecomp](https://github.com/TGP482/SCDA-XBERecomp) | MIT | Code adapted into `src/` (see the list below) |
| [XbSymbolDatabase](https://github.com/Cxbx-Reloaded/XbSymbolDatabase) (Cxbx-Reloaded) | MIT | XDK symbol addresses in `config/xdk_symbols.txt` and `config/symbols.txt`, produced with its CLI |
| [SMAA](https://github.com/iryoku/smaa) | MIT | Reference shader and precomputed textures in `src/gpu/smaa_shader.h` and `src/gpu/smaa_textures.h` |
| [Dear ImGui](https://github.com/ocornut/imgui) v1.91.0 | MIT | Settings menu; fetched by CMake and linked into the game executable |
| [SDL](https://github.com/libsdl-org/SDL) 3.4.16 | zlib | Controllers; fetched by CMake and linked statically into the game executable |
| [Zydis](https://github.com/zyantific/zydis) v4.1.1 | MIT | x86 decoder; fetched by CMake and linked into `fci-recomp` and `fci-semtest` |
| [Zycore](https://github.com/zyantific/zycore-c) (Zydis dependency) | MIT | Support library of Zydis, linked with it |

## SCDA-XBERecomp

https://github.com/TGP482/SCDA-XBERecomp - the model for this project's whole approach. Code adapted from it, each
file marked in its header:

- Direct3D 8 device creation and the scratch push buffer: `src/xdk/d3d8.cpp`; HLE macros: `src/xdk/hle.h`
- DirectSound HLE and its XAudio2 output: `src/xdk/dsound.cpp`, `src/audio/hle_output.cpp`, `src/audio/hle_output.h`
- XDVDFS disc image reader: `src/kernel/disc.cpp`, `src/kernel/disc.h`
- Guest time: `src/runtime/guest_time.cpp`
- SDL3 controller support: `src/hle/host_pad.cpp`; mouse wheel bindings: `src/hle/pc_input.cpp`
- Focus pause: `src/app/focus_pause.cpp`
- Settings menu and its colours: `src/app/menu.cpp`, `src/app/panel_colors.h`
- Display output options (monitor list, DXGI output lookup, SDR white level): `src/app/output.cpp`
- SMAA passes, the HDR10 shader and the colour-blind modes: `src/gpu/present.cpp`; SMAA packaging: `src/gpu/smaa_shader.h`, `src/gpu/smaa_textures.h`
- Ambient occlusion (SSAO and GTAO kernels, reconstruction, blur, fog fade): `src/gpu/ao.cpp`
- Texture dump and replacement: `src/gpu/texture_pack.cpp`, `src/gpu/texture_pack.h`
- FMV scaling: `src/gpu/fmv.cpp`, `src/gpu/fmv.h`; fps counter idea: `src/gpu/fps_overlay.h`

```
MIT License

Copyright (c) 2026 TGP482, Joshhhuaaa

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## XbSymbolDatabase

https://github.com/Cxbx-Reloaded/XbSymbolDatabase - the XDK 5849 symbol addresses in `config/xdk_symbols.txt` (output
of its CLI) and `config/symbols.txt` (the same, converted by `tools/symbols.py`). No code of it is included.

```
MIT License

Copyright (c) 2023 The XbSymbolDatabase authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## SMAA

https://github.com/iryoku/smaa - Enhanced Subpixel Morphological Antialiasing by Jorge Jimenez, Jose I. Echevarria,
Belen Masia, Fernando Navarro and Diego Gutierrez. Its reference shader and its area and search textures are
included in `src/gpu/smaa_shader.h` and `src/gpu/smaa_textures.h` (packaged as in SCDA-XBERecomp), with this
notice in both files.

```
Copyright (C) 2013 Jorge Jimenez (jorge@iryoku.com)
Copyright (C) 2013 Jose I. Echevarria (joseignacioechevarria@gmail.com)
Copyright (C) 2013 Belen Masia (bmasia@unizar.es)
Copyright (C) 2013 Fernando Navarro (fernandn@microsoft.com)
Copyright (C) 2013 Diego Gutierrez (diegog@unizar.es)

Permission is hereby granted, free of charge, to any person obtaining a copy
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software. As clarification, there is no
requirement that the copyright notice and permission be included in binary
distributions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

## Dear ImGui

https://github.com/ocornut/imgui - version 1.91.0, with its Win32 and Direct3D 11 backends.

```
The MIT License (MIT)

Copyright (c) 2014-2024 Omar Cornut

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## SDL

https://github.com/libsdl-org/SDL - SDL 3 (tag `release-3.4.16`), linked statically.

```
Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>
  
This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:
  
1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required. 
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```

## Zydis

https://github.com/zyantific/zydis - version 4.1.1, used by the recompiler and the semantics test (not part of the
game executable).

```
The MIT License (MIT)

Copyright (c) 2014-2024 Florian Bernd
Copyright (c) 2014-2024 Joel Höner

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Zycore

https://github.com/zyantific/zycore-c - fetched as a submodule of Zydis 4.1.1.

```
The MIT License (MIT)

Copyright (c) 2018-2024 Florian Bernd
Copyright (c) 2018-2024 Joel Höner

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## References (no code included)

These works informed parts of the implementation; nothing of their code or data is included.

- [XeGTAO](https://github.com/GameTechDev/XeGTAO) (Intel, MIT): the GTAO kernel in `src/gpu/ao.cpp` uses the
  parameters of its *High* preset. The algorithm is from Jimenez et al., *Practical Real-Time Strategies for
  Accurate Indirect Occlusion* (2016).
- Machado, Oliveira and Fernandes, *A Physiologically-based Model for Simulation of Color Vision Deficiency*
  (2009), and Fidaner's daltonization: the colour-blind modes in `src/gpu/present.cpp`.
- [nouveau](https://nouveau.freedesktop.org/) and [xemu](https://github.com/xemu-project/xemu): the NV2A (nv097)
  method names used in `src/gpu/kelvin.h`.
- Far Cry (PC, 2004): the ocean technique of `src/gpu/water.cpp` follows the one its shader scripts describe.
  The optional wave texture is read at run time from the user's own installation and is never included.
