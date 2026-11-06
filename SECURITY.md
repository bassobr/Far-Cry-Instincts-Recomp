# Security Policy

Far Cry Instincts Recomp contains no game files, no telemetry and no network service. The game
code is built on the user's machine from their own `default.xbe`: by `FarCryInstincts.exe` on its
first start (with the bundled `compiler\` folder), or by a build from source. At run time the
executable reads the game's data (extracted files or a disc image), its settings file and,
optionally, the user's replacement textures and a Far Cry (PC) installation.

## Reporting a vulnerability

Please use GitHub's private vulnerability reporting:

https://github.com/bassobr/Far-Cry-Instincts-Recomp/security/advisories/new

Do not open public issues for security problems. This is a solo-maintained project; reports are
handled on a best-effort basis. Include the version (`VERSION`) or the commit
(`git describe --tags --always`), the operating system (or the Proton version), the GPU and driver,
and the relevant part of `fci.log` (or `game_code\build.log` for the first start).

## Scope

- The executable and its runtime in `src/`, the first-start build of the game code
  (`src/app/game_code.cpp`, which runs `compiler\fci-recomp.exe`, `compiler\clang.exe` and
  `compiler\lld-link.exe` from the folder beside the executable and loads `game_code\game_code.dll`),
  the recompiler `fci-recomp`, the semantics test `fci-semtest`, the build and release scripts and
  the GitHub Actions workflows.
- Particularly interesting areas: the parsers for data the game is given at run time (the XBE
  loader `src/xbe/`, the XDVDFS disc image reader `src/kernel/disc.cpp`, the settings file
  `src/app/settings.cpp`, texture replacement `src/gpu/texture_pack.cpp` and the Far Cry (PC)
  archive reader `src/gpu/fc1_data.cpp`), and the check that a `game_code.dll` fits the executable
  (`src/runtime/game_dll.cpp`).
- The game files are supplied by the user and are out of scope. Anyone who can write to the
  release folder can replace the compiler, the DLL or the executable itself; keep it where only
  you can write.
- Third-party components (LLVM, Zydis, SDL3, Dear ImGui) should be reported upstream as well; see
  `THIRD_PARTY_NOTICES.md`.

## Supported versions

Only the latest `main` and the latest release receive fixes. There is no warranty; see `LICENSE`.

## Release integrity

- Releases are built by GitHub Actions from a tag, without any game files, and ship `SHA256SUMS`
  plus a build provenance attestation of the zip (keyless, Sigstore).
- Manual check: `sha256sum -c SHA256SUMS` and
  `gh attestation verify <zip> -R bassobr/Far-Cry-Instincts-Recomp`.
- The bundled LLVM is the official 19.1.5 release, checked against a pinned SHA-256 when the
  release is built (`tools/get_llvm.ps1`).
- Releases never contain the game, its recompiled code or anything built or extracted from it
  (`game_code.dll`, `generated/`, shader caches, textures): `tools/dist.ps1` and the release
  workflow refuse them, and an executable that names recompiled functions.
