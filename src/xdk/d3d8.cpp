// Direct3D 8 (XDK 5849) on the host: HLE graphics.
// Adapted from SCDA-XBERecomp (MIT, Copyright (c) 2026 TGP482, Joshhhuaaa): src/xdk/d3d8.cpp. Like SCDA this
// creates the device itself (the NV2A is never initialised) and points the device's push buffer pointer at a
// scratch buffer that MakeSpace rewinds. Unlike SCDA, the title's D3D state calls stay recompiled: what they
// push into the scratch buffer is the NV2A state the XDK computes (texture stages, combiners, fixed function,
// vertex arrays), and it is decoded on the spot by the method parser of the LLE path (gpu/kelvin.cpp) into the
// state the renderer draws from. Draws, clears, render target changes, visibility tests and flips arrive in
// order in that stream; only device creation, the GPU synchronisation points, recorded push buffers, vblank
// waits and the gamma ramp are host code here. The renderer keeps every render target on the host GPU (no
// copies to guest memory), which allows resolution scaling and antialiasing (gpu/renderer.cpp, gpu/present.cpp).
// FCI_LLE_GFX=1 runs all of it recompiled on the emulated NV2A as before.
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app/pcmode.h"
#include "app/settings.h"  // performance: frame_queue
#include "gpu/kelvin.h"
#include "gpu/nv2a.h"
#include "gpu/present.h"
#include "gpu/renderer.h"
#include "runtime/hitch.h"  // performance: FCI_TRACE_HITCHES
#include "runtime/memory.h"
#include "runtime/profiler.h"
#include "runtime/scheduler.h"
#include "xdk/hle.h"

namespace {

bool HleOff() { return !xdk::HleGraphics(); }

using rt::R32;
using rt::W32;
using xdk::CallGuest;

// D3D globals of this build (same XDK as SCDA's; the device is static, D3D__pDevice points at it)
constexpr uint32_t kDevicePtr = SYM(D3D_g_pDevice);
constexpr uint32_t kDevice = SYM(D3D_Device);
constexpr uint32_t kDeviceFlags = kDevice + 0x08;
constexpr uint32_t kDirtyFlags = SYM(D3D_DirtyFlags);
constexpr uint32_t kDeviceDwords = 0x928;

// recompiled D3D code the HLE device setup runs
constexpr uint32_t kInitHardwareState = SYM(D3D_InitHardwareState);  // object, DMA and semaphore pushes
constexpr uint32_t kInitFrameBuffers = SYM(CDevice_InitializeFrameBuffers);
constexpr uint32_t kInitDefaultState = SYM(D3D_InitDefaultState);
constexpr uint32_t kSetVertexShader = SYM(D3DDevice_SetVertexShader);
constexpr uint32_t kSetRenderTarget = SYM(D3DDevice_SetRenderTarget);
constexpr uint32_t kClear = SYM(D3DDevice_Clear);
constexpr uint32_t kSetStateVB = SYM(CDevice_SetStateVB);

// CDevice fields (offsets as the recompiled code uses them)
constexpr uint32_t kDevPush = 0x00, kDevThreshold = 0x04, kDevRingStart = 0x24, kDevRingEnd = 0x28,
                   kDevTime = 0x2C, kDevDoneTime = 0x30, kDevRingSize = 0x44, kDevVertexShader = 0x794,
                   kDevRegisters = 0x934, kDevUserRegs = 0x1C20, kDevMiniport = 0x1C28, kDevNotifiers = 0x2488,
                   kDevNotifiers2 = 0x248C, kDevNotifiers1 = 0x2490, kDevAutoDepth = 0x1A20,
                   kDevFrameBuffers = 0x1A14, kDevVBlankCallback = 0x1DB8, kDevSwapCallback = 0x1DB4;
constexpr uint32_t kFlagRecording = 0x04;  // a push buffer is being recorded: the push pointer is the title's
constexpr uint32_t kGpuRegisters = gpu::kMmioBase;

constexpr uint32_t kScratchSize = 4u << 20;
constexpr uint32_t kScratchMargin = 512u << 10;  // the XDK writes this far past the threshold check at most

std::recursive_mutex g_lock;
uint32_t g_scratch = 0;
uint32_t g_notifiers = 0;  // the 0x60 bytes of notifiers and the semaphore D3D's fences land in
uint32_t g_user_regs = 0;  // a stand-in for the channel's USER registers (PUT/GET) in guest memory
bool g_parsing = false;
uint64_t g_methods = 0, g_drains = 0;

uint32_t Dev(uint32_t off) { return R32(kDevice + off); }
void SetDev(uint32_t off, uint32_t v) { W32(kDevice + off, v); }

uint32_t PhysicalOf(uint32_t va) { return mem::DeviceAddress(va); }

// the DMA objects D3D's device creation would have made (CDevice::Init): the notifiers, the semaphore that
// receives fence times and everything else at physical 0
uint32_t ResolveDma(uint32_t handle) {
    switch (handle) {
    case 2: return PhysicalOf(Dev(kDevNotifiers1));
    case 7: return PhysicalOf(Dev(kDevNotifiers2));
    case 8: return PhysicalOf(Dev(kDevDoneTime));
    default: return 0;
    }
}

// ---- host vblank: 60 Hz from the first call ----

using Clock = std::chrono::steady_clock;
const Clock::time_point g_epoch = Clock::now();
constexpr double kVblankSeconds = 1.0 / 59.94;

uint64_t VblankCount() { return uint64_t(std::chrono::duration<double>(Clock::now() - g_epoch).count() / kVblankSeconds); }

double Seconds() { return std::chrono::duration<double>(Clock::now() - g_epoch).count(); }

// sleeps until that many seconds after the epoch (the last 1.5 ms spinning, for an exact frame time)
void WaitUntil(double t) {
    for (;;) {
        double at = t - Seconds();
        if (at <= 0) return;
        if (at > 0.002) std::this_thread::sleep_for(std::chrono::microseconds(int64_t((at - 0.0015) * 1e6)));
        else std::this_thread::yield();
    }
}

void WaitForVblank(uint64_t n) { WaitUntil(double(n) * kVblankSeconds); }

// ---- flips: the frame shown once the flip's interval has passed ----

struct PendingFlip {
    bool pending = false;
    uint32_t address = 0;  // physical address of the frame buffer
    uint32_t interval = 1;  // vblanks since the last flip, 0 for immediate
};
PendingFlip g_flip;
uint64_t g_last_flip_vblank = 0;
std::atomic<uint32_t> g_front{0};
std::atomic<uint64_t> g_flip_count{0};

void SoftwareMethod(uint32_t data) {
    switch (data & 0x1F) {
    case 1: {  // flip: (frame buffer | interval flags) << 5 | 1, see D3D's Swap
        uint32_t value = data >> 5;
        uint32_t flags = value & 0xF;
        g_flip.pending = true;
        // the console's interrupt routine calls these at vblank and flip; FCI has not been seen to set them
        if (Dev(kDevVBlankCallback) || Dev(kDevSwapCallback))
            LOG_FIRST(1, "xdk: vblank callback %08X, swap callback %08X are not called with HLE graphics",
                      Dev(kDevVBlankCallback), Dev(kDevSwapCallback));
        g_flip.address = value & ~0xFu;
        // bits 0-1: one, two or three vblanks; bit 3 alone: immediate (with an interval: that, or at once if late)
        g_flip.interval = (flags & 3) == 3 ? 3 : (flags & 2) ? 2 : (flags & 1) ? 1 : (flags & 8) ? 0 : 1;
        break;
    }
    case 5:  // wakes BlockOnTime, which does not wait with HLE
        break;
    default:
        LOG_FIRST(16, "xdk: software method %08X ignored", data);
        break;
    }
}

int64_t Ticks() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

constexpr uint32_t kNoOperation = 0x100;

// decodes NV2A push buffer commands, n words from w; guest_begin is the guest address of w[0] (for jumps)
void ParseWords(const uint32_t* w, uint32_t n, uint32_t guest_begin) {
    uint32_t i = 0, guard = 0;
    while (i < n) {
        uint32_t word = w[i++];
        if ((word & 0xE0030003) == 0 || (word & 0xE0030003) == 0x40000000) {
            uint32_t method = word & 0x1FFC, subchannel = (word >> 13) & 7, count = (word >> 18) & 0x7FF;
            bool non_increasing = (word & 0xE0030003) == 0x40000000;
            // performance: the data words at once
            count = std::min(count, n - i);
            if (count && subchannel == 0) gpu::kelvin::Methods(0, method, w + i, count, non_increasing);
            g_methods += count;
            i += count;
        } else if ((word & 3) == 1 || (word & 0xE0000003) == 0x20000000) {
            // a jump: the XDK only jumps to recorded push buffers, which RunPushBuffer runs itself here
            uint32_t to = (word & 3) == 1 ? word & 0xFFFFFFFC : word & 0x1FFFFFFC;
            LOG_FIRST(8, "xdk: push buffer jump %08X at %08X", word, guest_begin + 4 * (i - 1));
            uint32_t va = to | 0x80000000;
            if (va >= guest_begin && va < guest_begin + 4 * n && guard++ < 16) i = (va - guest_begin) / 4;
            else break;
        } else if (word) {
            LOG_FIRST(16, "xdk: bad push buffer command %08X at %08X", word, guest_begin + 4 * (i - 1));
        }
    }
}

// whether the commands hold a flip (the software method of D3D's Swap); walks the headers only
bool HasFlip(const uint32_t* w, uint32_t n) {
    // performance: a flip needs a NO_OPERATION header on subchannel 0 (increasing or not); without any word that could
    // be one (a branchless scan the compiler vectorizes) the walk over the headers is not needed (it was 2.4 % of the
    // guest thread)
    uint32_t candidates = 0;
    for (uint32_t k = 0; k < n; k++) candidates |= uint32_t((w[k] & 0xA003FFFFu) == kNoOperation);
    if (!candidates) return false;
    uint32_t i = 0;
    while (i < n) {
        uint32_t word = w[i++];
        if ((word & 0xE0030003) == 0 || (word & 0xE0030003) == 0x40000000) {
            uint32_t method = word & 0x1FFC, subchannel = (word >> 13) & 7, count = (word >> 18) & 0x7FF;
            count = std::min(count, n - i);
            if (method == kNoOperation && subchannel == 0)
                for (uint32_t k = 0; k < count; k++)
                    if ((w[i + k] & 0x1F) == 1) return true;
            i += count;
        } else if ((word & 3) == 1 || (word & 0xE0000003) == 0x20000000) {
            break;
        }
    }
    return false;
}

// ---- performance: render thread ----
// The push buffer the XDK writes is copied at each synchronisation point and decoded and drawn on a thread of its
// own (FCI_RENDER_THREAD=0: on the guest thread as before), so the game's code and the renderer overlap. As on the
// console, vertex buffers and textures are read from guest memory when the draw runs; the XDK's own fences say when
// the CPU may touch them again: the device's done time ([dev+0x30]) advances only once the render thread has drawn
// a batch (and with the semaphore releases in it), and the waits (BlockOnTime, BlockOnResource, locks, idle) wait
// for the render thread. The guest runs at most one frame ahead (its flip waits until the frame before is shown).

struct Command {
    bool words = true;          // push buffer words, else a call
    bool flip_ok = true;        // a flip pending afterwards is presented (not inside RunPushBuffer)
    uint32_t guest_begin = 0;   // guest address of the first word (jumps)
    uint32_t first = 0, count = 0;  // in Batch::words
    std::function<void()> call;
};

struct Batch {
    std::vector<uint32_t> words;
    std::vector<Command> commands;
    uint32_t time = 0;  // the device's time at submission: everything before it is done after the batch
    uint64_t sequence = 0;
    void Reset() {
        words.clear(), commands.clear();
        time = 0, sequence = 0;
    }
};

bool g_threaded = false;
// performance: frames the guest may finish before the render thread shows them (frame_queue, 1-3): 2 lets a single
// long guest frame (game logic) pass without a late present, for up to one frame more latency
std::atomic<uint64_t> g_frame_queue{1};
std::mutex g_queue_lock;
std::condition_variable g_queue_cv, g_done_cv;
std::deque<std::unique_ptr<Batch>> g_queue, g_spare;
std::unique_ptr<Batch> g_batch;  // being filled by the guest (under g_lock)
uint64_t g_submitted = 0;        // batches handed over (guest, under g_lock)
std::atomic<uint64_t> g_completed{0};
uint64_t g_frames_submitted = 0;  // flips handed over (guest)
std::atomic<uint64_t> g_frames_shown{0};
// guest side statistics for the frame lines (kelvin::FrameStats): waits for the render thread
int64_t g_sync_ticks = 0;

Batch& CurrentBatch() {
    if (!g_batch) {
        std::lock_guard q(g_queue_lock);
        if (!g_spare.empty()) {
            g_batch = std::move(g_spare.back());
            g_spare.pop_back();
        } else {
            g_batch = std::make_unique<Batch>();
        }
    }
    return *g_batch;
}

// copies [begin, end) of guest memory into the batch; true when it holds a flip
int64_t g_copy_ticks = 0;  // performance: the guest's time copying push buffers (frame statistics)

bool AddWords(uint32_t begin, uint32_t end, bool flip_ok, bool find_flip = true) {
    if (end <= begin) return false;
    struct Timed {
        int64_t t = Ticks();
        ~Timed() { g_copy_ticks += Ticks() - t; }
    } timed;
    Batch& b = CurrentBatch();
    const uint32_t* w = reinterpret_cast<const uint32_t*>(rt::Host(begin));
    const uint32_t n = (end - begin) / 4;
    Command c;
    c.flip_ok = flip_ok, c.guest_begin = begin, c.first = uint32_t(b.words.size()), c.count = n;
    b.words.insert(b.words.end(), w, w + n);
    b.commands.push_back(std::move(c));
    return find_flip && HasFlip(b.words.data() + b.commands.back().first, n);
}

void PresentFlip();

// on the render thread (or the guest's without it); the device context is the renderer's while it runs
void Execute(Batch& b) {
    for (Command& c : b.commands) {
        std::lock_guard ctx(gpu::render::ContextLock());
        if (c.words) {
            int64_t t = Ticks();
            rt::hitch::DecodeBegin();  // performance: FCI_HITCH_STACKS
            ParseWords(b.words.data() + c.first, c.count, c.guest_begin);
            rt::hitch::DecodeEnd();
            gpu::kelvin::AddWork(gpu::kelvin::Work::kDecode, Ticks() - t);
            gpu::render::PollReports();
            if (c.flip_ok && g_flip.pending && g_threaded) PresentFlip();
        } else if (c.call) {
            c.call();
        }
    }
    // what the XDK's null hardware mode does in KickOff: everything sent is done
    if (b.time) W32(Dev(kDevDoneTime), b.time - 2);
}

void Recycle(std::unique_ptr<Batch> b) {
    b->Reset();
    std::lock_guard q(g_queue_lock);
    if (g_spare.size() < 8) g_spare.push_back(std::move(b));
}

// hands the batch to the render thread (without one: runs it now)
void Submit() {
    if (!g_batch || g_batch->commands.empty()) return;
    std::unique_ptr<Batch> b = std::move(g_batch);
    b->time = Dev(kDevTime);
    b->sequence = ++g_submitted;
    if (!g_threaded) {
        Execute(*b);
        g_completed = b->sequence;
        Recycle(std::move(b));
        return;
    }
    {
        std::lock_guard q(g_queue_lock);
        g_queue.push_back(std::move(b));
    }
    g_queue_cv.notify_one();
}

// where the guest waits for the render thread (FCI_TRACE_RENDER_WAITS=1: per site every 5 s)
enum Site { kSiteFrame, kSiteDevice, kSiteIdle, kSiteTime, kSiteResource, kSiteLock, kSiteOverlay, kSites };
int64_t g_site_ticks[kSites] = {};
uint64_t g_site_calls[kSites] = {}, g_site_waits[kSites] = {};
const bool g_trace_waits = GetEnvironmentVariableA("FCI_TRACE_RENDER_WAITS", nullptr, 0) > 0;

void TraceWaits() {
    static int64_t since = Ticks();
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    if (!g_trace_waits || Ticks() - since < 5 * f.QuadPart) return;
    since = Ticks();
    static const char* const kNames[] = {"frame", "device", "idle", "time", "resource", "lock", "overlay"};
    std::string line;
    for (int i = 0; i < kSites; i++) {
        char item[96];
        snprintf(item, sizeof item, " %s %llu/%llu %.1f ms", kNames[i], (unsigned long long)g_site_waits[i],
                 (unsigned long long)g_site_calls[i], g_site_ticks[i] * 1000.0 / f.QuadPart);
        line += item;
        g_site_ticks[i] = 0, g_site_calls[i] = 0, g_site_waits[i] = 0;
    }
    rt::Log("xdk: render waits (waited/calls, time over 5 s):%s", line.c_str());
}

// waits (the other guest threads run meanwhile) until done() or the render thread has done everything submitted
template <typename F>
void WaitRender(Site site, F done) {
    g_site_calls[site]++;
    if (!g_threaded || g_completed >= g_submitted || done()) return;
    int64_t t = Ticks();
    {
        rt::CpuUnlock cpu;
        std::unique_lock q(g_queue_lock);
        g_done_cv.wait(q, [&] { return g_completed >= g_submitted || done(); });
    }
    g_sync_ticks += Ticks() - t;
    g_site_ticks[site] += Ticks() - t, g_site_waits[site]++;
}

// (never with g_lock held: the wait gives the guest CPU up)
void Finish(Site site) {
    {
        std::lock_guard lock(g_lock);
        Submit();
    }
    WaitRender(site, [] { return false; });
}

void RenderThread() {
    SetThreadDescription(GetCurrentThread(), L"FCI render");
    for (;;) {
        std::unique_ptr<Batch> b;
        const int64_t idle = Ticks();
        {
            std::unique_lock q(g_queue_lock);
            // occlusion results come in from the GPU while nothing is queued: looked at every millisecond
            while (g_queue.empty()) {
                if (g_queue_cv.wait_for(q, std::chrono::milliseconds(1)) == std::cv_status::timeout && g_queue.empty()) {
                    q.unlock();
                    {
                        std::lock_guard ctx(gpu::render::ContextLock());
                        gpu::render::PollReports();
                    }
                    q.lock();
                }
            }
            b = std::move(g_queue.front());
            g_queue.pop_front();
        }
        gpu::kelvin::AddWork(gpu::kelvin::Work::kIdle, Ticks() - idle);
        Execute(*b);
        uint64_t sequence = b->sequence;
        Recycle(std::move(b));
        {
            std::lock_guard q(g_queue_lock);
            g_completed = sequence;
        }
        g_done_cv.notify_all();
    }
}

void StartRenderThread() {
    char value[8] = {};
    g_threaded = !(GetEnvironmentVariableA("FCI_RENDER_THREAD", value, sizeof value) && value[0] == '0');
    if (g_threaded) std::thread(RenderThread).detach();
    g_frame_queue = uint64_t(std::clamp(settings::Int("frame_queue", 1), 1, 3));
    rt::Log("xdk: %s, frame queue %llu", g_threaded ? "render thread (FCI_RENDER_THREAD=0: on the guest thread)"
                                                     : "rendering on the guest thread (FCI_RENDER_THREAD=0)",
            (unsigned long long)g_frame_queue.load());
}

// a call on the render thread in push buffer order (gamma, overlay frames, values the renderer reads)
void Post(std::function<void()> fn) {
    std::lock_guard lock(g_lock);
    if (!g_scratch) return fn();
    Command c;
    c.words = false, c.call = std::move(fn);
    CurrentBatch().commands.push_back(std::move(c));
}
// ---- end performance ----

// the push buffer since the last drain goes to the renderer (performance: as a copy, to the render thread)
void Drain(bool present = true, bool submit = false) {
    std::unique_lock lock(g_lock);
    if (!g_scratch || g_parsing) return;
    if (R32(kDeviceFlags) & kFlagRecording) return;
    uint32_t end = Dev(kDevPush);
    if (end < g_scratch || end > g_scratch + kScratchSize) {
        LOG_FIRST(8, "xdk: push pointer %08X outside the scratch buffer", end);
        return;
    }
    bool flip = AddWords(g_scratch, end, present);
    g_drains++;
    SetDev(kDevPush, g_scratch);
    uint32_t put = PhysicalOf(g_scratch);
    W32(g_user_regs + 0x40, put);
    W32(g_user_regs + 0x44, put);
    if (!g_threaded) {
        // on this thread, as before the render thread: decoded now, the flip's wait gives the CPU up
        Submit();
        if (!present || !g_flip.pending) return;
        lock.unlock();
        PresentFlip();
        int64_t ticks;
        uint64_t waits;
        rt::CpuWaitStats(&ticks, &waits);
        gpu::kelvin::GuestWaits(ticks, waits, g_sync_ticks, kernel::KernelTicks(), g_copy_ticks);
        return;
    }
    if (!flip) {
        if (submit || CurrentBatch().words.size() >= (64u << 10)) Submit();
        return;
    }
    // a frame is complete: it goes now; the guest goes on with the next one once the one before it is shown
    g_frames_submitted++;
    Submit();
    const uint64_t before = g_frames_submitted - std::min(g_frames_submitted, g_frame_queue.load());
    lock.unlock();
    WaitRender(kSiteFrame, [before] { return g_frames_shown >= before; });
    TraceWaits();
    rt::hitch::GuestFlip();  // performance: FCI_HITCH_STACKS
    if (rt::hitch::Enabled()) {  // performance: FCI_TRACE_HITCHES, the guest's frame (flip to flip) and its waits
        static int64_t last = 0, last_cpu = 0, last_render = 0, last_hold[rt::kHoldSlots] = {};
        int64_t now = rt::hitch::Now(), cpu, hold[rt::kHoldSlots];
        uint64_t n;
        rt::CpuWaitStats(&cpu, &n);
        rt::CpuHoldTicks(hold);
        if (last) {
            // the CPU time of this thread, of the host contexts and of the two busiest other guest threads; the rest
            // of the frame this thread was blocked in kernel waits
            const int self = rt::CpuHoldSlot();
            int64_t d[rt::kHoldSlots];
            for (int i = 0; i < rt::kHoldSlots; i++) d[i] = hold[i] - last_hold[i];
            int a = -1, b = -1;
            for (int i = 1; i < rt::kHoldSlots; i++) {
                if (i == self) continue;
                if (a < 0 || d[i] > d[a]) b = a, a = i;
                else if (b < 0 || d[i] > d[b]) b = i;
            }
            const int64_t blocked = (now - last) - d[self] - (cpu - last_cpu) - (g_sync_ticks - last_render);
            char detail[256];
            snprintf(detail, sizeof detail,
                     ", running %.2f, blocked %.2f; host contexts %.2f, thread %d %.2f, thread %d %.2f",
                     rt::hitch::Ms(d[self]), rt::hitch::Ms(blocked), rt::hitch::Ms(d[0]), a - 1,
                     rt::hitch::Ms(a >= 0 ? d[a] : 0), b - 1, rt::hitch::Ms(b >= 0 ? d[b] : 0));
            gpu::kelvin::GuestFrame(now - last, cpu - last_cpu, g_sync_ticks - last_render, detail);
            // every 5 s: the presenting thread's frames on average (what headroom the guest has)
            static int64_t sum_run = 0, sum_blocked = 0, sum_cpu = 0, sum_render = 0, max_run = 0, since = now;
            static uint64_t frames = 0;
            sum_run += d[self], sum_blocked += blocked, sum_cpu += cpu - last_cpu, sum_render += g_sync_ticks - last_render;
            max_run = std::max(max_run, d[self]), frames++;
            if (rt::hitch::Ms(now - since) >= 5000) {
                rt::Log("hitch: guest frames over 5 s: running %.2f ms (max %.2f), blocked %.2f, cpu wait %.2f, render "
                        "wait %.2f",
                        rt::hitch::Ms(sum_run) / frames, rt::hitch::Ms(max_run), rt::hitch::Ms(sum_blocked) / frames,
                        rt::hitch::Ms(sum_cpu) / frames, rt::hitch::Ms(sum_render) / frames);
                sum_run = sum_blocked = sum_cpu = sum_render = max_run = 0, frames = 0, since = now;
            }
        }
        last = now, last_cpu = cpu, last_render = g_sync_ticks;
        std::memcpy(last_hold, hold, sizeof hold);
    }
    int64_t ticks;
    uint64_t waits;
    rt::CpuWaitStats(&ticks, &waits);
    gpu::kelvin::GuestWaits(ticks, waits, g_sync_ticks, kernel::KernelTicks(), g_copy_ticks);
}

// on the render thread: the frame waits for its vblank like the hardware's flip
void PresentFlip() {
    PendingFlip flip = g_flip;
    g_flip.pending = false;
    // ---- PC features: frame rate (app/pcmode.h) ----
    // FCI's game logic runs on elapsed time, so the flip interval (two vblanks: 30 fps) can give way to a frame
    // time limit: fps_limit = 30 keeps the console's vblank pacing, others wait 1 / limit after the last flip
    // (0 = not at all). Late frames are not caught up on, like a late flip on the console.
    // vsync = 1: the display paces instead (present.cpp: every n-th refresh for fps_limit)
    // (performance: the wait is on the render thread; the guest, one frame ahead at most, waits with it)
    // A display faster than the cap (vsync on a 144 Hz monitor: every refresh) is held to the cap by the time limit
    // as well; with variable refresh (G-Sync, FreeSync) that is the usual setup.
    const int limit = pcmode::FpsLimit();
    auto time_limit = [limit] {
        static double next = 0;
        double now = Seconds();
        if (next < now - 1.0 / limit) next = now;  // late: from now
        if (next > now) {
            rt::CpuUnlock cpu;
            int64_t t = Ticks();
            WaitUntil(next);
            gpu::kelvin::AddWork(gpu::kelvin::Work::kLimit, Ticks() - t);  // performance: FCI_TRACE_HITCHES
        }
        next += 1.0 / limit;
    };
    if (flip.interval && pcmode::Vsync()) {
        if (limit > 0 && gpu::present::VsyncFps() > limit * 1.02) time_limit();
        rt::CpuUnlock cpu;  // (without the render thread: the other guest threads run meanwhile)
        gpu::present::WaitForDisplay();
    } else if (flip.interval && limit != 30) {
        if (limit > 0) time_limit();
    } else  // ---- end PC features ----
    if (flip.interval) {
        rt::CpuUnlock cpu;
        WaitForVblank(std::max(g_last_flip_vblank + flip.interval, VblankCount()));
    }
    g_last_flip_vblank = VblankCount();
    g_front = flip.address;
    g_flip_count++;
    gpu::kelvin::FramePresented();  // performance: frame time statistics
    int64_t t = Ticks();
    {
        std::lock_guard ctx(gpu::render::ContextLock());
        gpu::render::Present(flip.address);
    }
    gpu::kelvin::AddWork(gpu::kelvin::Work::kPresent, Ticks() - t);
    {
        std::lock_guard q(g_queue_lock);
        g_frames_shown++;
    }
    g_done_cv.notify_all();
}

uint32_t AllocContiguous(uint32_t size, uint32_t align = 0x1000) {
    uint32_t va = mem::Contiguous().Alloc(size, align);
    if (!va) rt::Log("xdk: out of contiguous memory for %u bytes", size);
    else std::memset(rt::Host(va), 0, size);
    return va;
}

void ThisCall(rt::Cpu& c, uint32_t fn, uint32_t self, std::initializer_list<uint32_t> args) {
    c.ecx = self;
    rt::GuestCall(c, fn, args);
}

}  // namespace

// ---- device ----

// CreateDevice(adapter, type, window, behavior, presentation parameters, out device): the XDK's CDevice::Init
// without the hardware: the frame buffers, the default state and the initial state pushes as the XDK makes
// them, into the scratch push buffer
HLE_STDCALL(Direct3D_CreateDevice, 6) {
    std::lock_guard lock(g_lock);
    uint32_t pp = a[4];
    rt::Log("xdk: CreateDevice %ux%u format %X buffers %u multisample %X swap %X depth %u format %X flags %X "
            "interval %X (HLE graphics)",
            R32(pp), R32(pp + 4), R32(pp + 8), R32(pp + 12), R32(pp + 0x10), R32(pp + 0x14), R32(pp + 0x20),
            R32(pp + 0x24), R32(pp + 0x28), R32(pp + 0x30));
    if (!R32(SYM(D3D_g_PushBufferSize))) W32(SYM(D3D_g_PushBufferSize), 0x80000);
    if (!R32(SYM(D3D_g_PushBufferSegment))) W32(SYM(D3D_g_PushBufferSegment), 0x8000);
    std::memset(rt::Host(kDevice), 0, kDeviceDwords * 4);
    W32(kDevicePtr, kDevice);
    W32(kDeviceFlags, R32(kDeviceFlags) | (a[3] & 0x10));
    W32(SYM(D3D_g_DeviceCreated), 1);

    g_notifiers = AllocContiguous(0x60, 0x20);
    SetDev(kDevNotifiers, g_notifiers);
    SetDev(kDevDoneTime, g_notifiers);
    SetDev(kDevNotifiers1, g_notifiers + 0x20);
    SetDev(kDevNotifiers2, g_notifiers + 0x40);

    g_scratch = AllocContiguous(kScratchSize);
    SetDev(kDevPush, g_scratch);
    SetDev(kDevThreshold, g_scratch + kScratchSize - kScratchMargin);
    SetDev(kDevRingStart, g_scratch);
    SetDev(kDevRingEnd, g_scratch + kScratchSize);
    SetDev(kDevRingSize, kScratchSize);
    // the channel's PUT/GET as the XDK reads them: in guest memory, always equal
    g_user_regs = AllocContiguous(0x100, 0x100);
    SetDev(kDevUserRegs, g_user_regs);
    W32(g_user_regs + 0x40, PhysicalOf(g_scratch));
    W32(g_user_regs + 0x44, PhysicalOf(g_scratch));
    // register reads that remain (overlay, the semaphore register) go to the idle NV2A register file
    SetDev(kDevMiniport, kGpuRegisters);
    SetDev(kDevRegisters, kGpuRegisters);
    W32(SYM(D3D_g_GpuRegisters), kGpuRegisters);
    W32(kDirtyFlags, R32(kDirtyFlags) | 0xFF7F7F);
    SetDev(0x94C, 0x40);
    SetDev(kDevTime, 2);

    gpu::kelvin::HleHooks hooks;
    hooks.resolve_dma = ResolveDma;
    hooks.software_method = SoftwareMethod;
    gpu::kelvin::SetHle(hooks);
    gpu::render::SetHle(true);

    CallGuest(c, kInitHardwareState, {});
    ThisCall(c, kInitFrameBuffers, kDevice, {pp});
    SetDev(kDevVertexShader, SYM(D3D_g_FixedFunctionShader));
    CallGuest(c, kSetVertexShader, {4});
    CallGuest(c, kSetVertexShader, {2});
    std::memset(rt::Host(SYM(D3D_g_Stream) + 0xC0), 0, 0x46 * 4);
    W32(SYM(D3D_g_Stream) + 0xC4, 0x10);
    CallGuest(c, kSetRenderTarget, {Dev(kDevFrameBuffers), Dev(kDevAutoDepth)});
    CallGuest(c, kInitDefaultState, {});
    const float one = 1.0f;
    uint32_t one_bits;
    std::memcpy(&one_bits, &one, 4);
    CallGuest(c, kClear, {0, 0, 3, 0, one_bits, 0});
    StartRenderThread();  // performance
    Drain();
    Finish(kSiteDevice);
    rt::Log("xdk: device %08X, scratch push buffer %08X, frame buffers %08X %08X %08X, depth %08X", kDevice, g_scratch,
            Dev(kDevFrameBuffers), Dev(kDevFrameBuffers + 4), Dev(kDevFrameBuffers + 8), Dev(kDevAutoDepth));
    if (a[5]) W32(a[5], kDevice);
    return 0;
}

// ---- the push buffer and GPU synchronisation (performance: waits for the render thread where the XDK waits for
// the GPU) ----

// MakeRequestedSpace(min, want): with the scratch buffer there is always room
HLE_STDCALL(D3D_MakeRequestedSpace_8, 2) {
    if (R32(kDeviceFlags) & kFlagRecording) {
        return xdk::CallOriginal(c, GUEST_ORIG(D3D_MakeRequestedSpace_8));
    }
    Drain();
    return g_scratch;
}

HLE_FASTCALL(CDevice_KickOff, 0) {
    if (R32(kDeviceFlags) & kFlagRecording) {
        return xdk::CallOriginal(c, GUEST_ORIG(CDevice_KickOff));
    }
    Drain(true, true);
    return 0;
}

HLE_STDCALL(D3D_KickOffAndWaitForIdle, 0) {
    Drain();
    Finish(kSiteIdle);
    return 0;
}

// BlockOnTime(time, make space): until the GPU's done time has reached time
HLE_STDCALL(D3D_BlockOnTime, 2) {
    Drain(true, true);
    const uint32_t time = a[0];
    WaitRender(kSiteTime, [time] { return int32_t(R32(Dev(kDevDoneTime)) - time) >= 0; });
    return 0;
}

HLE_STDCALL(D3DDevice_IsBusy, 0) {
    Drain(true, true);
    return g_threaded && g_completed < g_submitted ? 1 : 0;
}

// the GPU is to be done with a resource (BlockUntilNotBusy, locks): the XDK's own test (performance: the
// resource's last use time, through BlockOnTime below; render targets in use wait for everything)
HLE_STDCALL(D3D_BlockOnResource, 1) {
    Drain();
    g_site_calls[kSiteResource]++;
    return xdk::CallOriginal(c, GUEST_ORIG(D3D_BlockOnResource));
}

// Lock2DSurface(resource, face, level, locked rect, rect, flags) is how the CPU reads or writes surfaces and
// textures: render targets drawn into that memory are written back first (the only GPU -> guest memory copies)
HLE_STDCALL(Lock2DSurface, 6) {
    Drain();
    uint32_t res = a[0];
    uint32_t data = res ? R32(res + 4) : 0;
    const bool no_overwrite = a[5] & 0x20;  // D3DLOCK_NOOVERWRITE: what the GPU had finished is enough
    bool read_back = false;
    if (no_overwrite && g_threaded) {
        // performance: what the GPU had finished, written by the render thread in push buffer order without a wait
        // for it (the CPU reads the copy of a frame later: the 4x2 luminance of the eye adaptation)
        if (data) Post([data] { gpu::render::SyncToGuest(data & 0x0FFFFFFF, true); });
        Drain(true, true);
    } else {
        Finish(kSiteLock);
        std::lock_guard ctx(gpu::render::ContextLock());
        read_back = data && gpu::render::SyncToGuest(data & 0x0FFFFFFF, no_overwrite);
    }
    if (read_back) {
        static std::vector<uint32_t> seen;  // which code reads render targets with the CPU, each caller once
        uint32_t from = R32(c.esp);
        if (seen.size() < 32 && std::find(seen.begin(), seen.end(), from) == seen.end()) {
            seen.push_back(from);
            rt::Log("xdk: Lock2DSurface(%08X, data %08X, flags %X) from %08X reads a render target back", res, data,
                    a[5], from);
        }
    }
    return xdk::CallOriginal(c, GUEST_ORIG(Lock2DSurface));
}

// recorded push buffers (precompiled index lists): run here with the current state instead of jumping into them
HLE_STDCALL(D3DDevice_RunPushBuffer, 2) {
    if (R32(kDeviceFlags) & kFlagRecording) {
        return xdk::CallOriginal(c, GUEST_ORIG(D3DDevice_RunPushBuffer));
    }
    uint32_t pb = a[0], fixup = a[1];
    ThisCall(c, kSetStateVB, kDevice, {0});  // the lazy state the XDK sends first
    Drain(false);
    uint32_t data = R32(pb + 4), size = R32(pb + 0xC);
    if (fixup) {
        // D3DFIXUP: (size, offset, bytes...) from its data + run offset until a size of ~0, into the buffer
        uint32_t f = R32(fixup + 4) + R32(fixup + 0xC);
        for (int n = 0; f && n < 100000; n++) {
            uint32_t bytes = R32(f);
            if (bytes == 0xFFFFFFFF) break;
            std::memmove(rt::Host(data + R32(f + 4)), rt::Host(f + 8), bytes);
            f += 8 + bytes;
        }
    }
    {
        // performance: a copy, as the next fixups of the same buffer change it (the last word is where the XDK
        // patches its jump back)
        std::lock_guard lock(g_lock);
        if (size >= 4) AddWords(data, data + size - 4, false, false);  // recorded: no flip in it
        if (!g_threaded) Submit();
    }
    W32(pb + 8, Dev(kDevTime));
    LOG_FIRST(4, "xdk: RunPushBuffer %08X: %u bytes at %08X, fixup %08X", pb, size, data, fixup);
    return 0;
}

// ---- display ----

HLE_STDCALL(D3DDevice_BlockUntilVerticalBlank, 0) {
    Drain(true, true);
    rt::CpuUnlock cpu;
    WaitForVblank(VblankCount() + 1);
    return 0;
}

HLE_STDCALL(D3DDevice_SetGammaRamp, 2) {
    std::array<uint8_t, 768> ramp;
    std::memcpy(ramp.data(), rt::Host(a[1]), ramp.size());
    Post([ramp] { gpu::render::SetGammaRamp(ramp.data()); });
    return 0;
}

HLE_STDCALL(D3DDevice_PersistDisplay, 0) { return 0; }

// videos show their frames through the overlay; without flips meanwhile, they are presented from here
HLE_STDCALL(D3DDevice_UpdateOverlay, 5) {
    uint32_t r = xdk::CallOriginal(c, GUEST_ORIG(D3DDevice_UpdateOverlay));
    Post([] {
        static uint64_t last_flips = 0;
        uint64_t flips = g_flip_count;
        if (flips == last_flips) gpu::render::Present(g_front);
        last_flips = flips;
        // performance: counts as a frame, the guest stays at most one ahead as with flips
        {
            std::lock_guard q(g_queue_lock);
            g_frames_shown++;
        }
        g_done_cv.notify_all();
    });
    uint64_t before;
    {
        std::lock_guard lock(g_lock);
        before = g_frames_submitted++;
    }
    Drain(true, true);
    WaitRender(kSiteOverlay, [before] { return g_frames_shown >= before; });
    return r;
}

// ---- performance: render thread ----
namespace xdk {
void SetFrameQueue(int frames) {  // performance
    g_frame_queue = uint64_t(std::clamp(frames, 1, 3));
    g_done_cv.notify_all();  // a guest waiting for the longer queue looks again
}

void RenderPost(std::function<void()> fn) {
    if (HleOff()) return fn();
    Post(std::move(fn));
}
// PC shadows (xdk/shadows.cpp): as RenderPost, but after all the guest has pushed so far (Post goes before what is
// not drained yet)
void RenderMark(std::function<void()> fn) {
    if (HleOff()) return fn();
    Drain(false);
    Post(std::move(fn));
}
}  // namespace xdk
// ---- end performance ----
