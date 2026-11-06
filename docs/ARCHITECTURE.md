# Architecture

The result is a standalone 64-bit `FarCryInstincts.exe`. It contains the game's code, translated ahead of time
into C++, and loads only the original game's data from `assets\` (the disc image or the extracted files).

```
assets\default.xbe ──► fci-recomp ──► generated\*.cpp ──┐
                                                        ├──► FarCryInstincts.exe ◄── assets\ (levels, sound, video)
src\runtime, src\kernel, src\xdk, src\gpu, ... ─────────┘
```

## 1. Recompiler (`tools/recomp`)

1. **Loading.** `src/xbe` maps the XBE the way the Xbox lays it out in memory.
2. **Analysis.** Recursive descent from the entry point. Further functions are found through `call` targets, code
   pointers in data sections (vtables, callbacks) and immediates. Jump tables (`jmp [reg*4+table]`) are resolved.
   Functions that were found only through a pointer and contain instructions no Xbox title executes (port I/O,
   `arpl`, BCD, segment instructions) are treated as data and dropped together with everything found only through
   them. A report goes to `generated/analysis.txt`.
3. **Symbols.** `config/symbols.txt` holds the addresses of the XDK 5849 library functions in this XBE, found
   offline with XbSymbolDatabase and converted by `tools/symbols.py`; `config/symbols_fci.txt` adds symbols found by
   hand. The names become extra entry points and `SYM_<name>` constants for the host code (`generated/image_symbols.h`).
4. **Code generation.** Every function becomes a C++ function `void f_XXXXXXXX(Cpu& c)`; the semantics helpers are
   in `src/runtime/guest.h`. The output is split into 128 files (`generated/code_NNN.cpp`) plus the dispatch table
   (`generated/table.cpp`).
   - The guest registers, the x87 stack and the XMM/MMX registers live in `Cpu`. Inside a function they are held in
     locals and written back only where control leaves it (calls, returns, tail jumps). Recompiled functions use
     clang's `preserve_none` convention.
   - Guest memory is a 4 GB window at the fixed host address `0x2000000000`, so a guest address is a constant
     offset. Accesses that could reach the hardware range (`0xFD000000` and up: GPU, APU, USB, flash) take a slow
     path to the MMIO emulation; stack accesses and accesses through a base register that cannot reach it skip the
     check.
   - Status flags are local variables. A liveness analysis over the function's control flow decides per instruction
     which flags are read later; only those are computed. Flags are treated as dead across calls; on a tail jump
     they are handed over in `Cpu`.
   - Branches inside a function are `goto`, jump tables `switch`, jumps into other functions tail calls. Loop back
     edges are preemption points for the scheduler. Direct calls call `f_XXXXXXXX` directly, indirect ones go through
     `rt::Call` and the table `rt::kFunctions`, kernel imports through `rt::KernelCall(ordinal)`.
   - `lock` instructions and `xchg`, `xadd`, `cmpxchg` with a memory operand are atomic on the host.
   - The CRT's `_setjmp3`/`longjmp` (Lua's protected calls) get host landing points; `_ftol2` is emitted inline.
   - Instructions that cannot be translated become `rt::Unimplemented` (a run-time trap).
5. **Host replacements.** A function named in `config/hle.txt` (or in `tools/recomp/hooks.h`) is emitted as a call
   to the host implementation `hle::<name>`; its recompiled body stays available as `o_XXXXXXXX`.

`fci-semtest` (`tools/semtest`) runs the generated semantics of a set of instructions against the host CPU with
random inputs and reports any difference in registers or flags.

The generated code can be optimised with a clang PGO profile (`config/game_code.profdata`, regenerated with
`tools/pgo.ps1`), and optionally with ThinLTO (`FCI_LTO`) or x86-64-v3 code (`FCI_X86_64_V3`).

## 2. Runtime (`src/`)

| Part | Role |
|---|---|
| `runtime` | Guest address space (4 GB reserved, pages committed on demand; virtual, physical/contiguous and system regions as on the Xbox), dispatch table, guest threads with the Xbox's KPCR/KTHREAD/TLS layout, the single-CPU scheduler, logging, profiler |
| `kernel` | The 153 imported `xboxkrnl` functions on Win32: memory, files (the DVD drive from `assets\` or straight from the `.iso` through an XDVDFS reader, the hard disk in `data\hdd\` next to the executable), threads, synchronisation, timers, time, title relaunch |
| `xdk` | Host implementations of XDK library functions: Direct3D 8 (`d3d8.cpp`), DirectSound (`dsound.cpp`), and game-specific hooks for widescreen and field of view (`game.cpp`), shadow range (`shadows.cpp`) and level of detail (`lod.cpp`) |
| `gpu` | The Direct3D 11 renderer: Kelvin (NV2A 3D class) method parser, vertex programs and register combiners translated to HLSL, textures, render targets, presentation with SMAA, HDR10 and colour-blind filters, ambient occlusion, the PC water shader, texture dump/replace, the fps overlay |
| `audio` | Host output of the DirectSound HLE through XAudio2 (`hle_output.cpp`); the low-level MCPX APU emulation (`apu.cpp`) |
| `hle` | Controllers: the XDK's XInput/XPP entry points (`xinput.cpp`) on SDL3 or XInput (`host_pad.cpp`), keyboard and mouse (`pc_input.cpp`); skip intro; direct level start for testing |
| `app` | `main`, settings (`FarCryInstincts.ini`), display output options, PC display features, focus pause, the F10 settings menu (Dear ImGui) |
| `video` | The window, and for the low-level graphics path the scanned-out frame buffer with the video overlay |
| `xbe` | XBE loader, shared with the recompiler |

### Scheduling

The Xbox has one CPU core and titles rely on it: no two guest threads ever run at the same time. Guest threads
hand a single "CPU" around. Blocking kernel calls give it up, and a quantum timer asks the running thread to yield
at its next loop back edge. Interrupts, DPCs and timers take the CPU the same way and respect the IRQL. Guest time
(`rdtsc`, the kernel clocks) runs at the Xbox's 733 MHz rate.

## Why HLE for the XDK libraries

Direct3D 8 on the Xbox writes push-buffer commands for the NV2A GPU directly. Instead of emulating the GPU, the
device is created on the host (the NV2A is never initialised) and the game's D3D state functions keep running
recompiled: what they push into a scratch push buffer is decoded on the spot (`gpu/kelvin.cpp`) into renderer
state. Only device creation, the GPU synchronisation points, recorded push buffers, vblank waits, the gamma ramp
and the video overlay are host code. Decoding and drawing run on a render thread. Render targets stay on the host
GPU, which allows a higher resolution, MSAA, widescreen and higher frame rates. The same applies to DirectSound:
the library's functions are replaced by a host mixer (`xdk/dsound.cpp`) that reads buffers and streams from guest
memory and applies the library's own I3DL2 reverb and HRTF tables.

The earlier low-level path, which runs the recompiled libraries on an emulated NV2A (`gpu/nv2a.cpp`,
`gpu/pgraph.cpp`) and MCPX APU (`audio/apu.cpp`), is still in the code and can be selected with `FCI_LLE_GFX=1` and
`FCI_LLE_AUDIO=1`.
