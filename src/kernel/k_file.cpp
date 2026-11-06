// Files, directories, volumes and symbolic links. Xbox object paths resolve through the symbolic link
// table to a device, and each device is a folder on the host. The DVD (\Device\CdRom0, D:) is a disc image
// instead when one is mounted (kernel/disc.h): its files are read from the image, nothing on it is written.
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>

#include "kernel/disc.h"
#include "kernel/kernel.h"
#include "runtime/hitch.h"  // performance: FCI_TRACE_HITCHES
#include "runtime/memory.h"
#include "runtime/scheduler.h"
#include "runtime/thread.h"

namespace kernel {

namespace {

namespace fs = std::filesystem;

std::string Lower(std::string s) {
    for (auto& ch : s) ch = char(std::tolower(uint8_t(ch)));
    return s;
}

struct Device {
    std::string name;  // lower case, e.g. \device\cdrom0
    fs::path host;
    bool read_only;
};

const bool g_trace_reads = GetEnvironmentVariableA("FCI_TRACE_FILES", nullptr, 0) > 0;

// FCI_READ_STATS=1: every 10 s the file reads of that time (DVD folder or image, hard disk): count, bytes, time
// spent inside the reads, slowest read; to compare the disc image with extracted files
const bool g_read_stats = GetEnvironmentVariableA("FCI_READ_STATS", nullptr, 0) > 0;
struct ReadTimer {
    LARGE_INTEGER start{};
    ReadTimer() {
        if (g_read_stats || rt::hitch::Enabled()) QueryPerformanceCounter(&start);
    }
    void Done(uint32_t bytes) const {
        if (rt::hitch::Enabled()) rt::hitch::Note(rt::hitch::kFileRead, rt::hitch::Now() - start.QuadPart, bytes);
        if (!g_read_stats) return;
        static std::mutex lock;
        static LARGE_INTEGER freq{}, since{};
        static uint64_t reads = 0, total = 0, busy = 0, slowest = 0;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        std::lock_guard guard(lock);
        if (!freq.QuadPart) QueryPerformanceFrequency(&freq), since = start;
        uint64_t took = uint64_t(now.QuadPart - start.QuadPart);
        reads++, total += bytes, busy += took, slowest = std::max(slowest, took);
        if (now.QuadPart - since.QuadPart >= 10 * freq.QuadPart) {
            double ms = 1000.0 / double(freq.QuadPart);
            rt::Log("file: %llu reads, %.1f MB in %.1f ms (%.0f MB/s), slowest %.2f ms", (unsigned long long)reads,
                    total / 1048576.0, busy * ms, busy ? total / 1048576.0 / (busy * ms / 1000.0) : 0.0, slowest * ms);
            since = now, reads = total = busy = slowest = 0;
        }
    }
};

std::mutex g_paths_lock;
std::vector<Device> g_devices;
std::map<std::string, std::string> g_links;  // lower case link name -> target

// FATX superblocks written by formatting (XMountUtilityDrive), by partition number
std::mutex g_superblock_lock;
std::map<int, std::vector<uint8_t>> g_superblocks;

int PartitionNumber(const std::string& guest) {
    std::string lower = Lower(guest);
    size_t p = lower.find("partition");
    return p == std::string::npos ? -1 : std::atoi(lower.c_str() + p + 9);
}

uint32_t SectorsPerCluster(const std::string& guest) {
    std::lock_guard lock(g_superblock_lock);
    auto it = g_superblocks.find(PartitionNumber(guest));
    if (it == g_superblocks.end() || it->second.size() < 12) return 32;
    uint32_t v;
    std::memcpy(&v, it->second.data() + 8, 4);
    return v ? v : 32;
}

struct FileObject : Object {
    HANDLE h = INVALID_HANDLE_VALUE;
    std::string guest;
    fs::path host;
    bool dir = false;
    bool read_only = false;
    // on the mounted disc image instead of a host file: the file or folder, the position, a folder's listing
    const Disc::Entry* entry = nullptr;
    uint64_t pos = 0;
    std::vector<const Disc::Entry*> listing;
    size_t next = 0;
    bool listed = false;
    // a device opened as a whole (\Device\Harddisk0\Partition0): sector reads and writes, no host file
    bool raw = false;
    bool fatx = false;  // raw partition that reads back an empty FATX volume
    uint64_t raw_pos = 0;
    HANDLE find = INVALID_HANDLE_VALUE;
    ~FileObject() override {
        if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
        if (find != INVALID_HANDLE_VALUE) FindClose(find);
    }
    const char* Kind() const override { return "file"; }
};

struct LinkObject : Object {
    std::string target;
    const char* Kind() const override { return "symlink"; }
};

// resolves symbolic links (longest prefix first) until a device path remains
std::string ResolveLinks(std::string path) {
    std::lock_guard lock(g_paths_lock);
    for (int depth = 0; depth < 8; depth++) {
        std::string lower = Lower(path);
        bool changed = false;
        for (auto it = g_links.rbegin(); it != g_links.rend(); ++it) {
            const std::string& link = it->first;
            if (lower.compare(0, link.size(), link) == 0 && (lower.size() == link.size() || lower[link.size()] == '\\')) {
                path = it->second + path.substr(link.size());
                changed = true;
                break;
            }
        }
        if (!changed) break;
    }
    return path;
}

// where a guest path lands: a host file under a device folder, or (the DVD with a mounted image) a disc entry
struct Where {
    fs::path host;
    std::string rest;  // the path below the device
    bool read_only = false;
    bool device_root = false;
    bool disc = false;
    const Disc::Entry* entry = nullptr;  // on the disc: the file or folder, null when missing
};

bool Resolve(const std::string& guest, Where* w) {
    std::string path = guest;
    // drive letter paths without the \??\ prefix
    if (path.size() >= 2 && path[1] == ':') path = "\\??\\" + path;
    path = ResolveLinks(path);
    std::string lower = Lower(path);
    std::lock_guard lock(g_paths_lock);
    for (const auto& d : g_devices) {
        if (lower.compare(0, d.name.size(), d.name) == 0 && (lower.size() == d.name.size() || lower[d.name.size()] == '\\')) {
            std::string rest = path.substr(d.name.size());
            while (!rest.empty() && rest.front() == '\\') rest.erase(rest.begin());
            w->rest = rest;
            w->host = rest.empty() ? d.host : d.host / fs::path(rest);
            w->read_only = d.read_only;
            // "\Device\Harddisk0\Partition1\" with the separator names the root directory, without it the volume
            w->device_root = lower.size() == d.name.size();
            if (d.name == "\\device\\cdrom0")
                if (const Disc* disc = MountedDisc()) {
                    w->disc = true;
                    w->read_only = true;
                    w->entry = disc->Find(rest);
                }
            return true;
        }
    }
    return false;
}

bool ToHost(const std::string& guest, fs::path* host, bool* read_only) {
    Where w;
    if (!Resolve(guest, &w)) return false;
    *host = w.host;
    *read_only = w.read_only;
    return true;
}

void SetIoStatus(uint32_t iosb, uint32_t status, uint32_t info) {
    if (!iosb) return;
    W32(iosb, status);
    W32(iosb + 4, info);
}

void WriteFileTime(uint32_t at, const FILETIME& ft) { W64(at, (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime); }

uint32_t Attributes(DWORD host_attributes) {
    return host_attributes & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM |
                              FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_NORMAL);
}

// a disc entry as the host's attribute data: the volume's time for every time, the disc's attributes
WIN32_FILE_ATTRIBUTE_DATA DiscAttributes(const Disc::Entry& e) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    uint64_t t = MountedDisc()->Time();
    d.ftCreationTime = d.ftLastAccessTime = d.ftLastWriteTime = {DWORD(t), DWORD(t >> 32)};
    d.dwFileAttributes = e.dir ? FILE_ATTRIBUTE_DIRECTORY | (e.attributes & ~FILE_ATTRIBUTE_NORMAL)
                               : e.attributes ? e.attributes : FILE_ATTRIBUTE_NORMAL;
    if (!e.dir) d.nFileSizeLow = e.size;
    return d;
}

// FILE_NETWORK_OPEN_INFORMATION
void WriteNetworkOpen(uint32_t out, const WIN32_FILE_ATTRIBUTE_DATA& d) {
    WriteFileTime(out + 0, d.ftCreationTime);
    WriteFileTime(out + 8, d.ftLastAccessTime);
    WriteFileTime(out + 16, d.ftLastWriteTime);
    WriteFileTime(out + 24, d.ftLastWriteTime);
    uint64_t size = (uint64_t(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
    W64(out + 32, (size + 0x3FFF) & ~0x3FFFull);
    W64(out + 40, size);
    W32(out + 48, Attributes(d.dwFileAttributes));
    W32(out + 52, 0);
}

enum : uint32_t {
    kFileSupersede = 0,
    kFileOpen = 1,
    kFileCreate = 2,
    kFileOpenIf = 3,
    kFileOverwrite = 4,
    kFileOverwriteIf = 5,
    kFileDirectoryFile = 0x1,
    kFileNonDirectoryFile = 0x40,
};

constexpr uint32_t kWriteAccess = 0x40000000 | 0x10000000 | 0x2 | 0x4 | 0x10000;  // GENERIC_WRITE/ALL, write/append data, DELETE

// a file or folder on the mounted disc image: read only like the console's DVD file system, which refuses write
// access and every disposition that would create or replace a file (STATUS_ACCESS_DENIED)
uint32_t OpenOnDisc(const std::string& guest, const Where& w, uint32_t handle_out, uint32_t access, uint32_t iosb,
                    uint32_t disposition, uint32_t options) {
    const Disc::Entry* e = w.entry;
    rt::Log("file: open %s (%s on the disc image, disposition %u, options %X)", guest.c_str(),
            !e ? "missing" : e->dir ? "directory" : "file", disposition, options);
    auto fail = [&](uint32_t status) {
        SetIoStatus(iosb, status, 0);
        return status;
    };
    if ((access & kWriteAccess) || (disposition != kFileOpen && disposition != kFileOpenIf)) {
        if (!e && disposition == kFileOverwrite) return fail(X_STATUS_OBJECT_NAME_NOT_FOUND);
        rt::Log("file: %s refused, the disc image is read only (access %08X, disposition %u)", guest.c_str(), access,
                disposition);
        return fail(X_STATUS_ACCESS_DENIED);
    }
    if (!e) {
        if (disposition == kFileOpenIf) return fail(X_STATUS_ACCESS_DENIED);
        // a missing folder on the way is a missing path
        size_t slash = w.rest.find_last_of('\\');
        const Disc::Entry* parent =
            MountedDisc()->Find(slash == std::string::npos ? std::string_view() : std::string_view(w.rest).substr(0, slash));
        return fail(parent && parent->dir ? X_STATUS_OBJECT_NAME_NOT_FOUND : X_STATUS_OBJECT_PATH_NOT_FOUND);
    }
    if (e->dir && (options & kFileNonDirectoryFile)) return fail(X_STATUS_FILE_IS_A_DIRECTORY);
    if (!e->dir && (options & kFileDirectoryFile)) return fail(X_STATUS_NOT_A_DIRECTORY);
    auto file = std::make_shared<FileObject>();
    file->guest = guest;
    file->host = w.host;
    file->read_only = true;
    file->entry = e;
    file->dir = e->dir;
    W32(handle_out, NewHandle(file));
    SetIoStatus(iosb, X_STATUS_SUCCESS, 1);  // FILE_OPENED
    return X_STATUS_SUCCESS;
}

uint32_t OpenFile(uint32_t handle_out, uint32_t access, uint32_t attributes, uint32_t iosb, uint32_t disposition,
                  uint32_t options) {
    std::string guest = ObjectName(attributes);
    Where w;
    if (!Resolve(guest, &w)) {
        rt::Log("file: no device for %s", guest.c_str());
        SetIoStatus(iosb, X_STATUS_OBJECT_PATH_NOT_FOUND, 0);
        return X_STATUS_OBJECT_PATH_NOT_FOUND;
    }
    fs::path host = w.host;
    bool read_only = w.read_only, device_root = w.device_root;
    // FCI_TEST_VIDEO=<file.bik> (diagnostics, fmv_scale): the first boot video plays this one from Videos instead
    static char test_video[MAX_PATH] = {};
    static const bool test = GetEnvironmentVariableA("FCI_TEST_VIDEO", test_video, sizeof test_video) > 0;
    if (test && Lower(host.filename().string()) == "ubisoft.bik") {
        host = host.parent_path() / test_video;
        if (w.disc) {
            size_t slash = w.rest.find_last_of('\\');
            w.rest = (slash == std::string::npos ? std::string() : w.rest.substr(0, slash + 1)) + test_video;
            w.host = host;
            w.entry = MountedDisc()->Find(w.rest);
        }
        rt::Log("file: %s plays %s (FCI_TEST_VIDEO)", guest.c_str(), test_video);
    }
    // the volume itself (\Device\CdRom0 without a separator) stays a raw device, as with the folder
    if (w.disc && !(device_root && !(options & kFileDirectoryFile)))
        return OpenOnDisc(guest, w, handle_out, access, iosb, disposition, options);
    if (device_root && !(options & kFileDirectoryFile)) {
        auto vol = std::make_shared<FileObject>();
        vol->guest = guest;
        vol->host = host;
        vol->read_only = read_only;
        vol->raw = true;
        // partition 0 is the raw disk with the config area, the others hold FATX volumes
        vol->fatx = !read_only && Lower(guest).find("partition0") == std::string::npos;
        rt::Log("file: open volume %s", guest.c_str());
        W32(handle_out, NewHandle(vol));
        SetIoStatus(iosb, X_STATUS_SUCCESS, 1);
        return X_STATUS_SUCCESS;
    }

    std::error_code ec;
    bool exists = fs::exists(host, ec);
    bool is_dir = exists && fs::is_directory(host, ec);
    rt::Log("file: open %s (%s, disposition %u, options %X)", guest.c_str(),
            !exists ? "missing" : is_dir ? "directory" : "file", disposition, options);
    bool want_write = access & kWriteAccess;
    uint32_t info = 1;  // FILE_OPENED

    auto file = std::make_shared<FileObject>();
    file->guest = guest;
    file->host = host;
    file->read_only = read_only;

    if ((options & kFileDirectoryFile) || is_dir) {
        if (is_dir && (options & kFileNonDirectoryFile)) {
            SetIoStatus(iosb, X_STATUS_FILE_IS_A_DIRECTORY, 0);
            return X_STATUS_FILE_IS_A_DIRECTORY;
        }
        if (!exists) {
            if (disposition == kFileOpen || disposition == kFileOverwrite || read_only) {
                SetIoStatus(iosb, X_STATUS_OBJECT_NAME_NOT_FOUND, 0);
                return X_STATUS_OBJECT_NAME_NOT_FOUND;
            }
            if (!fs::exists(host.parent_path(), ec)) {
                SetIoStatus(iosb, X_STATUS_OBJECT_PATH_NOT_FOUND, 0);
                return X_STATUS_OBJECT_PATH_NOT_FOUND;
            }
            fs::create_directory(host, ec);
            info = 2;  // FILE_CREATED
        } else if (disposition == kFileCreate) {
            SetIoStatus(iosb, X_STATUS_OBJECT_NAME_COLLISION, 0);
            return X_STATUS_OBJECT_NAME_COLLISION;
        }
        file->dir = true;
        file->h = CreateFileW(host.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    } else {
        if (read_only && (want_write || disposition != kFileOpen)) {
            if (!exists) {
                SetIoStatus(iosb, X_STATUS_OBJECT_NAME_NOT_FOUND, 0);
                return X_STATUS_OBJECT_NAME_NOT_FOUND;
            }
            want_write = false;
        }
        DWORD create;
        switch (disposition) {
        case kFileSupersede:
        case kFileOverwriteIf: create = CREATE_ALWAYS; info = exists ? 3 : 2; break;
        case kFileCreate: create = CREATE_NEW; info = 2; break;
        case kFileOpenIf: create = OPEN_ALWAYS; info = exists ? 1 : 2; break;
        case kFileOverwrite: create = TRUNCATE_EXISTING; info = 3; break;
        default: create = OPEN_EXISTING; break;
        }
        if (create != OPEN_EXISTING) want_write = true;
        // DELETE: XAPI's DeleteFile opens with it and then sets FileDispositionInformation, which Windows refuses
        // on a handle without that right (the file stayed, e.g. FCI's checkpoint slot fcs0.fcs, and the profile
        // with one checkpoint file too many counted as damaged)
        bool want_delete = (access & 0x10000) && !read_only;
        file->h = CreateFileW(host.c_str(), GENERIC_READ | (want_write ? GENERIC_WRITE : 0) | (want_delete ? DELETE : 0),
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, create,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file->h == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            uint32_t status = err == ERROR_FILE_EXISTS ? X_STATUS_OBJECT_NAME_COLLISION
                            : err == ERROR_PATH_NOT_FOUND ? X_STATUS_OBJECT_PATH_NOT_FOUND
                            : err == ERROR_ACCESS_DENIED ? X_STATUS_ACCESS_DENIED
                                                         : X_STATUS_OBJECT_NAME_NOT_FOUND;
            SetIoStatus(iosb, status, 0);
            return status;
        }
    }
    W32(handle_out, NewHandle(file));
    SetIoStatus(iosb, X_STATUS_SUCCESS, info);
    return X_STATUS_SUCCESS;
}

void Complete(Cpu& c, uint32_t event, uint32_t apc, uint32_t apc_context, uint32_t iosb) {
    if (event)
        if (auto e = FindObject(event); e && e->Waitable()) SetEvent(e->Waitable());
    if (apc) rt::GuestCall(c, apc, {apc_context, iosb, 0});
}

LONGLONG FileSize(HANDLE h) {
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    return size.QuadPart;
}

}  // namespace

void SetDevicePaths(const std::filesystem::path& dvd, const std::filesystem::path& hdd) {
    std::lock_guard lock(g_paths_lock);
    g_devices.clear();
    g_devices.push_back({"\\device\\cdrom0", dvd, true});
    const char* partitions[] = {"partition0", "partition1", "partition2", "partition3", "partition4", "partition5",
                                "partition6", "partition7"};
    for (const char* p : partitions) {
        fs::path dir = hdd / p;
        std::error_code ec;
        fs::create_directories(dir, ec);
        g_devices.push_back({std::string("\\device\\harddisk0\\") + p, dir, false});
    }
    // the kernel maps the boot device before the title starts
    g_links["\\??\\d:"] = "\\Device\\CdRom0";
}

bool ReadTitleFile(const std::string& guest, std::vector<uint8_t>* bytes) {
    Where w;
    if (!Resolve(guest, &w)) return false;
    if (w.disc) return w.entry && MountedDisc()->Load(w.rest, *bytes);
    std::ifstream in(w.host, std::ios::binary);
    if (!in) return false;
    bytes->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

std::string ObjectName(uint32_t object_attributes) {
    uint32_t root = R32(object_attributes);
    std::string name = ReadAnsiString(R32(object_attributes + 4));
    if (root) {
        if (auto f = Get<FileObject>(root)) return f->guest + (name.empty() ? "" : "\\" + name);
    }
    return name;
}

KERNEL_EXPORT(190, NtCreateFile) { return OpenFile(a[0], a[1], a[2], a[3], a[7], a[8]); }
KERNEL_EXPORT(202, NtOpenFile) { return OpenFile(a[0], a[1], a[2], a[3], kFileOpen, a[5]); }

// FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, Buffer, Length, ByteOffset*
KERNEL_EXPORT(219, NtReadFile) {
    auto f = Get<FileObject>(a[0]);
    if (!f || f->dir) return X_STATUS_INVALID_HANDLE;
    if (f->raw) {
        if (uint32_t off = a[7]; off && R32(off + 4) != 0xFFFFFFFF) f->raw_pos = rt::R64(off);
        std::memset(rt::Host(a[5]), 0, a[6]);
        std::vector<uint8_t> written;
        {
            std::lock_guard lock(g_superblock_lock);
            auto it = g_superblocks.find(PartitionNumber(f->guest));
            if (it != g_superblocks.end()) written = it->second;
        }
        if (f->raw_pos == 0 && !written.empty()) {
            std::memcpy(rt::Host(a[5]), written.data(), std::min<size_t>(written.size(), a[6]));
        } else if (f->fatx && f->raw_pos == 0 && a[6] >= 16) {
            // FATX superblock: signature, volume id, sectors per cluster, root directory cluster
            std::memcpy(rt::Host(a[5]), "FATX", 4);
            W32(a[5] + 4, 0x46434952);
            W32(a[5] + 8, 32);
            W32(a[5] + 12, 1);
        }
        rt::Log("file: raw read of %X bytes at %llX from %s", a[6], (unsigned long long)f->raw_pos, f->guest.c_str());
        f->raw_pos += a[6];
        SetIoStatus(a[4], X_STATUS_SUCCESS, a[6]);
        Complete(c, a[1], a[2], a[3], a[4]);
        return X_STATUS_SUCCESS;
    }
    if (f->entry) {
        // the disc image: a positional read on its one handle (no seek, no lock), at the given offset or at the
        // file's own position (no offset, or FILE_USE_FILE_POINTER_POSITION)
        uint64_t pos = f->pos;
        if (uint32_t off = a[7]; off && R64s(off) >= 0) pos = rt::R64(off);
        uint32_t n = 0;
        if (a[6]) mem::Commit(a[5], a[6]);
        if (a[6] && pos < f->entry->size) {
            rt::CpuUnlock unlock;  // other threads run during disc reads, as on the console
            ReadTimer timer;
            n = MountedDisc()->Read(*f->entry, pos, rt::Host(a[5]), a[6]);
            timer.Done(n);
        }
        f->pos = pos + n;
        uint32_t status = !a[6]                   ? X_STATUS_SUCCESS
                          : pos >= f->entry->size ? X_STATUS_END_OF_FILE
                          : n == 0                ? X_STATUS_UNSUCCESSFUL
                                                  : X_STATUS_SUCCESS;
        if (status == X_STATUS_UNSUCCESSFUL)
            rt::Log("file: read of %s at %llX from the disc image failed (%lu)", f->guest.c_str(), (unsigned long long)pos,
                    GetLastError());
        if (g_trace_reads)
            rt::Log("file: read %s %X at %llX -> %08X: %X bytes, event %X apc %08X", f->guest.c_str(), a[6],
                    (unsigned long long)pos, a[5], n, a[1], a[2]);
        SetIoStatus(a[4], status, n);
        Complete(c, a[1], a[2], a[3], a[4]);
        return status;
    }
    if (uint32_t off = a[7]; off && !(R32(off) == 0xFFFFFFFE && R32(off + 4) == 0xFFFFFFFF)) {
        LARGE_INTEGER pos;
        pos.QuadPart = R64s(off);
        SetFilePointerEx(f->h, pos, nullptr, FILE_BEGIN);
    }
    DWORD n = 0;
    if (a[6]) mem::Commit(a[5], a[6]);
    BOOL ok;
    {
        rt::CpuUnlock unlock;  // other threads run during disc reads, as on the console
        ReadTimer timer;
        ok = ReadFile(f->h, rt::Host(a[5]), a[6], &n, nullptr);
        timer.Done(n);
    }
    uint32_t status = !ok ? X_STATUS_UNSUCCESSFUL : (n == 0 && a[6]) ? X_STATUS_END_OF_FILE : X_STATUS_SUCCESS;
    if (g_trace_reads)
        rt::Log("file: read %s %X at %llX -> %08X: %X bytes, event %X apc %08X", f->guest.c_str(), a[6],
                a[7] ? (unsigned long long)rt::R64(a[7]) : ~0ull, a[5], n, a[1], a[2]);
    SetIoStatus(a[4], status, n);
    Complete(c, a[1], a[2], a[3], a[4]);
    return status;
}

KERNEL_EXPORT(236, NtWriteFile) {
    auto f = Get<FileObject>(a[0]);
    if (!f || f->dir) return X_STATUS_INVALID_HANDLE;
    if (f->read_only) return X_STATUS_ACCESS_DENIED;
    if (f->raw) {
        if (uint32_t off = a[7]; off && R32(off + 4) != 0xFFFFFFFF) f->raw_pos = rt::R64(off);
        if (f->raw_pos == 0 && a[6] >= 4 && std::memcmp(rt::Host(a[5]), "FATX", 4) == 0) {
            {
                std::lock_guard lock(g_superblock_lock);
                g_superblocks[PartitionNumber(f->guest)].assign(rt::Host(a[5]), rt::Host(a[5]) + std::min<uint32_t>(a[6], 0x1000));
            }
            std::error_code ec;
            for (const auto& entry : fs::directory_iterator(f->host, ec)) fs::remove_all(entry.path(), ec);
            rt::Log("file: %s formatted as FATX, %u sectors per cluster", f->guest.c_str(), SectorsPerCluster(f->guest));
        }
        f->raw_pos += a[6];
        SetIoStatus(a[4], X_STATUS_SUCCESS, a[6]);
        Complete(c, a[1], a[2], a[3], a[4]);
        return X_STATUS_SUCCESS;
    }
    if (uint32_t off = a[7]; off && !(R32(off) == 0xFFFFFFFE && R32(off + 4) == 0xFFFFFFFF)) {
        LARGE_INTEGER pos;
        pos.QuadPart = R64s(off);
        SetFilePointerEx(f->h, pos, nullptr, FILE_BEGIN);
    }
    DWORD n = 0;
    BOOL ok = WriteFile(f->h, rt::Host(a[5]), a[6], &n, nullptr);
    uint32_t status = ok ? X_STATUS_SUCCESS : X_STATUS_DISK_FULL;
    SetIoStatus(a[4], status, n);
    Complete(c, a[1], a[2], a[3], a[4]);
    return status;
}

// FileHandle, IoStatusBlock, FileInformation, Length, FileInformationClass
KERNEL_EXPORT(211, NtQueryInformationFile) {
    auto f = Get<FileObject>(a[0]);
    if (!f) return X_STATUS_INVALID_HANDLE;
    uint32_t out = a[2];
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (f->entry) d = DiscAttributes(*f->entry);
    else GetFileAttributesExW(f->host.c_str(), GetFileExInfoStandard, &d);
    uint64_t size = f->dir ? 0 : f->entry ? f->entry->size : uint64_t(FileSize(f->h));
    uint32_t written = 0;
    switch (a[4]) {
    case 4:  // FileBasicInformation
        WriteFileTime(out + 0, d.ftCreationTime);
        WriteFileTime(out + 8, d.ftLastAccessTime);
        WriteFileTime(out + 16, d.ftLastWriteTime);
        WriteFileTime(out + 24, d.ftLastWriteTime);
        W32(out + 32, Attributes(d.dwFileAttributes));
        written = 36;
        break;
    case 5:  // FileStandardInformation
        W64(out + 0, (size + 0x3FFF) & ~0x3FFFull);
        W64(out + 8, size);
        W32(out + 16, 1);
        W8(out + 20, 0);
        W8(out + 21, f->dir);
        written = 24;
        break;
    case 14: {  // FilePositionInformation
        LARGE_INTEGER zero{}, pos{};
        if (f->entry) pos.QuadPart = LONGLONG(f->pos);
        else SetFilePointerEx(f->h, zero, &pos, FILE_CURRENT);
        W64(out, pos.QuadPart);
        written = 8;
        break;
    }
    case 34:  // FileNetworkOpenInformation
        WriteNetworkOpen(out, d);
        written = 56;
        break;
    default:
        rt::Log("file: NtQueryInformationFile class %u not implemented", a[4]);
        SetIoStatus(a[1], X_STATUS_INVALID_PARAMETER, 0);
        return X_STATUS_INVALID_PARAMETER;
    }
    SetIoStatus(a[1], X_STATUS_SUCCESS, written);
    return X_STATUS_SUCCESS;
}

KERNEL_EXPORT(226, NtSetInformationFile) {
    auto f = Get<FileObject>(a[0]);
    if (!f) return X_STATUS_INVALID_HANDLE;
    uint32_t in = a[2];
    if (f->entry) {
        // the disc image: the position moves, the basic information is taken and ignored, nothing else changes
        if (a[4] == 14) {
            f->pos = uint64_t(R64s(in));
        } else if (a[4] != 4) {
            rt::Log("file: NtSetInformationFile class %u on %s refused, the disc image is read only", a[4], f->guest.c_str());
            SetIoStatus(a[1], X_STATUS_ACCESS_DENIED, 0);
            return X_STATUS_ACCESS_DENIED;
        }
        SetIoStatus(a[1], X_STATUS_SUCCESS, 0);
        return X_STATUS_SUCCESS;
    }
    switch (a[4]) {
    case 14: {  // FilePositionInformation
        LARGE_INTEGER pos;
        pos.QuadPart = R64s(in);
        SetFilePointerEx(f->h, pos, nullptr, FILE_BEGIN);
        break;
    }
    case 19:  // FileAllocationInformation
    case 4:   // FileBasicInformation
        break;
    case 20: {  // FileEndOfFileInformation
        FILE_END_OF_FILE_INFO eof;
        eof.EndOfFile.QuadPart = R64s(in);
        SetFileInformationByHandle(f->h, FileEndOfFileInfo, &eof, sizeof eof);
        break;
    }
    case 13: {  // FileDispositionInformation
        FILE_DISPOSITION_INFO disp{R8(in) != 0};
        if (f->dir) {
            ::CloseHandle(f->h);
            f->h = INVALID_HANDLE_VALUE;
            std::error_code ec;
            fs::remove(f->host, ec);
        } else if (!SetFileInformationByHandle(f->h, FileDispositionInfo, &disp, sizeof disp)) {
            rt::Log("file: delete of %s failed (%lu)", f->guest.c_str(), GetLastError());
            return X_STATUS_ACCESS_DENIED;
        }
        break;
    }
    case 10: {  // FileRenameInformation: ReplaceIfExists, RootDirectory, FileName (OBJECT_STRING)
        std::string target = ReadAnsiString(in + 8);
        fs::path host;
        bool ro;
        if (!ToHost(target, &host, &ro)) return X_STATUS_OBJECT_PATH_NOT_FOUND;
        if (ro) return X_STATUS_ACCESS_DENIED;  // onto the DVD
        ::CloseHandle(f->h);
        BOOL ok = MoveFileExW(f->host.c_str(), host.c_str(), R8(in) ? MOVEFILE_REPLACE_EXISTING : 0);
        f->host = host;
        f->guest = target;
        f->h = CreateFileW(host.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, f->dir ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL, nullptr);
        if (!ok) return X_STATUS_OBJECT_NAME_COLLISION;
        break;
    }
    default:
        rt::Log("file: NtSetInformationFile class %u not implemented", a[4]);
        return X_STATUS_INVALID_PARAMETER;
    }
    SetIoStatus(a[1], X_STATUS_SUCCESS, 0);
    return X_STATUS_SUCCESS;
}

KERNEL_EXPORT(210, NtQueryFullAttributesFile) {
    std::string guest = ObjectName(a[0]);
    Where w;
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!Resolve(guest, &w)) return X_STATUS_OBJECT_NAME_NOT_FOUND;
    if (w.disc) {
        if (!w.entry) return X_STATUS_OBJECT_NAME_NOT_FOUND;
        d = DiscAttributes(*w.entry);
    } else if (!GetFileAttributesExW(w.host.c_str(), GetFileExInfoStandard, &d)) {
        return X_STATUS_OBJECT_NAME_NOT_FOUND;
    }
    WriteNetworkOpen(a[1], d);
    return X_STATUS_SUCCESS;
}

// FileHandle, IoStatusBlock, FsInformation, Length, FsInformationClass
KERNEL_EXPORT(218, NtQueryVolumeInformationFile) {
    auto f = Get<FileObject>(a[0]);
    if (!f) return X_STATUS_INVALID_HANDLE;
    uint32_t out = a[2];
    constexpr uint64_t kTotalUnits = 0x10000, kFreeUnits = 0x8000;  // 16 KB clusters: 1 GB, 512 MB free
    uint64_t free_units = f->read_only ? 0 : kFreeUnits;
    uint32_t written = 0;
    switch (a[4]) {
    case 1:  // FileFsVolumeInformation
        for (int i = 0; i < 0x18; i += 4) W32(out + i, 0);
        written = 0x18;
        break;
    case 3:  // FileFsSizeInformation
        W64(out + 0, kTotalUnits);
        W64(out + 8, free_units);
        W32(out + 16, SectorsPerCluster(f->guest));
        W32(out + 20, 512);
        written = 24;
        break;
    case 4:  // FileFsDeviceInformation
        W32(out + 0, f->read_only ? 0x02 : 0x07);
        W32(out + 4, 0);
        written = 8;
        break;
    case 5: {  // FileFsAttributeInformation
        W32(out + 0, 0);
        W32(out + 4, 42);
        W32(out + 8, 4);
        std::memcpy(rt::Host(out + 12), "FATX", 4);
        written = 16;
        break;
    }
    case 7:  // FileFsFullSizeInformation
        W64(out + 0, kTotalUnits);
        W64(out + 8, free_units);
        W64(out + 16, free_units);
        W32(out + 24, SectorsPerCluster(f->guest));
        W32(out + 28, 512);
        written = 32;
        break;
    default:
        rt::Log("file: NtQueryVolumeInformationFile class %u not implemented", a[4]);
        return X_STATUS_INVALID_PARAMETER;
    }
    SetIoStatus(a[1], X_STATUS_SUCCESS, written);
    return X_STATUS_SUCCESS;
}

// FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, FileInformation, Length, FileInformationClass,
// FileName, RestartScan; one FILE_DIRECTORY_INFORMATION per call
KERNEL_EXPORT(207, NtQueryDirectoryFile) {
    auto f = Get<FileObject>(a[0]);
    if (!f || !f->dir) return X_STATUS_INVALID_HANDLE;
    if (a[7] != 1) {
        rt::Log("file: NtQueryDirectoryFile class %u not implemented", a[7]);
        return X_STATUS_INVALID_PARAMETER;
    }
    if (f->entry) {
        // the disc image: the folder's entries matching the mask of the first call (or of a restart), one per call
        if (!f->listed || (a[9] & 0xFF)) {
            std::string mask = a[8] ? ReadAnsiString(a[8]) : "*";
            f->listing.clear();
            for (const Disc::Entry& e : f->entry->children)
                if (Disc::Match(mask, e.name)) f->listing.push_back(&e);
            f->next = 0;
            f->listed = true;
            if (f->listing.empty()) {
                SetIoStatus(a[4], X_STATUS_NO_SUCH_FILE, 0);
                return X_STATUS_NO_SUCH_FILE;
            }
        }
        if (f->next >= f->listing.size()) {
            SetIoStatus(a[4], X_STATUS_NO_MORE_FILES, 0);
            return X_STATUS_NO_MORE_FILES;
        }
        const Disc::Entry& e = *f->listing[f->next];
        uint32_t out = a[5];
        if (0x40 + e.name.size() > a[6]) return X_STATUS_BUFFER_TOO_SMALL;
        f->next++;
        WIN32_FILE_ATTRIBUTE_DATA d = DiscAttributes(e);
        uint64_t size = e.dir ? 0 : e.size;
        W32(out + 0x00, 0);
        W32(out + 0x04, 0);
        WriteFileTime(out + 0x08, d.ftCreationTime);
        WriteFileTime(out + 0x10, d.ftLastAccessTime);
        WriteFileTime(out + 0x18, d.ftLastWriteTime);
        WriteFileTime(out + 0x20, d.ftLastWriteTime);
        W64(out + 0x28, size);
        W64(out + 0x30, (size + 0x3FFF) & ~0x3FFFull);
        W32(out + 0x38, Attributes(d.dwFileAttributes));
        W32(out + 0x3C, uint32_t(e.name.size()));
        std::memcpy(rt::Host(out + 0x40), e.name.data(), e.name.size());
        SetIoStatus(a[4], X_STATUS_SUCCESS, uint32_t(0x40 + e.name.size()));
        Complete(c, a[1], a[2], a[3], a[4]);
        return X_STATUS_SUCCESS;
    }
    WIN32_FIND_DATAW fd;
    bool found;
    if (f->find == INVALID_HANDLE_VALUE || a[9]) {
        if (f->find != INVALID_HANDLE_VALUE) FindClose(f->find);
        std::string mask = a[8] ? ReadAnsiString(a[8]) : "*";
        if (mask.empty()) mask = "*";
        f->find = FindFirstFileW((f->host / mask).c_str(), &fd);
        found = f->find != INVALID_HANDLE_VALUE;
        if (!found) {
            SetIoStatus(a[4], X_STATUS_NO_SUCH_FILE, 0);
            return X_STATUS_NO_SUCH_FILE;
        }
    } else {
        found = FindNextFileW(f->find, &fd);
    }
    while (found && (!std::wcscmp(fd.cFileName, L".") || !std::wcscmp(fd.cFileName, L".."))) found = FindNextFileW(f->find, &fd);
    if (!found) {
        SetIoStatus(a[4], X_STATUS_NO_MORE_FILES, 0);
        return X_STATUS_NO_MORE_FILES;
    }
    std::string name = fs::path(fd.cFileName).string();
    uint32_t out = a[5];
    if (0x40 + name.size() > a[6]) return X_STATUS_BUFFER_TOO_SMALL;
    uint64_t size = (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
    W32(out + 0x00, 0);
    W32(out + 0x04, 0);
    WriteFileTime(out + 0x08, fd.ftCreationTime);
    WriteFileTime(out + 0x10, fd.ftLastAccessTime);
    WriteFileTime(out + 0x18, fd.ftLastWriteTime);
    WriteFileTime(out + 0x20, fd.ftLastWriteTime);
    W64(out + 0x28, size);
    W64(out + 0x30, (size + 0x3FFF) & ~0x3FFFull);
    W32(out + 0x38, Attributes(fd.dwFileAttributes));
    W32(out + 0x3C, uint32_t(name.size()));
    std::memcpy(rt::Host(out + 0x40), name.data(), name.size());
    SetIoStatus(a[4], X_STATUS_SUCCESS, uint32_t(0x40 + name.size()));
    Complete(c, a[1], a[2], a[3], a[4]);
    return X_STATUS_SUCCESS;
}

KERNEL_EXPORT(195, NtDeleteFile) {
    fs::path host;
    bool ro;
    if (!ToHost(ObjectName(a[0]), &host, &ro) || ro) return X_STATUS_OBJECT_NAME_NOT_FOUND;
    std::error_code ec;
    return fs::remove(host, ec) ? X_STATUS_SUCCESS : X_STATUS_OBJECT_NAME_NOT_FOUND;
}

KERNEL_EXPORT(198, NtFlushBuffersFile) {
    auto f = Get<FileObject>(a[0]);
    if (!f) return X_STATUS_INVALID_HANDLE;
    if (!f->dir && f->h != INVALID_HANDLE_VALUE) FlushFileBuffers(f->h);
    SetIoStatus(a[1], X_STATUS_SUCCESS, 0);
    return X_STATUS_SUCCESS;
}

namespace {

// the retail disk layout: offset and length of partitions 1 to 5
struct PartitionLayout {
    uint64_t offset, length;
};
constexpr PartitionLayout kPartitions[] = {
    {0, 0},
    {0x0ABE80000ull, 0x1312D6000ull},  // E: title and user data
    {0x08CA80000ull, 0x01F400000ull},  // C: system
    {0x000080000ull, 0x02EE00000ull},  // X: cache
    {0x02EE80000ull, 0x02EE00000ull},  // Y: cache
    {0x05DC80000ull, 0x02EE00000ull},  // Z: cache
};

}  // namespace

// FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, IoControlCode, InputBuffer, InputBufferLength,
// OutputBuffer, OutputBufferLength
KERNEL_EXPORT(196, NtDeviceIoControlFile) {
    auto f = Get<FileObject>(a[0]);
    uint32_t code = a[5], out = a[8], out_len = a[9];
    if (out && out_len) std::memset(rt::Host(out), 0, out_len);
    uint32_t info = 0;
    int part = f ? PartitionNumber(f->guest) : -1;
    if (part >= 0 && part <= 5) {
        uint64_t length = part == 0 ? 0x1DD1A00000ull : kPartitions[part].length;  // partition 0: an 8 GB disk
        if (code == 0x00070000 && out_len >= 24) {  // IOCTL_DISK_GET_DRIVE_GEOMETRY
            W64(out, length / 512);  // Cylinders
            W32(out + 8, 12);        // FixedMedia
            W32(out + 12, 1);        // TracksPerCylinder
            W32(out + 16, 1);        // SectorsPerTrack
            W32(out + 20, 512);      // BytesPerSector
            info = 24;
        } else if (code == 0x00074004 && out_len >= 32) {  // IOCTL_DISK_GET_PARTITION_INFO
            W64(out, part == 0 ? 0 : kPartitions[part].offset);
            W64(out + 8, length);
            W32(out + 16, 0);
            W32(out + 20, uint32_t(part));
            W8(out + 24, 0);  // PartitionType
            W8(out + 26, 1);  // RecognizedPartition
            info = 32;
        } else {
            rt::Log("file: NtDeviceIoControlFile(%s) code %08X not implemented", f->guest.c_str(), code);
        }
    } else {
        rt::Log("file: NtDeviceIoControlFile(%08X) code %08X not implemented", a[0], code);
    }
    SetIoStatus(a[4], X_STATUS_SUCCESS, info);
    Complete(c, a[1], a[2], a[3], a[4]);
    return X_STATUS_SUCCESS;
}

KERNEL_EXPORT(200, NtFsControlFile) {
    rt::Log("file: NtFsControlFile(%08X) code %08X", a[0], a[5]);
    SetIoStatus(a[4], X_STATUS_SUCCESS, 0);
    Complete(c, a[1], a[2], a[3], a[4]);
    return X_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------------------------
// symbolic links

KERNEL_EXPORT(67, IoCreateSymbolicLink) {
    std::string link = ReadAnsiString(a[0]), target = ReadAnsiString(a[1]);
    rt::Log("file: link %s -> %s", link.c_str(), target.c_str());
    std::lock_guard lock(g_paths_lock);
    g_links[Lower(link)] = target;
    return X_STATUS_SUCCESS;
}

KERNEL_EXPORT(69, IoDeleteSymbolicLink) {
    std::lock_guard lock(g_paths_lock);
    return g_links.erase(Lower(ReadAnsiString(a[0]))) ? X_STATUS_SUCCESS : X_STATUS_OBJECT_NAME_NOT_FOUND;
}

KERNEL_EXPORT(203, NtOpenSymbolicLinkObject) {
    std::string name = Lower(ObjectName(a[1]));
    std::lock_guard lock(g_paths_lock);
    auto it = g_links.find(name);
    if (it == g_links.end()) return X_STATUS_OBJECT_NAME_NOT_FOUND;
    auto link = std::make_shared<LinkObject>();
    link->target = it->second;
    W32(a[0], NewHandle(link));
    return X_STATUS_SUCCESS;
}

KERNEL_EXPORT(215, NtQuerySymbolicLinkObject) {
    auto link = Get<LinkObject>(a[0]);
    if (!link) return X_STATUS_INVALID_HANDLE;
    uint32_t str = a[1];
    uint16_t max = R16(str + 2);
    if (a[2]) W32(a[2], uint32_t(link->target.size()));
    if (link->target.size() > max) return X_STATUS_BUFFER_TOO_SMALL;
    std::memcpy(rt::Host(R32(str + 4)), link->target.data(), link->target.size());
    W16(str, uint16_t(link->target.size()));
    return X_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------------------------
// the I/O manager below the file system: only XAPI's drive mounting reaches it

KERNEL_EXPORT(84, IoSynchronousDeviceIoControlRequest) {
    rt::Log("file: IoSynchronousDeviceIoControlRequest code %08X", a[0]);
    if (a[4] && a[5]) std::memset(rt::Host(a[4]), 0, a[5]);
    if (a[6]) W32(a[6], a[5]);
    return X_STATUS_SUCCESS;
}

KERNEL_EXPORT(85, IoSynchronousFsdRequest) {
    rt::Log("file: IoSynchronousFsdRequest major %u length %X", a[0], a[3]);
    if (a[0] == 3 && a[2] && a[3]) std::memset(rt::Host(a[2]), 0, a[3]);  // IRP_MJ_READ
    return X_STATUS_SUCCESS;
}

KERNEL_EXPORT(65, IoCreateDevice) {
    uint32_t dev = mem::AllocSystem(0x100 + a[1]);
    W32(dev + 0x18, dev + 0x100);  // DeviceExtension
    W32(a[5], dev);
    return X_STATUS_SUCCESS;
}

KERNEL_EXPORT(35, FscGetCacheSize) { return 64; }
KERNEL_EXPORT(37, FscSetCacheSize) { return X_STATUS_SUCCESS; }

}  // namespace kernel
