// The export table, kernel call dispatch, data exports and shared helpers.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "kernel/kernel.h"
#include "runtime/guest_time.h"  // PC comfort features: focus pause
#include "runtime/memory.h"
#include "runtime/runtime.h"
#include "runtime/thread.h"
#include "xbe/xbe.h"

namespace kernel {

namespace {

enum class Kind : uint8_t { Stdcall, Fastcall, Cdecl, Data };

struct Export {
    uint16_t ordinal;
    const char* name;
    Kind kind;
    uint16_t size;  // stdcall: argument dwords; data: bytes
};

constexpr Kind S = Kind::Stdcall, F = Kind::Fastcall, C = Kind::Cdecl, D = Kind::Data;

// every export default.xbe imports
const Export kExports[] = {
    {1, "AvGetSavedDataAddress", S, 0},          {2, "AvSendTVEncoderOption", S, 4},
    {3, "AvSetDisplayMode", S, 6},               {4, "AvSetSavedDataAddress", S, 1},
    {5, "DbgBreakPoint", S, 0},                  {8, "DbgPrint", C, 0},
    {15, "ExAllocatePoolWithTag", S, 2},         {16, "ExEventObjectType", D, 0x20},
    {17, "ExFreePool", S, 1},                    {23, "ExQueryPoolBlockSize", S, 1},
    {24, "ExQueryNonVolatileSetting", S, 5},     {35, "FscGetCacheSize", S, 0},
    {37, "FscSetCacheSize", S, 1},               {40, "HalDiskCachePartitionCount", D, 4},
    {41, "HalDiskModelNumber", D, 8 + 48},       {42, "HalDiskSerialNumber", D, 8 + 32},
    {44, "HalGetInterruptVector", S, 2},         {46, "HalReadWritePCISpace", S, 6},
    {47, "HalRegisterShutdownNotification", S, 2}, {49, "HalReturnToFirmware", S, 1},
    {62, "IoBuildSynchronousFsdRequest", S, 7},  {65, "IoCreateDevice", S, 6},
    {67, "IoCreateSymbolicLink", S, 2},          {69, "IoDeleteSymbolicLink", S, 1},
    {71, "IoFileObjectType", D, 0x20},           {74, "IoInvalidDeviceRequest", S, 2},
    {81, "IoStartNextPacket", S, 1},             {83, "IoStartPacket", S, 3},
    {84, "IoSynchronousDeviceIoControlRequest", S, 8}, {85, "IoSynchronousFsdRequest", S, 5},
    {86, "IofCallDriver", F, 0},                 {87, "IofCompleteRequest", F, 0},
    {95, "KeBugCheck", S, 1},                    {97, "KeCancelTimer", S, 1},
    {98, "KeConnectInterrupt", S, 1},            {99, "KeDelayExecutionThread", S, 3},
    {100, "KeDisconnectInterrupt", S, 1},        {107, "KeInitializeDpc", S, 3},
    {109, "KeInitializeInterrupt", S, 7},        {113, "KeInitializeTimerEx", S, 2},
    {119, "KeInsertQueueDpc", S, 3},             {126, "KeQueryPerformanceCounter", S, 0},
    {127, "KeQueryPerformanceFrequency", S, 0},  {128, "KeQuerySystemTime", S, 1},
    {129, "KeRaiseIrqlToDpcLevel", S, 0},        {137, "KeRemoveQueueDpc", S, 1},
    {139, "KeRestoreFloatingPointState", S, 1},  {142, "KeSaveFloatingPointState", S, 1},
    {143, "KeSetBasePriorityThread", S, 2},      {145, "KeSetEvent", S, 3},
    {149, "KeSetTimer", S, 4},                   {150, "KeSetTimerEx", S, 5},
    {151, "KeStallExecutionProcessor", S, 1},    {153, "KeSynchronizeExecution", S, 3},
    {156, "KeTickCount", D, 4},                  {158, "KeWaitForMultipleObjects", S, 8},
    {159, "KeWaitForSingleObject", S, 5},        {160, "KfRaiseIrql", F, 0},
    {161, "KfLowerIrql", F, 0},                  {164, "LaunchDataPage", D, 4},
    {165, "MmAllocateContiguousMemory", S, 1},   {166, "MmAllocateContiguousMemoryEx", S, 5},
    {168, "MmClaimGpuInstanceMemory", S, 2},     {169, "MmCreateKernelStack", S, 2},
    {170, "MmDeleteKernelStack", S, 2},          {171, "MmFreeContiguousMemory", S, 1},
    {173, "MmGetPhysicalAddress", S, 1},         {175, "MmLockUnlockBufferPages", S, 3},
    {176, "MmLockUnlockPhysicalPage", S, 2},     {178, "MmPersistContiguousMemory", S, 3},
    {179, "MmQueryAddressProtect", S, 1},        {180, "MmQueryAllocationSize", S, 1},
    {181, "MmQueryStatistics", S, 1},            {182, "MmSetAddressProtect", S, 3},
    {184, "NtAllocateVirtualMemory", S, 5},      {186, "NtClearEvent", S, 1},
    {187, "NtClose", S, 1},                      {189, "NtCreateEvent", S, 4},
    {190, "NtCreateFile", S, 9},                 {192, "NtCreateMutant", S, 3},
    {195, "NtDeleteFile", S, 1},                 {196, "NtDeviceIoControlFile", S, 10},
    {197, "NtDuplicateObject", S, 3},            {198, "NtFlushBuffersFile", S, 2},
    {199, "NtFreeVirtualMemory", S, 3},          {200, "NtFsControlFile", S, 10},
    {202, "NtOpenFile", S, 6},                   {203, "NtOpenSymbolicLinkObject", S, 2},
    {207, "NtQueryDirectoryFile", S, 10},        {210, "NtQueryFullAttributesFile", S, 2},
    {211, "NtQueryInformationFile", S, 5},       {215, "NtQuerySymbolicLinkObject", S, 3},
    {217, "NtQueryVirtualMemory", S, 2},         {218, "NtQueryVolumeInformationFile", S, 5},
    {219, "NtReadFile", S, 8},                   {221, "NtReleaseMutant", S, 2},
    {224, "NtResumeThread", S, 2},               {225, "NtSetEvent", S, 2},
    {226, "NtSetInformationFile", S, 5},         {228, "NtSetSystemTime", S, 2},
    {231, "NtSuspendThread", S, 2},              {233, "NtWaitForSingleObject", S, 3},
    {234, "NtWaitForSingleObjectEx", S, 4},      {235, "NtWaitForMultipleObjectsEx", S, 6},
    {236, "NtWriteFile", S, 8},                  {246, "ObReferenceObjectByHandle", S, 3},
    {247, "ObReferenceObjectByName", S, 5},      {250, "ObfDereferenceObject", F, 0},
    {252, "PhyGetLinkState", S, 1},              {253, "PhyInitialize", S, 2},
    {255, "PsCreateSystemThreadEx", S, 10},      {258, "PsTerminateSystemThread", S, 1},
    {259, "PsThreadObjectType", D, 0x20},        {260, "RtlAnsiStringToUnicodeString", S, 3},
    {269, "RtlCompareMemoryUlong", S, 3},        {270, "RtlCompareString", S, 3},
    {277, "RtlEnterCriticalSection", S, 1},      {279, "RtlEqualString", S, 3},
    {289, "RtlInitAnsiString", S, 2},            {291, "RtlInitializeCriticalSection", S, 1},
    {294, "RtlLeaveCriticalSection", S, 1},      {301, "RtlNtStatusToDosError", S, 1},
    {302, "RtlRaiseException", S, 1},           {304, "RtlTimeFieldsToTime", S, 2},
    {305, "RtlTimeToTimeFields", S, 2},          {308, "RtlUnicodeStringToAnsiString", S, 3},
    {312, "RtlUnwind", S, 4},                    {322, "XboxHardwareInfo", D, 8},
    {323, "XboxHDKey", D, 16},                   {324, "XboxKrnlVersion", D, 8},
    {325, "XboxSignatureKey", D, 16},            {326, "XeImageFileName", D, 8 + 64},
    {327, "XeLoadSection", S, 1},                {328, "XeUnloadSection", S, 1},
    {335, "XcSHAInit", S, 1},                    {336, "XcSHAUpdate", S, 3},
    {337, "XcSHAFinal", S, 2},                   {338, "XcRC4Key", S, 3},
    {339, "XcRC4Crypt", S, 3},                   {340, "XcHMAC", S, 7},
    {344, "XcVerifyPKCS1Signature", S, 3},       {345, "XcModExp", S, 5},
    {346, "XcDESKeyParity", S, 2},               {347, "XcKeyTable", S, 3},
    {349, "XcBlockCryptCBC", S, 7},              {353, "XboxLANKey", D, 16},
    {354, "XboxAlternateSignatureKeys", D, 16 * 16}, {355, "XePublicKeyData", D, 284},
    {356, "HalBootSMCVideoMode", D, 4},          {357, "IdexChannelObject", D, 0x40},
    {358, "HalIsResetOrShutdownPending", S, 0},  {359, "IoMarkIrpMustComplete", S, 1},
    {360, "HalInitiateShutdown", S, 0},
};

constexpr uint32_t kMaxOrdinal = 400;

struct Entry {
    const Export* info = nullptr;
    Handler handler = nullptr;
    uint32_t data = 0;  // guest address of a data export
    std::atomic<bool> reported{false};
};

Entry* Entries() {
    static Entry entries[kMaxOrdinal];
    return entries;
}

bool g_trace = false;

// handles
std::mutex g_handles_lock;
std::unordered_map<uint32_t, std::shared_ptr<Object>> g_handles;
uint32_t g_next_handle = 0x1000;

}  // namespace

Registrar::Registrar(uint32_t ordinal, Handler handler) { Entries()[ordinal].handler = handler; }

uint32_t DataAddress(uint32_t ordinal) { return ordinal < kMaxOrdinal ? Entries()[ordinal].data : 0; }

void Init(const xbe::Image& image) {
    auto* entries = Entries();
    for (const auto& e : kExports) {
        entries[e.ordinal].info = &e;
        if (e.kind == D) entries[e.ordinal].data = mem::AllocSystem(e.size);
    }
    g_trace = GetEnvironmentVariableA("FCI_TRACE_KERNEL", nullptr, 0) > 0;

    // the thunk table: data exports point at their data, functions at a callable fake address
    for (size_t i = 0; i < image.kernel_imports.size(); i++) {
        uint32_t ord = image.kernel_imports[i];
        uint32_t slot = image.kernel_thunk + uint32_t(i) * 4;
        if (ord >= kMaxOrdinal || !entries[ord].info) rt::Fatal("kernel: import of unknown ordinal %u", ord);
        W32(slot, entries[ord].data ? entries[ord].data : rt::kKernelThunkBase + ord * 4);
    }

    // data exports
    W32(DataAddress(40), 3);  // cache partitions X, Y, Z
    auto put_string = [](uint32_t str, const char* text) {
        uint32_t len = uint32_t(std::strlen(text));
        std::memcpy(rt::Host(str + 8), text, len + 1);
        W16(str, uint16_t(len));
        W16(str + 2, uint16_t(len + 1));
        W32(str + 4, str + 8);
    };
    put_string(DataAddress(41), "FCI-RECOMP HDD");
    put_string(DataAddress(42), "0000000000000000");
    put_string(DataAddress(326), "\\Device\\CdRom0\\default.xbe");
    W32(DataAddress(322), 0x00000020);  // XboxHardwareInfo.Flags: retail
    W8(DataAddress(322) + 4, 0xD2);     // GpuRevision
    W8(DataAddress(322) + 5, 0xD4);     // McpRevision
    W16(DataAddress(324), 1);           // XboxKrnlVersion 1.0.5838.1
    W16(DataAddress(324) + 2, 0);
    W16(DataAddress(324) + 4, 5838);
    W16(DataAddress(324) + 6, 1);
    W32(DataAddress(356), 1);  // HalBootSMCVideoMode
    W32(DataAddress(164), 0);  // LaunchDataPage: cold boot
    InitLaunchData();          // unless FCI_START_LEVEL or a relaunch brings launch data (k_launch.cpp)
    // IdexChannelObject: the queue of pending IRPs (CancelIo walks it) is an empty list
    uint32_t irps = DataAddress(357) + 0x28;
    W32(irps, irps);
    W32(irps + 4, irps);
}

thread_local int64_t t_kernel_ticks = 0;
int64_t KernelTicks() { return t_kernel_ticks; }

void KernelCall(Cpu& c, uint32_t ordinal) {
    Entry* e = ordinal < kMaxOrdinal ? &Entries()[ordinal] : nullptr;
    if (!e || !e->info) rt::Fatal("kernel: call of unknown ordinal %u", ordinal);
    const Export& info = *e->info;
    if (g_trace) {
        Args a{c};
        rt::Log("kernel: %s(%08X, %08X, %08X, %08X) from %08X", info.name, a[0], a[1], a[2], a[3], R32(c.esp));
    }
    // FCI_COUNT_KERNEL=1: calls per export, every 10 seconds
    static const bool count = GetEnvironmentVariableA("FCI_COUNT_KERNEL", nullptr, 0) > 0;
    if (count) {
        static std::atomic<uint64_t> calls[kMaxOrdinal];
        static std::atomic<ULONGLONG> last{GetTickCount64()};
        calls[ordinal]++;
        ULONGLONG now = GetTickCount64(), then = last;
        if (now - then > 10000 && last.compare_exchange_strong(then, now))
            for (uint32_t i = 0; i < kMaxOrdinal; i++)
                if (uint64_t n = calls[i].exchange(0); n > 1000)
                    rt::Log("kernel: %llu calls of %s", (unsigned long long)n, Entries()[i].info ? Entries()[i].info->name : "?");
    }
    uint32_t result = 0;
    if (e->handler) {
        // performance: time in kernel calls per thread (frame statistics), with FCI_KERNEL_TIME=1 (two clock reads per
        // call cost 2 % of the guest thread)
        static const bool timed = GetEnvironmentVariableA("FCI_KERNEL_TIME", nullptr, 0) > 0;
        if (timed) {
            LARGE_INTEGER a, b;
            QueryPerformanceCounter(&a);
            result = e->handler(c, Args{c});
            QueryPerformanceCounter(&b);
            t_kernel_ticks += b.QuadPart - a.QuadPart;
        } else {
            result = e->handler(c, Args{c});
        }
    } else if (!e->reported.exchange(true)) {
        rt::Log("kernel: %s (%u) is not implemented, returning 0", info.name, ordinal);
    }
    c.eax = result;
    c.esp += info.kind == S ? 4 + 4 * info.size : 4;
}

// ---------------------------------------------------------------------------------------------
// strings

std::string ReadCString(uint32_t ptr) {
    if (!ptr) return {};
    return std::string(reinterpret_cast<const char*>(rt::Host(ptr)));
}

std::string ReadAnsiString(uint32_t string_ptr) {
    if (!string_ptr) return {};
    uint16_t len = R16(string_ptr);
    uint32_t buf = R32(string_ptr + 4);
    if (!buf) return {};
    return std::string(reinterpret_cast<const char*>(rt::Host(buf)), len);
}

uint32_t TimeoutMs(uint32_t timeout_ptr) {
    if (!timeout_ptr) return INFINITE;
    int64_t t = R64s(timeout_ptr);
    if (t < 0) return uint32_t(std::min<int64_t>(-t / 10000, INFINITE - 1));
    int64_t cur = int64_t(rt::GuestFileTime());  // PC comfort features: absolute times are guest time
    return t <= cur ? 0 : uint32_t(std::min<int64_t>((t - cur) / 10000, INFINITE - 1));
}

uint32_t WaitStatus(unsigned long result, uint32_t count) {
    if (result == WAIT_TIMEOUT) return X_STATUS_TIMEOUT;
    if (result >= WAIT_ABANDONED_0 && result < WAIT_ABANDONED_0 + count) return X_STATUS_ABANDONED + (result - WAIT_ABANDONED_0);
    if (result < WAIT_OBJECT_0 + count) return X_STATUS_WAIT_0 + (result - WAIT_OBJECT_0);
    return X_STATUS_UNSUCCESSFUL;
}

// ---------------------------------------------------------------------------------------------
// handles

uint32_t NewHandle(std::shared_ptr<Object> object) {
    std::lock_guard lock(g_handles_lock);
    uint32_t h = g_next_handle;
    g_next_handle += 4;
    g_handles[h] = std::move(object);
    return h;
}

std::shared_ptr<Object> FindObject(uint32_t handle) {
    std::lock_guard lock(g_handles_lock);
    auto it = g_handles.find(handle);
    return it == g_handles.end() ? nullptr : it->second;
}

bool CloseObject(uint32_t handle) {
    std::lock_guard lock(g_handles_lock);
    return g_handles.erase(handle) > 0;
}

KERNEL_EXPORT(187, NtClose) { return CloseObject(a[0]) ? X_STATUS_SUCCESS : X_STATUS_INVALID_HANDLE; }

KERNEL_EXPORT(197, NtDuplicateObject) {
    auto obj = FindObject(a[0]);
    if (!obj) return X_STATUS_INVALID_HANDLE;
    W32(a[1], NewHandle(obj));
    if (a[2] & 1) CloseObject(a[0]);  // DUPLICATE_CLOSE_SOURCE
    return X_STATUS_SUCCESS;
}

KERNEL_EXPORT(246, ObReferenceObjectByHandle) {
    auto obj = FindObject(a[0]);
    if (!obj) {
        // NtCurrentThread()
        if (a[0] == 0xFFFFFFFE) {
            if (auto* t = rt::CurrentThread()) {
                W32(a[2], t->kthread);
                return X_STATUS_SUCCESS;
            }
        }
        return X_STATUS_INVALID_HANDLE;
    }
    if (!obj->guest_body) obj->guest_body = mem::AllocSystem(0x40);
    W32(a[2], obj->guest_body);
    return X_STATUS_SUCCESS;
}

KERNEL_EXPORT(247, ObReferenceObjectByName) {
    rt::Log("kernel: ObReferenceObjectByName(%s)", ReadAnsiString(a[0]).c_str());
    W32(a[4], mem::AllocSystem(0x100));
    return X_STATUS_SUCCESS;
}

KERNEL_EXPORT(250, ObfDereferenceObject) { return 0; }

}  // namespace kernel

namespace rt {
void KernelCall(Cpu& FCI_NOESCAPE c, uint32_t ordinal) { kernel::KernelCall(c, ordinal); }
}  // namespace rt
