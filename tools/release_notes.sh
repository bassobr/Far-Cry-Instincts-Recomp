#!/bin/sh
# Release notes for GitHub: the version's section of CHANGELOG.md, how to install, how to verify the download.
#   sh tools/release_notes.sh <version> [<previous tag>] > notes.md
ver="$1"; prev="$2"
{
  echo "## What's new"
  echo
  awk -v v="$ver" 'index($0, "## " v " ") == 1 {f=1; next} f && /^## / {exit} f' CHANGELOG.md | sed '/./,$!d'
  echo
  echo "## Install"
  echo
  echo "Unpack the zip, put your own copy of Far Cry Instincts (disc image or extracted files) into the \`assets\` folder"
  echo "and start \`FarCryInstincts.exe\`. The first start translates and compiles the game code once (about a minute on a"
  echo "desktop PC, about 6 minutes on a Steam Deck); see the README for details. Settings: **F10** in the game."
  echo
  echo "## Verify the download"
  echo
  echo '```'
  echo "sha256sum -c SHA256SUMS"
  echo "gh attestation verify FarCryInstincts-Recomp-v${ver}-win64.zip -R bassobr/Far-Cry-Instincts-Recomp"
  echo '```'
  if [ -n "$prev" ]; then echo; echo "**Full changelog**: https://github.com/bassobr/Far-Cry-Instincts-Recomp/compare/${prev}...v${ver}"; fi
}
