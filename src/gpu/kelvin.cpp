#include "gpu/kelvin.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include <unordered_map>

#include "gpu/nv2a.h"
#include "gpu/pgraph.h"
#include "gpu/renderer.h"
#include "runtime/log.h"
#include "runtime/memory.h"
#include "runtime/hitch.h"  // performance: FCI_TRACE_HITCHES
#include "runtime/profiler.h"
#include "runtime/scheduler.h"

namespace gpu::kelvin {

namespace {

State g_state;
uint32_t g_primitive = 0;
std::vector<uint32_t> g_inline;    // INLINE_ARRAY words
std::vector<uint32_t> g_elements;  // ARRAY_ELEMENT16/32 indices
struct Range {
    uint32_t start, count;
};
std::vector<Range> g_ranges;  // DRAW_ARRAYS
ULONGLONG g_last_flip = 0;    // when the last frame was completed

float AsFloat(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

uint32_t AttributeDwords(uint32_t format) {
    uint32_t type = format & 0xF, size = (format >> 4) & 0xF;
    if (!size) return 0;
    switch (type) {
    case kFloat: return size;
    case kS1:
    case kS32k: return (size + 1) / 2;
    default: return 1;  // ub and cmp pack into one word
    }
}

void Convert(uint32_t format, const uint8_t* p, float* out) {
    uint32_t type = format & 0xF, size = (format >> 4) & 0xF;
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    switch (type) {
    case kFloat:
        std::memcpy(out, p, size * 4);
        break;
    case kUbD3d:  // D3DCOLOR: bytes B G R A
        if (size >= 1) out[0] = p[2] / 255.0f;
        if (size >= 2) out[1] = p[1] / 255.0f;
        if (size >= 3) out[2] = p[0] / 255.0f;
        if (size >= 4) out[3] = p[3] / 255.0f;
        break;
    case kUbOgl:
        for (uint32_t i = 0; i < size && i < 4; i++) out[i] = p[i] / 255.0f;
        break;
    case kS1:
        for (uint32_t i = 0; i < size && i < 4; i++) {
            int16_t s;
            std::memcpy(&s, p + 2 * i, 2);
            out[i] = (2.0f * s + 1.0f) / 65535.0f;
        }
        break;
    case kS32k:
        for (uint32_t i = 0; i < size && i < 4; i++) {
            int16_t s;
            std::memcpy(&s, p + 2 * i, 2);
            out[i] = float(s);
        }
        break;
    case kCmp: {  // 11:11:10 signed normalized
        uint32_t v;
        std::memcpy(&v, p, 4);
        int32_t x = int32_t(v << 21) >> 21, y = int32_t(v << 10) >> 21, z = int32_t(v) >> 22;
        out[0] = x / 1023.0f;
        out[1] = y / 1023.0f;
        out[2] = z / 511.0f;
        break;
    }
    default:
        break;
    }
}

std::unordered_map<uint32_t, uint32_t> g_dma_cache;

// ---- performance: frame times (ms) between presented frames, for the 5 s statistics ----
std::vector<float> g_frame_ms;
int64_t g_work[int(Work::kCount)] = {};
std::atomic<int64_t> g_guest_cpu_ticks{0}, g_guest_render_ticks{0};
std::atomic<uint64_t> g_guest_cpu_waits{0};
std::atomic<int64_t> g_guest_kernel_ticks{0}, g_guest_submit_ticks{0};
LARGE_INTEGER g_last_present{};
bool g_presents_hle = false;  // HLE reports presents itself (FramePresented), LLE counts flips

void RecordFrame() {
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    if (g_last_present.QuadPart)
        g_frame_ms.push_back(float(double(now.QuadPart - g_last_present.QuadPart) * 1000.0 / double(freq.QuadPart)));
    g_last_present = now;
}

// "frame avg A ms, 1% low L fps (p99 P ms), max M ms": 1% low = frames per second over the slowest 1 % of frames
std::string FrameStats() {
    if (g_frame_ms.size() < 2) return "";
    std::vector<float> t = g_frame_ms;
    g_frame_ms.clear();
    std::sort(t.begin(), t.end());
    double sum = 0;
    for (float v : t) sum += v;
    size_t worst = std::max<size_t>(1, t.size() / 100);
    double worst_sum = 0;
    for (size_t i = t.size() - worst; i < t.size(); i++) worst_sum += t[i];
    float p99 = t[std::min(t.size() - 1, size_t(t.size() * 0.99))];
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    const double per_frame = 1000.0 / double(freq.QuadPart) / double(t.size());
    char line[400];
    snprintf(line, sizeof line,
             ", frame avg %.2f ms, 1%% low %.1f fps (p99 %.2f ms), max %.2f ms; decode %.2f ms, present %.2f ms, "
             "render idle %.2f ms",
             sum / t.size(), 1000.0 * worst / worst_sum, p99, t.back(), g_work[0] * per_frame, g_work[1] * per_frame,
             g_work[2] * per_frame);
    for (int64_t& w : g_work) w = 0;
    // the presenting guest thread's waits for the CPU (other guest threads ran meanwhile) and for the render thread
    static int64_t last_ticks = 0, last_render = 0, last_kernel = 0, last_submit = 0;
    static uint64_t last_waits = 0;
    int64_t ticks = g_guest_cpu_ticks, render = g_guest_render_ticks, kernel = g_guest_kernel_ticks,
            submit = g_guest_submit_ticks;
    uint64_t waits = g_guest_cpu_waits;
    size_t at = std::strlen(line);
    snprintf(line + at, sizeof line - at,
             ", cpu wait %.2f ms (%.1f), render wait %.2f ms; guest kernel %.2f ms, push buffer copies %.2f ms",
             (ticks - last_ticks) * per_frame, double(waits - last_waits) / double(t.size()),
             (render - last_render) * per_frame, (kernel - last_kernel) * per_frame, (submit - last_submit) * per_frame);
    last_ticks = ticks, last_waits = waits, last_render = render, last_kernel = kernel, last_submit = submit;
    return line;
}
// ---- end performance ----
HleHooks g_hle;

void FlushDraw() {
    // performance: one Draw reused, its vectors keep their memory
    static Draw reused;
    Draw& d = reused;
    d.Reset();
    d.primitive = g_primitive;
    int order[kAttributes];
    int n = 0;
    for (int i = 0; i < kAttributes; i++) {
        if ((g_state.reg[SET_VERTEX_DATA_ARRAY_FORMAT / 4 + i] >> 4) & 0xF) {
            d.enabled |= uint16_t(1u << i);
            order[n++] = i;
        }
    }
    d.stride = n * 4;

    if (!g_inline.empty()) {
        uint32_t dwords = 0;
        for (int k = 0; k < n; k++) dwords += AttributeDwords(g_state.reg[SET_VERTEX_DATA_ARRAY_FORMAT / 4 + order[k]]);
        if (!dwords) return;
        d.count = uint32_t(g_inline.size() / dwords);
        d.vertices.resize(size_t(d.count) * d.stride);
        const uint32_t* w = g_inline.data();
        float* out = d.vertices.data();
        for (uint32_t v = 0; v < d.count; v++) {
            for (int k = 0; k < n; k++) {
                uint32_t fmt = g_state.reg[SET_VERTEX_DATA_ARRAY_FORMAT / 4 + order[k]];
                Convert(fmt, reinterpret_cast<const uint8_t*>(w), out);
                w += AttributeDwords(fmt);
                out += 4;
            }
        }
    } else {
        // vertex arrays in memory, through the indices or the ranges
        const uint8_t* base[kAttributes] = {};
        uint32_t stride[kAttributes] = {}, address[kAttributes] = {};
        bool raw = true;
        for (int k = 0; k < n; k++) {
            int i = order[k];
            uint32_t offset = g_state.reg[SET_VERTEX_DATA_ARRAY_OFFSET / 4 + i];
            uint32_t dma = g_state.Reg(offset & 0x80000000 ? SET_CONTEXT_DMA_VERTEX_B : SET_CONTEXT_DMA_VERTEX_A);
            address[i] = ResolveDma(dma) + (offset & 0x7FFFFFFF);
            base[i] = Physical(address[i]);
            uint32_t format = g_state.reg[SET_VERTEX_DATA_ARRAY_FORMAT / 4 + i];
            stride[i] = format >> 8;
            // ---- performance: raw when the shader can fetch it as stored (4 byte aligned for D3D 11's input
            // assembler) ----
            d.format[i] = uint8_t(format);
            if (!RawBytes(d.format[i]) || (address[i] | stride[i]) & 3) raw = false;
        }
        // ---- performance: raw draws. The vertices the draw uses, once each, decoded by the vertex shader; the
        // indices (rebased to the first vertex used) stay. Before, every index was expanded to float4s on the CPU
        // (240000 vertices, 14 MB a frame in the first levels: a third of the guest thread) ----
        if (raw) {
            uint32_t lo = ~0u, hi = 0;
            if (!g_elements.empty()) {
                for (uint32_t e : g_elements) lo = std::min(lo, e), hi = std::max(hi, e);
            } else {
                for (const auto& r : g_ranges) lo = std::min(lo, r.start), hi = std::max(hi, r.start + r.count - 1);
            }
            if (lo > hi) return;
            for (int k = 0; k < n && raw; k++) {
                int i = order[k];
                uint64_t end = uint64_t(address[i]) + uint64_t(hi) * stride[i] + RawBytes(d.format[i]);
                if (end > mem::kPhysicalSize) raw = false;
            }
            if (raw) {
                d.raw = true;
                d.first = lo, d.span = hi - lo + 1;
                for (int k = 0; k < n; k++) {
                    int i = order[k];
                    d.data[i] = base[i] + size_t(lo) * stride[i];
                    d.data_stride[i] = stride[i];
                }
                if (!g_elements.empty()) {
                    d.ids.resize(g_elements.size());
                    for (size_t v = 0; v < g_elements.size(); v++) d.ids[v] = g_elements[v] - lo;
                } else {
                    for (const auto& r : g_ranges)
                        for (uint32_t j = 0; j < r.count; j++) d.ids.push_back(r.start + j - lo);
                }
                d.count = uint32_t(d.ids.size());
            }
        }
        // ---- end performance ----
        if (!d.raw) {
            auto emit = [&](uint32_t index, float* out) {
                for (int k = 0; k < n; k++) {
                    int i = order[k];
                    Convert(g_state.reg[SET_VERTEX_DATA_ARRAY_FORMAT / 4 + i], base[i] + size_t(index) * stride[i], out);
                    out += 4;
                }
            };
            if (!g_elements.empty()) {
                d.count = uint32_t(g_elements.size());
                d.vertices.resize(size_t(d.count) * d.stride);
                for (uint32_t v = 0; v < d.count; v++) emit(g_elements[v], d.vertices.data() + size_t(v) * d.stride);
            } else {
                for (const auto& r : g_ranges) d.count += r.count;
                d.vertices.resize(size_t(d.count) * d.stride);
                uint32_t v = 0;
                for (const auto& r : g_ranges)
                    for (uint32_t j = 0; j < r.count; j++, v++) emit(r.start + j, d.vertices.data() + size_t(v) * d.stride);
            }
        }
    }
    if (d.count) render::DrawPrimitives(g_state, d);
}

}  // namespace

const State& Current() { return g_state; }

void AddWork(Work what, int64_t ticks) { g_work[int(what)] += ticks; }

void GuestWaits(int64_t cpu_ticks, uint64_t cpu_waits, int64_t render_ticks, int64_t kernel_ticks,
                int64_t submit_ticks) {
    g_guest_cpu_ticks = cpu_ticks, g_guest_cpu_waits = cpu_waits, g_guest_render_ticks = render_ticks;
    g_guest_kernel_ticks = kernel_ticks, g_guest_submit_ticks = submit_ticks;
}

// ---- performance: FCI_TRACE_HITCHES ----
std::atomic<int64_t> g_guest_frame{0}, g_guest_frame_cpu{0}, g_guest_frame_render{0};
std::mutex g_guest_detail_lock;
std::string g_guest_detail;
void GuestFrame(int64_t frame_ticks, int64_t cpu_wait_ticks, int64_t render_wait_ticks, const char* detail) {
    g_guest_frame = frame_ticks, g_guest_frame_cpu = cpu_wait_ticks, g_guest_frame_render = render_wait_ticks;
    std::lock_guard lock(g_guest_detail_lock);
    g_guest_detail = detail;
}

// every frame over twice the running average (and 2 ms over it), with its parts and what happened in it
void TraceHitch() {
    static int64_t last = 0, work[int(Work::kCount)] = {};
    static double average = 0;
    static uint64_t frame = 0;
    int64_t now = rt::hitch::Now();
    int64_t delta[int(Work::kCount)];
    for (int i = 0; i < int(Work::kCount); i++)  // (FrameStats zeroes g_work every 5 s)
        delta[i] = g_work[i] >= work[i] ? g_work[i] - work[i] : g_work[i], work[i] = g_work[i];
    rt::hitch::Counts events = rt::hitch::Take();
    rt::hitch::CountFrame();
    frame = rt::hitch::FrameNumber();
    if (!last) {
        last = now;
        return;
    }
    const double ms = rt::hitch::Ms(now - last);
    last = now;
    if (average > 0 && ms > rt::hitch::Factor() * average && ms > average + 2) {
        char what[512];
        std::string detail;
        {
            std::lock_guard lock(g_guest_detail_lock);
            detail = g_guest_detail;
        }
        rt::Log("hitch: frame %llu %.2f ms (average %.2f): render thread decode %.2f, present %.2f, idle %.2f, limit "
                "%.2f; guest frame %.2f ms (cpu wait %.2f, render wait %.2f%s); %s",
                (unsigned long long)frame, ms, average, rt::hitch::Ms(delta[0]), rt::hitch::Ms(delta[1]),
                rt::hitch::Ms(delta[2]), rt::hitch::Ms(delta[3]), rt::hitch::Ms(g_guest_frame), rt::hitch::Ms(g_guest_frame_cpu),
                rt::hitch::Ms(g_guest_frame_render), detail.c_str(), rt::hitch::Describe(events, what, sizeof what));
    }
    // the average of the last ~30 frames, long ones counted only a little (a level load does not raise it for long)
    average = average > 0 ? average + (std::min(ms, 3 * average) - average) / 30 : ms;
}
// ---- end performance ----

void FramePresented() {
    g_presents_hle = true;
    RecordFrame();
    if (rt::hitch::Enabled()) TraceHitch();  // performance
}

// ---- performance: raw draws ----
uint32_t RawBytes(uint8_t format) {
    uint32_t type = format & 0xF, size = (format >> 4) & 0xF;
    if (!size || size > 4) return 0;
    switch (type) {
    case kFloat: return size * 4;
    case kUbD3d:
    case kUbOgl:
    case kCmp: return 4;
    case kS1:
    case kS32k: return size == 1 ? 2 : size == 2 ? 4 : 8;
    default: return 0;
    }
}

const std::vector<float>& Vertices(const Draw& d) {
    if (!d.raw || !d.vertices.empty()) return d.vertices;
    d.vertices.resize(size_t(d.count) * d.stride);
    float* out = d.vertices.data();
    for (uint32_t id : d.ids)
        for (int i = 0; i < kAttributes; i++)
            if (d.enabled & (1u << i)) {
                Convert(d.format[i], d.data[i] + size_t(id) * d.data_stride[i], out);
                out += 4;
            }
    return d.vertices;
}
// ---- end performance ----

void SetHle(const HleHooks& hooks) { g_hle = hooks; }
bool Hle() { return g_hle.resolve_dma != nullptr; }

uint32_t ResolveDma(uint32_t handle) {
    if (g_hle.resolve_dma) return g_hle.resolve_dma(handle);
    auto it = g_dma_cache.find(handle);
    if (it != g_dma_cache.end()) return it->second;
    uint32_t instance = RamhtLookup(handle);
    uint32_t base = instance == 0xFFFFFFFF ? 0 : DmaBase(instance);
    g_dma_cache[handle] = base;
    return base;
}

void Methods(uint32_t subchannel, uint32_t method, const uint32_t* words, uint32_t count, bool non_increasing) {
    if (non_increasing && count > 1) {
        switch (method) {
        case ARRAY_ELEMENT16: {
            size_t at = g_elements.size();
            g_elements.resize(at + 2 * size_t(count));
            uint32_t* e = g_elements.data() + at;
            for (uint32_t i = 0; i < count; i++) e[2 * i] = words[i] & 0xFFFF, e[2 * i + 1] = words[i] >> 16;
            g_state.reg[method / 4] = words[count - 1];
            return;
        }
        case ARRAY_ELEMENT32:
        case INLINE_ARRAY: {
            std::vector<uint32_t>& to = method == INLINE_ARRAY ? g_inline : g_elements;
            to.insert(to.end(), words, words + count);
            g_state.reg[method / 4] = words[count - 1];
            return;
        }
        default:
            break;
        }
    }
    for (uint32_t i = 0; i < count; i++) {
        Method(subchannel, method, words[i]);
        if (!non_increasing) method += 4;
    }
}

void Method(uint32_t subchannel, uint32_t method, uint32_t data) {
    if (method < 0x2000) g_state.reg[method / 4] = data;

    if (method == SET_TRANSFORM_PROGRAM_START || method == SET_TRANSFORM_EXECUTION_MODE) g_state.program_generation++;
    if (method >= SET_TRANSFORM_PROGRAM && method < SET_TRANSFORM_PROGRAM + 0x80) {
        g_state.program_generation++;
        if (g_state.program_load < kProgramSlots) g_state.program[g_state.program_load][g_state.program_word] = data;
        if (++g_state.program_word == 4) {
            g_state.program_word = 0;
            g_state.program_load++;
        }
        return;
    }
    if (method >= SET_TRANSFORM_CONSTANT && method < SET_TRANSFORM_CONSTANT + 0x80) {
        if (g_state.constant_load < kConstants) g_state.constants[g_state.constant_load][g_state.constant_word] = AsFloat(data);
        if (++g_state.constant_word == 4) {
            g_state.constant_word = 0;
            g_state.constant_load++;
        }
        return;
    }
    // the viewport transform lives in the transform context, where vertex programs read it as c[-38] (scale)
    // and c[-37] (offset), slots 58 and 59
    if (method >= SET_VIEWPORT_SCALE && method < SET_VIEWPORT_SCALE + 16) {
        g_state.constants[58][(method - SET_VIEWPORT_SCALE) / 4] = AsFloat(data);
        return;
    }
    if (method >= SET_VIEWPORT_OFFSET && method < SET_VIEWPORT_OFFSET + 16) {
        g_state.constants[59][(method - SET_VIEWPORT_OFFSET) / 4] = AsFloat(data);
        return;
    }
    if (method >= SET_VERTEX_DATA4F_M && method < SET_VERTEX_DATA4F_M + 0x100) {
        uint32_t i = (method - SET_VERTEX_DATA4F_M) / 16, c = ((method - SET_VERTEX_DATA4F_M) / 4) & 3;
        g_state.attribute_value[i][c] = AsFloat(data);
        return;
    }
    if (method >= SET_VERTEX_DATA2F_M && method < SET_VERTEX_DATA2F_M + 0x80) {
        uint32_t i = (method - SET_VERTEX_DATA2F_M) / 8, c = ((method - SET_VERTEX_DATA2F_M) / 4) & 1;
        g_state.attribute_value[i][c] = AsFloat(data);
        g_state.attribute_value[i][2] = 0.0f;
        g_state.attribute_value[i][3] = 1.0f;
        return;
    }
    if (method >= SET_VERTEX_DATA4UB && method < SET_VERTEX_DATA4UB + 0x40) {
        uint32_t i = (method - SET_VERTEX_DATA4UB) / 4;
        for (int c = 0; c < 4; c++) g_state.attribute_value[i][c] = ((data >> (8 * c)) & 0xFF) / 255.0f;
        return;
    }
    if (method >= SET_VERTEX_DATA2S && method < SET_VERTEX_DATA2S + 0x40) {
        uint32_t i = (method - SET_VERTEX_DATA2S) / 4;
        g_state.attribute_value[i][0] = float(int16_t(data));
        g_state.attribute_value[i][1] = float(int16_t(data >> 16));
        g_state.attribute_value[i][2] = 0.0f;
        g_state.attribute_value[i][3] = 1.0f;
        return;
    }
    if (method >= SET_VERTEX_DATA4S_M && method < SET_VERTEX_DATA4S_M + 0x80) {
        uint32_t i = (method - SET_VERTEX_DATA4S_M) / 8, c = ((method - SET_VERTEX_DATA4S_M) / 4) & 1;
        g_state.attribute_value[i][c * 2] = float(int16_t(data));
        g_state.attribute_value[i][c * 2 + 1] = float(int16_t(data >> 16));
        return;
    }

    switch (method) {
    case NO_OPERATION:
        // D3D uses a NOP with a parameter as a software method: it expects an interrupt. Method 1 queues a
        // flip to the surface in the upper bits, so the frame must be in memory by then.
        if (g_hle.software_method) {
            if (data) g_hle.software_method(data);
            if ((data & 0x1F) == 1) g_last_flip = GetTickCount64();
            break;
        }
        if ((data & 0x1F) == 1) render::Flush(), g_last_flip = GetTickCount64();
        if (data) RaisePgraphInterrupt(pgraph::kIntrError, method, subchannel, data);
        break;
    case SET_TRANSFORM_PROGRAM_LOAD:
        g_state.program_load = data;
        g_state.program_word = 0;
        break;
    case SET_TRANSFORM_CONSTANT_LOAD:
        g_state.constant_load = data;
        g_state.constant_word = 0;
        break;
    case SET_CONTEXT_DMA_A:
    case SET_CONTEXT_DMA_B:
    case SET_CONTEXT_DMA_COLOR:
    case SET_CONTEXT_DMA_ZETA:
    case SET_CONTEXT_DMA_VERTEX_A:
    case SET_CONTEXT_DMA_VERTEX_B:
    case SET_CONTEXT_DMA_SEMAPHORE:
        g_dma_cache.erase(data);
        break;
    case SET_FLIP_READ:
        if (!Hle()) SetFlip(Flip::kRead, data);
        break;
    case SET_FLIP_WRITE:
        if (!Hle()) SetFlip(Flip::kWrite, data);
        break;
    case SET_FLIP_MODULO:
        if (!Hle()) SetFlip(Flip::kModulo, data);
        break;
    case FLIP_INCREMENT_WRITE: {
        if (!Hle()) {
            render::Flush();  // the frame is complete: the display may show it from now on
            FlipIncrementWrite();
        }
        g_last_flip = GetTickCount64();
        rt::Heartbeat(rt::Beat::kFlip);
        if (!g_presents_hle) RecordFrame();  // performance: frame times
        // frames per second the game completes, every 5 seconds
        static uint32_t frames = 0;
        static ULONGLONG since = GetTickCount64();
        frames++;
        if (ULONGLONG now = GetTickCount64(); now - since >= 5000) {
            rt::Log("render: %.1f fps%s", frames * 1000.0 / double(now - since), FrameStats().c_str());
            LogTimings();
            frames = 0, since = now;
        }
        break;
    }
    case FLIP_STALL:
        if (!Hle()) FlipStall();  // HLE: the flip's software method paces the frames
        break;
    case SET_BEGIN_END:
        if (data == kEnd) {
            FlushDraw();
        } else {
            g_primitive = data;
        }
        g_inline.clear();
        g_elements.clear();
        g_ranges.clear();
        break;
    case INLINE_ARRAY:
        g_inline.push_back(data);
        break;
    case ARRAY_ELEMENT16:
        g_elements.push_back(data & 0xFFFF);
        g_elements.push_back(data >> 16);
        break;
    case ARRAY_ELEMENT32:
        g_elements.push_back(data);
        break;
    case DRAW_ARRAYS:
        g_ranges.push_back({data & 0xFFFFFF, (data >> 24) + 1});
        break;
    case CLEAR_SURFACE:
        render::Clear(g_state, data);
        break;
    case SET_ZPASS_PIXEL_COUNT_ENABLE:
        render::SetPixelCounting(data != 0);
        break;
    case CLEAR_REPORT_VALUE:
        render::ClearPixelCount();
        break;
    case GET_REPORT: {
        // a 16 byte report: timestamp, value, status (0 = done)
        uint32_t address = ResolveDma(g_state.Reg(SET_CONTEXT_DMA_REPORT)) + (data & 0xFFFFFF);
        render::QueueReport(Physical(address));
        break;
    }
    case BACK_END_WRITE_SEMAPHORE_RELEASE: {
        // render targets go back to memory at the end of each frame (FLIP_INCREMENT_WRITE) and when a texture
        // reads them; FCI_FLUSH_SEMAPHORE=1 also writes them back at every fence, for games that read them
        // with the CPU
        // with FCI_FLUSH_SEMAPHORE=1 also at every fence; loading screens draw without flipping, so they are
        // written back at fences once no flip has come for a while
        static const bool flush = GetEnvironmentVariableA("FCI_FLUSH_SEMAPHORE", nullptr, 0) > 0;
        if (!Hle() && (flush || GetTickCount64() - g_last_flip > 50)) render::Flush();
        uint32_t address = ResolveDma(g_state.Reg(SET_CONTEXT_DMA_SEMAPHORE)) + g_state.Reg(SET_SEMAPHORE_OFFSET);
        *reinterpret_cast<volatile uint32_t*>(Physical(address)) = data;
        SemaphoreReleased(data);
        break;
    }
    default:
        break;
    }
}

}  // namespace gpu::kelvin
