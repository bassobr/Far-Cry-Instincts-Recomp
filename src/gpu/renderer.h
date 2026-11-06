// Executes Kelvin draws and clears with Direct3D 11. Render targets live on the host GPU and are written
// back to guest memory when the guest can look at them (semaphore release, texture reads of them), so the
// display and the CPU see what the NV2A would have drawn.
#pragma once

#include <mutex>

#include <cstdint>

#include "gpu/kelvin.h"

struct ID3D10Blob;

namespace gpu::render {

// D3DCompile (same arguments) for every HLSL compile of the exe. A failure is logged; the first of a run also shows
// a message box (from its own thread, the game goes on): the HLSL compiler is the system's (Windows' own, Proton's
// builtin), and one that lacks something leaves objects invisible. Under Wine/Proton the box explains protontricks.
long CompileHlsl(const void* source, size_t size, const char* name, const void* defines, void* include,
                 const char* entry, const char* target, unsigned flags1, unsigned flags2, ID3D10Blob** code,
                 ID3D10Blob** errors);

void DrawPrimitives(const kelvin::State& s, const kelvin::Draw& d);
void Clear(const kelvin::State& s, uint32_t flags);
void Flush();  // dirty render targets back to guest memory

// occlusion counting: pixels that pass the depth test while enabled, until cleared. A report (16 bytes:
// timestamp, count, status 0) is written once the host GPU has the count, as the NV2A writes it once its
// pipeline got there; waiting for it here would hold up the push buffer.
void SetPixelCounting(bool enable);
void ClearPixelCount();
void QueueReport(uint8_t* report);
void PollReports();  // writes the reports whose counts are in, oldest first

// HLE graphics (xdk/d3d8.cpp): render targets stay on the host GPU, nothing is written back to guest memory
// unless the CPU asks for it; flips present the front buffer's host copy directly (gpu/present.cpp)
void SetHle(bool on);
void Present(uint32_t front);  // physical address of the frame buffer to show, 0 for none
// the CPU reads a resource at this physical address: targets drawn there are written back; true if any was
// late: a lock that does not wait for the GPU (NOOVERWRITE): the newest finished copy, without a stall
bool SyncToGuest(uint32_t address, bool late = false);
void SetGammaRamp(const uint8_t ramp[768]);
// performance: held while the device context is in use (the render thread's commands, the settings menu's frames,
// CPU reads of render targets)
std::recursive_mutex& ContextLock();

}  // namespace gpu::render
