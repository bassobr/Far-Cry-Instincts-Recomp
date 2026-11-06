# Changelog

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
