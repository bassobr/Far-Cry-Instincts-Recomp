// Game patch for the PC level of detail settings (app/pcmode.h): lod_distance and view_distance.
//
// FCI's 3D engine (CryEngine 1) keeps its console variables in one block of 0x108 bytes, allocated and filled by the
// engine's constructor (0x0045FC46) through 0x00481B30 (Game_EngineCVars, block in eax), which registers each
// variable bound to its field (console 0x006B38D8, RegisterFloat 0x001FDCD0 returns the value). The engine keeps the
// block at [engine+0x2D0] and in [0x006B4AB8]. Three floats of it decide how far objects keep their detail:
//   +0xF8  e_obj_lod_ratio = 8                 mesh LOD of vegetation, brushes and entities:
//                                              lod = distance / (ratio x object's LodRatio x radius)
//                                              (0x0047F6F3 brushes, 0x00488AB4 vegetation, 0x00032771 entities)
//   +0xFC  e_obj_view_dist_ratio = 48          how far they are drawn: ratio x ViewDistanceRatio x scale x radius
//                                              (0x00488FA0 vegetation, 0x004808A0 brushes, 0x00033160 entities,
//                                              0x00480FB1 the vegetation's render test)
//   +0x100 e_obj_sprite_switch_dist_ratio = 7.2  where vegetation turns into a sprite:
//                                              ratio x SpriteSwitchDistanceRatio x scale x radius (0x00489010; the
//                                              vegetation's render 0x004880A0 draws either the mesh or the sprite and
//                                              fades the mesh over the last half, gs_vegetationSpriteEarlySwitch 0.5)
// (+0x104 e_max_view_distance = 0 is "do not change".) The per object factors come from the level (vegetation
// groups: LodRatio, ViewDistanceRatio, SpriteSwitchDistanceRatio, mostly 1-2; entities: ViewDistRatio, Lua
// SetViewDistRatio), the global ratios from the code only: nothing in the game's data or scripts sets them, and every
// use computes from them anew (per object and frame). A palm of radius ~5 m turns into a sprite at ~36 m and drops
// to its lower LOD at ~40 m on the console.
//
// lod_distance scales the LOD and the sprite ratios, view_distance the view distance ratio, once after the
// registration (so from the first frame of every level; the game restarts the program between levels and the hook
// runs again) and again when the settings menu changes them. 1 leaves the console's values untouched. The camera's
// far plane (the level's fog, 425 m in the first level) still ends the view.
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include "app/pcmode.h"
#include "runtime/memory.h"
#include "xdk/hle.h"

namespace {

bool HleOff() { return !xdk::HleGraphics(); }

constexpr uint32_t kLodRatio = 0xF8, kViewDistRatio = 0xFC, kSpriteRatio = 0x100;
struct Field {
    uint32_t offset;
    const char* name;
    bool view;          // view_distance, else lod_distance
    float console = 0;  // the value as registered
    float written = 0;  // what was last written
};
Field g_fields[] = {{kLodRatio, "e_obj_lod_ratio", false},
                    {kViewDistRatio, "e_obj_view_dist_ratio", true},
                    {kSpriteRatio, "e_obj_sprite_switch_dist_ratio", false}};
uint32_t g_cvars = 0;  // the engine's variable block, 0 until registered
std::mutex g_lock;

void SetF(uint32_t va, float v) { std::memcpy(rt::Host(va), &v, 4); }

// the fields as the console's times the settings; a field the game changed since (it does not) counts as its new value
void Apply(float lod, float view, const char* why) {
    if (!g_cvars) return;
    char text[256];
    int n = 0;
    for (Field& f : g_fields) {
        const uint32_t va = g_cvars + f.offset;
        const float now = xdk::GF(va);
        if (now != f.written) f.console = now;
        const float k = f.view ? view : lod;
        f.written = k == 1.0f ? f.console : f.console * k;
        SetF(va, f.written);
        n += std::snprintf(text + n, sizeof text - n, "%s %s %g", n ? "," : "", f.name, f.written);
    }
    rt::Log("lod: lod distance x%.2f, view distance x%.2f (%s, variables at %08X):%s", lod, view, why, g_cvars, text);
}

// FCI_TRACE_MEM=1: the guest's memory in use every 5 s and its peak, against the Xbox's 64 MB (the emulation does not
// hold the sum to that, the console would): committed pages of the virtual range (NtAllocateVirtualMemory, the XBE
// image; the game's heaps reserve more than they commit) plus the contiguous blocks (MmAllocateContiguousMemory:
// textures, vertex and push buffers, committed when allocated). For checking what more detail costs in memory.
uint32_t CommittedVirtual() {
    uint64_t total = 0;
    for (uint64_t a = mem::kVirtualBegin; a < mem::kVirtualEnd;) {
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery(rt::Host(uint32_t(a)), &info, sizeof info)) break;
        const uint64_t end = uint64_t(static_cast<uint8_t*>(info.BaseAddress) - rt::Host(0)) + info.RegionSize;
        if (info.State == MEM_COMMIT) total += std::min<uint64_t>(end, mem::kVirtualEnd) - a;
        a = end;
    }
    return uint32_t(total);
}

void TraceMemory() {
    if (GetEnvironmentVariableA("FCI_TRACE_MEM", nullptr, 0) == 0) return;
    static std::once_flag once;
    std::call_once(once, [] {
        std::thread([] {
            uint32_t peak = 0;
            for (;;) {
                Sleep(5000);
                const uint32_t v = CommittedVirtual(), p = mem::Contiguous().used(), total = v + p;
                peak = std::max(peak, total);
                rt::Log("lod: guest memory %.2f MB (virtual committed %.2f of %.2f reserved, contiguous %.2f), peak "
                        "%.2f MB of 64", total / 1048576.0, v / 1048576.0, mem::Virtual().used() / 1048576.0,
                        p / 1048576.0, peak / 1048576.0);
            }
        }).detach();
    });
}

// FCI_TRACE_ENTITY=<x>,<y>,<z> (diagnostics): finds the entity placed there (CEntity: vtable 0x005F6AA0, position
// at +0x34C) and logs every change of its first 0x440 bytes, polled every 20 ms. For entities that vanish (GitHub #6).
void TraceEntity() {
    char text[128] = {};
    if (!GetEnvironmentVariableA("FCI_TRACE_ENTITY", text, sizeof text)) return;
    float want[3] = {};
    if (std::sscanf(text, "%f,%f,%f", &want[0], &want[1], &want[2]) != 3) return;
    static std::once_flag once;
    std::call_once(once, [want] {
        std::thread([want] {
            constexpr uint32_t kVtable = 0x005F6AA0, kPos = 0x34C, kSize = 0x440;
            uint32_t entity = 0;
            while (!entity) {
                Sleep(1000);
                for (uint64_t a = mem::kVirtualBegin; a < mem::kVirtualEnd && !entity;) {
                    MEMORY_BASIC_INFORMATION info;
                    if (!VirtualQuery(rt::Host(uint32_t(a)), &info, sizeof info)) break;
                    const uint64_t end = std::min<uint64_t>(
                        uint64_t(static_cast<uint8_t*>(info.BaseAddress) - rt::Host(0)) + info.RegionSize, mem::kVirtualEnd);
                    if (info.State == MEM_COMMIT && info.Protect == PAGE_READWRITE)
                        for (uint64_t p = a; p + kSize <= end; p += 4) {
                            uint32_t vtable;
                            std::memcpy(&vtable, rt::Host(uint32_t(p)), 4);
                            if (vtable != kVtable) continue;
                            const uint32_t q = uint32_t(p) + kPos;
                            if (std::fabs(xdk::GF(q) - want[0]) < 0.01f && std::fabs(xdk::GF(q + 4) - want[1]) < 0.01f &&
                                std::fabs(xdk::GF(q + 8) - want[2]) < 0.01f) {
                                entity = uint32_t(p);
                                break;
                            }
                        }
                    a = end;
                }
            }
            rt::Log("lod: FCI_TRACE_ENTITY %g,%g,%g is the entity at %08X", want[0], want[1], want[2], entity);
            uint32_t last[kSize / 4];
            std::memcpy(last, rt::Host(entity), kSize);
            for (;;) {
                Sleep(20);
                uint32_t now[kSize / 4];
                std::memcpy(now, rt::Host(entity), kSize);
                for (uint32_t i = 0; i < kSize / 4; i++) {
                    if (now[i] == last[i] || i == 0x110 / 4) continue;  // (+0x110: the time of its last update)
                    float fo, fn;
                    std::memcpy(&fo, &last[i], 4);
                    std::memcpy(&fn, &now[i], 4);
                    rt::Log("lod: entity +%03X %08X -> %08X (%g -> %g)", i * 4, last[i], now[i], fo, fn);
                }
                std::memcpy(last, now, kSize);
            }
        }).detach();
    });
}

}  // namespace

namespace xdk {
void SetLodScales(float lod_distance, float view_distance) {
    std::lock_guard lock(g_lock);
    Apply(lod_distance, view_distance, "settings menu");
}
}  // namespace xdk

// eax = the engine's console variable block (0x108 bytes); registers them all
HLE_STDCALL(Game_EngineCVars, 0) {
    const uint32_t cvars = c.eax;
    uint32_t r = xdk::CallOriginal(c, GUEST_ORIG(Game_EngineCVars));
    std::lock_guard lock(g_lock);
    g_cvars = cvars;
    for (Field& f : g_fields) f.console = f.written = xdk::GF(cvars + f.offset);
    Apply(pcmode::LodDistance(), pcmode::ViewDistance(), "engine start");
    // FCI_TEST_CVAR=<hex offset>=<float>[,...] (diagnostics): overwrite fields of the block after the registration
    char test[256] = {};
    if (GetEnvironmentVariableA("FCI_TEST_CVAR", test, sizeof test))
        for (char* item = std::strtok(test, ","); item; item = std::strtok(nullptr, ",")) {
            char* end = nullptr;
            const uint32_t off = uint32_t(std::strtoul(item, &end, 16));
            if (*end != '=' || off >= 0x108) continue;
            const float before = xdk::GF(cvars + off), value = float(std::atof(end + 1));
            SetF(cvars + off, value);
            rt::Log("lod: FCI_TEST_CVAR +0x%X %g -> %g", off, before, value);
        }
    TraceMemory();
    TraceEntity();
    return r;
}
