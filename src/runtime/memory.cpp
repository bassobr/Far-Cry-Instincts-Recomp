#include "runtime/memory.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <thread>

#include "runtime/guest.h"
#include "runtime/log.h"
#include "runtime/thread.h"
#include "xbe/xbe.h"

// this file's statics, the guest address space reservation among them, initialize before the rest of ours
#pragma warning(suppress : 4073)
#pragma init_seg(lib)

namespace mem {

namespace {

constexpr uint32_t kPage = 0x1000;
constexpr uint64_t kGuestSize = 0x100000000ull;

uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

Region g_virtual(kVirtualBegin, kVirtualEnd, 0x10000);
Region g_contiguous(kContiguousBegin, kContiguousEnd, kPage);
Region g_system(kSystemBegin, kSystemEnd, kPage);
std::atomic<int> g_lazy_reports{0};

// FCI_WATCH=<address>:<bytes>@<seconds> (hex address and size): from then on, every write into that guest
// range is logged with the writing function and the old and new value. The pages are made read-only; a write
// faults, the page opens for one single-stepped instruction and closes again. For finding who corrupts memory.
uint32_t g_watch_lo = 0, g_watch_hi = 0;
std::atomic<bool> g_watch_armed{false};
struct WatchHit {
    bool pending = false, log = false;
    uint32_t page = 0, address = 0, old = 0;
    uint64_t rip = 0;
};
thread_local WatchHit t_watch;

bool Watched(uint32_t page) { return g_watch_armed && page + kPage > g_watch_lo && page < g_watch_hi; }

void ProtectWatch(DWORD protect) {
    for (uint32_t page = g_watch_lo & ~(kPage - 1); page < g_watch_hi; page += kPage) {
        VirtualAlloc(rt::Host(page), kPage, MEM_COMMIT, PAGE_READWRITE);
        DWORD old;
        VirtualProtect(rt::Host(page), kPage, protect, &old);
    }
}

LONG WatchFault(EXCEPTION_POINTERS* info, uint32_t guest) {
    uint32_t page = guest & ~(kPage - 1);
    DWORD old;
    VirtualProtect(rt::Host(page), kPage, PAGE_READWRITE, &old);
    t_watch = {true, guest >= g_watch_lo && guest < g_watch_hi, page, guest & ~3u,
               *reinterpret_cast<uint32_t*>(rt::Host(guest & ~3u)), info->ContextRecord->Rip};
    info->ContextRecord->EFlags |= 0x100;  // trap after the write
    return EXCEPTION_CONTINUE_EXECUTION;
}

LONG WatchStep(EXCEPTION_POINTERS* info) {
    t_watch.pending = false;
    info->ContextRecord->EFlags &= ~0x100u;
    if (t_watch.log) {
        uint32_t now = *reinterpret_cast<uint32_t*>(rt::Host(t_watch.address));
        const rt::GuestThread* t = rt::CurrentThread();
        // code addresses on the guest stack: the callers, roughly
        char calls[160] = {};
        size_t at = 0;
        for (uint32_t i = 0, n = 0; t && i < 64 && n < 10; i++) {
            uint32_t v = rt::R32(t->cpu.esp + 4 * i);
            if (v >= 0x10000 && v < 0x700000 && at < sizeof calls - 10) at += snprintf(calls + at, sizeof calls - at, " %08X", v), n++;
        }
        rt::Log("watch: %08X %08X -> %08X in %s (thread %u, esp %08X, stack%s)", t_watch.address, t_watch.old, now,
                rt::SymbolAt(t_watch.rip).c_str(), t ? t->id : 0, t ? t->cpu.esp : 0, calls);
    }
    DWORD old;
    if (g_watch_armed) VirtualProtect(rt::Host(t_watch.page), kPage, PAGE_READONLY, &old);
    return EXCEPTION_CONTINUE_EXECUTION;
}

void StartWatch() {
    char value[64] = {};
    if (!GetEnvironmentVariableA("FCI_WATCH", value, sizeof value)) return;
    char* end = nullptr;
    g_watch_lo = uint32_t(std::strtoul(value, &end, 16));
    uint32_t bytes = *end == ':' ? uint32_t(std::strtoul(end + 1, &end, 16)) : 4;
    double after = *end == '@' ? std::atof(end + 1) : 0;
    g_watch_hi = g_watch_lo + std::max(bytes, 1u);
    std::thread([after] {
        Sleep(DWORD(after * 1000));
        g_watch_armed = true;
        ProtectWatch(PAGE_READONLY);
        rt::Log("watch: %08X-%08X armed", g_watch_lo, g_watch_hi);
    }).detach();
}

// any guest page that is touched but not committed gets committed, except the null page range;
// the first few such pages are logged since they point at missing emulation (hardware registers)
LONG CALLBACK OnFault(EXCEPTION_POINTERS* info) {
    auto* rec = info->ExceptionRecord;
    if (rec->ExceptionCode == EXCEPTION_SINGLE_STEP && t_watch.pending) return WatchStep(info);
    if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || rec->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    uint64_t host = rec->ExceptionInformation[1];
    if (host < rt::kGuestBase || host >= rt::kGuestBase + kGuestSize) return EXCEPTION_CONTINUE_SEARCH;
    uint32_t guest = uint32_t(host - rt::kGuestBase);
    if (rec->ExceptionInformation[0] == 1 && Watched(guest & ~(kPage - 1))) return WatchFault(info, guest);
    if (guest < 0x10000) {
        rt::Log("guest null pointer access at %08X (%s)", guest, rec->ExceptionInformation[0] ? "write" : "read");
        if (auto* t = rt::CurrentThread()) {
            const rt::Cpu& c = t->cpu;
            rt::Log("  thread %u at %08X: eax %08X ecx %08X edx %08X ebx %08X esp %08X ebp %08X esi %08X edi %08X",
                    t->id, c.pc, c.eax, c.ecx, c.edx, c.ebx, c.esp, c.ebp, c.esi, c.edi);
            rt::DumpTrace(c);
        }
        rt::PrintStack(info->ContextRecord);
        TerminateProcess(GetCurrentProcess(), 1);
    }
    uint32_t page = guest & ~(kPage - 1);
    if (!VirtualAlloc(rt::Host(page), kPage, MEM_COMMIT, PAGE_READWRITE)) return EXCEPTION_CONTINUE_SEARCH;
    if (g_lazy_reports.fetch_add(1) < 64)
        rt::Log("memory: %s of unallocated guest page %08X in %s", rec->ExceptionInformation[0] ? "write" : "read", page,
                rt::SymbolAt(info->ContextRecord->Rip).c_str());
    return EXCEPTION_CONTINUE_EXECUTION;
}

}  // namespace

uint32_t Region::Alloc(uint32_t size, uint32_t align, bool commit) {
    size = AlignUp(size ? size : 1, kPage);
    align = std::max(align, granularity_);
    std::lock_guard lock(lock_);
    uint32_t at = AlignUp(begin_, align);
    for (const auto& [base, len] : blocks_) {
        if (uint64_t(at) + size <= base) break;
        at = std::max(at, AlignUp(base + len, align));
    }
    if (uint64_t(at) + size > end_) return 0;
    blocks_[at] = size;
    if (commit) Commit(at, size);
    return at;
}

bool Region::AllocAt(uint32_t addr, uint32_t size, bool commit) {
    size = AlignUp(size ? size : 1, kPage);
    std::lock_guard lock(lock_);
    if (addr < begin_ || uint64_t(addr) + size > end_) return false;
    auto it = blocks_.upper_bound(addr);
    if (it != blocks_.end() && it->first < addr + size) return false;
    if (it != blocks_.begin()) {
        auto prev = std::prev(it);
        if (prev->first + prev->second > addr) return false;
    }
    blocks_[addr] = size;
    if (commit) Commit(addr, size);
    return true;
}

uint32_t Region::Free(uint32_t addr) {
    std::lock_guard lock(lock_);
    auto it = blocks_.find(addr);
    if (it == blocks_.end()) return 0;
    uint32_t size = it->second;
    blocks_.erase(it);
    Decommit(addr, size);
    return size;
}

bool Region::Find(uint32_t addr, uint32_t* base, uint32_t* size) const {
    std::lock_guard lock(lock_);
    auto it = blocks_.upper_bound(addr);
    if (it == blocks_.begin()) return false;
    --it;
    if (addr >= it->first + it->second) return false;
    *base = it->first;
    *size = it->second;
    return true;
}

uint32_t Region::used() const {
    std::lock_guard lock(lock_);
    uint32_t total = 0;
    for (const auto& [base, len] : blocks_) total += len;
    return total;
}

namespace {

// the guest address space is reserved before anything else of ours runs (static initializers, threads,
// D3D), so no heap or driver mapping can land in it first
void* ReserveGuestSpace() {
    return VirtualAlloc(reinterpret_cast<void*>(rt::kGuestBase), kGuestSize, MEM_RESERVE, PAGE_READWRITE);
}
void* const g_reserved = ReserveGuestSpace();

}  // namespace

bool Init() {
    void* p = g_reserved;
    if (p != reinterpret_cast<void*>(rt::kGuestBase)) p = ReserveGuestSpace();
    if (p != reinterpret_cast<void*>(rt::kGuestBase)) {
        rt::Log("memory: cannot reserve the guest address space at %llX", (unsigned long long)rt::kGuestBase);
        // name what is in the way
        for (uint64_t a = rt::kGuestBase; a < rt::kGuestBase + kGuestSize;) {
            MEMORY_BASIC_INFORMATION info;
            if (!VirtualQuery(reinterpret_cast<void*>(a), &info, sizeof info)) break;
            if (info.State != MEM_FREE)
                rt::Log("memory:   %p size %llX state %lX type %lX", info.BaseAddress, (unsigned long long)info.RegionSize,
                        info.State, info.Type);
            a = reinterpret_cast<uint64_t>(info.BaseAddress) + info.RegionSize;
        }
        return false;
    }
    AddVectoredExceptionHandler(1, OnFault);
    StartWatch();
    return true;
}

void LoadImage(const xbe::Image& image) {
    g_virtual.AllocAt(image.base, AlignUp(uint32_t(image.memory.size()), 0x10000), true);
    std::memcpy(rt::Host(image.base), image.memory.data(), image.memory.size());
}

void Commit(uint32_t addr, uint32_t size) {
    if (!size) return;
    uint32_t start = addr & ~(kPage - 1);
    uint32_t end = AlignUp(addr + size, kPage);
    if (!VirtualAlloc(rt::Host(start), end - start, MEM_COMMIT, PAGE_READWRITE))
        rt::Fatal("memory: commit of %08X+%X failed (%lu)", start, end - start, GetLastError());
}

void Decommit(uint32_t addr, uint32_t size) {
    if (!size) return;
    uint32_t start = addr & ~(kPage - 1);
    uint32_t end = AlignUp(addr + size, kPage);
    VirtualFree(rt::Host(start), end - start, MEM_DECOMMIT);
}

bool IsCommitted(uint32_t addr) {
    MEMORY_BASIC_INFORMATION info;
    return VirtualQuery(rt::Host(addr), &info, sizeof info) && info.State == MEM_COMMIT;
}

Region& Virtual() { return g_virtual; }
Region& Contiguous() { return g_contiguous; }
Region& System() { return g_system; }

uint8_t* Physical(uint32_t address) { return rt::Host(kPhysicalBase + (address & (kPhysicalSize - 1))); }

uint32_t DeviceAddress(uint32_t guest) {
    if (guest - kPhysicalBase < kPhysicalSize) return guest - kPhysicalBase;
    if (guest < kPhysicalSize) return kLowAlias + guest;
    return guest;
}

uint8_t* Device(uint32_t address) {
    if (address < kPhysicalSize) return Physical(address);
    if (address - kLowAlias < kPhysicalSize) return rt::Host(address - kLowAlias);
    return rt::Host(address);
}

uint32_t AllocSystem(uint32_t size) {
    uint32_t a = g_system.Alloc(size);
    if (!a) rt::Fatal("memory: system region exhausted");
    return a;
}

}  // namespace mem
