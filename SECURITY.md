# Security Policy

Far Cry Instincts Recomp contains no game files, no telemetry and no network service. The game
executable is built on the user's machine from their own `default.xbe`; at run time it reads the
game's data (extracted files or a disc image), its settings file and, optionally, the user's
replacement textures and a Far Cry (PC) installation.

## Reporting a vulnerability

Please use GitHub's private vulnerability reporting:

https://github.com/bassobr/Far-Cry-Instincts-Recomp/security/advisories/new

Do not open public issues for security problems. This is a solo-maintained project; reports are
handled on a best-effort basis. Include the commit (`git describe --tags --always`), the operating
system (or the Proton version), the GPU and driver, and the relevant part of `fci.log`.

## Scope

- The recompiler `fci-recomp`, the semantics test `fci-semtest`, the runtime in `src/` and the
  executable built from it, the build scripts and the GitHub Actions workflows.
- Particularly interesting areas: the parsers for data the game is given at run time (the XBE
  loader `src/xbe/`, the XDVDFS disc image reader `src/kernel/disc.cpp`, the settings file
  `src/app/settings.cpp`, texture replacement `src/gpu/texture_pack.cpp` and the Far Cry (PC)
  archive reader `src/gpu/fc1_data.cpp`).
- The game files are supplied by the user and are out of scope.
- Third-party components (Zydis, SDL3, Dear ImGui) should be reported upstream as well; see
  `THIRD_PARTY_NOTICES.md`.

## Supported versions

Only the latest `main` and the latest release receive fixes. There is no warranty; see `LICENSE`.

## Release integrity

- Releases are built by GitHub Actions from a tag and ship `SHA256SUMS` plus a build provenance
  attestation of the zip (keyless, Sigstore).
- Manual check: `sha256sum -c SHA256SUMS` and
  `gh attestation verify <zip> -R bassobr/Far-Cry-Instincts-Recomp`.
- Releases never contain the game, its recompiled code or anything built or extracted from it
  (`FarCryInstincts.exe`, `generated/`, shader caches, textures). CI has no game files.
