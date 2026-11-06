// FCI_TRACE_HITCHES=1: frames that took much longer than the frames around them, with what happened in them.
// Producers anywhere note events (shader compiles, texture and target creation, file reads); the render thread logs
// a breakdown of every frame over twice the running average (gpu/kelvin.cpp), the guest thread its own long frames
// (xdk/d3d8.cpp).
#pragma once

#include <cstdint>

namespace rt::hitch {

enum Event { kShader, kTexture, kTarget, kFileRead, kUpload, kShaderMade, kSkippedDraw, kCount };
// a scope's time as an event
struct Timed {
    Event e;
    int64_t t0;
    explicit Timed(Event e);
    ~Timed();
};

bool Enabled();
// FCI_TRACE_HITCHES=1 logs frames over 2x the average, =1.4 over 1.4x
double Factor();
// frames presented so far (gpu/kelvin.cpp counts them)
uint64_t FrameNumber();
void CountFrame();
// the GPU time of a frame (timestamp queries, FCI_GPU_TIMING), logged when it is long
// an event that took ticks (QueryPerformanceCounter) and moved bytes; thread safe
void Note(Event e, int64_t ticks, uint64_t bytes = 0);

struct Counts {
    uint64_t n[kCount] = {};
    int64_t ticks[kCount] = {};
    uint64_t bytes[kCount] = {};
};
// the events since the last call
Counts Take();
// " 2 shaders 23.1 ms, 3 textures 1.2 MB 4.0 ms, ..." or ""
const char* Describe(const Counts& c, char* out, int size);

// ---- FCI_HITCH_STACKS=<ms>: the stacks of the render thread in a decode, and of the presenting guest thread in a
// frame, that last longer than ms (a sampler thread looks every ms / 4; up to 4 samples each) ----
void DecodeBegin();
void DecodeEnd();
void GuestFlip();

int64_t Now();
double Ms(int64_t ticks);

}  // namespace rt::hitch
