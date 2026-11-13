# Changelog

## 0.9.1 (2026-11-13)

- **F10 menu with a controller:** after you changed a value with left or right (for example FIELD OF VIEW), down
  stopped at the first row the list had to scroll for (HDR10 OUTPUT on a 1440p screen), and the next left or right
  changed that row instead of moving on, so Exit Game at the bottom couldn't be reached. Each row's focus box was a
  few pixels wider than the list, so with a controller the menu took every row for one outside the view and lost its
  place after a value change. The rows now end at the list's edges; down goes all the way to the last row and on to
  Exit Game. The keyboard and the mouse weren't affected, and the menu looks the same.
- The game code is rebuilt once on the first start after updating (about 1 min on a desktop PC, about 5.5 min on the
  Steam Deck); saves carry over.

## 0.9.0 (2026-11-13)

0.9 is the feature freeze before 1.0: no new features from here on, only fixes from a full playthrough and from your
reports, on PC and Steam Deck. It follows 0.1.11; there was no separate 0.1.12.

- **Exit Game in the F10 menu (#40):** a button at the bottom left of the menu quits to the desktop after a
  confirmation (A or Enter exits, B or Escape goes back; keyboard, mouse and controller). It ends the program the
  same way as closing the window: the settings are saved, a save the game is writing is finished first, and on the
  Steam Deck Proton ends with it. Closing the window while the menu or the pause on focus loss holds the game now
  also lets a save that the pause interrupted finish before the program ends.
- **No more launches when strafing diagonally on the keyboard (#31):** two movement keys at once (W+A, W+D, ...)
  pushed both axes of the left stick to full. The game doesn't limit that, so the player ran at 7.3 m/s instead of
  the 5.15 m/s top speed, and above the top speed the game's collision flings a character moving up against an
  edge (pallets, tank traps) at 10 to 22 m/s. A keyboard diagonal now deflects the stick as far as a fully pushed
  stick counts after the game's dead zone, so diagonal running is as fast as straight running and never faster.
  `FCI_KEY_DIAGONAL_FULL=1` brings back the old diagonal. Controllers are unchanged.
- **ROG Ally X preset:** on a ROG Xbox Ally X (RC73XA) or a 2024 ROG Ally X (RC72LA), detected from the firmware's
  product name under Windows or Proton, the default multisampling is now 2x instead of 4x; everything else stays at
  the PC defaults. On a ROG Xbox Ally X under Windows 11 (1920x1080, 120 Hz) the story opening ran at 119.8 fps with
  a 1 % low of 109.1 fps, against 112.4 / 87.0 with 0.1.11; at rock faces it holds 120 fps (about 109 with 4x).
  `msaa` in the ini file or the F10 menu still wins, and the menu marks 2x as the device's default. `FCI_DEVICE=ally`
  or `FCI_DEVICE=pc` overrides the detection. The 2024 model and SteamOS on the Ally are not tested yet.
- **Faster multisampling on AMD graphics under Windows:** the terrain reads the pixels around its own from the
  finished frame (the fix for the seams of #19). Until now that meant resolving the whole multisampled frame once
  per terrain piece, 7 to 10 times a frame; on the Radeon 890M of the ROG Xbox Ally X each of those costs about
  0.2 ms at 1080p 4x. The game now draws the piece once more just before, without colour, and averages only the
  pixels it will read. The picture is the same (single pixels differ by a few levels). On the Ally X at 4x and an
  unlimited frame rate: 98.6 -> 108.6 fps at a rock face, 118.1 -> 141.0 fps in the story opening. It is used on
  AMD graphics under Windows and on a detected ROG Ally X; NVIDIA, Intel and the Steam Deck keep the resolve as
  before. `FCI_MIRROR_TAPS=resolve` brings back the old way, `FCI_MIRROR_TAPS=pass` forces the new one.
- The recompiler now treats the game's frame-pointer accesses (`[ebp + offset]` in functions that keep their local
  variables there) like stack accesses and skips the hardware-register check for them. The game code is 2.4 %
  smaller; on the Steam Deck the frame rate is unchanged. `FCI_RECOMP_EBP_FRAME=0` brings back the old code.
- The game code is rebuilt once on the first start after updating (about 1 min on a desktop PC, about 5.5 min on the
  Steam Deck); saves carry over.

## 0.1.11 (2026-11-12)

- **Shadow cascades: moving objects (#16):** with `shadow_cascades = on`, the shadows on and of things that move (a
  boat or car you drive, characters, pushed objects) no longer jump between two positions every frame. The game keeps
  memory for only one shadow map per frame, so the near and the far map were drawn in turns, one per frame, and
  whatever moved was a frame old in the map that was not redrawn; a driving boat's front cover shaded itself dark every
  other frame. Both maps are now drawn in every frame. This costs some speed: about 0.5 ms per frame on a desktop GPU,
  and on the Steam Deck (Shadow Distance 8x) about 1.5 ms while driving and 3 ms standing, so there cascades are best
  left off. Shadow Cascades stays off by default, and on the Deck the default Shadow Distance doesn't use them.
- Fixed a rare crash in the sound code when a level is torn down a few seconds after it started (a restart or level
  change right after loading); seen once on the Steam Deck.
- The game code is rebuilt once on the first start after updating (about 1 min on a desktop PC, about 5.5 min on the
  Steam Deck); saves carry over.

## 0.1.10 (2026-11-11)

- **Widescreen HUD (experimental, #20):** new setting **HUD LAYOUT (EXPERIMENTAL)** in the F10 menu (Display) and
  `hud_aspect` in `FarCryInstincts.ini` (`4:3` = as before, the default; `16:9`; `auto` = the screen's edges; applied
  immediately). With `16:9` or `auto` the HUD elements the game places at the edges of its 4:3 picture move out to the
  edges of a 16:9 box or of the screen: radar, health/armour, stance, breath and feral bars and the messages at the top
  left (hints, objectives, pickups, "Saving...") to the left, ammunition and the icon at the top right to the right.
  The compass, crosshair, damage indicator, pickup icons, dialogue boxes and menus stay in the middle, and no element
  changes its size or shape. Single player only: multiplayer, split screen and map-editor maps keep the 4:3 HUD, and
  so does the whole HUD while binoculars or a scope are up. On a 21:9 or 32:9 screen `16:9` keeps the elements closer
  to the middle than `auto`.
- Keyboard (#38): number keys 1-4 for the D-pad and R for reload are now extra default keys; the old keys and your own
  ini bindings stay.
- The game code is rebuilt once on the first start after updating; saves carry over.

## 0.1.9 (2026-11-10)

- Speech volume (#25): new setting **SPEECH VOLUME** in the F10 menu (0 dB = console, +3, +6) and `speech_volume` in
  `FarCryInstincts.ini` (−12 to +12 dB). It raises everything spoken (player, radio, other characters, cutscenes) and
  leaves music and effects as they are. The quiet dialogue itself is the Xbox's own mix, reproduced as on the console.
- Weapons no longer look see-through and bright lights no longer leave copies of themselves (#27, #30): the game's
  glare (bloom) and eye adaptation build their glow from small downscaled copies of the frame, blurred for the Xbox's
  640x480. We rendered those at the render scale, so the glow kept sharp, offset copies of bright surfaces (over dark
  weapons, around lamps at night). They now run at the console's size and read a properly averaged copy of the frame;
  the glow is smooth again, the rest of the picture stays at full resolution. `FCI_SCALED_POST=1` restores the old way.
- Light shafts in the Mines caves no longer show dark lines (#24): our SSAO/GTAO mistook the additive shafts for the
  game's per-object light passes and was sometimes applied after them. It now goes in before such effects.
- Split screen on widescreen: the lower player's sky (and sun) is no longer cut to the 4:3 middle with black beside
  it (the lower view draws its sky before its first 3D object, and that sky was still taken for the upper view's
  HUD), and with 3 or 4 players each quarter's HUD and crosshair are centred in that quarter instead of being pulled
  towards the middle of the screen. `FCI_NO_SPLIT_VIEW_START=1` / `FCI_NO_VIEW_HUD_CENTRE=1` restore the old way.
- The game code is rebuilt once on the first start after updating (about 45 s on a desktop PC, about 5.5 min on the Steam
  Deck); saves from 0.1.8 carry over.
- Level changes no longer leave a hidden, frozen copy of the game behind when the game ends at the wrong moment
  (window closed, crash, killed from outside) during the restart between levels: the new process now runs from its
  start instead of waiting suspended for the old one. Such a leftover also stopped the next start from repairing a
  damaged profile (#29); one left by an older version is now ignored. Closing the window during a level change also
  ends the new process instead of letting the game come back in it.

## 0.1.8 (2026-11-10)

- **Save games** (#29):
  - Saves are no longer left broken. A save could be cut off when the game was closed or relaunched between levels; all pending writes now finish first.
  - Saves are backed up. On start, a damaged save is repaired from the backup, or moved aside so it no longer blocks the profile.
  - The game resumes from the save it last wrote. On Windows, reading a file also updates its access time, which the Xbox never does, so the game could pick an older save after a restart. One result was losing the last checkpoint and starting the chapter over (#35).
  - Tested on Windows and on the Steam Deck.
- **Black Forest no longer gets stuck at the start** (#22). An AI attached to the scripted scene was below the water line for a few milliseconds. At higher frame rates the game caught that moment and decided the character had drowned, so the sequence never went on. The opening now plays through at every frame rate.
- **Clicking into the window to focus it no longer fires your weapon** (#28). This includes grabbing the title bar to move the window, and a window that was started in the background.
- **Frame rate and game logic.** The game was written for the Xbox's ~30 fps, and at higher frame rates several parts of its logic behaved differently. These now behave as at 30 fps:
  - boats, the Zodiac and the jet ski (#26), which were up to 40 % slower at 120 fps;
  - swimming: Jack could stay under the surface and drown at 120 fps, and currents were weaker;
  - how characters step off edges and down stairs, and when they land;
  - fall damage: a 9 m fall was lethal at 120 fps;
  - drowning, which took nearly three times as long at 120 fps;
  - AI paths around corners: an enemy in Mines stayed on a landing instead of coming down.

  Each fix can be switched off for comparison. The README's new chapter "Frame rate and game logic" lists them all, together with what is still open.
- **The frame rate is capped at 120 fps.** The game logic is verified up to 120 fps; `fps_limit` and the menu go no higher. For more, set `unsupported_uncapped_fps = 1` in `FarCryInstincts.ini`. This is unsupported, and bugs above 120 fps won't be fixed.
- **Shadow cascades no longer flicker** (#16). Measured on NVIDIA, AMD (ROG Ally X) and the Steam Deck, at the reporter's spot and around the levels, standing, turning and walking. Large rock and wall shadows near the player are back. The cost is about 0.1–0.4 ms per frame. Cascades stay off by default; turn them on with Shadow Cascades in the F10 menu.
- **No more seams in the terrain with MSAA** (#19). They showed as fine lines, for example on Cliffs. The fix keeps a copy of what the previous terrain pass drew, so it costs almost no performance: the Steam Deck holds 90 fps with it.
- **Widescreen HUD and masks:**
  - The HUD stays in the 4:3 centre while swimming (#17). In some water areas it used to stretch across the whole screen.
  - Binoculars and scopes keep their round shape on widescreen, and their dark border reaches the screen edges.
- **A crash at the start of Cliffs is fixed.** It was a bug in the original game's collision code: rare contact shapes overflowed a buffer and overwrote a physics object.
- **Akimbo controls are documented** (#34). Each trigger fires its own gun; on keyboard and mouse that is the left and right mouse button.
- **First start after updating:**
  - The game code is rebuilt once, with a new PGO profile.
  - The shader cache is rebuilt once, because a shader interface fix changed the shaders. They had passed one value in mismatched registers; drivers coped, but it was undefined behaviour.

## 0.1.7 (2026-11-07)

- Water no longer looks blocky or choppy (#14): the game's small wave-distortion texture holds signed values, which
  the Xbox filters as signed; we filtered the raw bytes as unsigned, so wherever a wave value crossed zero the
  reflection jumped to a completely different spot, giving hard-edged blocks with round outlines. This affected every
  water setting and the surface seen from below, in all earlier versions. The original water's depth comparison
  also scales up to the render resolution properly now.
- Shadow cascades no longer flicker (#16, #18): after drawing the near shadow map, the console's short shadow range
  was left in place for the rest of the frame, so on every second frame objects and ground a few metres away were
  shaded without their shadow. Measured frame by frame while walking and turning, the flicker is gone at 30, 60, 120
  fps and unlimited. Every view now keeps its own pair of maps. Shadow Cascades switches at once in the F10 menu
  (before, it only applied after a restart). It stays off by default for now.

## 0.1.6 (2026-11-07)

- Shadow cascades are off by default (#16). In 0.1.5 they drew the near and the far shadow map on alternate frames,
  and on some systems the shadows near the player vanished every second frame, which flickered. `shadow_cascades = on`
  keeps the experimental path until a flicker-free version is ready.
- Feral vision on widescreen (#17): the HUD no longer stretches across the whole screen while feral vision is on (the
  extra 3D passes feral vision draws after its tint were taken for a new view, which switched off the HUD's 4:3
  centring), and the feral colour tint covers the whole screen instead of only the 4:3 middle.

## 0.1.5 (2026-11-07)

- Original water (#14): `water_quality = original` draws the game's own water exactly as the Xbox does, with none of
  the PC water changes. The old `console` mode was not the original: a translation bug read the depth buffer of the
  game's water shader from memory that is never written, so the sea came out opaque turquoise everywhere. That is
  fixed, and `console` now means `original`.
- New water (`medium`/`high`): no more violet patches (the game's deep-blue sky was reflected too strongly on wave
  flanks), and the water's own colour, foam and sun glints follow the light around them, so dark and foggy levels
  such as the swamp stay murky instead of glowing turquoise.
- Shadow cascades (#16): with Shadow Distance 2x or more, the shadows near you get their own map drawn exactly as on
  the console, and the far shadows continue in the large map. Before, at large distances the game picked fewer and
  simpler shadow casters (ferns lost their shadows, palm crowns no longer lined up with their trunks). New setting
  `shadow_cascades` (on by default).
- Button prompts (#15): the game's button icons follow the device you use: your bound keys and mouse buttons with
  keyboard and mouse, Cross/Circle/Square/Triangle on PlayStation pads, Switch symbols on Switch pads. New setting
  `button_prompts` (`auto`, `xbox`, `keyboard`, `playstation`, `switch`); all icons are drawn at runtime.
- High frame rates (#6): the view sway while swimming was added once per frame, so above 30 fps the view tilted far
  more and you sank or rose, and ladders in the water climbed up or down at random; it is now the same at any frame
  rate. Scripted vehicles (e.g. the Humvee at the start of Native) missed their path above about 150 fps because the
  tyres put less engine force on the road at very small physics steps; wheel forces are now applied at most 60 times
  per second, as in the console's physics substeps, while everything still moves every frame.
- New PGO profile; the game code is rebuilt once on the first start after updating.

## 0.1.4 (2026-11-07)

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
