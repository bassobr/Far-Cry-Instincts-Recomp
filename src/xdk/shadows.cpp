// Game patch for the PC shadow feature (app/pcmode.h): how far the sun's shadows reach.
//
// FCI has one dynamic shadow map per frame (1024x1024, 16 bit depth; four of 512x512 in another mode), for the
// strongest shadow casting light: the sun, else a spot light. The engine ([0x006B3900], 0x0072E230 in the first
// level) holds the shadow radius R, "DynamicShadowRadius" (+0x10B4: level property or Lua SetDynamicShadowRadius,
// default 10 m, also in the first level). It is used twice:
//  - the view's environment ([engine+0x3348], 0x0072FC88) eases +0x468 towards it (or towards a zone's own value)
//    at ShadowRadiusSpeed (0x00463183); the frame copies that to +0x34 just before the shadow pass (0x00475247,
//    0 with r_DynamicShadow off). The shadow pass 0x00458C80 (Game_ShadowPass) picks the shadow casting lights
//    and renders their maps; for the sun (light flag 2) 0x004585D0 puts an orthographic camera over a square of
//    side S = max(R, 2 R sin(fov / 2)) (fov: the view camera's horizontal one), centred S/2 ahead of the camera,
//    snapped to whole texels. The casters' culling and the receivers' texture matrix come from that camera.
//  - which terrain sectors receive the shadow: 0x0047A2C0 (Game_ShadowReceiveTest, per sector) lets those with
//    (distance to the camera - 46 m) <= R through.
// With R = 10 m the square is 16-20 m wide: shadows end some 15 m ahead. shadow_distance scales R at both places
// (only while they run: the eased value and the level's value stay the game's, so easing and zones work as
// before); the renderer raises the map's resolution by the same factor (gpu/renderer.cpp), so a shadow texel
// keeps the size it has at shadow_scale.
//
// Shadow cascades (GitHub #16): the pass picks its casters and their level of detail by their size in the map, so
// with the square k times larger it leaves out small casters (ferns) and draws palm crowns with their low detail
// meshes: near the player the shadows no longer match the objects. With shadow_cascades (and k >= 2) the pass
// alternates per view: one frame with the console's radius, which the renderer draws into a near map of its own,
// the next with k times it, as before (a second pass in the same frame draws no casters: tried first, the far map
// stayed empty). After each pass the pass's sun camera (a local of the pass, read where its frame was) gives its
// view-projection; the renderer gets the map from this pass's texture coordinates to the other map's, drawn the
// frame before (gpu/renderer.cpp, "shadow cascades"). The radius goes back to k times the game's after every pass:
// the rest of the frame reads it too, and with the console's radius left there after a near pass the receivers a few
// metres away were shaded without their shadow in every other frame (GitHub #16, flicker in 0.1.5).
// The near pass finds no casters in the game's visibility areas (bunkers, buildings with inside areas: its caster query
// enters them through portals in the sun camera's frustum, which the console's square misses); the renderer adds the far
// pass's opaque casters near the player from a map of their own (gpu/renderer.cpp, kSolidsMargin).
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "app/pcmode.h"
#include "gpu/renderer.h"
#include "xdk/hle.h"

namespace {

bool HleOff() { return !xdk::HleGraphics(); }

using rt::R32;

constexpr uint32_t kEngine = 0x006B3900;  // the 3D engine's pointer
constexpr uint32_t kEngineRadius = 0x10B4, kEnvCamera = 0, kEnvRadius = 0x34;
// the shadow pass's light camera: a local at [esp + 0x700] inside the pass (sub esp 0xAC0 and four pushes), so
// 0x3D0 below the stack pointer the pass is called with; CCamera +0x364 orthographic, +0x37C/+0x380 its width and
// height, +0x2B4 view x projection (row vectors)
constexpr uint32_t kPassCamera = 0x3D0, kCamOrtho = 0x364, kCamWidth = 0x37C, kCamHeight = 0x380, kCamViewProj = 0x2B4;

void SetF(uint32_t va, float v) { std::memcpy(rt::Host(va), &v, 4); }

// the side of the sun's square for radius r (0x004585D0)
float SquareSide(uint32_t env, float r) {
    const uint32_t cam = R32(env + kEnvCamera);
    const float fov = cam ? xdk::GF(cam + 0x38) : 0;
    return std::fmax(r, 2 * r * std::sin(fov * 0.5f));
}

// the pass's camera, if the sun's for radius r was set up: its clip from world (column vectors, c = m p)
bool SunCamera(uint32_t cam, float side, double m[4][4]) {
    const float w = xdk::GF(cam + kCamWidth), h = xdk::GF(cam + kCamHeight);
    if (R32(cam + kCamOrtho) != 1 || !(std::fabs(w - side) <= 0.01f * side) || w != h) return false;
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) m[k][r] = xdk::GF(cam + kCamViewProj + 4 * (4 * r + k));  // transposed
    return true;
}

bool Invert(const double a[4][4], double out[4][4]) {
    double m[4][8];
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) m[r][k] = a[r][k], m[r][k + 4] = r == k ? 1 : 0;
    for (int col = 0; col < 4; col++) {
        int pivot = col;
        for (int r = col + 1; r < 4; r++)
            if (std::fabs(m[r][col]) > std::fabs(m[pivot][col])) pivot = r;
        if (std::fabs(m[pivot][col]) < 1e-12) return false;
        for (int k = 0; k < 8; k++) std::swap(m[col][k], m[pivot][k]);
        const double d = m[col][col];
        for (int k = 0; k < 8; k++) m[col][k] /= d;
        for (int r = 0; r < 4; r++)
            if (r != col) {
                const double f = m[r][col];
                for (int k = 0; k < 8; k++) m[r][k] -= f * m[col][k];
            }
    }
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) out[r][k] = m[r][k + 4];
    return true;
}

void Mul(const double a[4][4], const double b[4][4], double out[4][4]) {
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) {
            double s = 0;
            for (int j = 0; j < 4; j++) s += a[r][j] * b[j][k];
            out[r][k] = s;
        }
}

// texture coordinates of a sun camera's map: u = 0.5 + 0.5 x, v = 0.5 - 0.5 y, depth = z
void ToTexture(const double clip[4][4], double out[4][4]) {
    static const double kTex[4][4] = {{0.5, 0, 0, 0.5}, {0, -0.5, 0, 0.5}, {0, 0, 1, 0}, {0, 0, 0, 1}};
    Mul(kTex, clip, out);
}

// shadow cascades: per view (split screen: one pass each, told apart by the view's environment)
struct View {
    uint32_t env = 0;
    uint64_t passes = 0;        // this view's passes so far
    uint64_t done[2] = {};      // the pass that last drew each map (near, far) with the sun, 0 = none
    double clip[2][4][4] = {};  // and its camera
};
View g_views[4];

View& ViewOf(uint32_t env) {
    View* v = nullptr;
    for (View& x : g_views)
        if (x.env == env || (!v && !x.env)) v = &x;
    if (!v) v = &g_views[0], *v = View{};
    v->env = env;
    return *v;
}

// ---- diagnostics (GitHub #16: why a second pass in the same frame draws no casters) ----
// FCI_SHADOW_PASS_DIFF=<n>[,twice]: around the n-th shadow pass with the sun, every dword of writable guest memory the
// pass changed: runs of changed memory and the values written most often (a frame stamp shows up as one value written
// into many objects). With ",twice" the pass then runs a second time at once with the same arguments and is diffed as
// well (what the second pass still changes; its draws show in FCI_TRACE_CASCADE as the guest map's address). Copies
// all committed guest memory: a one-off, the game stalls for a moment. ",twice,reset" clears the pass's "begun" byte
// (its second argument, a local of the caller set to 0 before every pass; the pass calls the device's +0x1B0 once
// while it is 0 and sets it) before the second run, as a new pass would find it; ",twice,unstamp" puts back every dword
// the first pass set to the frame stamp (the value it wrote most often over a slightly smaller one: the objects it drew
// are marked with the frame number) - does the second pass draw its casters then?
struct GuestRegion {
    uint32_t base;
    std::vector<uint32_t> data;
};
std::vector<GuestRegion> SnapGuest() {
    std::vector<GuestRegion> out;
    uint64_t a = 0x10000;
    while (a < 0x100000000ull) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(rt::Host(uint32_t(a)), &mi, sizeof mi)) break;
        const uint64_t base = uint64_t(reinterpret_cast<uintptr_t>(mi.BaseAddress)) - rt::kGuestBase;
        const uint64_t end = base + mi.RegionSize;
        if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
            (mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY))) {
            GuestRegion r{uint32_t(base), std::vector<uint32_t>(size_t(mi.RegionSize / 4))};
            std::memcpy(r.data.data(), mi.BaseAddress, mi.RegionSize);
            out.push_back(std::move(r));
        }
        if (end <= a) break;
        a = end;
    }
    return out;
}
struct GuestChange {
    uint32_t address, old, now;
};
std::vector<GuestChange> DiffGuest(const char* what, const std::vector<GuestRegion>& before) {
    using Change = GuestChange;
    std::vector<Change> changes;
    uint64_t bytes = 0;
    for (const GuestRegion& r : before) {
        bytes += r.data.size() * 4;
        const uint32_t* now = reinterpret_cast<const uint32_t*>(rt::Host(r.base));
        for (size_t i = 0; i < r.data.size(); i++)
            if (now[i] != r.data[i]) changes.push_back({uint32_t(r.base + 4 * i), r.data[i], now[i]});
    }
    rt::Log("shadows: pass diff (%s): %zu dwords changed in %llu MB of guest memory", what, changes.size(),
            (unsigned long long)(bytes >> 20));
    // runs (changes at most 64 bytes apart)
    int runs = 0;
    for (size_t i = 0; i < changes.size();) {
        size_t j = i + 1;
        while (j < changes.size() && changes[j].address - changes[j - 1].address <= 64) j++;
        if (runs++ < 400) {
            char sample[256] = {};
            for (size_t k = i; k < j && k < i + 4; k++) {
                const size_t n = std::strlen(sample);
                std::snprintf(sample + n, sizeof sample - n, " +%X:%08X>%08X", changes[k].address - changes[i].address,
                              changes[k].old, changes[k].now);
            }
            rt::Log("shadows: pass diff run %08X-%08X %zu dwords%s", changes[i].address, changes[j - 1].address + 3,
                    j - i, sample);
        }
        i = j;
    }
    rt::Log("shadows: pass diff: %d runs", runs);
    // the values written most often, and the most frequent differences new - old (a counter stepping by 1)
    std::unordered_map<uint32_t, uint32_t> values, steps;
    for (const Change& c : changes) values[c.now]++, steps[c.now - c.old]++;
    for (auto* table : {&values, &steps}) {
        std::vector<std::pair<uint32_t, uint32_t>> v(table->begin(), table->end());
        std::sort(v.begin(), v.end(), [](auto& x, auto& y) { return x.second > y.second; });
        for (size_t i = 0; i < v.size() && i < 24; i++) {
            uint32_t first = 0, last = 0, n = 0;
            for (const Change& c : changes)
                if ((table == &values ? c.now : c.now - c.old) == v[i].first) {
                    if (!n++) first = c.address;
                    last = c.address;
                }
            rt::Log("shadows: pass diff %s %08X: %u dwords (%08X .. %08X)", table == &values ? "value" : "step",
                    v[i].first, v[i].second, first, last);
        }
    }
    return changes;
}
uint32_t RunShadowPass(rt::Cpu& c, uint32_t begun, uint32_t engine) {
    static const auto arm = [] {
        char v[32] = {};
        long n = 0;
        int twice = 0;
        if (GetEnvironmentVariableA("FCI_SHADOW_PASS_DIFF", v, sizeof v))
            n = std::atol(v), twice = std::strstr(v, "twice") ? 1 : 0;
        if (std::strstr(v, "reset")) twice |= 2;
        if (std::strstr(v, "unstamp")) twice |= 4;
        return std::pair{n, twice};
    }();
    static long passes = 0;
    if (arm.first <= 0 || ++passes != arm.first) return xdk::CallOriginal(c, GUEST_ORIG(Game_ShadowPass));
    const rt::Cpu saved = c;
    std::vector<GuestRegion> before = SnapGuest();
    const uint32_t r = xdk::CallOriginal(c, GUEST_ORIG(Game_ShadowPass));
    const std::vector<GuestChange> first = DiffGuest("first pass", before);
    // the lights the pass picked (engine + 0x18: a vector of 8 byte entries at +4 .. +8) and the caster query's lists
    auto lists = [engine](const char* when) {
        rt::Log("shadows: pass diff: %s: %u lights picked, caster lists %u / %u", when,
                (R32(engine + 0x20) - R32(engine + 0x1C)) / 8, R32(engine + 0x3358), R32(engine + 0x335C));
    };
    lists("after the first pass");
    if (arm.second) {
        if ((arm.second & 2) && begun) *rt::Host(begun) = 0;
        if (arm.second & 4) {
            // ",unstamp": the frame stamp is the value most often written over an older, slightly smaller one (a
            // frame number); put those dwords back as they were before the first pass
            std::unordered_map<uint32_t, uint32_t> count;
            for (const GuestChange& g : first)
                if (g.now > 0x100 && g.now > g.old && g.now - g.old <= 64) count[g.now]++;
            uint32_t stamp = 0, best = 0;
            for (auto& [value, n] : count)
                if (n > best) best = n, stamp = value;
            uint32_t restored = 0;
            for (const GuestChange& g : first)
                if (stamp && g.now == stamp && g.old < stamp && stamp - g.old <= 64)
                    std::memcpy(rt::Host(g.address), &g.old, 4), restored++;
            rt::Log("shadows: pass diff: frame stamp %08X, %u dwords put back before the second pass", stamp, restored);
        }
        before = SnapGuest();
        rt::Cpu again = saved;
        xdk::CallOriginal(again, GUEST_ORIG(Game_ShadowPass));
        lists("after the second pass");
        DiffGuest((arm.second & 4) ? "second pass, same frame, stamps put back" : (arm.second & 2)
                  ? "second pass, same frame, begun byte cleared" : "second pass, same frame", before);
    }
    return r;
}
// FCI_TRACE_CASTER_QUERY=<n>: after each of the first n cascade passes, what the pass asked the engine's visibility
// object ([engine + 0x3354], filled by 0x00483990 for the casters, called from 0x00475BE0) and what it got: its reference
// position (+0x10), +0x1C, flags (+0x20), the factor f of its size test (+0x24: an object is left out when its
// horizontal distance from the reference times f exceeds its radius, 0x00483C52), +0x28, +0x30 (mode); the result list
// at [engine + 0x3358] (+0: count, +4: entries) and the pass's sun camera (eye, width)
void TraceCasterQuery(uint32_t engine, uint32_t cam, const char* which) {
    static const long n = [] {
        char v[16] = {};
        return GetEnvironmentVariableA("FCI_TRACE_CASTER_QUERY", v, sizeof v) ? std::atol(v) : 0L;
    }();
    static long logged = 0;
    if (logged >= n) return;
    logged++;
    const uint32_t vis = R32(engine + 0x3354), list = engine + 0x3358;
    if (!vis) return rt::Log("shadows: caster query (%s): no visibility object", which);
    rt::Log("shadows: caster query (%s): reference %.2f %.2f %.2f, +1C %08X, flags %08X, f %.6g, +28 %08X, +2C %u, "
            "mode %u; list %u / %u (+1208 %08X); sun eye %.2f %.2f %.2f, square %.2f m", which, xdk::GF(vis + 0x10),
            xdk::GF(vis + 0x14), xdk::GF(vis + 0x18), R32(vis + 0x1C), R32(vis + 0x20), xdk::GF(vis + 0x24),
            R32(vis + 0x28), unsigned(*rt::Host(vis + 0x2C)), R32(vis + 0x30), R32(list), R32(list + 4),
            R32(list + 0x1208), xdk::GF(cam), xdk::GF(cam + 4), xdk::GF(cam + 8), xdk::GF(cam + kCamWidth));
}
// ---- end diagnostics ----

}  // namespace

HLE_STDCALL(Game_ShadowPass, 7) {
    const uint32_t engine = a[0], env = a[2];
    const float k = pcmode::ShadowDistance();
    const float radius = xdk::GF(env + kEnvRadius);  // 0: no sun shadow (r_DynamicShadow off)
    {
        static float logged = -1;  // the game's radius, when it changes by a metre or more
        if (std::fabs(radius - logged) >= 1.0f) {
            logged = radius;
            rt::Log("shadows: sun shadow radius %g m (level %g m) x%.2f: square of %.1f m", radius,
                    xdk::GF(engine + kEngineRadius), k, SquareSide(env, radius * k));
        }
    }
    // ---- shadow cascades: the near pass (the console's radius) and the far one on alternate frames ----
    const bool cascades = gpu::render::ShadowCascades();
    static bool cascades_before = false;
    if (cascades_before && !cascades) {  // switched off in the F10 menu: the game's own maps from here on
        for (View& x : g_views) x = View{};
        xdk::RenderMark([] { gpu::render::DropSunShadows(); });
    }
    cascades_before = cascades;
    if (k >= 2.0f && radius > 0 && cascades) {
        View* v = &ViewOf(env);
        const int view = int(v - g_views);
        const uint64_t pass = ++v->passes;
        // 0: near, 1: far. While the view's passes draw more than the sun's map (spot lights besides it, the game's
        // four map mode) or no sun, the renderer cannot pair them and its receivers read each pass's own map: the far
        // square in every frame then, as without cascades (alternating, every other frame had the console's square)
        // (diagnostics: FCI_CASCADE_MULTI=alternate alternates regardless, as 0.1.7 did)
        static const bool always = [] {
            char v[16] = {};
            return GetEnvironmentVariableA("FCI_CASCADE_MULTI", v, sizeof v) && !std::strcmp(v, "alternate");
        }();
        const int which = always || gpu::render::SunCascadesUsable(view) ? int(pass & 1) : 1;
        // diagnostics: FCI_CASCADE_NEAR_SCALE=<f>: the near pass with f times the console's radius (the near map's
        // square f times larger; the renderer's matrices follow the cameras)
        static const float near_scale = [] {
            char v[16] = {};
            const float f = GetEnvironmentVariableA("FCI_CASCADE_NEAR_SCALE", v, sizeof v) ? float(std::atof(v)) : 1.0f;
            return f > 0 ? f : 1.0f;
        }();
        const float r_pass = which ? radius * k : radius * std::fmin(near_scale, k);
        const uint32_t cam = c.esp - kPassCamera;
        SetF(cam + kCamWidth, 0);  // (so that a pass without the sun is told apart)
        SetF(env + kEnvRadius, r_pass);
        xdk::RenderMark([view, which] { gpu::render::BeginSunShadowPass(view, !which); });
        const uint32_t r = RunShadowPass(c, a[1], engine);
        TraceCasterQuery(engine, cam, which ? "far" : "near");
        // the rest of the frame sees the far radius in every frame, whichever pass ran: the receivers' shading
        // depends on it (GitHub #16, second report: with the near pass's radius left there, objects and ground a few
        // metres away lost their shadow in every other frame although both maps held it).
        // FCI_SHADOW_CASCADE_RADIUS=pass keeps the pass's radius (0.1.5, to reproduce)
        static const bool pass_radius = [] {
            char v[16] = {};
            return GetEnvironmentVariableA("FCI_SHADOW_CASCADE_RADIUS", v, sizeof v) && !std::strcmp(v, "pass");
        }();
        if (!pass_radius) SetF(env + kEnvRadius, radius * k);
        double clip[4][4];
        const bool sun = SunCamera(cam, SquareSide(env, r_pass), clip);
        bool ok = sun;
        const float far_plane = xdk::GF(cam + 0x34);
        // the other map's texture coordinates from this one's: T other_clip this_clip^-1 T^-1 (the other map was drawn
        // by the view's pass before, with its camera)
        float m[3][4] = {};
        if (ok) {
            std::memcpy(v->clip[which], clip, sizeof clip);
            v->done[which] = pass;
            double ot[4][4], tt[4][4], ti[4][4], map[4][4];
            ok = v->done[!which] && v->done[!which] == pass - 1;
            if (ok) {
                ToTexture(v->clip[!which], ot), ToTexture(clip, tt);
                ok = Invert(tt, ti);
            }
            if (ok) {
                Mul(ot, ti, map);
                for (int i = 0; i < 3; i++)
                    for (int j = 0; j < 4; j++) m[i][j] = float(map[i][j]);
            }
        } else {
            v->done[which] = 0;
        }
        static int logged = 0;
        if (logged < 4) {
            logged++;
            // (far plane: the depth range, over which the game's constant depth bias spreads)
            rt::Log("shadows: cascades: %s pass, camera %s (square %.1f m, far plane %.1f m); the other map: u = %.4f u "
                    "%+.4f v %+.4f d %+.4f, v = %.4f u %+.4f v %+.4f d %+.4f, d = %.4f u %+.4f v %+.4f d %+.4f",
                    which ? "far" : "near", ok ? "sun" : "not the sun (or no other map)", SquareSide(env, r_pass),
                    far_plane, m[0][0], m[0][1], m[0][2], m[0][3], m[1][0], m[1][1], m[1][2], m[1][3], m[2][0],
                    m[2][1], m[2][2], m[2][3]);
        }
        float cf[4][4] = {};  // (diagnostics: FCI_DUMP_CASCADE_MAPS)
        if (sun)
            for (int i = 0; i < 4; i++)
                for (int j = 0; j < 4; j++) cf[i][j] = float(clip[i][j]);
        xdk::RenderMark([sun, ok, m, cf] { gpu::render::EndSunShadowPass(sun, ok, m, cf); });
        return r;
    }
    if (k >= 2.0f && cascades) {  // no sun this frame: the receivers read the game's maps
        View& v = ViewOf(env);
        v.done[0] = v.done[1] = 0;
        const int view = int(&v - g_views);
        xdk::RenderMark([view] { gpu::render::DropSunShadow(view); });
    }
    // ---- end shadow cascades ----
    // the frame copies the eased radius here before every pass, so this does not compound
    if (k != 1.0f && radius > 0) SetF(env + kEnvRadius, radius * k);
    if (radius <= 0) return xdk::CallOriginal(c, GUEST_ORIG(Game_ShadowPass));
    const uint32_t r = RunShadowPass(c, a[1], engine);
    TraceCasterQuery(engine, c.esp - kPassCamera, "single map");  // diagnostics: FCI_TRACE_CASTER_QUERY
    return r;
}

// eax = the sector, ecx = the camera position; 5 stack arguments
HLE_STDCALL(Game_ShadowReceiveTest, 5) {
    const float k = pcmode::ShadowDistance();
    const uint32_t engine = R32(kEngine);
    if (k == 1.0f || !engine) return xdk::CallOriginal(c, GUEST_ORIG(Game_ShadowReceiveTest));
    const float radius = xdk::GF(engine + kEngineRadius);
    SetF(engine + kEngineRadius, radius * k);
    uint32_t r = xdk::CallOriginal(c, GUEST_ORIG(Game_ShadowReceiveTest));
    SetF(engine + kEngineRadius, radius);
    return r;
}
