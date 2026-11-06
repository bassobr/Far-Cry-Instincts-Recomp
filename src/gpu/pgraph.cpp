#include "gpu/pgraph.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <vector>

#include "gpu/kelvin.h"
#include "gpu/nv2a.h"
#include "runtime/log.h"

namespace gpu::pgraph {

namespace {

struct State {
    uint32_t subchannel_class[8] = {};
};

State g_state;
// methods per subchannel, written by the pusher only; the dump reads them without a lock
uint64_t g_stats[8][0x2000 / 4];
FILE* g_trace = nullptr;
uint64_t g_traced = 0;

}  // namespace

void Method(uint32_t subchannel, uint32_t method, uint32_t data) {
    // FCI_TRACE_GPU=<seconds>: from that many seconds after start, the next two million methods go to gpu.log
    static bool trace_checked = false;
    static ULONGLONG trace_from = 0;
    if (!trace_checked) {
        trace_checked = true;
        char value[16] = {};
        if (GetEnvironmentVariableA("FCI_TRACE_GPU", value, sizeof value)) {
            g_trace = std::fopen("gpu.log", "w");
            trace_from = GetTickCount64() + ULONGLONG(std::atoi(value)) * 1000;
        }
    }
    uint32_t cls = g_state.subchannel_class[subchannel & 7];
    if (g_trace && GetTickCount64() >= trace_from && g_traced++ < 2000000)
        std::fprintf(g_trace, "%u:%02X %04X = %08X\n", subchannel, cls, method, data);
    g_stats[subchannel & 7][(method & 0x1FFC) / 4]++;

    if (method == 0) {  // SET_OBJECT binds an object to the subchannel
        uint32_t instance = RamhtLookup(data);
        uint32_t bound = instance == 0xFFFFFFFF ? 0 : Ramin32(instance) & 0xFF;
        g_state.subchannel_class[subchannel & 7] = bound;
        rt::Log("gpu: subchannel %u = object %08X, class %02X", subchannel, data, bound);
        return;
    }
    if (cls == 0x97) kelvin::Method(subchannel, method, data);
}

void DumpStatistics() {
    std::vector<std::pair<uint32_t, uint64_t>> sorted;
    for (uint32_t sub = 0; sub < 8; sub++)
        for (uint32_t m = 0; m < 0x2000 / 4; m++)
            if (uint64_t n = g_stats[sub][m]) sorted.push_back({(g_state.subchannel_class[sub] << 16) | (m * 4), n});
    std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.second > b.second; });
    rt::Log("gpu: %zu distinct methods", sorted.size());
    for (size_t i = 0; i < std::min<size_t>(sorted.size(), 60); i++)
        rt::Log("  class %02X method %04X: %llu", sorted[i].first >> 16, sorted[i].first & 0xFFFF,
                (unsigned long long)sorted[i].second);
    if (g_trace) std::fflush(g_trace);
}

}  // namespace gpu::pgraph
