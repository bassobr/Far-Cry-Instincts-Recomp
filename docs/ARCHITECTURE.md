# Architecture

The result is a 64-bit `FarCryInstincts.exe` without any game code, and next to it `game_code\game_code.dll` with
the game's code, translated ahead of time into C++. The executable loads only the original game's data from
`assets\` (the disc image or the extracted files). The DLL is built from the player's own `default.xbe`: by the
executable itself on its first start (a release), or by CMake (the developer build).

```
assets\default.xbe ──► fci-recomp ──► *.cpp ──► clang ──► lld-link ──► game_code\game_code.dll
                                                             ▲              │ fci_game_code (tables)
                                  compiler\FarCryInstincts.lib              ▼
src\runtime, src\kernel, src\xdk, src\gpu, ... ─────────────────────► FarCryInstincts.exe ◄── assets\ (levels, sound, video)
```

### The interface between the executable and the DLL

Declared in `src/runtime/guest.h`:

- The DLL has exactly one export, `fci_game_code` (`rt::GameCode`): a version, the ABI hash, the hash of the XBE it
  was translated from, the first-start build key, the table of recompiled functions, the table of originals
  (`o_XXXXXXXX`, the recompiled bodies of the functions host code replaces), and pointers to its hot data.
- The executable exports the runtime functions (`FCI_RT_API`) and the host replacements. The replacements are
  declared in `hle_hooks.h`, which `fci-recomp --host-headers` writes from `config/hle.txt` without an XBE. It also
  exports the C runtime functions the generated code calls (`memcpy`, `floor`, `_setjmp`, ...) from its own static
  C runtime. The DLL links against the executable's import library, `compiler\FarCryInstincts.lib`.
- The DLL has no C runtime of its own (`/NOENTRY /NODEFAULTLIB`, `/GS- /Zl`). It is compiled without the MSVC and
  Windows SDK headers (`-nostdinc`), against the small headers in `src/runtime/freestanding/`; they declare the C
  functions as the UCRT does, so clang still treats them as builtins and generates the same code.
- What was inline before stays inline: memory accesses, the MMIO check and the flag helpers. The dispatch table of
  indirect calls and the preemption flag live in the DLL, so the hot code reads them RIP-relative, as in a static
  build. Apart from the stack cookies the generated machine code is the same as when it was linked into the
  executable.
- `FCI_ABI_HASH` is a SHA-256 over `guest.h`, the freestanding headers, the recompiler, the XBE loader, the
  configuration and the compiler flags, computed by CMake (line endings do not matter). It is built into the
  executable (`fci_abi.h`) and into the DLL. A DLL with another hash, another XBE hash or another build key is
  stale and is built again; the relaunch between levels (`FCI_RELAUNCHED`) only loads it.

### The first start

`src/app/game_code.cpp` (adapted from SCDA-XBERecomp) builds the DLL when it is missing or stale, using the
`compiler\` folder beside the executable:

1. `compiler\fci-recomp.exe` translates the `default.xbe` it finds (extracted, or read from the `.iso`) with the
   configuration in `compiler\config\`.
2. `compiler\clang.exe` (LLVM 19.1.5) compiles the 130 files in parallel with the same flags and the same PGO profile
   as the developer build (`compiler\config\game_code.profdata`). It runs one process per logical core, at most one
   per 640 MB of free memory (a compiler peaks at about 340 MB), the largest files first, at below-normal priority,
   in a job object, so closing the window or a crash ends the compilers too.
3. `compiler\lld-link.exe` links the objects against `compiler\FarCryInstincts.lib`.

A progress window shows the share done and the remaining time. A mutex keeps two starts from building at once.
The work happens in `game_code\work\`. Finished objects are kept until the link, so an interrupted build continues
on the next start. The folder is deleted after a successful build. The log is `game_code\build.log`, and a failure
shows its last lines. Then the game starts. On a Ryzen 9 7950X3D (32 threads) the build takes about 41 s; on a
Steam Deck OLED (Proton 10, 8 compilers) about 5.8 min. It needs about 1 GB of disk space while it runs and about
60 MB afterwards.

| Environment variable | Effect |
|---|---|
| `FCI_REBUILD_GAME_CODE=1` | builds the DLL again even when it is current |
| `FCI_GAME_CODE_JOBS=<n>` | runs this many compilers at once |
| `FCI_GAME_CODE_PARTITIONS=<n>` | splits the code into this many files instead of 128 (`fci-recomp --partitions`) |
| `FCI_KEEP_GAME_CODE_BUILD=1` | keeps `game_code\work\` (sources, objects) after the build |

## 1. Recompiler (`tools/recomp`)

1. **Loading.** `src/xbe` maps the XBE the way the Xbox lays it out in memory.
2. **Analysis.** Recursive descent from the entry point. Further functions are found through `call` targets, code
   pointers in data sections (vtables, callbacks) and immediates. Jump tables (`jmp [reg*4+table]`) are resolved.
   Code pointers in byte-packed structures whose address the code takes (an immediate pointing to a data address
   that is not 4-aligned and does not start a string) count too: for example the handler tables of the CRT's x87
   math (`tanh`, `exp`), which its dispatcher reaches with `jmp [ebx]`.
   Functions that were found only through a pointer and contain instructions no Xbox title executes (port I/O,
   `arpl`, BCD, segment instructions) are treated as data and dropped together with everything found only through
   them. A report goes to `generated/analysis.txt`, with an audit of every value that points into code (aligned and
   unaligned data, gaps in the code, immediates) by what it hits: a function, an instruction without a function, or
   unreached code.
3. **Symbols.** `config/symbols.txt` holds the addresses of the XDK 5849 library functions in this XBE, found
   offline with XbSymbolDatabase and converted by `tools/symbols.py`; `config/symbols_fci.txt` adds symbols found by
   hand. The names become extra entry points and `SYM_<name>` constants for the host code (`image_symbols.h`,
   written by `fci-recomp --host-headers` from the configuration alone).
4. **Code generation.** Every function becomes a C++ function `void f_XXXXXXXX(Cpu& c)`; the semantics helpers are
   in `src/runtime/guest.h`. The output is split into 128 files (`generated/code_NNN.cpp`) plus the tables and the
   DLL's export (`generated/table.cpp`).
   - The guest registers, the x87 stack and the XMM/MMX registers live in `Cpu`. Inside a function they are held in
     locals and written back only where control leaves it (calls, returns, tail jumps). Recompiled functions use
     clang's `preserve_none` convention.
   - Guest memory is a 4 GB window at the fixed host address `0x2000000000`, so a guest address is a constant
     offset. Accesses that could reach the hardware range (`0xFD000000` and up: GPU, APU, USB, flash) take a slow
     path to the MMIO emulation; stack accesses and accesses through a base register that cannot reach it skip the
     check.
   - Status flags are local variables. A liveness analysis over the function's control flow decides per instruction
     which flags are read later; only those are computed. A whole-program pass works out, to a fixpoint, which flags
     each function reads on entry and which flags its callers read after it returns; only there are flags handed
     over in `Cpu` across calls and returns (the hand-written x87 math of the C runtime relies on it). On a tail
     jump they are handed over as far as the target reads them.
   - Branches inside a function are `goto`, jump tables `switch`, jumps into other functions tail calls. Loop back
     edges are preemption points for the scheduler. Direct calls call `f_XXXXXXXX` directly, indirect ones go through
     `rt::CallIndirect` (the table lookup inline) or `rt::Call`, kernel imports through `rt::KernelCall(ordinal)`.
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
