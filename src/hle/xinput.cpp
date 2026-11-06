// The game's XInput on the host: its four controller ports are the host's controllers in their slots
// (hle/host_pad.cpp: SDL3 or XInput; the first one together with the keyboard on port 0, which is always plugged
// in). The XDK's USB stack below XPP never sees a device, so the entry points the game uses are replaced
// (tools/recomp/hooks.h). Keyboard and mouse: hle/pc_input.cpp.

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "app/menu.h"  // settings menu
#include "funcs.h"
#include "hle/host_pad.h"
#include "hle/pc_input.h"
#include "kernel/kernel.h"
#include "runtime/guest.h"
#include "runtime/log.h"
#include "runtime/memory.h"

namespace hle {

namespace {

using namespace rt;
using namespace hle::pc;

constexpr uint32_t kGamepadType = 0x005BDE38;  // XDEVICE_TYPE_GAMEPAD in this XBE
constexpr uint32_t kErrorSuccess = 0, kErrorIoPending = 997;

constexpr int kPorts = 4;
uint32_t g_handle[kPorts] = {};  // guest handles of the opened pads, 0 until opened
uint32_t g_packet[kPorts] = {};
Pad g_last[kPorts];
uint32_t g_script_ports = 0;  // ports the input script plays (bit per port)

int PortOf(uint32_t handle) {
    for (int p = 0; p < kPorts; p++)
        if (handle && g_handle[p] == handle) return p;
    return -1;
}

uint32_t Arg(Cpu& c, int i) { return R32(c.esp + 4 + 4 * i); }

// stdcall return: result in eax, return address and arguments popped
void Return(Cpu& c, uint32_t value, int args) {
    c.eax = value;
    c.esp += 4 + 4 * args;
}

bool Focused() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

// FCI_INPUT_SCRIPT="30:start,33.5:a,40:lup:3,41:a@2": presses a button or pushes a stick fully (lup, ldown,
// lleft, lright, rup, ...) at that many seconds after start, for 0.2 s or the given number of seconds, on
// player 1's pad or the one named after @ (1 to 4); for unattended test runs. A value after = presses only
// that far (0 to 1, "200:rright=0.5:1": the right stick half way right for a second).
void ApplyScript(Pad& p, int port) {
    struct Press {
        double at, duration;
        uint16_t button;
        int analog;
        int axis, sign;  // stick: thumb index and direction
        int port;
        double value;  // 0 to 1
    };
    static std::vector<Press> script = [] {
        std::vector<Press> out;
        char text[4096] = {};
        if (!GetEnvironmentVariableA("FCI_INPUT_SCRIPT", text, sizeof text)) return out;
        static const struct {
            const char* name;
            uint16_t button;
            int analog, axis, sign;
        } kNames[] = {{"start", kStart, -1, -1, 0},    {"back", kBack, -1, -1, 0},      {"up", kDpadUp, -1, -1, 0},
                      {"down", kDpadDown, -1, -1, 0},  {"left", kDpadLeft, -1, -1, 0},  {"right", kDpadRight, -1, -1, 0},
                      {"a", 0, kA, -1, 0},             {"b", 0, kB, -1, 0},             {"x", 0, kX, -1, 0},
                      {"y", 0, kY, -1, 0},             {"lt", 0, kLeftTrigger, -1, 0},  {"rt", 0, kRightTrigger, -1, 0},
                      {"white", 0, kWhite, -1, 0},     {"black", 0, kBlack, -1, 0},     {"lthumb", kLeftThumb, -1, -1, 0},
                      {"rthumb", kRightThumb, -1, -1, 0},
                      {"lleft", 0, -1, 0, -1},         {"lright", 0, -1, 0, 1},         {"ldown", 0, -1, 1, -1},
                      {"lup", 0, -1, 1, 1},            {"rleft", 0, -1, 2, -1},         {"rright", 0, -1, 2, 1},
                      {"rdown", 0, -1, 3, -1},         {"rup", 0, -1, 3, 1}};
        for (char* item = std::strtok(text, ","); item; item = std::strtok(nullptr, ",")) {
            char* name = std::strchr(item, ':');
            if (!name) continue;
            *name++ = 0;
            int to = 0;
            if (char* at = std::strchr(name, '@')) *at = 0, to = std::clamp(std::atoi(at + 1), 1, kPorts) - 1;
            double duration = 0.2;
            if (char* d = std::strchr(name, ':')) *d = 0, duration = std::atof(d + 1);
            double value = 1;
            if (char* v = std::strchr(name, '=')) *v = 0, value = std::clamp(std::atof(v + 1), 0.0, 1.0);
            bool known = false;
            for (const auto& n : kNames)
                if (!std::strcmp(name, n.name))
                    out.push_back({std::atof(item), duration, n.button, n.analog, n.axis, n.sign, to, value}), known = true;
            if (!known) rt::Log("xinput: unknown script input '%s'", name);
            g_script_ports |= 1u << to;
        }
        return out;
    }();
    double now = ScriptSeconds();
    for (const auto& press : script) {
        if (press.port != port || now < press.at || now >= press.at + press.duration) continue;
        p.buttons |= press.button;
        if (press.analog >= 0) p.analog[press.analog] = uint8_t(255 * press.value + 0.5);
        if (press.axis >= 0) p.thumb[press.axis] = int16_t(press.sign * int(32767 * press.value + 0.5));
    }
}

static_assert(kPorts == host::kSlots);
// settings menu: Back + Start held (the menu was asked for); buttons wait for their release after the menu
bool g_combo_held[kPorts] = {};
bool g_pad_settle[kPorts] = {};

// port 0 is always there (keyboard); the others while a host controller or the input script plays them
uint32_t ConnectedPorts() {
    Pad unused;
    ApplyScript(unused, -1);  // reads the script once, for g_script_ports
    uint32_t mask = 1 | g_script_ports;
    for (int p = 0; p < kPorts; p++)
        if (host::Connected(p)) mask |= 1u << p;
    return mask;
}

Pad ReadHost(int port) {
    Pad p;
    if (host::Virtual(port)) {
        // self test: the script presses the virtual controller's buttons and comes back through SDL
        Pad script;
        ApplyScript(script, port);
        host::FeedVirtual(port, script);
        host::Read(port, p);
    } else {
        host::Read(port, p);
        ApplyScript(p, port);
    }
    // ---- settings menu (app/menu.cpp): Back + Start together open it; both stay away from the game until let
    // go. After the menu closes, a pad's buttons reach the game once all of them were let go ----
    constexpr uint16_t kCombo = kBack | kStart;
    if ((p.buttons & kCombo) == kCombo && !g_combo_held[port]) {
        g_combo_held[port] = true;
        app::RequestMenu();
    }
    if (g_combo_held[port]) {
        if (!(p.buttons & kCombo)) g_combo_held[port] = false;
        p.buttons &= uint16_t(~kCombo);
    }
    if (g_pad_settle[port]) {
        bool any = (p.buttons & 0xFF) != 0;
        for (uint8_t a : p.analog) any = any || a > 30;
        if (any) {
            p.buttons = 0;
            std::memset(p.analog, 0, sizeof p.analog);
        } else {
            g_pad_settle[port] = false;
        }
    }
    // ---- end settings menu ----
    // keyboard and mouse (bindings, mouse look, FCI_KEY_SCRIPT) on player 1
    if (port == 0) {
        // performance: what the keyboard/mouse poll costs (GetAsyncKeyState and the focus test are server calls
        // under Wine), every 5 s with FCI_TRACE_INPUT_COST=1
        static const bool trace = GetEnvironmentVariableA("FCI_TRACE_INPUT_COST", nullptr, 0) > 0;
        LARGE_INTEGER a, b, f;
        if (trace) QueryPerformanceCounter(&a);
        ApplyKeyboardMouse(p, Focused());
        if (trace) {
            QueryPerformanceCounter(&b);
            QueryPerformanceFrequency(&f);
            static double ms = 0;
            static uint64_t polls = 0;
            static ULONGLONG since = GetTickCount64();
            ms += double(b.QuadPart - a.QuadPart) * 1000.0 / double(f.QuadPart), polls++;
            if (GetTickCount64() - since >= 5000) {
                rt::Log("input: %llu keyboard/mouse polls in 5 s, %.2f ms (%.1f us each)", (unsigned long long)polls, ms,
                        polls ? ms * 1000.0 / double(polls) : 0.0);
                ms = 0, polls = 0, since = GetTickCount64();
            }
        }
    }
    return p;
}

// ---- diagnostics for calibrating the mouse look ----
// FCI_MEM_SNAPSHOT=200,203,...: at those script seconds, every committed page of guest memory goes to
// mem_<seconds>.bin beside the executable (records: guest address, size, bytes), to find the view angles.
// FCI_TRACE_FLOATS=<hex address>[,<hex address>...]: logs those guest floats at each poll of player 1.
void Diagnostics() {
    static std::vector<double> snapshots = [] {
        std::vector<double> at;
        char text[512] = {};
        if (GetEnvironmentVariableA("FCI_MEM_SNAPSHOT", text, sizeof text))
            for (char* item = std::strtok(text, ","); item; item = std::strtok(nullptr, ",")) at.push_back(std::atof(item));
        return at;
    }();
    // an address is hex, or a pointer chain "A>B>C": the dword at A plus B, the dword there plus C
    static std::vector<std::vector<uint32_t>> floats = [] {
        std::vector<std::vector<uint32_t>> at;
        char text[512] = {};
        if (GetEnvironmentVariableA("FCI_TRACE_FLOATS", text, sizeof text))
            for (char* item = std::strtok(text, ","); item; item = std::strtok(nullptr, ",")) {
                std::vector<uint32_t> chain;
                for (char* p = item; p;) {
                    chain.push_back(uint32_t(std::strtoul(p, &p, 16)));
                    p = *p == '>' ? p + 1 : nullptr;
                }
                at.push_back(chain);
            }
        return at;
    }();
    double now = ScriptSeconds();
    if (!floats.empty()) {
        static double last = -1;
        if (now - last < 0.01) return;  // the boot polls many times a millisecond
        last = now;
        char line[1024];
        int n = snprintf(line, sizeof line, "input: %.3f s floats", now);
        for (const auto& chain : floats) {
            uint32_t a = chain[0];
            for (size_t i = 1; i < chain.size() && a; i++) a = mem::IsCommitted(a) ? R32(a) + chain[i] : 0;
            if (n < int(sizeof line) - 32)
                n += snprintf(line + n, sizeof line - n, " %.4f", a && mem::IsCommitted(a) ? RF32(a) : 0.0f);
        }
        rt::Log("%s", line);
    }
    for (double& at : snapshots) {
        if (at < 0 || now < at) continue;
        char name[64];
        snprintf(name, sizeof name, "mem_%.0f.bin", at);
        at = -1;
        FILE* f = std::fopen(name, "wb");
        if (!f) continue;
        size_t total = 0;
        for (uint64_t a = 0x10000; a < 0xF0000000ull;) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(rt::Host(uint32_t(a)), &mbi, sizeof mbi)) break;
            uint64_t end = uint64_t(static_cast<uint8_t*>(mbi.BaseAddress) - rt::Host(0)) + mbi.RegionSize;
            if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_EXECUTE_READWRITE))) {
                uint32_t start = uint32_t(a), size = uint32_t(std::min<uint64_t>(end, 0xF0000000ull) - a);
                std::fwrite(&start, 4, 1, f);
                std::fwrite(&size, 4, 1, f);
                std::fwrite(rt::Host(start), 1, size, f);
                total += size;
            }
            a = end;
        }
        std::fclose(f);
        rt::Log("input: %.3f s guest memory snapshot %s, %zu bytes", now, name, total);
    }
}

}  // namespace

// ---- PC comfort features: no rumble while the game is paused (app/focus_pause.cpp) ----
void PauseRumble(bool paused) { host::PauseRumble(paused); }

// ---- settings menu: the menu closed (the guest is still held) ----
void SettlePads() {
    for (bool& s : g_pad_settle) s = true;
}

// XPP derives insertions and removals from the connected mask
void Trace(const char* what, Cpu& c, int args) {
    static int traced = 0;
    if (traced++ >= 40) return;
    rt::Log("xinput: %s(%08X, %08X, %08X, %08X) from %08X, type mask %08X", what, Arg(c, 0), args > 1 ? Arg(c, 1) : 0,
            args > 2 ? Arg(c, 2) : 0, args > 3 ? Arg(c, 3) : 0, R32(c.esp), args ? R32(Arg(c, 0)) : 0);
}

// XPP_DEVICE_TYPE: connected ports, changed ports, the ports last reported
void Connect(uint32_t type) {
    if (type != kGamepadType) return;
    uint32_t now = ConnectedPorts();
    static uint32_t logged = 0;
    if (now != logged) rt::Log("xinput: connected ports %X (was %X)", now, logged), logged = now;
    W32(type, now);
    W32(type + 4, R32(type + 4) | (now ^ R32(type + 8)));
}

void XGetDevices(Cpu& c) {
    Connect(Arg(c, 0));
    Trace("XGetDevices", c, 1);
    o_005BEE47(c);
}

void XGetDeviceChanges(Cpu& c) {
    Connect(Arg(c, 0));
    Trace("XGetDeviceChanges", c, 3);
    o_005BEE69(c);
}

void XInputOpen(Cpu& c) {
    uint32_t type = Arg(c, 0), port = Arg(c, 1);
    Trace("XInputOpen", c, 4);
    if (type != kGamepadType || port >= uint32_t(kPorts) || !(ConnectedPorts() & (1u << port))) return o_005C5926(c);
    if (!g_handle[port]) {
        g_handle[port] = mem::AllocSystem(0x100);
        rt::Log("xinput: gamepad on port %u opened, handle %08X", port, g_handle[port]);
    }
    Return(c, g_handle[port], 4);
}

void XInputClose(Cpu& c) {
    int port = PortOf(Arg(c, 0));
    if (port < 0) return o_005C597C(c);
    rt::Log("xinput: gamepad on port %d closed", port);
    Return(c, 0, 1);
}

void XInputGetState(Cpu& c) {
    int port = PortOf(Arg(c, 0));
    if (port < 0) return o_005C5988(c);
    uint32_t state = Arg(c, 1);
    Pad p = ReadHost(port);
    if (port == 0) TracePad(p), Diagnostics();
    if (!(p == g_last[port])) g_packet[port]++, g_last[port] = p;
    W32(state, g_packet[port]);
    W16(state + 4, p.buttons);
    for (int i = 0; i < 8; i++) W8(state + 6 + i, p.analog[i]);
    for (int i = 0; i < 4; i++) W16(state + 14 + 2 * i, uint16_t(p.thumb[i]));
    Return(c, kErrorSuccess, 2);
}

// rumble: XINPUT_FEEDBACK is a 66 byte header (status, event) followed by the two motor speeds
void XInputSetState(Cpu& c) {
    int port = PortOf(Arg(c, 0));
    if (port < 0) return o_005C59FB(c);
    uint32_t feedback = Arg(c, 1);
    host::Rumble(port, R16(feedback + 66), R16(feedback + 68));
    W32(feedback, kErrorSuccess);
    if (uint32_t event = R32(feedback + 4))
        if (auto object = kernel::FindObject(event); object && object->Waitable()) SetEvent(object->Waitable());
    Return(c, kErrorIoPending, 2);
}

void XInputPoll(Cpu& c) {
    if (PortOf(Arg(c, 0)) < 0) return o_005C6220(c);
    Return(c, kErrorSuccess, 1);
}

}  // namespace hle
