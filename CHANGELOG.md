# Changelog

## 0.1.2 (unreleased)

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
