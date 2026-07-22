#include "memory.h"
#include "../common/log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

namespace Memory {

// ===========================================================================
// Internal region tracking
// ===========================================================================
namespace {

struct Region {
    guest_addr_t base;
    u64          size;        // bytes (page-aligned)
    u32          protection;  // PROT_* bitmask
    u32          win32_prot;
    bool         committed;
};

std::mutex              g_regions_mutex;
std::vector<Region>     g_regions;

GuestFaultHandler       g_fault_handler = nullptr;
void*                   g_fault_user    = nullptr;
void*                   g_fault_veh     = nullptr; // AddVectoredExceptionHandle

// ---------------------------------------------------------------------------
// Guest image write tracking (see memory.h).  All fields are guarded by
// g_regions_mutex; the VEH path takes the same lock (VirtualProtect from a
// vectored handler is safe).
// ---------------------------------------------------------------------------
struct TrackedWriteRange {
    guest_addr_t base;        // key: address TrackGuestWrites was given
    guest_addr_t start;       // page-aligned span actually protected
    u64          length;
    u64          generation;
    bool         armed;
};

std::vector<TrackedWriteRange> g_write_ranges;

void ArmWriteRangeLocked(TrackedWriteRange& r) {
    DWORD old_prot = 0;
    r.armed = VirtualProtect(reinterpret_cast<void*>(r.start), r.length,
                             PAGE_READONLY, &old_prot) != 0;
}

void DisarmWriteRangeLocked(TrackedWriteRange& r) {
    if (!r.armed) return;
    DWORD old_prot = 0;
    VirtualProtect(reinterpret_cast<void*>(r.start), r.length,
                   PAGE_READWRITE, &old_prot);
    r.armed = false;
}

// First write to an armed range: restore write access and bump the
// generation.  Later writes are free-running until the owner re-arms.
bool HandleTrackedWriteFaultLocked(guest_addr_t fault_addr) {
    for (auto& r : g_write_ranges) {
        if (fault_addr < r.start || fault_addr >= r.start + r.length) {
            continue;
        }
        if (!r.armed) return false;
        DisarmWriteRangeLocked(r);
        ++r.generation;
        return true;
    }
    return false;
}

DWORD TranslateProtection(u32 protection) {
    bool r = (protection & PROT_READ)  != 0;
    bool w = (protection & PROT_WRITE) != 0;
    bool x = (protection & PROT_EXEC)  != 0;
    if (x) {
        if (w) return PAGE_EXECUTE_READWRITE;
        if (r) return PAGE_EXECUTE_READ;
        return PAGE_EXECUTE;
    }
    if (w) return PAGE_READWRITE;
    if (r) return PAGE_READONLY;
    return PAGE_NOACCESS;
}

u32 TranslateFromWin32(DWORD win_prot) {
    switch (win_prot) {
        case PAGE_EXECUTE_READWRITE: return PROT_READ | PROT_WRITE | PROT_EXEC;
        case PAGE_EXECUTE_READ:      return PROT_READ | PROT_EXEC;
        case PAGE_EXECUTE:           return PROT_EXEC;
        case PAGE_READWRITE:         return PROT_READ | PROT_WRITE;
        case PAGE_READONLY:          return PROT_READ;
        default:                     return PROT_NONE;
    }
}

void TrackRegion(guest_addr_t base, u64 size, u32 prot, DWORD win_prot, bool committed) {
    std::lock_guard<std::mutex> lock(g_regions_mutex);
    g_regions.push_back(Region{base, size, prot, win_prot, committed});
}

void UntrackRegion(guest_addr_t base, u64 size) {
    std::lock_guard<std::mutex> lock(g_regions_mutex);
    g_regions.erase(std::remove_if(g_regions.begin(), g_regions.end(),
        [&](const Region& r) {
            return r.base == base && r.size == size;
        }),
        g_regions.end());
}

bool IsPageAligned(guest_addr_t a) {
    return (a & (PAGE_SIZE - 1)) == 0;
}

// VEH that intercepts EXCEPTION_ACCESS_VIOLATION raised by guest code.  We
// cannot tell from the exception alone whether the faulting IP is guest or
// host code, so the handler is conservative: it only forwards faults whose
// address falls inside a tracked guest region.  Otherwise it returns
// EXCEPTION_CONTINUE_SEARCH so the next handler (or the default SEH filter)
// takes over.
LONG WINAPI GuestFaultVeh(struct _EXCEPTION_POINTERS* ep) {
    if (!ep || ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (ep->ExceptionRecord->NumberParameters < 2) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const auto fault_addr =
        static_cast<guest_addr_t>(ep->ExceptionRecord->ExceptionInformation[1]);
    const bool is_write = ep->ExceptionRecord->ExceptionInformation[0] == 1;

    // A write to an armed write-tracked range (guest texture upload sources)
    // is disarmed + generation-bumped here and resumed; the registered fault
    // handler (demand commit) never sees it.
    if (is_write) {
        std::lock_guard<std::mutex> lock(g_regions_mutex);
        if (HandleTrackedWriteFaultLocked(fault_addr)) {
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    GuestFaultHandler handler = nullptr;
    void* user = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_regions_mutex);
        handler = g_fault_handler;
        user = g_fault_user;
    }
    if (!handler) return EXCEPTION_CONTINUE_SEARCH;

    if (handler(fault_addr, ep->ExceptionRecord->ExceptionCode, user)) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

// ===========================================================================
// Public API
// ===========================================================================
const char* StatusAsString(Status s) {
    switch (s) {
        case Status::Ok:              return "Ok";
        case Status::InvalidArgument: return "InvalidArgument";
        case Status::OutOfMemory:     return "OutOfMemory";
        case Status::NotMapped:       return "NotMapped";
        case Status::AccessDenied:    return "AccessDenied";
        case Status::Win32Error:      return "Win32Error";
    }
    return "Unknown";
}

bool Initialize() {
    LOG_INFO(Memory, "Initializing guest memory manager...");
    g_fault_veh = AddVectoredExceptionHandler(/*First=*/1, GuestFaultVeh);
    if (!g_fault_veh) {
        LOG_WARN(Memory, "Failed to install guest fault VEH (err=%lu)", GetLastError());
    } else {
        LOG_INFO(Memory, "Guest fault VEH installed at 0x%p", g_fault_veh);
    }
    
    // Map guest framebuffer region (32MB starting at 0x200000000)
    guest_addr_t fb_addr = 0x200000000ULL;
    guest_addr_t out_fb_addr = 0;
    Status status = Map(fb_addr, 0x2000000, PROT_READ | PROT_WRITE, &out_fb_addr);
    if (status != Status::Ok) {
        LOG_ERROR(Memory, "Failed to map guest framebuffer at 0x%llx (status=%s)", fb_addr, StatusAsString(status));
    } else {
        LOG_INFO(Memory, "Mapped guest framebuffer region at 0x%llx-0x%llx", out_fb_addr, out_fb_addr + 0x2000000);
    }
    return true;
}

void Shutdown() {
    LOG_INFO(Memory, "Shutting down guest memory manager...");
    if (g_fault_veh) {
        RemoveVectoredExceptionHandler(g_fault_veh);
        g_fault_veh = nullptr;
    }
    std::lock_guard<std::mutex> lock(g_regions_mutex);
    g_regions.clear();
    g_write_ranges.clear();
    g_fault_handler = nullptr;
    g_fault_user = nullptr;
}

Status Map(guest_addr_t address, u64 size, u32 protection, guest_addr_t* out_addr) {
    if (out_addr) *out_addr = 0;
    if (size == 0 || !out_addr) return Status::InvalidArgument;
    if (!IsPageAligned(address)) {
        LOG_ERROR(Memory, "Map: address 0x%llx is not page-aligned", address);
        return Status::InvalidArgument;
    }

    const u64 aligned_size = ALIGN_UP(size, PAGE_SIZE);
    const DWORD win_prot = TranslateProtection(protection);
    void* requested = reinterpret_cast<void*>(address);
    void* allocated = VirtualAlloc(requested, aligned_size, MEM_RESERVE | MEM_COMMIT, win_prot);
    if (!allocated) {
        const DWORD err = GetLastError();
        LOG_ERROR(Memory, "Map: VirtualAlloc failed at 0x%llx size=0x%llx prot=%u (err=%lu)",
                  address, aligned_size, protection, err);
        return (err == ERROR_NOT_ENOUGH_MEMORY || err == ERROR_COMMITMENT_LIMIT)
                 ? Status::OutOfMemory : Status::Win32Error;
    }
    const guest_addr_t guest_addr = reinterpret_cast<guest_addr_t>(allocated);
    TrackRegion(guest_addr, aligned_size, protection, win_prot, /*committed=*/true);
    *out_addr = guest_addr;
    LOG_DEBUG(Memory, "Mapped [0x%llx-0x%llx] prot=%u (size=0x%llx)",
              guest_addr, guest_addr + aligned_size, protection, aligned_size);
    return Status::Ok;
}

Status Reserve(guest_addr_t address, u64 size, guest_addr_t* out_addr) {
    if (out_addr) *out_addr = 0;
    if (size == 0 || !out_addr) return Status::InvalidArgument;
    if (!IsPageAligned(address)) {
        LOG_ERROR(Memory, "Reserve: address 0x%llx is not page-aligned", address);
        return Status::InvalidArgument;
    }
    // Windows allocation granularity is 64KB; align to it.
    const u64 aligned_size = ALIGN_UP(size, 65536);
    void* requested = reinterpret_cast<void*>(address);
    void* reserved = VirtualAlloc(requested, aligned_size, MEM_RESERVE, PAGE_NOACCESS);
    if (!reserved) {
        const DWORD err = GetLastError();
        LOG_ERROR(Memory, "Reserve: VirtualAlloc failed at 0x%llx size=0x%llx (err=%lu)",
                  address, aligned_size, err);
        return (err == ERROR_NOT_ENOUGH_MEMORY) ? Status::OutOfMemory : Status::Win32Error;
    }
    const guest_addr_t guest_addr = reinterpret_cast<guest_addr_t>(reserved);
    TrackRegion(guest_addr, aligned_size, PROT_NONE, PAGE_NOACCESS, /*committed=*/false);
    *out_addr = guest_addr;
    LOG_DEBUG(Memory, "Reserved [0x%llx-0x%llx] (size=0x%llx)",
              guest_addr, guest_addr + aligned_size, aligned_size);
    return Status::Ok;
}

Status Commit(guest_addr_t address, u64 size, u32 protection) {
    if (size == 0) return Status::InvalidArgument;
    if (!IsPageAligned(address)) {
        LOG_ERROR(Memory, "Commit: address 0x%llx is not page-aligned", address);
        return Status::InvalidArgument;
    }
    const u64 aligned_size = ALIGN_UP(size, PAGE_SIZE);
    const DWORD win_prot = TranslateProtection(protection);
    void* ptr = reinterpret_cast<void*>(address);

    // Already-committed region (e.g. overlapping ELF segments sharing one
    // 16KB guest page): MEM_COMMIT cannot change protection on committed
    // pages, so merge the requested rights into the existing ones instead.
    // Region tracking is coarse (one entry per reservation), so the exact
    // range may be committed only in part: if the merge hits uncommitted
    // pages, fall back to committing them below.
    {
        std::lock_guard<std::mutex> lock(g_regions_mutex);
        for (auto& r : g_regions) {
            if (r.base <= address && address + aligned_size <= r.base + r.size &&
                r.committed) {
                const u32 merged = r.protection | protection;
                const DWORD merged_win = TranslateProtection(merged);
                DWORD old_prot = 0;
                if (VirtualProtect(ptr, aligned_size, merged_win, &old_prot)) {
                    r.protection = merged;
                    r.win32_prot = merged_win;
                    LOG_DEBUG(Memory, "Commit merged protection at [0x%llx-0x%llx] -> prot=%u",
                              address, address + aligned_size, merged);
                    return Status::Ok;
                }
                if (GetLastError() != ERROR_INVALID_ADDRESS) {
                    const DWORD err = GetLastError();
                    LOG_ERROR(Memory, "Commit: VirtualProtect(merge) failed at 0x%llx (err=%lu)",
                              address, err);
                    return Status::Win32Error;
                }
                r.protection = merged;
                r.win32_prot = merged_win;
                break; // range not committed yet: commit it below
            }
        }
    }

    void* committed = VirtualAlloc(ptr, aligned_size, MEM_COMMIT, win_prot);
    if (!committed) {
        const DWORD err = GetLastError();
        LOG_ERROR(Memory, "Commit: VirtualAlloc failed at 0x%llx size=0x%llx (err=%lu)",
                  address, aligned_size, err);
        return (err == ERROR_NOT_ENOUGH_MEMORY) ? Status::OutOfMemory : Status::Win32Error;
    }
    // Mark the region committed (or insert a new one if none existed).
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(g_regions_mutex);
        for (auto& r : g_regions) {
            if (r.base <= address && address + aligned_size <= r.base + r.size) {
                if (!r.committed) {
                    r.committed = true;
                    r.protection = protection;
                    r.win32_prot = win_prot;
                }
                found = true;
                break;
            }
        }
    }
    if (!found) {
        TrackRegion(reinterpret_cast<guest_addr_t>(committed), aligned_size,
                    protection, win_prot, /*committed=*/true);
    }
    LOG_DEBUG(Memory, "Committed [0x%llx-0x%llx] prot=%u (size=0x%llx)",
              address, address + aligned_size, protection, aligned_size);
    return Status::Ok;
}

Status Unmap(guest_addr_t address, u64 size) {
    if (!IsPageAligned(address) || size == 0) return Status::InvalidArgument;
    // VirtualFree(MEM_RELEASE) requires the exact base + size of the original
    // allocation; reject requests that fall inside a region.
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(g_regions_mutex);
        for (const auto& r : g_regions) {
            if (r.base == address && r.size == ALIGN_UP(size, PAGE_SIZE)) {
                found = true;
                break;
            }
        }
    }
    if (!found) {
        LOG_ERROR(Memory, "Unmap: no tracked region at 0x%llx size=0x%llx", address, size);
        return Status::NotMapped;
    }
    void* ptr = reinterpret_cast<void*>(address);
    if (!VirtualFree(ptr, 0, MEM_RELEASE)) {
        const DWORD err = GetLastError();
        LOG_ERROR(Memory, "Unmap: VirtualFree failed at 0x%llx (err=%lu)", address, err);
        return Status::Win32Error;
    }
    UntrackRegion(address, ALIGN_UP(size, PAGE_SIZE));
    // Drop write-tracked ranges the unmap made stale.
    {
        std::lock_guard<std::mutex> lock(g_regions_mutex);
        const guest_addr_t end = address + ALIGN_UP(size, PAGE_SIZE);
        g_write_ranges.erase(std::remove_if(g_write_ranges.begin(),
            g_write_ranges.end(),
            [&](const TrackedWriteRange& r) {
                return r.start >= address && r.start + r.length <= end;
            }),
            g_write_ranges.end());
    }
    LOG_DEBUG(Memory, "Unmapped [0x%llx-0x%llx]", address, address + size);
    return Status::Ok;
}

Status Protect(guest_addr_t address, u64 size, u32 protection) {
    if (!IsPageAligned(address) || size == 0) return Status::InvalidArgument;
    const u64 aligned_size = ALIGN_UP(size, PAGE_SIZE);
    const DWORD win_prot = TranslateProtection(protection);
    DWORD old_prot = 0;
    void* ptr = reinterpret_cast<void*>(address);
    bool ok = VirtualProtect(ptr, aligned_size, win_prot, &old_prot) != 0;
    DWORD err = ok ? 0 : GetLastError();
    if (!ok && err == ERROR_INVALID_ADDRESS) {
        // The range is reserved but not committed yet (e.g. a PRX segment
        // loaded into reserved address space).  Commit it, which also applies
        // the requested protection.
        ok = VirtualAlloc(ptr, aligned_size, MEM_COMMIT, win_prot) != nullptr;
        if (ok) {
            std::lock_guard<std::mutex> lock(g_regions_mutex);
            for (auto& r : g_regions) {
                if (r.base <= address && address + aligned_size <= r.base + r.size) {
                    r.committed = true;
                    break;
                }
            }
        } else {
            err = GetLastError();
        }
    }
    if (!ok) {
        LOG_ERROR(Memory, "Protect: VirtualProtect failed at 0x%llx size=0x%llx (err=%lu)",
                  address, aligned_size, err);
        return (err == ERROR_ACCESS_DENIED) ? Status::AccessDenied : Status::Win32Error;
    }
    // Update tracking: split or merge as needed.  Easiest correct approach:
    // for each tracked region overlapping the range, update the overlapped
    // pages.  We only support protect on a single tracked region for now.
    {
        std::lock_guard<std::mutex> lock(g_regions_mutex);
        for (auto& r : g_regions) {
            if (r.base <= address && address + aligned_size <= r.base + r.size) {
                r.protection = protection;
                r.win32_prot = win_prot;
                break;
            }
        }
    }
    LOG_DEBUG(Memory, "Protected [0x%llx-0x%llx] prot=%u", address, address + aligned_size, protection);
    return Status::Ok;
}

Status Query(guest_addr_t address, MemoryInfo* out_info) {
    if (!out_info) return Status::InvalidArgument;
    *out_info = MemoryInfo{};
    // First consult our own region table.
    {
        std::lock_guard<std::mutex> lock(g_regions_mutex);
        for (const auto& r : g_regions) {
            if (r.base <= address && address < r.base + r.size) {
                out_info->base_address     = r.base;
                out_info->size             = r.size;
                out_info->protection       = r.protection;
                out_info->is_committed     = r.committed;
                out_info->is_reserved      = !r.committed;
                out_info->win32_protection = r.win32_prot;
                return Status::Ok;
            }
        }
    }
    // Fall back to VirtualQuery for any host allocation that we did not track.
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi)) == 0) {
        return Status::NotMapped;
    }
    if (mbi.State == MEM_FREE) return Status::NotMapped;
    out_info->base_address     = reinterpret_cast<guest_addr_t>(mbi.BaseAddress);
    out_info->size             = mbi.RegionSize;
    out_info->protection       = TranslateFromWin32(mbi.Protect);
    out_info->is_committed     = (mbi.State == MEM_COMMIT);
    out_info->is_reserved      = (mbi.State == MEM_RESERVE);
    out_info->win32_protection = mbi.Protect;
    return Status::Ok;
}

bool IsReadable(guest_addr_t address, u64 size) {
    if (size == 0) return true;
    u64 start_page = address & ~(static_cast<u64>(PAGE_SIZE - 1));
    u64 end_page   = (address + size - 1) & ~(static_cast<u64>(PAGE_SIZE - 1));
    for (u64 p = start_page; p <= end_page; p += PAGE_SIZE) {
        MemoryInfo info{};
        if (Query(p, &info) != Status::Ok) return false;
        if (!info.is_committed) return false;
        if (!(info.protection & PROT_READ)) return false;
    }
    return true;
}

bool IsWritable(guest_addr_t address, u64 size) {
    if (size == 0) return true;
    u64 start_page = address & ~(static_cast<u64>(PAGE_SIZE - 1));
    u64 end_page   = (address + size - 1) & ~(static_cast<u64>(PAGE_SIZE - 1));
    for (u64 p = start_page; p <= end_page; p += PAGE_SIZE) {
        MemoryInfo info{};
        if (Query(p, &info) != Status::Ok) return false;
        if (!info.is_committed) return false;
        if (!(info.protection & PROT_WRITE)) return false;
    }
    return true;
}

bool IsExecutable(guest_addr_t address, u64 size) {
    if (size == 0) return true;
    for (u64 off = 0; off < size; off += PAGE_SIZE) {
        MemoryInfo info{};
        if (Query(address + off, &info) != Status::Ok) return false;
        if (!info.is_committed) return false;
        if (!(info.protection & PROT_EXEC)) return false;
    }
    return true;
}

MemoryStats GetStats() {
    MemoryStats s{};
    std::lock_guard<std::mutex> lock(g_regions_mutex);
    s.region_count = g_regions.size();
    for (const auto& r : g_regions) {
        s.total_reserved += r.size;
        if (r.committed) s.total_committed += r.size;
    }
    return s;
}

void ReadBuffer(guest_addr_t addr, void* dest, u64 size) {
    if (addr >= 0x200000000ULL && addr < 0x202000000ULL) {
        LOG_DEBUG(Memory, "Framebuffer read at guest 0x%llx (size=%llu)", addr, size);
    }
    std::memcpy(dest, reinterpret_cast<const void*>(addr), size);
}

void WriteBuffer(guest_addr_t addr, const void* src, u64 size) {
    if (addr >= 0x200000000ULL && addr < 0x202000000ULL) {
        LOG_DEBUG(Memory, "Framebuffer write at guest 0x%llx (size=%llu)", addr, size);
    }
    std::memcpy(reinterpret_cast<void*>(addr), src, size);
}

bool CommitOnFault(guest_addr_t address) {
    constexpr u64 kGranularity = 65536; // Windows allocation granularity
    const guest_addr_t base = address & ~(kGranularity - 1);
    // Only act when the fault address lies inside a tracked guest region.
    bool covered = false;
    {
        std::lock_guard<std::mutex> lock(g_regions_mutex);
        for (const auto& r : g_regions) {
            if (r.base <= address && address < r.base + r.size) {
                covered = true;
                break;
            }
        }
    }
    if (!covered) return false;
    // The region record may be marked committed as a whole while only a
    // subrange actually is (Memory::Protect's commit fallback flips the
    // containing record).  Decide per fault page instead: commit only when
    // the page is genuinely still reserved (LOST EPIC's Unity heap does
    // exactly this: reserve 8 MB, mprotect-commit 4 MB, then touch more).
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi)) == 0) {
        return false;
    }
    if (mbi.State != MEM_RESERVE) return false;
    if (!VirtualAlloc(reinterpret_cast<void*>(base), kGranularity, MEM_COMMIT, PAGE_READWRITE)) {
        LOG_WARN(Memory, "CommitOnFault: failed to commit 64 KiB at 0x%llx (err=%lu)",
                 base, GetLastError());
        return false;
    }
    LOG_DEBUG(Memory, "CommitOnFault: committed 64 KiB at 0x%llx (fault at 0x%llx)", base, address);
    return true;
}

void TrackGuestWrites(guest_addr_t address, u64 byte_count) {
    if (address == 0 || byte_count == 0) return;
    const guest_addr_t start = address & ~(static_cast<guest_addr_t>(PAGE_SIZE) - 1);
    const u64 length = ALIGN_UP(address + byte_count, PAGE_SIZE) - start;
    std::lock_guard<std::mutex> lock(g_regions_mutex);
    for (auto& r : g_write_ranges) {
        if (r.base != address) continue;
        if (r.start != start || r.length != length) {
            // Replace-range: carry the generation so a resize does not hide
            // earlier guest CPU rewrites from cache owners.
            const u64 generation = r.generation;
            DisarmWriteRangeLocked(r);
            r = TrackedWriteRange{address, start, length, generation, false};
        }
        ArmWriteRangeLocked(r);
        return;
    }
    TrackedWriteRange fresh{address, start, length, 0, false};
    ArmWriteRangeLocked(fresh);
    g_write_ranges.push_back(fresh);
}

void UntrackGuestWrites(guest_addr_t address) {
    std::lock_guard<std::mutex> lock(g_regions_mutex);
    for (auto it = g_write_ranges.begin(); it != g_write_ranges.end(); ++it) {
        if (it->base == address) {
            DisarmWriteRangeLocked(*it);
            g_write_ranges.erase(it);
            return;
        }
    }
}

bool TryGetGuestWriteGeneration(guest_addr_t address, u64* generation_out) {
    if (!generation_out) return false;
    std::lock_guard<std::mutex> lock(g_regions_mutex);
    for (const auto& r : g_write_ranges) {
        if (r.base == address) {
            *generation_out = r.generation;
            return true;
        }
    }
    return false;
}

void RearmGuestWrites(guest_addr_t address) {
    std::lock_guard<std::mutex> lock(g_regions_mutex);
    for (auto& r : g_write_ranges) {
        if (r.base == address) {
            ArmWriteRangeLocked(r);
            return;
        }
    }
}

void SetGuestFaultHandler(GuestFaultHandler handler, void* user_data) {
    std::lock_guard<std::mutex> lock(g_regions_mutex);
    g_fault_handler = handler;
    g_fault_user = user_data;
    LOG_INFO(Memory, "Guest fault handler set: %p (user=%p)",
             reinterpret_cast<const void*>(handler), user_data);
}

GuestFaultHandler GetGuestFaultHandler() {
    std::lock_guard<std::mutex> lock(g_regions_mutex);
    return g_fault_handler;
}

void* GetGuestFaultHandlerUserData() {
    std::lock_guard<std::mutex> lock(g_regions_mutex);
    return g_fault_user;
}

} // namespace Memory
