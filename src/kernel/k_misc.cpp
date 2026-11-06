// Time, HAL/AV, debug output, Rtl strings, XBE sections and crypto.
#include <windows.h>
#include <timeapi.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <thread>
#include <utility>

#include "hle/host_pad.h"  // controllers: motors off when the title quits
#include "kernel/kernel.h"
#include "runtime/guest_time.h"  // PC comfort features: focus pause
#include "runtime/memory.h"
#include "runtime/thread.h"

namespace kernel {

namespace {

uint32_t PoolAlloc(uint32_t size) {
    uint32_t block = mem::AllocSystem(size + 16);
    W32(block, size);
    return block + 16;
}

// ---------------------------------------------------------------------------------------------
// SHA-1, the hash behind XcSHA* and XcHMAC (save game signatures)

struct Sha1 {
    uint32_t h[5];
    uint64_t length;
    uint8_t buffer[64];

    void Init() {
        h[0] = 0x67452301;
        h[1] = 0xEFCDAB89;
        h[2] = 0x98BADCFE;
        h[3] = 0x10325476;
        h[4] = 0xC3D2E1F0;
        length = 0;
    }

    static uint32_t Rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

    void Block(const uint8_t* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) w[i] = uint32_t(p[i * 4]) << 24 | uint32_t(p[i * 4 + 1]) << 16 | uint32_t(p[i * 4 + 2]) << 8 | p[i * 4 + 3];
        for (int i = 16; i < 80; i++) w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
            else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
            else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
            else f = b ^ c ^ d, k = 0xCA62C1D6;
            uint32_t t = Rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = Rol(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }

    void Update(const uint8_t* p, size_t n) {
        while (n) {
            size_t used = length % 64;
            size_t take = std::min(n, 64 - used);
            std::memcpy(buffer + used, p, take);
            length += take;
            p += take;
            n -= take;
            if (length % 64 == 0) Block(buffer);
        }
    }

    void Final(uint8_t out[20]) {
        uint64_t bits = length * 8;
        uint8_t pad = 0x80;
        Update(&pad, 1);
        uint8_t zero = 0;
        while (length % 64 != 56) Update(&zero, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; i++) len[i] = uint8_t(bits >> (56 - 8 * i));
        Update(len, 8);
        for (int i = 0; i < 5; i++)
            for (int j = 0; j < 4; j++) out[i * 4 + j] = uint8_t(h[i] >> (24 - 8 * j));
    }
};

Sha1* GuestSha(uint32_t ctx) { return reinterpret_cast<Sha1*>(rt::Host(ctx)); }

// ---------------------------------------------------------------------------------------------
// DbgPrint's format, with arguments from the guest stack

std::string FormatGuest(const char* fmt, uint32_t args) {
    std::string out;
    for (const char* p = fmt; *p; p++) {
        if (*p != '%') {
            out += *p;
            continue;
        }
        std::string spec = "%";
        p++;
        while (*p && std::strchr("-+ #0123456789.", *p)) spec += *p++;
        while (*p == 'l' || *p == 'h') p++;
        if (!*p) break;
        char buf[256];
        switch (*p) {
        case 's': {
            std::string s = ReadCString(R32(args));
            args += 4;
            spec += 's';
            snprintf(buf, sizeof buf, spec.c_str(), s.c_str());
            break;
        }
        case 'd':
        case 'i':
        case 'u':
        case 'x':
        case 'X':
        case 'c':
            spec += *p;
            snprintf(buf, sizeof buf, spec.c_str(), R32(args));
            args += 4;
            break;
        case 'p':
            snprintf(buf, sizeof buf, "%08X", R32(args));
            args += 4;
            break;
        case 'f':
        case 'g':
            spec += *p;
            snprintf(buf, sizeof buf, spec.c_str(), rt::RF64(args));
            args += 8;
            break;
        case '%':
            buf[0] = '%';
            buf[1] = 0;
            break;
        default:
            buf[0] = 0;
            break;
        }
        out += buf;
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

}  // namespace

void StartTickCount() {
    std::thread([] {
        timeBeginPeriod(1);
        uint32_t tick = DataAddress(156);
        for (;;) {
            W32(tick, rt::GuestTickCount());  // PC comfort features: stops during a focus pause
            Sleep(1);
        }
    }).detach();
}

// ---------------------------------------------------------------------------------------------
// time

KERNEL_EXPORT(126, KeQueryPerformanceCounter) {
    int64_t v = rt::GuestQpc();  // PC comfort features: stops during a focus pause
    c.edx = uint32_t(v >> 32);
    return uint32_t(v);
}

KERNEL_EXPORT(127, KeQueryPerformanceFrequency) {
    LARGE_INTEGER v;
    QueryPerformanceFrequency(&v);
    c.edx = uint32_t(v.QuadPart >> 32);
    return uint32_t(v.QuadPart);
}

KERNEL_EXPORT(128, KeQuerySystemTime) {
    W64(a[0], rt::GuestFileTime());  // PC comfort features: stops during a focus pause
    return 0;
}

KERNEL_EXPORT(228, NtSetSystemTime) { return X_STATUS_SUCCESS; }

// TIME_FIELDS: Year, Month, Day, Hour, Minute, Second, Milliseconds, Weekday
KERNEL_EXPORT(304, RtlTimeFieldsToTime) {
    uint32_t tf = a[0];
    SYSTEMTIME st{};
    st.wYear = R16(tf);
    st.wMonth = R16(tf + 2);
    st.wDay = R16(tf + 4);
    st.wHour = R16(tf + 6);
    st.wMinute = R16(tf + 8);
    st.wSecond = R16(tf + 10);
    st.wMilliseconds = R16(tf + 12);
    FILETIME ft;
    if (!SystemTimeToFileTime(&st, &ft)) return 0;
    W64(a[1], (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
    return 1;
}

KERNEL_EXPORT(305, RtlTimeToTimeFields) {
    uint64_t t = rt::R64(a[0]);
    FILETIME ft{uint32_t(t), uint32_t(t >> 32)};
    SYSTEMTIME st{};
    FileTimeToSystemTime(&ft, &st);
    uint32_t tf = a[1];
    W16(tf, st.wYear);
    W16(tf + 2, st.wMonth);
    W16(tf + 4, st.wDay);
    W16(tf + 6, st.wHour);
    W16(tf + 8, st.wMinute);
    W16(tf + 10, st.wSecond);
    W16(tf + 12, st.wMilliseconds);
    W16(tf + 14, st.wDayOfWeek);
    return 0;
}

KERNEL_EXPORT(151, KeStallExecutionProcessor) {
    // FCI_TRACE_STALLS=1: callers and requested time, every 10 seconds
    static const bool trace = GetEnvironmentVariableA("FCI_TRACE_STALLS", nullptr, 0) > 0;
    if (trace) {
        static std::map<uint32_t, std::pair<uint64_t, uint64_t>> calls;  // return address -> calls, microseconds
        static ULONGLONG last = GetTickCount64();
        auto& entry = calls[R32(c.esp)];
        entry.first++, entry.second += a[0];
        if (GetTickCount64() - last > 10000) {
            last = GetTickCount64();
            for (auto& [from, n] : calls)
                rt::Log("stall: from %08X, %llu calls, %llu us", from, (unsigned long long)n.first,
                        (unsigned long long)n.second);
            calls.clear();
        }
    }
    LARGE_INTEGER f, start, now;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&start);
    int64_t ticks = int64_t(a[0]) * f.QuadPart / 1000000;
    do {
        YieldProcessor();
        QueryPerformanceCounter(&now);
    } while (now.QuadPart - start.QuadPart < ticks);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// hal, av, eeprom

KERNEL_EXPORT(1, AvGetSavedDataAddress) { return 0; }
KERNEL_EXPORT(3, AvSetDisplayMode) { return 0; }
KERNEL_EXPORT(4, AvSetSavedDataAddress) { return 0; }

// RegisterBase, Option, Param, Result*
KERNEL_EXPORT(2, AvSendTVEncoderOption) {
    if (a[3]) {
        uint32_t result = 0;
        if (a[1] == 6) result = 0x00400101;  // AV_QUERY_AV_CAPABILITIES: NTSC-M, 60 Hz, standard AV pack
        W32(a[3], result);
    }
    return 0;
}

// ValueIndex, Type*, Value, ValueLength, ResultLength*
KERNEL_EXPORT(24, ExQueryNonVolatileSetting) {
    uint32_t value = 0;
    switch (a[0]) {
    case 0x07: value = 1; break;           // XC_LANGUAGE: English
    case 0x08: value = 0x00400000; break;  // XC_VIDEO_FLAGS: 60 Hz allowed
    case 0x09: value = 0; break;           // XC_AUDIO_FLAGS: stereo
    case 0x103: value = 0x00400100; break; // XC_FACTORY_AV_REGION: NTSC-M
    case 0x104: value = 1; break;          // XC_FACTORY_GAME_REGION: North America
    default: break;
    }
    if (a[1]) W32(a[1], 4);  // REG_DWORD
    if (a[2] && a[3] >= 4) W32(a[2], value);
    else if (a[2] && a[3]) std::memset(rt::Host(a[2]), 0, a[3]);
    if (a[4]) W32(a[4], std::min<uint32_t>(a[3], 4));
    return X_STATUS_SUCCESS;
}

KERNEL_EXPORT(44, HalGetInterruptVector) {
    if (a[1]) W8(a[1], uint8_t(a[0]));
    return a[0] + 0x30;
}

// BusNumber, SlotNumber, RegisterNumber, Buffer, Length, WritePCISpace
KERNEL_EXPORT(46, HalReadWritePCISpace) {
    if (!a[5] && a[3] && a[4]) std::memset(rt::Host(a[3]), 0, a[4]);
    return 0;
}

KERNEL_EXPORT(47, HalRegisterShutdownNotification) { return 0; }
KERNEL_EXPORT(358, HalIsResetOrShutdownPending) { return 0; }

KERNEL_EXPORT(49, HalReturnToFirmware) {
    hle::host::StopRumble();
    // XLaunchNewImage of the title itself (FCI reboots between levels): start again with the launch data
    if (Relaunch(a[0])) rt::FlushLog(), ExitProcess(0);
    rt::Log("kernel: HalReturnToFirmware(%u), the title quit", a[0]);
    rt::FlushLog();
    ExitProcess(0);
}

KERNEL_EXPORT(360, HalInitiateShutdown) {
    rt::Log("kernel: HalInitiateShutdown");
    hle::host::StopRumble();
    rt::FlushLog();
    ExitProcess(0);
}

KERNEL_EXPORT(252, PhyGetLinkState) { return 0; }
KERNEL_EXPORT(253, PhyInitialize) { return 0; }

KERNEL_EXPORT(95, KeBugCheck) { rt::Fatal("KeBugCheck(%08X)", a[0]); }

// ---------------------------------------------------------------------------------------------
// debug

KERNEL_EXPORT(5, DbgBreakPoint) {
    rt::Log("[guest] DbgBreakPoint from %08X", R32(c.esp));
    return 0;
}

KERNEL_EXPORT(8, DbgPrint) {
    rt::Log("[guest] %s", FormatGuest(ReadCString(a[0]).c_str(), c.esp + 8).c_str());
    return X_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------------------------
// rtl strings: STRING and UNICODE_STRING are Length, MaximumLength, Buffer

KERNEL_EXPORT(289, RtlInitAnsiString) {
    uint32_t dest = a[0], src = a[1];
    uint32_t len = src ? uint32_t(std::strlen(reinterpret_cast<const char*>(rt::Host(src)))) : 0;
    W16(dest, uint16_t(len));
    W16(dest + 2, uint16_t(src ? len + 1 : 0));
    W32(dest + 4, src);
    return 0;
}

KERNEL_EXPORT(260, RtlAnsiStringToUnicodeString) {
    uint32_t dest = a[0];
    std::string s = ReadAnsiString(a[1]);
    uint32_t bytes = uint32_t(s.size()) * 2;
    if (a[2]) {
        W32(dest + 4, PoolAlloc(bytes + 2));
        W16(dest + 2, uint16_t(bytes + 2));
    } else if (R16(dest + 2) < bytes) {
        return X_STATUS_BUFFER_TOO_SMALL;
    }
    uint32_t buf = R32(dest + 4);
    for (size_t i = 0; i < s.size(); i++) W16(buf + uint32_t(i) * 2, uint8_t(s[i]));
    if (R16(dest + 2) > bytes) W16(buf + bytes, 0);
    W16(dest, uint16_t(bytes));
    return X_STATUS_SUCCESS;
}

KERNEL_EXPORT(308, RtlUnicodeStringToAnsiString) {
    uint32_t dest = a[0], src = a[1];
    uint32_t chars = R16(src) / 2;
    if (a[2]) {
        W32(dest + 4, PoolAlloc(chars + 1));
        W16(dest + 2, uint16_t(chars + 1));
    } else if (R16(dest + 2) < chars) {
        return X_STATUS_BUFFER_TOO_SMALL;
    }
    uint32_t in = R32(src + 4), out = R32(dest + 4);
    for (uint32_t i = 0; i < chars; i++) {
        uint16_t ch = R16(in + i * 2);
        W8(out + i, ch < 0x100 ? uint8_t(ch) : '?');
    }
    if (R16(dest + 2) > chars) W8(out + chars, 0);
    W16(dest, uint16_t(chars));
    return X_STATUS_SUCCESS;
}

KERNEL_EXPORT(270, RtlCompareString) {
    std::string s1 = ReadAnsiString(a[0]), s2 = ReadAnsiString(a[1]);
    bool ci = a[2] & 0xFF;
    size_t n = std::min(s1.size(), s2.size());
    for (size_t i = 0; i < n; i++) {
        int c1 = uint8_t(s1[i]), c2 = uint8_t(s2[i]);
        if (ci) {
            c1 = toupper(c1);
            c2 = toupper(c2);
        }
        if (c1 != c2) return uint32_t(c1 - c2);
    }
    return uint32_t(int32_t(s1.size()) - int32_t(s2.size()));
}

KERNEL_EXPORT(279, RtlEqualString) {
    std::string s1 = ReadAnsiString(a[0]), s2 = ReadAnsiString(a[1]);
    if (s1.size() != s2.size()) return 0;
    if (!(a[2] & 0xFF)) return s1 == s2;
    return _stricmp(s1.c_str(), s2.c_str()) == 0;
}

KERNEL_EXPORT(269, RtlCompareMemoryUlong) {
    uint32_t n = 0;
    for (uint32_t i = 0; i + 4 <= a[1] && R32(a[0] + i) == a[2]; i += 4) n += 4;
    return n;
}

KERNEL_EXPORT(301, RtlNtStatusToDosError) {
    using Fn = ULONG(WINAPI*)(LONG);
    static Fn fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlNtStatusToDosError"));
    return fn ? fn(LONG(a[0])) : 317;
}

KERNEL_EXPORT(302, RtlRaiseException) {
    uint32_t rec = a[0];
    rt::Fatal("guest raised exception %08X at %08X (flags %08X)", R32(rec), R32(rec + 12), R32(rec + 4));
}

// TargetFrame, TargetIp, ExceptionRecord, ReturnValue: drops the registrations above TargetFrame
KERNEL_EXPORT(312, RtlUnwind) {
    if (auto* t = rt::CurrentThread(); t && a[0]) W32(t->kpcr, a[0]);
    return a[3];
}

// ---------------------------------------------------------------------------------------------
// XBE sections: everything is mapped at load, only the reference count changes

KERNEL_EXPORT(327, XeLoadSection) {
    W32(a[0] + 0x18, R32(a[0] + 0x18) + 1);
    return X_STATUS_SUCCESS;
}

KERNEL_EXPORT(328, XeUnloadSection) {
    if (R32(a[0] + 0x18)) W32(a[0] + 0x18, R32(a[0] + 0x18) - 1);
    return X_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------------------------
// crypto

KERNEL_EXPORT(335, XcSHAInit) {
    GuestSha(a[0])->Init();
    return 0;
}

KERNEL_EXPORT(336, XcSHAUpdate) {
    GuestSha(a[0])->Update(rt::Host(a[1]), a[2]);
    return 0;
}

KERNEL_EXPORT(337, XcSHAFinal) {
    GuestSha(a[0])->Final(rt::Host(a[1]));
    return 0;
}

// KeyMaterial, KeyMaterialLength, Data, DataLength, Data2, Data2Length, Digest
KERNEL_EXPORT(340, XcHMAC) {
    uint8_t key[64] = {};
    if (a[1] > 64) {
        Sha1 s;
        s.Init();
        s.Update(rt::Host(a[0]), a[1]);
        s.Final(key);
    } else {
        std::memcpy(key, rt::Host(a[0]), a[1]);
    }
    uint8_t pad[64], inner[20];
    Sha1 s;
    for (int i = 0; i < 64; i++) pad[i] = key[i] ^ 0x36;
    s.Init();
    s.Update(pad, 64);
    if (a[2]) s.Update(rt::Host(a[2]), a[3]);
    if (a[4]) s.Update(rt::Host(a[4]), a[5]);
    s.Final(inner);
    for (int i = 0; i < 64; i++) pad[i] = key[i] ^ 0x5C;
    s.Init();
    s.Update(pad, 64);
    s.Update(inner, 20);
    s.Final(rt::Host(a[6]));
    return 0;
}

// RC4 state: S[256], i, j
KERNEL_EXPORT(338, XcRC4Key) {
    uint8_t* st = rt::Host(a[0]);
    const uint8_t* key = rt::Host(a[2]);
    for (int i = 0; i < 256; i++) st[i] = uint8_t(i);
    uint8_t j = 0;
    for (int i = 0; i < 256; i++) {
        j = uint8_t(j + st[i] + key[i % a[1]]);
        std::swap(st[i], st[j]);
    }
    st[256] = st[257] = 0;
    return 0;
}

KERNEL_EXPORT(339, XcRC4Crypt) {
    uint8_t* st = rt::Host(a[0]);
    uint8_t* data = rt::Host(a[2]);
    uint8_t i = st[256], j = st[257];
    for (uint32_t n = 0; n < a[1]; n++) {
        i = uint8_t(i + 1);
        j = uint8_t(j + st[i]);
        std::swap(st[i], st[j]);
        data[n] ^= st[uint8_t(st[i] + st[j])];
    }
    st[256] = i;
    st[257] = j;
    return 0;
}

// public key and block ciphers serve Xbox Live only
KERNEL_EXPORT(344, XcVerifyPKCS1Signature) { return 1; }
KERNEL_EXPORT(345, XcModExp) { return 0; }
KERNEL_EXPORT(346, XcDESKeyParity) { return 0; }
KERNEL_EXPORT(347, XcKeyTable) { return 0; }

// dwCipher, dwInputLength, pbOutput, pbInput, pbKeyTable, dwOp, pbFeedback
KERNEL_EXPORT(349, XcBlockCryptCBC) {
    std::memmove(rt::Host(a[2]), rt::Host(a[3]), a[1]);
    return 0;
}

}  // namespace kernel
