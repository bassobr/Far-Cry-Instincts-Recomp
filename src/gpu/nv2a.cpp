#include "gpu/nv2a.h"

#include <windows.h>
#include <timeapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <map>
#include <mutex>
#include <thread>

#include "gpu/kelvin.h"
#include "gpu/pgraph.h"
#include "gpu/renderer.h"
#include "kernel/kernel.h"
#include "runtime/log.h"
#include "runtime/memory.h"
#include "runtime/profiler.h"
#include "runtime/scheduler.h"
#include "runtime/thread.h"
#include "video/display.h"

namespace gpu {

namespace {

constexpr uint32_t kGpuVector = 3 + 48;  // IRQ 3; the kernel names it by bus interrupt level
constexpr uint32_t kIntrPfifo = 1u << 8, kIntrPgraph = 1u << 12, kIntrPtimer = 1u << 20, kIntrPcrtc = 1u << 24;
constexpr uint64_t kCoreClock = 233333324;  // NVPLL as programmed below

uint32_t* g_regs = nullptr;  // register file, backed by the guest pages at 0xFD000000
std::mutex g_lock;           // register side effects and the pusher's view of them
std::condition_variable g_pusher_wake;
std::condition_variable g_pgraph_released;  // the ISR cleared a PGRAPH interrupt the pusher waits on
std::condition_variable g_irq_wake;
uint32_t g_flip_read = 0, g_flip_write = 0, g_flip_modulo = 1;  // 3D flip counters, under g_lock
std::condition_variable g_flip_changed;
std::chrono::steady_clock::time_point g_start;
std::atomic<uint64_t> g_isr_calls{0};
// timing statistics, logged with the frame rate: vblank -> service routine latency, flip stall waits
std::atomic<int64_t> g_vblank_at{0};
std::atomic<uint64_t> g_isr_latency_us{0}, g_isr_latency_max{0}, g_isr_latency_n{0};
std::atomic<uint64_t> g_stall_us{0}, g_stall_max{0}, g_stall_n{0};
int64_t MicrosNow() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
void Track(std::atomic<uint64_t>& sum, std::atomic<uint64_t>& max, std::atomic<uint64_t>& n, uint64_t v) {
    sum += v, n++;
    for (uint64_t m = max.load(); v > m && !max.compare_exchange_weak(m, v);) {
    }
}
bool g_alarm_armed = false;  // ALARM_0 was written and has not fired yet
std::atomic<bool> g_overlay_active{false};
std::atomic<int> g_overlay_buffer{0};
const bool g_trace_timer = GetEnvironmentVariableA("FCI_TRACE_WAITS", nullptr, 0) > 0;

uint32_t Read32(uint32_t off);

uint32_t& R(uint32_t offset) { return g_regs[offset / 4]; }

uint64_t NowNs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - g_start).count());
}

// ticks = ns * core clock / 1e9 * denominator / numerator; D3D picks the ratio so that TIME_0 (ticks << 5)
// counts nanoseconds
uint64_t TimerTicks() {
    uint64_t num = R(reg::PTIMER_NUMERATOR), den = R(reg::PTIMER_DENOMINATOR);
    if (!num || !den) return NowNs();
    double ticks = double(NowNs()) * double(kCoreClock) / 1e9 * double(den) / double(num);
    return uint64_t(ticks);
}

uint32_t PendingInterrupts() {
    uint32_t r = 0;
    if (R(reg::PFIFO_INTR_0) & R(reg::PFIFO_INTR_EN_0)) r |= kIntrPfifo;
    if (R(reg::PGRAPH_INTR) & R(reg::PGRAPH_INTR_EN)) r |= kIntrPgraph;
    if (R(reg::PTIMER_INTR_0) & R(reg::PTIMER_INTR_EN_0)) r |= kIntrPtimer;
    if (R(reg::PCRTC_INTR_0) & R(reg::PCRTC_INTR_EN_0)) r |= kIntrPcrtc;
    return r;
}

uint32_t Read32(uint32_t off) {
    switch (off) {
    case reg::PMC_BOOT_0: return 0x02A000A1;
    case reg::PMC_INTR_0: return PendingInterrupts();
    case reg::PFIFO_RUNOUT_STATUS: return 0x10;  // empty
    case reg::PFIFO_CACHE1_STATUS: return 0x10;  // empty
    case reg::PFIFO_CACHE1_DMA_PUSH:
        // STATUS (bit 8) reads busy while the pusher has work
        return (R(off) & ~0x100u) | (R(reg::PFIFO_CACHE1_DMA_PUT) != R(reg::PFIFO_CACHE1_DMA_GET) ? 0x100u : 0);
    case reg::PGRAPH_STATUS: return R(reg::PFIFO_CACHE1_DMA_PUT) != R(reg::PFIFO_CACHE1_DMA_GET) ? 1 : 0;
    case reg::PTIMER_TIME_0: return uint32_t((TimerTicks() & 0x7FFFFFF) << 5);
    case reg::PTIMER_TIME_1: return uint32_t((TimerTicks() >> 27) & 0x1FFFFFFF);
    case reg::PCRTC_RASTER: return uint32_t(NowNs() * 60 * 525 / 1000000000ull % 525);
    case reg::PRAMDAC_NVPLL_COEFF: return 0x00011C01;
    case reg::PRAMDAC_MPLL_COEFF: return 0x00007702;
    case reg::PRAMDAC_VPLL_COEFF: return 0x0003C20D;
    case reg::PFB_CFG0: return 3;
    case reg::PFB_CSTATUS: return mem::kPhysicalSize;
    case reg::PFB_WBC: return R(off) & ~0x10000u;  // flushes complete at once
    case reg::USER_DMA_PUT: return R(reg::PFIFO_CACHE1_DMA_PUT);
    case reg::USER_DMA_GET: return R(reg::PFIFO_CACHE1_DMA_GET);
    case reg::USER_REF: return R(reg::PFIFO_CACHE1_REF);
    case reg::PVIDEO_BUFFER: return 0;  // queued overlay buffers are taken at once
    case reg::PGRAPH_INCREMENT: return 0;
    default: return R(off);
    }
}

void Write32(uint32_t off, uint32_t v) {
    switch (off) {
    case reg::PFIFO_INTR_0:
    case reg::PGRAPH_INTR:
    case reg::PCRTC_INTR_0:
    case reg::PTIMER_INTR_0:
        R(off) &= ~v;  // write one to clear
        if (off == reg::PGRAPH_INTR) g_pgraph_released.notify_all();
        break;
    case reg::PFIFO_CACHE1_DMA_PUT:
    case reg::USER_DMA_PUT:
        R(reg::PFIFO_CACHE1_DMA_PUT) = v;
        g_pusher_wake.notify_one();
        break;
    case reg::PFB_WBC:
        R(off) = v & ~0x10000u;
        break;
    case reg::PCRTC_START:
        R(off) = v;
        if (g_trace_timer) {
            static int traced = 0;
            if (traced++ < 50) rt::Log("gpu: PCRTC_START = %08X", v);
        }
        break;
    case reg::PGRAPH_FIFO:
        R(off) = v;
        g_pgraph_released.notify_all();
        break;
    case reg::PGRAPH_INCREMENT:
        if (v & 2) {  // READ_3D: a flip completed
            g_flip_read = (g_flip_read + 1) % g_flip_modulo;
            g_flip_changed.notify_all();
        }
        break;
    case reg::PTIMER_ALARM_0:
        R(off) = v;
        g_alarm_armed = true;
        if (g_trace_timer) rt::Log("gpu: PTIMER alarm %08X (time %08X)", v, Read32(reg::PTIMER_TIME_0));
        break;
    case reg::PTIMER_INTR_EN_0:
        R(off) = v;
        if (g_trace_timer) rt::Log("gpu: PTIMER interrupt enable %X", v);
        break;
    case reg::PVIDEO_BUFFER:
        R(off) = v;
        if (v & 0x01) g_overlay_buffer = 0;
        if (v & 0x10) g_overlay_buffer = 1;
        if (v & 0x11) g_overlay_active = true;
        rt::Heartbeat(rt::Beat::kFlip);  // videos show frames through the overlay, without flips
        break;
    case reg::PVIDEO_STOP:
        R(off) = v;
        if (v & 1) g_overlay_active = false;
        break;
    default:
        R(off) = v;
        break;
    }
    if (((off >= 0x8000 && off < 0x9000) || (off >= 0x600000 && off < 0x602000 && off != reg::PCRTC_INTR_0)) &&
        g_trace_timer) {
        static uint32_t last[0xC00];
        static int traced = 0;
        uint32_t slot = off < 0x9000 ? (off - 0x8000) / 4 : 0x400 + (off - 0x600000) / 4;
        if (last[slot] != v && traced++ < 2000)
            rt::Log("gpu: write %06X = %08X (%.2fs)", off, v,
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count());
        last[slot] = v;
    }
    if (off == reg::PMC_INTR_EN_0 || off == reg::PFIFO_INTR_EN_0 || off == reg::PGRAPH_INTR_EN || off == reg::PCRTC_INTR_EN_0)
        g_irq_wake.notify_one();
}

// ---------------------------------------------------------------------------------------------
// interrupts: the XDK's D3D connected its service routine to the GPU's vector

void IrqThread() {
    rt::GuestThread* ctx = rt::CreateServiceContext("gpu interrupt");
    rt::BindServiceContext(ctx);
    rt::W8(ctx->kpcr + rt::kKpcrIrql, 0x1C);
    for (;;) {
        {
            std::unique_lock lock(g_lock);
            g_irq_wake.wait_for(lock, std::chrono::milliseconds(2), [] {
                return (R(reg::PMC_INTR_EN_0) & 1) && PendingInterrupts();
            });
            if (!(R(reg::PMC_INTR_EN_0) & 1) || !PendingInterrupts()) continue;
        }
        uint32_t object, routine, context;
        if (!kernel::GetInterrupt(kGpuVector, &object, &routine, &context)) continue;
        rt::AcquireCpu(true);
        if (int64_t at = g_vblank_at.exchange(0)) Track(g_isr_latency_us, g_isr_latency_max, g_isr_latency_n, MicrosNow() - at);
        rt::GuestCall(ctx->cpu, routine, {object, context});
        rt::ReleaseCpu();
        g_isr_calls++;
    }
}

// fires the PTIMER alarm once TIME_0 has passed ALARM_0
void CheckAlarm() {
    if (!g_alarm_armed) return;
    uint32_t now = uint32_t((TimerTicks() & 0x7FFFFFF) << 5);
    if (int32_t(now - R(reg::PTIMER_ALARM_0)) < 0) return;
    g_alarm_armed = false;
    R(reg::PTIMER_INTR_0) |= 1;
    g_irq_wake.notify_one();
}

void VblankThread() {
    timeBeginPeriod(1);
    auto next = std::chrono::steady_clock::now();
    for (uint64_t frame = 1;; frame++) {
        // FCI_VBLANK_HZ=<n>: a faster display for frame-rate experiments (the game flips every second vblank)
        static const int64_t period_us = [] {
            char v[16] = {};
            int hz = GetEnvironmentVariableA("FCI_VBLANK_HZ", v, sizeof v) ? std::atoi(v) : 0;
            return hz > 0 ? int64_t(1000000 / hz) : int64_t(16683);
        }();
        next += std::chrono::microseconds(period_us);
        // the alarm is checked every millisecond until the next vblank
        while (std::chrono::steady_clock::now() < next) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            std::lock_guard lock(g_lock);
            CheckAlarm();
        }
        {
            std::lock_guard lock(g_lock);
            R(reg::PCRTC_INTR_0) |= 1;
            g_vblank_at = MicrosNow();
            g_irq_wake.notify_one();
        }
        display::Vblank();
        rt::Heartbeat(rt::Beat::kVblank);
        if (frame % 600 == 0) {
            rt::Log("gpu: vblank %llu, display %08X, put %08X get %08X, isr calls %llu, intr en pmc %X crtc %X pgraph %X, "
                    "pending crtc %X pgraph %X",
                    (unsigned long long)frame, R(reg::PCRTC_START), R(reg::PFIFO_CACHE1_DMA_PUT),
                    R(reg::PFIFO_CACHE1_DMA_GET), (unsigned long long)g_isr_calls.load(), R(reg::PMC_INTR_EN_0),
                    R(reg::PCRTC_INTR_EN_0), R(reg::PGRAPH_INTR_EN), R(reg::PCRTC_INTR_0), R(reg::PGRAPH_INTR));
            rt::Log("gpu: ptimer time %08X alarm %08X armed %d intr %X en %X num %u den %u", Read32(reg::PTIMER_TIME_0),
                    R(reg::PTIMER_ALARM_0), g_alarm_armed, R(reg::PTIMER_INTR_0), R(reg::PTIMER_INTR_EN_0),
                    R(reg::PTIMER_NUMERATOR), R(reg::PTIMER_DENOMINATOR));
            pgraph::DumpStatistics();
        }
    }
}

// ---------------------------------------------------------------------------------------------
// DMA pusher: walks the push buffer between GET and PUT and hands methods to PGRAPH

// D3D runs recorded push buffers by jumping out of its ring. It reads GET to see what the GPU is done with;
// outside the ring it assumes the GPU is through once its fences are, and then re-patches a buffer's jump back
// for the next run. A real GPU is through such a buffer in microseconds; this pusher can be held up in it (host
// GPU waits), so a CPU read of GET waits briefly for it to come back into the ring.
std::condition_variable g_pusher_home;
bool g_pusher_away = false;  // reading outside the ring (g_lock)
bool g_pusher_in_isr = false;  // waiting for the service routine to take a PGRAPH interrupt (g_lock)

// the last words the pusher took, to see how it got somewhere it should not be (also in hang reports)
struct Taken {
    uint32_t at, word, put;
};
Taken g_history[32] = {};
Taken g_nops[8] = {};  // the last NOPs with a parameter (they interrupt): at, parameter
// D3D's BlockOnTime about to wait for the interrupt it patched in at P (kernel: KeWaitForSingleObject)
struct Wait {
    uint32_t time, p, words[4], get, put, write, done, now;
    bool away;
};
Wait g_waits[8] = {};
std::atomic<uint32_t> g_wait_count{0};
std::atomic<uint32_t> g_nop_count{0};
std::atomic<uint32_t> g_taken{0};
uint32_t g_ring_lo = ~0u, g_ring_hi = 0;  // the ring, as far as PUT has been seen in it (0 before D3D set it up)

// for the watchdog: what the pusher and D3D's device see when the game stops presenting frames
void ReportHang() {
    uint32_t taken = g_taken;
    rt::Log("gpu: hang report: put %08X get %08X, ring %08X-%08X, pusher away %d, in isr %d, pgraph intr %X",
            R(reg::PFIFO_CACHE1_DMA_PUT), R(reg::PFIFO_CACHE1_DMA_GET), g_ring_lo, g_ring_hi, g_pusher_away,
            g_pusher_in_isr, R(reg::PGRAPH_INTR));
    for (uint32_t i = 16; i > 0; i--) {
        const Taken& t = g_history[(taken - i) & 31];
        rt::Log("gpu:   took %08X: %08X (put %08X)", t.at, t.word, t.put);
    }
    // XDK D3D's device (0x00517578): ring start [0], current time [0x2C], completed time [[0x30]], ring size [0x44]
    uint32_t dev = rt::R32(0x00517578);
    if (!dev) return;
    std::string words;
    for (uint32_t o = 0; o < 0x50; o += 4) {
        char part[24];
        snprintf(part, sizeof part, " %02X:%08X", o, rt::R32(dev + o));
        words += part;
    }
    rt::Log("gpu: d3d device %08X:%s", dev, words.c_str());
    for (uint32_t i = std::min<uint32_t>(g_nop_count, 8); i > 0; i--) {
        const Taken& t = g_nops[(g_nop_count - i) & 7];
        rt::Log("gpu:   interrupting NOP at %08X, parameter %X (put %08X)", t.at, t.word, t.put);
    }
    uint32_t done = rt::R32(dev + 0x30);
    rt::Log("gpu: d3d completed time %08X, 1DCC %08X %08X %08X", done ? rt::R32(done) : 0, rt::R32(dev + 0x1DCC),
            rt::R32(dev + 0x1DD0), rt::R32(dev + 0x1DD4));
    // the last BlockOnTime waits (see TraceBlockOnTime)
    for (uint32_t i = std::min<uint32_t>(g_wait_count, 8); i > 0; i--) {
        const Wait& w = g_waits[(g_wait_count - i) & 7];
        rt::Log("gpu:   BlockOnTime(%08X) waits: P %08X [%08X %08X %08X %08X], get %08X put %08X%s, cpu writes "
                "at %08X, done %08X now %08X", w.time, w.p, w.words[0], w.words[1], w.words[2], w.words[3], w.get,
                w.put, w.away ? " (away)" : "", w.write, w.done, w.now);
    }
}

// FCI_TRACE_RECORDED=1: which methods the title sends from recorded push buffers (outside the ring) and
// which from the ring, every 10 seconds; tells what an HLE renderer would have to interpret itself
const bool g_trace_recorded = GetEnvironmentVariableA("FCI_TRACE_RECORDED", nullptr, 0) > 0;

void TraceRecorded(uint32_t method, bool recorded) {
    static uint64_t counts[2][0x2000 / 4];
    static ULONGLONG since = GetTickCount64();
    counts[recorded][(method & 0x1FFF) / 4]++;
    if (GetTickCount64() - since < 10000) return;
    since = GetTickCount64();
    for (int r = 0; r < 2; r++) {
        std::vector<std::pair<uint64_t, uint32_t>> top;
        uint64_t total = 0;
        for (uint32_t m = 0; m < 0x2000 / 4; m++)
            if (counts[r][m]) top.push_back({counts[r][m], m * 4}), total += counts[r][m];
        std::sort(top.rbegin(), top.rend());
        std::string line;
        for (size_t i = 0; i < top.size() && i < 24; i++) {
            char part[32];
            snprintf(part, sizeof part, " %04X:%llu", top[i].second, (unsigned long long)top[i].first);
            line += part;
        }
        rt::Log("gpu: %s methods %llu (%zu distinct):%s", r ? "recorded" : "ring", (unsigned long long)total,
                top.size(), line.c_str());
        std::memset(counts[r], 0, sizeof counts[r]);
    }
}

void PusherThread() {
    uint32_t method = 0, subchannel = 0, count = 0;
    bool non_increasing = false;
    uint32_t subroutine_return = 0;
    bool in_subroutine = false;
    Taken* history = g_history;
    uint32_t taken = 0, bad = 0;
    uint32_t& ring_lo = g_ring_lo;
    uint32_t& ring_hi = g_ring_hi;
    auto see_put = [&](uint32_t put) {
        if (put) ring_lo = std::min(ring_lo, put), ring_hi = std::max(ring_hi, put);
    };
    auto in_ring = [&](uint32_t at) {
        constexpr uint32_t kSlack = 0x10000;
        return ring_lo <= ring_hi && at + kSlack >= ring_lo && at <= ring_hi + kSlack;
    };
    for (;;) {
        // HLE graphics: the renderer belongs to the guest thread that draws (xdk/d3d8.cpp), this pusher idles
        if (!kelvin::Hle()) render::PollReports();
        uint32_t get, put, base;
        {
            std::unique_lock lock(g_lock);
            g_pusher_wake.wait_for(lock, std::chrono::milliseconds(1), [] {
                return (R(reg::PFIFO_CACHE1_DMA_PUSH) & 1) && R(reg::PFIFO_CACHE1_DMA_PUT) != R(reg::PFIFO_CACHE1_DMA_GET);
            });
            get = R(reg::PFIFO_CACHE1_DMA_GET);
            put = R(reg::PFIFO_CACHE1_DMA_PUT);
            see_put(put);
            if (!(R(reg::PFIFO_CACHE1_DMA_PUSH) & 1) || get == put) continue;
            base = DmaBase(R(reg::PFIFO_CACHE1_DMA_INSTANCE) << 4);
        }
        while (get != put) {
            uint32_t word = *reinterpret_cast<uint32_t*>(Physical(base + get));
            history[taken++ & 31] = {get, word, put};
            g_taken = taken;
            get += 4;
            if (count) {
                if (method == 0x100 && word) g_nops[g_nop_count++ & 7] = {get - 4, word, put};
                if (g_trace_recorded) TraceRecorded(method, !in_ring(get - 4));
                pgraph::Method(subchannel, method, word);
                if (!non_increasing) method += 4;
                count--;
            } else if ((word & 0xE0000003) == 0x20000000) {  // old jump
                get = word & 0x1FFFFFFC;
            } else if ((word & 3) == 1) {  // jump
                get = word & 0xFFFFFFFC;
            } else if ((word & 3) == 2) {  // call
                subroutine_return = get;
                in_subroutine = true;
                get = word & 0xFFFFFFFC;
            } else if (word == 0x00020000) {  // return
                if (in_subroutine) get = subroutine_return;
                in_subroutine = false;
            } else if ((word & 0xE0030003) == 0 || (word & 0xE0030003) == 0x40000000) {
                method = word & 0x1FFC;
                subchannel = (word >> 13) & 7;
                count = (word >> 18) & 0x7FF;
                non_increasing = (word & 0xE0030003) == 0x40000000;
            } else {
                if (bad < 20) rt::Log("gpu: bad push buffer command %08X at %08X", word, get - 4);
                if (bad++ == 0) {
                    rt::Log("gpu: push buffer base %08X, subroutine %d return %08X; words before:", base, in_subroutine,
                            subroutine_return);
                    for (uint32_t i = 32; i > 0; i--) {
                        const Taken& t = history[(taken - i) & 31];
                        rt::Log("  %08X: %08X (put %08X)", t.at, t.word, t.put);
                    }
                }
            }
            if ((taken & 4095) == 0) render::PollReports();
            {
                std::lock_guard lock(g_lock);
                R(reg::PFIFO_CACHE1_DMA_GET) = get;
                put = R(reg::PFIFO_CACHE1_DMA_PUT);
                see_put(put);
                bool away = !in_ring(get);
                if (g_pusher_away && !away) g_pusher_home.notify_all();
                g_pusher_away = away;
            }
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------------------------------

// GPU (bus master) addresses: physical memory below 64 MB, device addresses of other guest memory above
// (see mem::Device); masking those to 64 MB would land reports and semaphores in unrelated physical pages
uint8_t* Physical(uint32_t address) {
    if (address >= mem::kPhysicalSize) {
        static std::atomic<int> logged{0};
        if (logged++ < 8) rt::Log("gpu: access to device address %08X", address);
    }
    return mem::Device(address);
}

uint32_t Ramin32(uint32_t offset) { return rt::R32(mem::kRaminBase + (offset & 0xFFFFF)); }

// DMA object in instance memory: flags (adjust in bits 20-31), limit, frame address
uint32_t DmaBase(uint32_t instance) {
    uint32_t flags = Ramin32(instance);
    uint32_t frame = Ramin32(instance + 8);
    return (frame & 0xFFFFF000) | (flags >> 20);
}

uint32_t RamhtLookup(uint32_t handle) {
    uint32_t ramht = R(reg::PFIFO_RAMHT);
    uint32_t base = ((ramht >> 4) & 0x1F) << 12;
    uint32_t size = 1u << (((ramht >> 16) & 3) + 12);
    uint32_t bits = 0;
    while (!((size >> bits) & 1)) bits++;
    bits -= 1;
    uint32_t hash = 0;
    for (uint32_t h = handle; h; h >>= bits) hash ^= h & ((1u << bits) - 1);
    uint32_t entry = base + hash * 8;
    if (Ramin32(entry) != handle) {
        for (entry = base; entry < base + size; entry += 8)
            if (Ramin32(entry) == handle) break;
        if (entry >= base + size) return 0xFFFFFFFF;
    }
    return (Ramin32(entry + 4) & 0xFFFF) << 4;
}

void RaisePgraphInterrupt(uint32_t bits, uint32_t method, uint32_t subchannel, uint32_t data) {
    std::unique_lock lock(g_lock);
    R(reg::PGRAPH_TRAPPED_ADDR) = method | (subchannel << 16);
    R(reg::PGRAPH_TRAPPED_DATA_LOW) = data;
    // D3D's handler ignores the trap unless a source is named; software methods report a notification
    R(reg::PGRAPH_NSOURCE) = 1;
    R(reg::PGRAPH_INTR) |= bits;
    R(reg::PGRAPH_FIFO) = 0;
    g_irq_wake.notify_one();
    // The pusher stalls until the service routine has dealt with it: until it gives PGRAPH its FIFO back, not
    // just until it acknowledges the interrupt. D3D's handler (0x0050DFF0) acknowledges first and only then
    // reads TRAPPED_DATA; a pusher running on at the acknowledgement could overwrite it with the next
    // interrupting NOP, and the NOP that BlockOnTime waits for (parameter 5) was lost: a hang.
    g_pusher_in_isr = true;
    g_pusher_home.notify_all();  // a CPU waiting for the pusher to come home must let the service routine run
    g_pgraph_released.wait_for(lock, std::chrono::seconds(2), [bits] {
        return !(R(reg::PGRAPH_INTR) & bits) && (R(reg::PGRAPH_FIFO) & 1);
    });
    g_pusher_in_isr = false;
    if ((R(reg::PGRAPH_INTR) & bits) || !(R(reg::PGRAPH_FIFO) & 1)) {
        rt::Log("gpu: PGRAPH interrupt %X for method %X was not serviced", bits, method);
        R(reg::PGRAPH_INTR) &= ~bits;
        R(reg::PGRAPH_FIFO) = 1;
    }
}

// D3D compares the low bits of this register (value << 2) with the fence it reads back from memory to
// know the pipeline has caught up with the last semaphore release
void SemaphoreReleased(uint32_t value) {
    std::lock_guard lock(g_lock);
    R(reg::PGRAPH_SEMAPHORE) = value << 2;
}

void SetFlip(Flip which, uint32_t value) {
    std::lock_guard lock(g_lock);
    if (which == Flip::kRead) g_flip_read = value;
    if (which == Flip::kWrite) g_flip_write = value;
    if (which == Flip::kModulo) g_flip_modulo = value ? value : 1;
    g_flip_changed.notify_all();
}

void FlipIncrementWrite() {
    std::lock_guard lock(g_lock);
    g_flip_write = (g_flip_write + 1) % g_flip_modulo;
    g_flip_changed.notify_all();
}

void FlipStall() {
    int64_t begin = MicrosNow();
    std::unique_lock lock(g_lock);
    if (!g_flip_changed.wait_for(lock, std::chrono::milliseconds(250), [] { return g_flip_read != g_flip_write; })) {
        static int logged = 0;
        if (logged++ < 5) rt::Log("gpu: flip stall timed out (read %u write %u)", g_flip_read, g_flip_write);
    }
    Track(g_stall_us, g_stall_max, g_stall_n, MicrosNow() - begin);
}

void LogTimings() {
    uint64_t in = g_isr_latency_n.exchange(0), sn = g_stall_n.exchange(0);
    uint64_t is = g_isr_latency_us.exchange(0), ss = g_stall_us.exchange(0);
    rt::Log("gpu: vblank isr latency avg %.2f max %.2f ms (%llu), flip stall avg %.2f max %.2f ms (%llu)",
            in ? is / 1000.0 / in : 0.0, g_isr_latency_max.exchange(0) / 1000.0, (unsigned long long)in,
            sn ? ss / 1000.0 / sn : 0.0, g_stall_max.exchange(0) / 1000.0, (unsigned long long)sn);
}

uint32_t DisplayAddress() { return g_regs ? R(reg::PCRTC_START) : 0; }

namespace {
// the VGA style palette port of PRMDIO: an index, then red, green and blue per entry
constexpr uint32_t kDacWriteIndex = 0x6813C8, kDacData = 0x6813C9;
uint8_t g_gamma[256][3];
uint32_t g_dac_index = 0, g_dac_component = 0;
std::atomic<bool> g_gamma_set{false};
uint64_t g_gamma_writes = 0;

void DacWrite(uint32_t offset, uint8_t value) {
    if (offset == kDacWriteIndex) {
        g_dac_index = value, g_dac_component = 0;
        return;
    }
    g_gamma[g_dac_index & 255][g_dac_component] = value;
    if (++g_dac_component == 3) g_dac_component = 0, g_dac_index++;
    if (++g_gamma_writes == 1) rt::Log("gpu: the game loads a gamma ramp");
    g_gamma_set = true;
}
}  // namespace

bool GammaRamp(uint8_t out[256][3]) {
    if (!g_gamma_set) return false;
    std::lock_guard lock(g_lock);
    bool identity = true;
    for (int i = 0; i < 256; i++)
        for (int c = 0; c < 3; c++) {
            out[i][c] = g_gamma[i][c];
            identity &= g_gamma[i][c] == i;
        }
    static bool logged = false;
    if (!identity && !logged) {
        logged = true;
        rt::Log("gpu: gamma ramp 0->%u 64->%u 128->%u 192->%u 255->%u (green)", out[0][1], out[64][1], out[128][1],
                out[192][1], out[255][1]);
    }
    return !identity;
}
// D3D turns the overlay off by setting an empty source size rather than through PVIDEO_STOP
bool OverlayActive() {
    return g_overlay_active && R(reg::PVIDEO_SIZE_IN_0 + 4 * g_overlay_buffer) != 0xFFFFFFFF;
}
int OverlayBuffer() { return g_overlay_buffer; }

void Init() {
    g_start = std::chrono::steady_clock::now();
    mem::Commit(kMmioBase, kMmioSize);
    mem::Commit(mem::kRaminBase, 0x100000);
    g_regs = reinterpret_cast<uint32_t*>(rt::Host(kMmioBase));
    rt::SetHangReport(ReportHang);
    std::thread(PusherThread).detach();
    std::thread(IrqThread).detach();
    std::thread(VblankThread).detach();
}

namespace {
// The pusher can be held up for long inside a recorded push buffer (shader compiles on the host); a GET from
// outside the ring then lets D3D take a buffer for finished that is not, and BlockOnTime waits forever on an
// interrupt the pusher has already passed. So the read waits for it, letting the service routine run on this
// CPU if the pusher needs it.
void WaitForPusherHome(std::unique_lock<std::mutex>& lock) {
    constexpr auto kLimit = std::chrono::seconds(2);
    auto home = [] { return !g_pusher_away; };
    bool done = g_pusher_home.wait_for(lock, kLimit, [] { return !g_pusher_away || g_pusher_in_isr; });
    if (g_pusher_away && g_pusher_in_isr) {
        lock.unlock();
        {
            rt::CpuUnlock cpu;
            std::unique_lock again(g_lock);
            done = g_pusher_home.wait_for(again, kLimit, home);
        }
        lock.lock();
    }
    if (!done || g_pusher_away) {
        static int logged = 0;
        if (logged++ < 30)
            rt::Log("gpu: GET read while the pusher is away at %08X for 2 s%s", R(reg::PFIFO_CACHE1_DMA_GET),
                    g_pusher_in_isr ? " (it waits for the service routine)" : "");
    }
}
}  // namespace

void TraceBlockOnTime(uint32_t record, uint32_t time) {
    uint32_t dev = rt::R32(0x00517578);
    if (!dev || !record) return;
    Wait w{};
    w.time = time;
    w.p = rt::R32(record + 4);
    for (int i = 0; i < 4; i++) w.words[i] = rt::R32(w.p + 0x10 + 4 * i);
    {
        std::lock_guard lock(g_lock);
        w.get = R(reg::PFIFO_CACHE1_DMA_GET), w.put = R(reg::PFIFO_CACHE1_DMA_PUT), w.away = g_pusher_away;
    }
    w.write = rt::R32(dev), w.now = rt::R32(dev + 0x2C);
    uint32_t done = rt::R32(dev + 0x30);
    w.done = done ? rt::R32(done) : 0;
    g_waits[g_wait_count++ & 7] = w;
    if (g_wait_count <= 3)
        rt::Log("gpu: BlockOnTime(%08X) waits: P %08X [%08X %08X %08X %08X], get %08X put %08X, cpu writes at %08X, "
                "done %08X now %08X", w.time, w.p, w.words[0], w.words[1], w.words[2], w.words[3], w.get, w.put, w.write,
                w.done, w.now);
}

uint32_t Read(uint32_t offset, unsigned size) {
    if (offset >= reg::PRAMIN && offset < reg::PRAMIN + 0x100000) {
        uint32_t a = mem::kRaminBase + (offset - reg::PRAMIN);
        return size == 4 ? rt::R32(a) : size == 2 ? rt::R16(a) : rt::R8(a);
    }
    std::unique_lock lock(g_lock);
    if ((offset & ~3u) == reg::PFIFO_CACHE1_DMA_GET || (offset & ~3u) == reg::USER_DMA_GET)
        WaitForPusherHome(lock);
    uint32_t v = Read32(offset & ~3u);
    return size == 4 ? v : (v >> ((offset & 3) * 8)) & (size == 2 ? 0xFFFF : 0xFF);
}

void Write(uint32_t offset, uint32_t value, unsigned size) {
    if (offset >= reg::PRAMIN && offset < reg::PRAMIN + 0x100000) {
        uint32_t a = mem::kRaminBase + (offset - reg::PRAMIN);
        if (size == 4) rt::W32(a, value);
        else if (size == 2) rt::W16(a, uint16_t(value));
        else rt::W8(a, uint8_t(value));
        return;
    }
    std::lock_guard lock(g_lock);
    if (size == 1 && (offset == kDacWriteIndex || offset == kDacData)) return DacWrite(offset, uint8_t(value));
    if (size != 4) {  // merge partial writes into the register
        uint32_t shift = (offset & 3) * 8;
        uint32_t mask = (size == 2 ? 0xFFFFu : 0xFFu) << shift;
        value = (R(offset & ~3u) & ~mask) | ((value << shift) & mask);
    }
    Write32(offset & ~3u, value);
}

}  // namespace gpu
