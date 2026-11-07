# Changelog

## 0.1.4 (unreleased)

- The water surface seen from below no longer flickers (#9). A recompiler bug: status flags were not passed between a
  function and the code it calls or returns to. The C runtime's hand-written x87 `acos` and `asin` rely on that, so
  they returned wrong values and sometimes NaN; under water the game computes the window to the sky with `acos`,
  and a NaN made the whole surface opaque for a frame. The recompiler now carries flags across calls and returns
  where the code reads them. Every `acos`/`asin` in the game was affected; the view up through the surface now
  shows the full window to the sky.
- Crash on a sniper shot fixed (#7): the smoke trail of the Barrett (AI snipers) and the SL8 fades out with the
  game's `tanh`, whose handlers sit behind byte-packed tables that the recompiler did not follow. It now follows code
  pointers in such tables (11 more functions; nothing else in the generated code changes). The game code is rebuilt
  once on the first start after updating.
- Hands and weapon no longer jitter while turning with the mouse (#5): they are drawn with the view they were placed
  for, while the world still turns in the same frame (no added latency).
- Light no longer ends in straight lines at the sides of a widescreen picture (#8), also at the default field of
  view: the engine's per-light screen rectangle is too narrow towards the sides, which the 4:3 console picture never
  showed. Light passes now cover the whole view whenever the picture differs from the console's.
- Level changes keep the window (#10): mode (F11/Alt+Enter), monitor, position, size and maximized state carry over
  to the next level's process, also on "Restart level" and "Quit to main menu". Settings changed in the F10 menu
  still win, and nothing is written to the ini file.
- After "Quit to main menu" the game no longer seems frozen (#11): it plays a two-minute trailer while it loads,
  which the game itself does not let you skip. Any button or key now ends it, and `skip_intro` skips it.
- Choosing the Map Editor no longer closes the game (#12): it is a separate program on the disc (`ige.xbe`) that the
  recomp does not include; a notice says so and the game stays in the main menu.
- Menu background videos play at their own frame rate at any game frame rate (#13). Before, the title screen video
  ran 2× too fast at 60 fps, 4× at 120 fps and far faster unlimited. The full-screen videos were already right.
- Mouse look in vehicles: a fast flick no longer makes the view jump to the opposite edge of a vehicle's look range
  (the game wraps the angle before limiting it; on a boat a 30° flick in one frame was enough). The mouse turns at
  most 25° sideways per frame and the rest follows in the next frames, which on foot only matters above 3000°/s at
  120 fps. The pitch limit learned on a vehicle is released again on foot.
- First start: the game-code build no longer depends on the progress window or a display. The compilers no longer
  look for a Visual Studio installation (which under Proton needs the window system), they run without a console,
  and the log shows the build's progress every 15 seconds. If the window cannot be shown, the build goes on
  without it.

## 0.1.3 (2026-11-07)

- Direct mouse look (#2): the mouse no longer goes through the right stick. Every mouse count turns the view by a
  fixed angle (0.1° at sensitivity 1), shown in the same frame, with no dead zone, acceleration ramp, turn-rate cap,
  smoothing or aim assist, the same at any frame rate. Before, the stick path capped mouse turns at 124°/s
  sideways and 78°/s up and down, and large movements arrived up to half a second late.
- New mouse settings, in the F10 menu ("Mouse and Keyboard") and the ini file: mouse look mode (`direct` or the
  old `stick`), sensitivity for both axes or per axis (0.1–10 in the menu), invert Y, optional acceleration with a
  cap, optional smoothing in milliseconds, and the sensitivity while zoomed (by the zoom's field of view, by
  tan(fov/2) or off, plus a factor).
- Language setting (#4): `language` in the ini file and the F10 menu chooses the game's text and voice language
  (English, French, Italian, Spanish, German, or the Windows display language). The NTSC disc has English, French,
  Italian and Spanish.
- `fov` now works up to 2.0 on every aspect ratio. Before, the view stopped widening at about 1.45 on 16:9 and 1.1
  on 21:9, and on 32:9 it was narrower than the console's even at 1. The engine's culling angle is decoupled from
  the projection.
- Fixed an access violation at almost every start, including the restart the game does at every level change: a
  system thread started while the C runtime was still initializing ran a thread-local constructor that allocated
  memory before the heap existed. Windows logged it quietly as an "Application Error" (ntdll.dll); in tests it once
  crashed the game at startup.
- Level changes: if the next level's process ends before its first frame, the game now shows an error message
  instead of closing silently. If two instances would not fit into the graphics memory, the old one now closes
  before the new one starts instead of overlapping. The log names the settings changed before a level change and
  the GPU memory of both processes.

## 0.1.2 (2026-11-06)

- Wider field of view (`fov` above 1) no longer cuts the light of lamps and other dynamic lights off in straight lines
  at the sides of the screen (bright or dark bands on walls and windows indoors, e.g. in the boat's cabin). The engine
  clips each light to a screen rectangle that did not follow the `fov` factor; with `fov` other than 1 the 3D draws
  now use their whole view (in split screen, each player's half). `fov = 1` is unchanged.
- `skip_intro` is read at every video, so turning it on in the F10 menu (now "Skip startup videos") takes effect at
  once instead of at the next start. Its help text says what it covers: the three logo videos, not the story's
  cutscenes, which the game can't skip.
- The log names the settings file it read (or that there is none) and whether `skip_intro` is on; the menu says when
  it cannot write `FarCryInstincts.ini`. A UTF-8 byte order mark (Notepad) no longer hides the file's first setting.
- `tools/tests/skip_intro.ps1`: end-to-end test of the setting (menu, settings file, next start).
- The release ships Microsoft's `d3dcompiler_47.dll` again, next to the executable. Proton loads it instead of its
  builtin compiler, so the Steam Deck needs no protontricks step and gets the full GPU performance (split screen about
  8 % faster than with Proton's compiler). The Proton shader fix and the message box of 0.1.1 stay as a safety net.

## 0.1.1 (2026-11-06)

- The release no longer contains Microsoft's `d3dcompiler_47.dll`. The game uses the system's HLSL compiler: the
  copy in `System32` on Windows (8.1, 10, 11), Proton's builtin one (vkd3d-shader) on the Steam Deck and Linux.
- Proton's compiler lacks `EvaluateAttributeCentroid`, which the pixel shaders of W-buffered draws (with polygon
  offset) used; they now get the centroid depth through a `centroid` input instead, the same value on every
  compiler. All shaders compile under Proton 10 without Microsoft's DLL.
- If a shader still fails to compile (another Proton version, for example), the game shows a message box once per run
  and goes on; under Proton it explains how to install Microsoft's compiler with protontricks
  (`protontricks <id> d3dcompiler_47`, also in the README).
- Proton's compiler needs no setup but is a little slower: its shaders cost the Steam Deck's GPU about 3 % more in
  single player and 8 % in split screen (about 69 instead of 76 fps), and it compiles shaders about 3.4 times slower.
  Installing Microsoft's compiler into the game's Proton prefix with protontricks gets that back (README, Steam Deck).
- Shaders are compiled once more after the update (their source changed, so the shader cache gets new entries).

## 0.1.0 (2026-11-06)

- First release. `FarCryInstincts.exe` contains no game code: on the first start it translates the player's own
  `default.xbe` (extracted, or read from the `.iso`) and compiles it into `game_code\game_code.dll`, with the
  bundled LLVM 19.1.5 (`clang.exe`, `lld-link.exe`), the recompiler and the same flags and PGO profile as the
  developer build. A progress window shows the build; it takes about 1 minute on a desktop PC and about 6 minutes
  on a Steam Deck, once. Later starts load the DLL.
- The game code DLL has a versioned interface to the executable (ABI hash, XBE hash, build key); a stale DLL is
  built again, an interrupted build continues on the next start.
- Graphics on Direct3D 11: resolution scaling, MSAA, SMAA, anisotropic filtering, higher-resolution and longer-range
  shadows, SSAO/GTAO, widescreen (Hor+) and field of view, unlocked frame rate, HDR10, level-of-detail and view
  distance multipliers, the PC ocean shader (optionally with Far Cry (PC)'s wave texture), colour-blind modes,
  background shader compilation with an on-disk cache, texture dump and replacement, fps counter.
- Audio: DirectSound on XAudio2 with the XDK's I3DL2 reverb and HRTF 3D sound, stereo or 5.1.
- Input: up to four controllers through SDL3, keyboard and mouse with remappable bindings and raw mouse look.
- Settings menu on F10 (controller: Back + Start), windowed, borderless or exclusive fullscreen, skip intro, focus
  pause, plays from the disc image, Steam Deck (Proton) defaults.
- Releases are built by GitHub Actions without any game files and ship `SHA256SUMS` and a build provenance
  attestation.
