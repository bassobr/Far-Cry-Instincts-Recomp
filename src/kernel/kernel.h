// xboxkrnl.exe on Windows: the exports default.xbe imports, by ordinal.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "runtime/guest.h"
#include "runtime/log.h"

namespace xbe {
class Image;
}

namespace kernel {

using rt::Cpu;
using rt::R16;
using rt::R32;
using rt::R8;
using rt::W16;
using rt::W32;
using rt::W8;

// stdcall arguments on the guest stack, above the return address
struct Args {
    Cpu& c;
    uint32_t operator[](int i) const { return R32(c.esp + 4 + 4 * i); }
};

using Handler = uint32_t (*)(Cpu&, const Args&);

struct Registrar {
    Registrar(uint32_t ordinal, Handler handler);
};

// defines the handler of an export; the return value goes to eax (64-bit results also set c.edx)
#define KERNEL_EXPORT(ordinal, name)                                                   \
    static uint32_t K_##name(rt::Cpu& c, const kernel::Args& a);                      \
    static kernel::Registrar g_reg_##name(ordinal, K_##name);                          \
    static uint32_t K_##name([[maybe_unused]] rt::Cpu& c, [[maybe_unused]] const kernel::Args& a)

enum : uint32_t {
    X_STATUS_SUCCESS = 0x00000000,
    X_STATUS_WAIT_0 = 0x00000000,
    X_STATUS_ABANDONED = 0x00000080,
    X_STATUS_USER_APC = 0x000000C0,
    X_STATUS_ALERTED = 0x00000101,
    X_STATUS_TIMEOUT = 0x00000102,
    X_STATUS_PENDING = 0x00000103,
    X_STATUS_NO_MORE_FILES = 0x80000006,
    X_STATUS_UNSUCCESSFUL = 0xC0000001,
    X_STATUS_NOT_IMPLEMENTED = 0xC0000002,
    X_STATUS_INVALID_HANDLE = 0xC0000008,
    X_STATUS_INVALID_PARAMETER = 0xC000000D,
    X_STATUS_NO_SUCH_FILE = 0xC000000F,
    X_STATUS_END_OF_FILE = 0xC0000011,
    X_STATUS_NO_MEMORY = 0xC0000017,
    X_STATUS_CONFLICTING_ADDRESSES = 0xC0000018,
    X_STATUS_ACCESS_DENIED = 0xC0000022,
    X_STATUS_BUFFER_TOO_SMALL = 0xC0000023,
    X_STATUS_OBJECT_TYPE_MISMATCH = 0xC0000024,
    X_STATUS_OBJECT_NAME_INVALID = 0xC0000033,
    X_STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034,
    X_STATUS_OBJECT_NAME_COLLISION = 0xC0000035,
    X_STATUS_OBJECT_PATH_NOT_FOUND = 0xC000003A,
    X_STATUS_MUTANT_NOT_OWNED = 0xC0000046,
    X_STATUS_FILE_IS_A_DIRECTORY = 0xC00000BA,
    X_STATUS_NOT_A_DIRECTORY = 0xC0000103,
    X_STATUS_DISK_FULL = 0xC000007F,
};

void Init(const xbe::Image& image);
void InitTimers();
void StartTickCount();
// host folders behind \Device\CdRom0 and \Device\Harddisk0\PartitionN
void SetDevicePaths(const std::filesystem::path& dvd, const std::filesystem::path& hdd);
// a whole title file by its guest path (drive letters, symbolic links), from the host folder or the mounted disc
// image (kernel/disc.h), for host code that reads title files
bool ReadTitleFile(const std::string& guest, std::vector<uint8_t>* bytes);
uint32_t DataAddress(uint32_t ordinal);
// performance: QueryPerformanceCounter ticks the calling thread spent in kernel calls (blocking waits included),
// total since start
int64_t KernelTicks();
// launch data (k_launch.cpp): FCI_START_LEVEL / relaunch page at boot, relaunch on HalReturnToFirmware
void InitLaunchData();
bool Relaunch(uint32_t routine);
// the event a relaunched instance sets once its window shows its first frames (named after its process id)
inline std::wstring HandoverEvent(unsigned long process) { return L"Local\\FCIHandover-" + std::to_wstring(process); }
// FCI_START_LEVEL=<level>[:<mission>[:<players>]], false when unset (or this process is a relaunch)
bool StartLevelSetting(std::string* level, std::string* mission, int* players);

// guest strings
std::string ReadCString(uint32_t ptr);
std::string ReadAnsiString(uint32_t string_ptr);  // STRING / OBJECT_STRING
std::string ObjectName(uint32_t object_attributes);  // OBJECT_ATTRIBUTES.ObjectName, joined with its root

// 64-bit values in guest memory
inline int64_t R64s(uint32_t a) { return int64_t(rt::R64(a)); }
inline void W64(uint32_t a, uint64_t v) { rt::W64(a, v); }

// 100ns timeout pointer (null: infinite, negative: relative, positive: absolute) as host milliseconds
uint32_t TimeoutMs(uint32_t timeout_ptr);

// ---------------------------------------------------------------------------------------------
// handles

struct Object {
    virtual ~Object() = default;
    virtual void* Waitable() { return nullptr; }  // host HANDLE to wait on
    virtual const char* Kind() const = 0;
    uint32_t guest_body = 0;  // guest address handed out by ObReferenceObjectByHandle
};

uint32_t NewHandle(std::shared_ptr<Object> object);
std::shared_ptr<Object> FindObject(uint32_t handle);
template <typename T>
std::shared_ptr<T> Get(uint32_t handle) {
    return std::dynamic_pointer_cast<T>(FindObject(handle));
}
bool CloseObject(uint32_t handle);

// host wait object for a guest dispatcher object (KEVENT, KSEMAPHORE, KTHREAD, ...) by address
void* DispatcherWaitable(uint32_t object);
// status of a Win32 wait result for `count` objects
uint32_t WaitStatus(unsigned long result, uint32_t count);

// runs a DPC routine on the DPC context
void QueueDpc(uint32_t dpc, uint32_t arg1, uint32_t arg2);

// the KINTERRUPT connected to a vector (bus interrupt level), with its service routine and context
bool GetInterrupt(uint32_t vector, uint32_t* object, uint32_t* routine, uint32_t* context);

}  // namespace kernel
