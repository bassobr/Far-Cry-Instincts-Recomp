# Far Cry Instincts Recomp

An unofficial native PC port of the original Xbox version of *Far Cry Instincts*, made by static recompilation. The
game's x86 code is translated ahead of time into C++ and compiled into native 64-bit Windows code. The Xbox kernel,
Direct3D 8 and DirectSound are reimplemented on Windows, with graphics on Direct3D 11. `FarCryInstincts.exe`
contains none of the game's code: on its first start it translates and compiles the code of your own copy of the
game, once, into `game_code\game_code.dll`. At run time it loads only the data of your copy.

This project follows the approach of, and adapts a large amount of code from,
**[SCDA-XBERecomp](https://github.com/TGP482/SCDA-XBERecomp)** by TGP482 and Joshhhuaaa. Many thanks to them, and to
the other open-source projects listed under [Credits](#credits).

## Legal

- You need your own, legally obtained copy of the original Xbox game *Far Cry Instincts*.
- Nothing from the game is included in this repository or in its releases: no executable, no code, no data, no
  textures, no shaders. The recompiled code is generated on your machine from your copy of `default.xbe` and is
  never committed or distributed.
- The optional Far Cry (PC) water data is read at run time from your own installation and is never included or
  stored.
- This project is not affiliated with or endorsed by Ubisoft, Crytek or Microsoft. All trademarks belong to their
  owners.

## Features

**Graphics** (high-level emulation of Direct3D 8 on Direct3D 11)
- Render resolution scaling (default: the output's height), MSAA up to 8x, SMAA 1x, anisotropic filtering up to 16x
- Higher-resolution shadow maps (up to 8192x8192), longer sun shadow range, soft or console-sharp shadow filtering
- Screen-space ambient occlusion: SSAO or GTAO, at full or half resolution
- Widescreen (Hor+) with the HUD kept in the 4:3 centre, adjustable field of view, full-screen videos fitted to the
  width
- Unlocked frame rate (default: the display's refresh rate), optional vsync, frames-ahead setting
- HDR10 output with paper white, peak brightness and highlight boost settings
- Level-of-detail and object view distance multipliers
- An improved ocean shader (Fresnel reflection, refraction, depth-based colour and opacity, sun glint), optionally
  with the wave texture of an installed Far Cry (PC)
- Colour-blind modes (protanopia, deuteranopia, tritanopia)
- Background shader compilation and an on-disk shader cache
- Texture dump and replacement, fps counter

**Audio**
- DirectSound reimplemented on a host mixer (XAudio2): the XDK's I3DL2 reverb per environment, the library's full
  HRTF tables for 3D sound, voice filters, stereo or 5.1 output

**Input**
- Up to four controllers through SDL3 (Xbox, PlayStation, Switch Pro and most other gamepads), hot-plug, rumble,
  remappable buttons, split screen
- Keyboard and mouse for player 1 with remappable keys and direct raw mouse look (a fixed angle per count, shown in
  the same frame), with adjustable sensitivity, optional acceleration and smoothing, and zoom sensitivity

**Comfort**
- Settings menu on **F10** (controller: **Back + Start**), applied at once where possible and saved to the ini file
- Windowed, borderless or exclusive fullscreen, monitor and resolution selection, **F11** / **Alt+Enter** toggles
  fullscreen
- Skip the intro videos, pause when the window loses focus
- Plays straight from the disc image (`.iso`); saves go to `data\hdd\` next to the executable
- Steam Deck support (Proton) with its own default preset

## Installation

1. Download the latest release from the
   [Releases page](https://github.com/bassobr/Far-Cry-Instincts-Recomp/releases) and unpack it into a folder of
   your choice.
2. Put your game into the `assets` folder of the release: its disc image (`.iso`), or the disc's extracted files
   (`default.xbe` with the folders next to it). See [Providing the game](#providing-the-game).
3. Run `FarCryInstincts.exe`.

The first start compiles the game's code for your PC, once: the bundled recompiler and compiler (`compiler\`)
translate your `default.xbe` into `game_code\game_code.dll` next to the executable. A window shows the progress.
It takes about 1 minute on a current desktop PC and about 6 minutes on a Steam Deck, and needs about 1 GB of free
disk space while it runs (60 MB afterwards). Later starts load the compiled code at once. If the build fails, a
window shows the end of `game_code\build.log`. Settings can be changed in the menu on **F10** (controller:
**Back + Start**) while the game runs.

A release contains nothing from the game: the executable, Microsoft's HLSL compiler `d3dcompiler_47.dll`, the
`compiler\` folder (LLVM's `clang.exe` and `lld-link.exe`, the recompiler `fci-recomp.exe`, the executable's import
library, headers and the recompiler's configuration with the PGO profile) and the documents. Each release has a
`SHA256SUMS` file and a build provenance attestation (see [SECURITY.md](SECURITY.md)).

## Requirements

**To play:** Windows 10 or 11 (64-bit), or Linux / Steam Deck with Proton; a GPU with Direct3D 11 (feature level
11_0); about 1 GB of free disk space for the first start.

**To build:**
- Visual Studio 2022 or the Build Tools for Visual Studio 2022 with *Desktop development with C++*, the *C++ Clang
  tools for Windows*, *C++ CMake tools for Windows* and a Windows 10/11 SDK
- Git, and internet access for the first configure (CMake fetches Zydis, SDL3 and Dear ImGui)
- Your copy of the game, see below

## Providing the game

Put the game into a folder named `assets`: next to `FarCryInstincts.exe` for a release, in the repository root for
a build from source. A release works from either form; building from source with `build.bat` needs the extracted
`default.xbe`:

- **Extracted files:** `default.xbe` with the disc's folders and files next to it in `assets\`. The included helper
  extracts a disc image:

  ```
  powershell -File tools\xiso.ps1 -Iso "<path to your game>.iso" -Out assets
  powershell -File tools\xbeinfo.ps1 assets\default.xbe
  ```

  `xiso.ps1` without `-Out` lists the image's contents; `xbeinfo.ps1` prints the XBE's header, sections and XDK
  libraries. The addresses in `config/` were made for the USA/Europe release (title ID `55530008`, XDK 5849);
  other builds of the game are untested.
- **Disc image:** the `.iso` (an xiso of the game partition or a full disc dump) in `assets\`. The drive is read
  straight from the image, nothing has to be extracted. With several images the first one containing Far Cry
  Instincts is used; `FCI_ISO=<path>` picks one explicitly. Extracted files win when both are present.

The executable looks for `assets\` next to itself and in the folders above it, so the build output finds the
repository's `assets\` folder.

## Building from source

```
build.bat
```

`build.bat` sets up the Visual Studio environment, builds the recompiler `fci-recomp`, translates
`assets\default.xbe` into `generated\` (128 C++ files, about 350 MB) and compiles the result into
`out\build\release\game_code\game_code.dll`. It also builds `out\build\release\FarCryInstincts.exe` (the runtime alone,
about 7 MB, no game code) and the `compiler\` folder next to it (recompiler, the executable's import library, headers,
configuration: the layout of a release), and copies Microsoft's `d3dcompiler_47.dll` from the Windows SDK next to the
executable (a release ships the same file). Without `assets\default.xbe` only the executable is built,
and it compiles the game code on its first start like a release, once LLVM's `clang.exe` and `lld-link.exe` are in its
`compiler\` folder (see below). Other presets: `build.bat debug`, `build.bat precise` (exact guest state at faults,
slower).

To check the recompiler's instruction semantics against the host CPU:

```
out\build\release\fci-semtest.exe
```

`build.bat tools` builds only `fci-recomp` and `fci-semtest` into `out\build\tools\`, without the game; it needs no
`assets\` folder.

### Release folder

CI builds the release without any game files (`.github/workflows/release.yml`), with the official LLVM 19.1.5
release that the release also bundles:

```
powershell -File tools\get_llvm.ps1 -Out C:\llvm
cmake --preset release -DCMAKE_C_COMPILER=C:/llvm/bin/clang-cl.exe -DCMAKE_CXX_COMPILER=C:/llvm/bin/clang-cl.exe ^
      -DCMAKE_LINKER=C:/llvm/bin/lld-link.exe -DCMAKE_MT=C:/llvm/bin/llvm-mt.exe -DFCI_LLVM_DIR=C:/llvm
cmake --build --preset release --target dist
```

Run this from a Visual Studio developer prompt (x64). `tools\get_llvm.ps1` downloads LLVM and checks it against a
pinned SHA-256. The `dist` target runs `tools\dist.ps1 -Llvm C:\llvm -Zip`, which assembles
`out\dist\FarCryInstincts-Recomp-v<VERSION>-win64\` and its zip with `SHA256SUMS`, and refuses anything that comes
from the game (`game_code\`, generated sources, objects, PDBs, `.xbe`/`.iso`, shaders, an executable containing
recompiled functions). The bundled clang must have the major version that wrote `config\game_code.profdata` (19).

### PGO profile

The release build optimises the recompiled code with the clang PGO profile `config/game_code.profdata` when it
exists. The profile belongs to one version of the generated code. After a change to the recompiler, the symbols or
`config/hle.txt`, regenerate it from the repository root:

```
powershell -File tools\pgo.ps1
```

The script builds the instrumented preset `pgo-gen` (only the DLL is instrumented; it brings its own C runtime for
that), plays a scripted single-player run for 250 seconds and merges the counters into `config\game_code.profdata`.
A stale profile is not an error: functions whose code changed are compiled without profile data (correct, only
slower).

| CMake option | Effect |
|---|---|
| `-DFCI_PGO=<file.profdata>` / `OFF` / `generate` | profile for the game code (default `config/game_code.profdata`) / none / instrumented build |
| `-DFCI_LTO=ON` | ThinLTO for the game code |
| `-DFCI_X86_64_V3=ON` | AVX2/BMI2 code for the game code (requires a Haswell / Zen CPU or newer) |

## Running

Run `out\build\release\FarCryInstincts.exe`. To use a build from source elsewhere, copy `FarCryInstincts.exe`,
`d3dcompiler_47.dll` and the `game_code\` folder and put an `assets` folder with the game next to them (or above them). The log is written to `fci.log` next to the executable; `FCI_CONSOLE=1` opens a console window for it.

**Default keyboard and mouse controls (player 1):** WASD move, mouse look (click into the window to capture the
mouse), right / left mouse button = left / right trigger, Space = A, C = B, E = X, Q = Y, F = White, G = Black,
Enter = Start, Escape = Back, arrow keys = D-pad, IJKL = right stick, Left Shift / V = stick clicks. Controllers map
as on the Xbox, with LB = White and RB = Black.

## Configuration

Settings are read from `FarCryInstincts.ini` next to the executable (`key = value`, `#` or `;` comments). Every key
can also be set as an environment variable `FCI_<KEY>` (upper case), which takes precedence. The F10 menu shows all
of them, applies what can change at run time and saves to the ini file; settings that need a restart are marked
there.

| Key | Default | Meaning |
|---|---|---|
| `scale` | `auto` | Render resolution: `auto` = the output's height, `<n>%` = share of it (20–200 %), a number = multiple of 640x480 (0.5–6) |
| `msaa` | `4` | Multisampling: 1, 2, 4, 8 (limited to what the GPU supports) |
| `smaa` | `1` | SMAA on the final image (0 = off) |
| `aniso` | `16` | Anisotropic filtering, 1–16 (1 = off) |
| `shadow_scale` | `2` (Steam Deck `4`) | Shadow map resolution, 1–8 times the console's |
| `shadow_distance` | `1` | Sun shadow range, 1–8 times the console's (the map grows with it) |
| `shadow_filter` | `soft` | `soft` or `sharp` (as the console) |
| `ao` | `ssao` (Steam Deck `gtao`) | Ambient occlusion: `off`, `ssao`, `gtao` |
| `ao_strength` | `1` | 0–4 |
| `ao_resolution` | `auto` | `full`, `half`; `auto` = half above 720 lines |
| `water_quality` | `high` | `console` (the original water), `medium` (new shader), `high` (new shader with refraction) |
| `fc1_path` | (empty) | Far Cry (PC) install folder for the water's wave texture, see below |
| `lod_distance` | `4` (Steam Deck `1`) | Level-of-detail and sprite distances, 0.5–8 times the console's |
| `view_distance` | `2` (Steam Deck `1`) | Object view distance, 0.5–8 times the console's |
| `aspect` | `auto` | `auto` (the output's shape), `4:3`, `16:9`, `16:10`, `21:9`, `W:H` or a number |
| `fov` | `1` | Field of view multiplier, 0.5–2, on the game's vertical view (67.5° in play), on every aspect ratio; `2` gives 135° vertically (160° horizontally on 21:9) |
| `fps_limit` | `auto` | `auto` = display refresh rate, `30` = console, `0` = unlimited, or a number |
| `vsync` | `0` | `1` = present with the display's refresh |
| `frame_queue` | `1` (Steam Deck `2`) | Frames the game may finish ahead of display, 1–3 |
| `shader_async` | `1` | Compile new shaders in the background (`0` = wait for each) |
| `display_mode` | `windowed` | `windowed`, `borderless`, `fullscreen` (exclusive); borderless if the default window does not fit the screen |
| `monitor` | `0` | `0` = primary, `1`, `2`, ... or a device name such as `\\.\DISPLAY2` |
| `resolution` | (unset) | `<width>x<height>`: window client size or fullscreen mode |
| `hdr` | `auto` | HDR10: `auto` (when Windows HDR is on), `on`, `off` |
| `hdr_paper_white` | `200` | Brightness of white in nits (80–1000), `auto` = the Windows SDR content brightness |
| `hdr_peak` | (display) | Peak brightness in nits (100–10000); default: what Windows reports, else 1000 |
| `hdr_boost` | `0` | 0–1, stretches highlights towards `hdr_peak` |
| `fmv_scale` | `fill` | Videos on widescreen: `fill` (as large as fits without cropping) or `4:3` |
| `colour_blind` | `0` | `0` off, `1` protanopia, `2` deuteranopia, `3` tritanopia |
| `fps_counter` | `0` | `1` = show frame rate and frame times |
| `skip_intro` | `0` | `1` = skip the three logo videos at start (Ubisoft, Crytek, Far Cry Instincts) and the trailer after "Quit to main menu" (any button ends it otherwise); not the story's cutscenes |
| `language` | `english` | Text and voice language: `english`, `french`, `italian`, `spanish`, `german`, `auto` (the Windows display language). The NTSC disc has English, French, Italian and Spanish; `german` shows English there. Takes effect after a restart |
| `pause_on_focus_loss` | `0` | `1` = pause game, sound and rumble while the window is in the background |
| `mouse_mode` | `direct` | `direct` = every mouse count turns the view by the same angle, at once, with no ramp, speed cap, smoothing or dead zone; `stick` = the old way through the right stick (the game's stick handling) |
| `mouse_sensitivity` | `1` | 1 = 0.1 degrees per mouse count (900 counts = 90°), 0.01–100; `mouse_sensitivity_x` / `_y` per axis |
| `mouse_invert_y` | `0` | `1` = inverted vertical mouse look |
| `mouse_acceleration` | `0` | 0 = none (raw input); > 0 = extra gain per count/ms of mouse speed (0.05: 1.5× at 10 counts/ms), independent of the frame rate |
| `mouse_acceleration_cap` | `2` | Highest gain the acceleration reaches (≥ 1) |
| `mouse_smoothing` | `0` | 0 = off; else milliseconds (up to 200) over which each movement is spread, by time, not frames; the total stays exact |
| `mouse_zoom_scale` | `fov` | While zoomed (scope, binoculars): `fov` = slower by the zoom's field of view ratio (as the game does for the stick), `tan` = by the ratio of tan(fov/2), `off` = unchanged |
| `mouse_zoom_sensitivity` | `1` | Extra factor while zoomed (0.05–10) |
| `pad_backend` | `sdl` | `sdl`, `xinput` (XInput pads only, without SDL), `off` |
| `pad_dead_zone` | `0` | Extra stick dead zone, 0.05–0.9 (0 = the game's own only) |
| `pad_rumble` | `100` | Rumble strength in percent |
| `texture_dump` | `0` | `1` = write every game texture once to `textures\dump\` |
| `texture_replace` | `0` | `1` = use images from `textures\replace\` instead |

**Key and button bindings.** `bind_<button> = <keys>` maps keyboard keys and mouse buttons, `pad_bind_<button> =
<buttons>` maps controller buttons (several separated by commas, `none` for none). Buttons are the Xbox pad's: `a b
x y white black lt rt start back lthumb rthumb up down left right`; for the keyboard also the stick directions `lup
ldown lleft lright rup rdown rleft rright`. Keys are named like `space`, `lshift`, `f1`, `num0`, `mouse1`–`mouse5`,
`wheelup`, `wheeldown` or a virtual key code such as `0x41`; controller buttons by their position on an Xbox pad
(`a b x y lb rb lt rt start back ls rs up down left right guide misc1 touchpad`, paddles `lpaddle1` ...), and
PlayStation names (`cross circle square triangle l1 r1 ...`) work too. Example:

```
# FarCryInstincts.ini
bind_y = q, wheelup
pad_bind_black = rb, rpaddle1
```

**Texture replacement.** Dumped textures are named by a hash of their content (`<hash>.png`, cube maps
`<hash>.dds`). A file in `textures\replace\` (subfolders allowed) whose name contains the hash is drawn instead, at
any resolution; PNG and DDS (BGRA8, BC1–3, BC7) are supported, and changes are picked up while the game runs.

## Far Cry (PC) water data (optional)

With `water_quality` set to `medium` or `high`, the ocean uses a wave normal map. If you own Far Cry (PC, 2004), the
game can use its `Waterbump` texture: set `fc1_path` to the installation folder (the one containing `FCData\`), or
place the installation as `asset_fc1\Far Cry` next to the `assets` folder. Only `Textures\Waterbump.dds` is read, at
run time, from `FCData\Textures.pak` or a loose copy; nothing is copied or stored. Without it the game generates
its own waves. The log reports which source is used.

```
# FarCryInstincts.ini
fc1_path = D:\Games\Far Cry
```

## Steam Deck and Linux (Proton)

The Windows executable runs under Proton (tested with Proton 10.0 on a Steam Deck OLED). It needs the `assets` folder
next to it or above it. Shaders are compiled by Microsoft's `d3dcompiler_47.dll` from the release folder, which
Proton loads instead of its builtin one (application directory first); nothing has to be installed. The first start
compiles the game code under Proton too (about 6 minutes on the Deck).

1. Copy the unpacked release folder to the Deck, for example as `~/Games/FarCryInstincts/`, with the game in
   `assets/` (the `.iso` alone is enough).
2. In Desktop Mode, add the executable to Steam with *Add a Non-Steam Game*.
3. Under *Properties → Compatibility*, force Proton 10.0 or newer.
4. Start it in Game Mode. The built-in controls are detected through SDL3 (use the *Gamepad* Steam Input layout),
   and the game uses the display's native mode (1280x800, 90 Hz on the OLED model, 60 Hz on the LCD model).

**Proton's builtin HLSL compiler.** Without the release's `d3dcompiler_47.dll` (deleted, or a DLL override)
Proton's own compiler (vkd3d-shader) takes over. All shaders compile with it under Proton 10, but they cost the
Deck's GPU about 3 % more in single player and 8 % in split screen, and it compiles shaders about 3.4 times slower.
If a shader fails to compile (an older or different Proton whose compiler lacks something), the game shows a message
box once and goes on; objects may be invisible and `fci.log` shows `compile failed`. Putting Microsoft's compiler back
next to the executable fixes it, or installing it into the game's Proton prefix with
[protontricks](https://github.com/Matoking/protontricks): `protontricks -l` lists the games with their app IDs (a
non-Steam shortcut appears as `Non-Steam shortcut: FarCryInstincts.exe (<id>)`), then
`protontricks <id> d3dcompiler_47`. The log's `render: HLSL compiler …` line names the compiler in use.

On a Steam Deck (detected from the firmware's board and product names, or `SteamDeck=1`) some defaults change:
`ao = gtao`, `shadow_scale = 4`, `frame_queue = 2`, `lod_distance = 1`, `view_distance = 1`. Values set in the ini
file, the environment or the menu still win. `FCI_DEVICE=deck` or `FCI_DEVICE=pc` overrides the detection.

## Architecture

`fci-recomp` (`tools/recomp`) decodes the XBE with Zydis, finds the functions and turns each into a C++ function
over a guest CPU state. Guest memory is a 4 GB window at a fixed host address. The runtime (`src/`) implements the
153 imported Xbox kernel functions on Win32 and replaces the XDK's Direct3D 8 and DirectSound with host
implementations, with graphics on a Direct3D 11 renderer that decodes the game's push buffers. Library functions are
located with symbols from XbSymbolDatabase (`config/symbols.txt`); `config/hle.txt` lists the ones replaced by host
code. Details: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Known issues

- Building from source with `build.bat` needs the extracted `default.xbe`; a release (and its first start) works from
  the disc image alone.
- Display mode, monitor, resolution, render scale, MSAA, anisotropic filtering, shadow resolution, HDR on/off and
  a few other settings take effect after a restart.
- With background shader compilation (`shader_async = 1`), an object may be missing for one to three frames the
  first time its shader is needed. `shader_async = 0` waits instead, at the cost of stutter.
- HDR10 output has been verified numerically, not yet on a physical HDR display.
- Full disc dumps (with the video partition) are supported by the image reader but have only been tested with
  xiso images.
- Launching as a non-Steam game in the Steam Deck's Game Mode has not been fully verified; the game itself runs
  under Proton.
- The story's cutscenes, such as the opening on the boat, are scripted sequences in the levels and can't be skipped
  (as on the Xbox); `skip_intro` covers the logo videos at start.
- Direct mouse look has been measured on foot, while zoomed, swimming, in feral vision, on the quad and in split
  screen; boats, the glider and mounted guns use the same input path but have not been tested yet.
  `mouse_mode = stick` brings back the old behaviour.
- The Map Editor is a separate program on the disc (`ige.xbe`) that the recomp does not include; choosing it shows a
  notice and the game stays in the main menu.
- Online play (Xbox Live, system link) is not supported.

## Credits

- **[SCDA-XBERecomp](https://github.com/TGP482/SCDA-XBERecomp)** by TGP482 and Joshhhuaaa (MIT): the model for this
  whole project. Large parts are adapted from it: the Direct3D 8 device and push-buffer HLE, the DirectSound HLE and
  its XAudio2 output, the XDVDFS disc image reader, the SDL3 controller support, the F10 settings menu, ambient
  occlusion, the display output and HDR10 options, SMAA integration, colour-blind modes, FMV scaling, texture dump and
  replacement, focus pause and guest time.
- **[XbSymbolDatabase](https://github.com/Cxbx-Reloaded/XbSymbolDatabase)** from the Cxbx-Reloaded project (MIT):
  the XDK symbol data in `config/symbols.txt`.
- **[Zydis](https://github.com/zyantific/zydis)** and **[Zycore](https://github.com/zyantific/zycore-c)** by
  zyantific (MIT): the x86 decoder of the recompiler.
- **[SDL](https://github.com/libsdl-org/SDL)** (zlib licence): controller input.
- **[Dear ImGui](https://github.com/ocornut/imgui)** by Omar Cornut (MIT): the settings menu.
- **[SMAA](https://github.com/iryoku/smaa)** by Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro and
  Diego Gutierrez (MIT): antialiasing (`src/gpu/smaa_*`).
- **[XeGTAO](https://github.com/GameTechDev/XeGTAO)** by Intel: the parameters of the GTAO kernel follow its *High*
  preset.
- **[xemu](https://github.com/xemu-project/xemu)** and **[nouveau](https://nouveau.freedesktop.org/)**: NV2A method
  names.

Full licence texts are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Licence

The code of this project is released under the [MIT licence](LICENSE). Third-party components keep their own
licences, see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
