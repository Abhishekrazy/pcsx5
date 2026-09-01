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
    // Set when part of this region has been protected differently from the
    // rest. One protection value cannot describe the region any more, so
    // Query must take the OS per-page state as authoritative instead.
    bool         mixed_protection = false;
    Owner        owner   = Owner::None;
    std::string  name;         // diagnostic label (module name, "stack", ...)
    // True when THIS manager performed the host allocation (Reserve/Map/
    // AllocateRange/Commit) and therefore must VirtualFree it on release.
    // False for AdoptRange registrations whose host memory the adopter owns.
    bool         managed = true;
};

std::mutex              g_regions_mutex;
std::vector<Region>     g_regions;

// Released ranges available for reuse by AllocateRange.  A range lands here
// via ReleaseRange/Unmap of a tracked region.  Coalescing is deliberately
// NOT done yet (correctness first; the guest module window has few ranges).
struct FreeRange { guest_addr_t base; u64 size; };
std::vector<FreeRange>  g_free_ranges;

GuestFaultHandler       g_fault_handler = nullptr;
void*                   g_fault_user    = nullptr;
void*                   g_fault_veh     = nullptr; // AddVectoredExceptionHandle

// ---------------------------------------------------------------------------
// O1.2 / I3.2: Direct-mapped guest memory pool.
//
// Pre-allocates a large contiguous VA range at init so that the common
// "hint=0" allocations (heap, stack, scratch buffers) sub-allocate from the
// pool instead of calling VirtualAlloc per call.  The content-load phase in
// Dreaming Sarah does ~450+ × 64 KB allocations; each one takes ~1 ms via
// VirtualAlloc (kernel transition), and with the pool it becomes a 64-byte
// bump-pointer increment — ~1000× faster.
//
// When the pool is exhausted, new allocations fall back to VirtualAlloc.
// The pool is never shrunk; freed pool pages are added to a free-list for
// reuse rather than being returned to the OS.
// ---------------------------------------------------------------------------
constexpr u64 kPoolSize    = 1ULL * 1024 * 1024 * 1024;  // 1 GB — enough for content-load + heap

void*  g_pool_base    = nullptr;  // VirtualAlloc base (page-aligned)
u64    g_pool_used    = 0;        // bytes consumed so far (bump allocator)
bool   g_pool_ok      = false;    // set after successful pool reservation

// True when `address` lies inside the direct-mapped pool's reserved span.
// Pool pages stay physically committed after PoolFree, so VirtualQuery alone
// cannot distinguish a live sub-allocation from a freed one — the region
// table decides.
bool IsInPool(guest_addr_t address) {
    if (!g_pool_ok) return false;
    const guest_addr_t pool_start = reinterpret_cast<guest_addr_t>(g_pool_base);
    return address >= pool_start && address < pool_start + kPoolSize;
}

// Free-list for pool allocations that are Unmap'd. Only slots allocated
// via PoolAlloc with matching address+size can be freed.
struct PoolFreeSlot { guest_addr_t base; u64 size; };
std::vector<PoolFreeSlot> g_pool_free;

// Pool sub-allocator.  Size must already be page-aligned.  Returns 0 on
// failure (pool exhausted or not initialized).
guest_addr_t PoolAlloc(u64 aligned_size) {
    if (!g_pool_ok) return 0;
    // PCSX5_DISABLE_POOL=1 bypasses the pool for testing intermittency.
    {
        char buf[4] = {};
        if (GetEnvironmentVariableA("PCSX5_DISABLE_POOL", buf, sizeof(buf)) > 0 && atoi(buf) == 1)
            return 0;
    }
    std::lock_guard<std::mutex> lock(g_regions_mutex);

    // 1. Search free list for available reuse
    for (auto it = g_pool_free.begin(); it != g_pool_free.end(); ++it) {
        if (it->size >= aligned_size) {
            guest_addr_t addr = it->base;
            if (it->size == aligned_size) {
                g_pool_free.erase(it);
            } else {
                it->base += aligned_size;
                it->size -= aligned_size;
            }
            std::memset(reinterpret_cast<void*>(addr), 0, aligned_size);
            return addr;
        }
    }

    if (g_pool_used + aligned_size > kPoolSize) return 0;  // OOM
    guest_addr_t addr = reinterpret_cast<guest_addr_t>(g_pool_base) + g_pool_used;
    g_pool_used += aligned_size;
    // Check whether these pages were previously committed (LIFO rewind reuse).
    // Fresh commits are zero-initialized by the kernel; a reused block that
    // PoolFree rewound keeps its stale contents, which breaks guest code that
    // relies on Orbis-style zero-initialized memory.
    bool reused_committed = false;
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) &&
            mbi.State == MEM_COMMIT) {
            reused_committed = true;
        }
    }
    // Pages in the pool are reserved-only at init; commit the sub-range now.
    if (!VirtualAlloc(reinterpret_cast<void*>(addr), aligned_size,
                      MEM_COMMIT, PAGE_READWRITE)) {
        g_pool_used -= aligned_size;  // roll back the bump
        return 0;
    }
    if (reused_committed) {
        std::memset(reinterpret_cast<void*>(addr), 0, aligned_size);
    }
    return addr;
}

bool PoolFreeLocked(guest_addr_t base, u64 size) {
    if (!g_pool_ok) return false;
    guest_addr_t pool_start = reinterpret_cast<guest_addr_t>(g_pool_base);
    guest_addr_t pool_end   = pool_start + kPoolSize;
    if (base < pool_start || base + size > pool_end) return false;

    // Last-bump release (LIFO common case): just rewind the bump pointer.
    if (base + size == reinterpret_cast<guest_addr_t>(g_pool_base) + g_pool_used) {
        g_pool_used -= size;
        return true;  // pages stay committed for immediate reuse
    }
    g_pool_free.push_back({base, size});  // non-LIFO: add to free-list
    return true;
}

bool PoolFree(guest_addr_t base, u64 size) {
    std::lock_guard<std::mutex> lock(g_regions_mutex);
    return PoolFreeLocked(base, size);
}

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
    DWORD base_prot = win_prot & 0xFF;
    switch (base_prot) {
        case PAGE_EXECUTE_READWRITE: return PROT_READ | PROT_WRITE | PROT_EXEC;
        case PAGE_EXECUTE_READ:      return PROT_READ | PROT_EXEC;
        case PAGE_EXECUTE:           return PROT_EXEC;
        case PAGE_EXECUTE_WRITECOPY: return PROT_READ | PROT_WRITE | PROT_EXEC;
        case PAGE_READWRITE:         return PROT_READ | PROT_WRITE;
        case PAGE_READONLY:          return PROT_READ;
        case PAGE_WRITECOPY:         return PROT_READ | PROT_WRITE;
        default:                     return PROT_NONE;
    }
}

void TrackRegion(guest_addr_t base, u64 size, u32 prot, DWORD win_prot, bool committed) {
    std::lock_guard<std::mutex> lock(g_regions_mutex);
    Region r;
    r.base = base; r.size = size; r.protection = prot;
    r.win32_prot = win_prot; r.committed = committed;
    r.owner = Owner::None;
    g_regions.push_back(std::move(r));
}

// Owner/name-carrying variant used by the Stage 2 allocation paths.
void TrackRegionOwned(guest_addr_t base, u64 size, u32 prot, DWORD win_prot,
                      bool committed, Owner owner, const char* name) {
    std::lock_guard<std::mutex> lock(g_regions_mutex);
    Region r;
    r.base = base; r.size = size; r.protection = prot;
    r.win32_prot = win_prot; r.committed = committed;
    r.owner = owner;
    if (name) r.name = name;
    // A newly tracked range must not also sit on the free list.
    g_free_ranges.erase(std::remove_if(g_free_ranges.begin(), g_free_ranges.end(),
        [&](const FreeRange& f) { return f.base == base && f.size <= size; }),
        g_free_ranges.end());
    g_regions.push_back(std::move(r));
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

const char* OwnerAsString(Owner o) {
    switch (o) {
        case Owner::Loader: return "loader";
        case Owner::Kernel: return "kernel";
        case Owner::Hle:    return "hle";
        case Owner::Guest:  return "guest";
        default:            return "none";
    }
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

    // O1.3 (retired): this used to force-map 256 MB at 0x800000000 as a
    // boot-time pre-commit optimization.  It conflicted with the loader's
    // PIE base hint (elf.cpp kPieBaseHint = 0x800000000): whichever ran first
    // silently displaced the other, and in practice the fixed-address
    // VirtualAlloc failed with ERROR_INVALID_ADDRESS in most processes —
    // so the optimization never actually engaged while still breaking the
    // loader's preferred base.  Demand-commit via CommitOnFault covers the
    // same pages lazily and is exercised by memory_query_tests.

    // O1.2 / I3.2: pre-reserve a large VA pool for fast sub-allocation.
    // Let the kernel choose the base — forcing a specific address (0x4000000000)
    // caused intermittent boot hangs because it shifted the subsequent VA layout
    // in a way that triggered a guest-initialization race (B1.3 investigation).
    {
        void* pool = VirtualAlloc(nullptr, kPoolSize,
                                   MEM_RESERVE, PAGE_NOACCESS);
        if (pool) {
            g_pool_base = pool;
            g_pool_ok = true;
            LOG_INFO(Memory, "Direct-mapped memory pool: %llu MB at 0x%llx",
                     kPoolSize / (1024 * 1024),
                     reinterpret_cast<guest_addr_t>(pool));
        } else {
            LOG_WARN(Memory, "Direct-mapped memory pool failed (err=%lu) — "
                     "falling back to per-call VirtualAlloc", GetLastError());
        }
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
    // Release manager-owned host allocations so a subsequent Initialize()
    // starts clean instead of leaking reservations (the framebuffer Map at
    // 0x200000000 fails with ERROR_INVALID_ADDRESS on the second init
    // otherwise).  Adopted ranges stay — their owner frees them.
    for (const auto& r : g_regions) {
        if (!r.managed || IsInPool(r.base)) continue;
        if (!VirtualFree(reinterpret_cast<void*>(r.base), 0, MEM_RELEASE)) {
            LOG_DEBUG(Memory, "Shutdown: VirtualFree [0x%llx-0x%llx] failed (err=%lu)",
                      r.base, r.base + r.size, GetLastError());
        }
    }
    // Release the direct-mapped pool so a subsequent Initialize() does not
    // orphan a 1 GB reservation per init/shutdown cycle.
    if (g_pool_base) {
        VirtualFree(g_pool_base, 0, MEM_RELEASE);
        g_pool_base = nullptr;
    }
    g_pool_used = 0;
    g_pool_ok = false;
    g_pool_free.clear();  // PoolFreeSlot vector
    g_regions.clear();
    g_free_ranges.clear();
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

    u64 aligned_size = ALIGN_UP(size, PAGE_SIZE);
    const DWORD win_prot = TranslateProtection(protection);
    void* requested = reinterpret_cast<void*>(address);

    // O1.2 / I3.2: when no fixed address is requested, sub-allocate from
    // the direct-mapped pool rather than calling VirtualAlloc.  This is
    // ~1000× faster and eliminates the 1 ms+ kernel transition per call
    // that makes the Dreaming Sarah content-load phase stall for minutes.
    if (address == 0) {
        guest_addr_t pool_addr = PoolAlloc(aligned_size);
        if (pool_addr) {
            if (protection != (PROT_READ | PROT_WRITE)) {
                // Pool pages default to RW; change protection if needed.
                DWORD actual_win = TranslateProtection(protection);
                DWORD old = 0;
                VirtualProtect(reinterpret_cast<void*>(pool_addr),
                               aligned_size, actual_win, &old);
            }
            TrackRegionOwned(pool_addr, aligned_size, protection,
                             TranslateProtection(protection), true,
                             Owner::Kernel, "map");
            *out_addr = pool_addr;
            return Status::Ok;
        }
        // Pool exhausted — fall through to VirtualAlloc below.
    }

    // O1.1: try large pages (2 MB) for allocations >= 2 MB.  Falls back
    // to regular 4 KB pages if the privilege is not held or the system
    // does not support large pages.
    void* allocated = nullptr;
    const size_t large_page_min = ::GetLargePageMinimum();
    if (large_page_min > 0 && aligned_size >= large_page_min) {
        // SeLockMemoryPrivilege is required; enable best-effort.
        HANDLE token = nullptr;
        if (::OpenProcessToken(::GetCurrentProcess(),
                                TOKEN_ADJUST_PRIVILEGES, &token)) {
            TOKEN_PRIVILEGES tp{};
            tp.PrivilegeCount = 1;
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            ::LookupPrivilegeValueA(nullptr, SE_LOCK_MEMORY_NAME,
                                    &tp.Privileges[0].Luid);
            ::AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr);
            ::CloseHandle(token);
        }
        // Align to large page size for MEM_LARGE_PAGES.
        aligned_size = ALIGN_UP(aligned_size, large_page_min);
        allocated = ::VirtualAlloc(requested, aligned_size,
                                   MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES,
                                   win_prot);
    }
    if (!allocated) {
        allocated = ::VirtualAlloc(requested, aligned_size,
                                   MEM_RESERVE | MEM_COMMIT, win_prot);
    }
    if (!allocated) {
        const DWORD err = GetLastError();
        LOG_ERROR(Memory, "Map: VirtualAlloc failed at 0x%llx size=0x%llx prot=%u (err=%lu)",
                  address, aligned_size, protection, err);
        return (err == ERROR_NOT_ENOUGH_MEMORY || err == ERROR_COMMITMENT_LIMIT)
                 ? Status::OutOfMemory : Status::Win32Error;
    }
    const guest_addr_t guest_addr = reinterpret_cast<guest_addr_t>(allocated);
    TrackRegionOwned(guest_addr, aligned_size, protection, win_prot,
                     /*committed=*/true, Owner::Kernel,
                     (address == kGuestVaBase || address >= kGuestVaBase) &&
                     address < kGuestVaEnd ? "module-window" : "map");
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
    TrackRegionOwned(guest_addr, aligned_size, PROT_NONE, PAGE_NOACCESS,
                     /*committed=*/false, Owner::Loader,
                     (address >= kGuestVaBase && address < kGuestVaEnd)
                         ? "module" : "reserve");
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
    const u64 aligned_size = ALIGN_UP(size, PAGE_SIZE);
    void* ptr = reinterpret_cast<void*>(address);

    // Not-mapped semantics: an address we never tracked and that the host
    // reports free has nothing to release.  Checked before the VirtualFree
    // attempts, which would otherwise surface Win32Error for a plain
    // use-after-unmap / bogus-address case.
    {
        bool tracked = false;
        {
            std::lock_guard<std::mutex> lock(g_regions_mutex);
            for (const auto& r : g_regions) {
                if (r.base <= address && address < r.base + r.size) {
                    tracked = true;
                    break;
                }
            }
        }
        if (!tracked) {
            MEMORY_BASIC_INFORMATION mbi{};
            // VirtualQuery returning 0 means the address is beyond any host
            // allocation — the same condition Query uses for NotMapped.
            if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0 || mbi.State == MEM_FREE) {
                LOG_DEBUG(Memory, "Unmap of untracked free range [0x%llx-0x%llx] -> NotMapped",
                          address, address + size);
                return Status::NotMapped;
            }
        }
    }

    // I3.2: try pool free first (fast, no VirtualFree).  Pool VAs are NOT
    // added to the free list: their pages stay committed inside the pool
    // span, and PoolAlloc's bump/free-list already handles reuse.
    {
        std::lock_guard<std::mutex> lock(g_regions_mutex);
        if (PoolFreeLocked(address, aligned_size)) {
            g_regions.erase(std::remove_if(g_regions.begin(), g_regions.end(),
                [&](const Region& r) {
                    return r.base == address && r.size == aligned_size;
                }),
                g_regions.end());
            LOG_DEBUG(Memory, "Unmapped from pool [0x%llx-0x%llx]", address, address + size);
            return Status::Ok;
        }
    }

    // First attempt full release
    if (VirtualFree(ptr, 0, MEM_RELEASE)) {
        UntrackRegion(address, aligned_size);
        std::lock_guard<std::mutex> lock(g_regions_mutex);
        // Full release: the span becomes reusable by AllocateRange.
        g_free_ranges.push_back({address, aligned_size});
        const guest_addr_t end = address + aligned_size;
        g_write_ranges.erase(std::remove_if(g_write_ranges.begin(),
            g_write_ranges.end(),
            [&](const TrackedWriteRange& r) {
                return r.start >= address && r.start + r.length <= end;
            }),
            g_write_ranges.end());
        LOG_DEBUG(Memory, "Unmapped [0x%llx-0x%llx]", address, address + size);
        return Status::Ok;
    }

    // Partial unmap / decommit fallback: back free pages of partially-overlapping fixed mapping
    if (VirtualFree(ptr, aligned_size, MEM_DECOMMIT)) {
        std::lock_guard<std::mutex> lock(g_regions_mutex);
        const guest_addr_t end = address + aligned_size;
        g_write_ranges.erase(std::remove_if(g_write_ranges.begin(),
            g_write_ranges.end(),
            [&](const TrackedWriteRange& r) {
                return r.start >= address && r.start + r.length <= end;
            }),
            g_write_ranges.end());
        LOG_DEBUG(Memory, "Decommitted partial sub-range [0x%llx-0x%llx]", address, address + size);
        return Status::Ok;
    }

    const DWORD err = GetLastError();
    LOG_ERROR(Memory, "Unmap: VirtualFree failed at 0x%llx (err=%lu)", address, err);
    return Status::Win32Error;
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
                if (address == r.base && aligned_size == r.size) {
                    // The whole region changed, so one value still describes it.
                    r.protection = protection;
                    r.win32_prot = win_prot;
                    r.mixed_protection = false;
                } else if (protection != r.protection) {
                    // A sub-range changed. Recording it on the region would
                    // claim the whole region has these rights, which is how a
                    // module's writable .bss came to be reported read-only:
                    // each ELF segment protected in turn overwrote the record
                    // for all 8MB, and the last segment processed won.
                    r.mixed_protection = true;
                }
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

    // Direct-mapped pool memory: pages stay committed in Win32 after PoolFree,
    // so the region table is the sole authority for pool allocations.
    if (IsInPool(address)) {
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
        return Status::NotMapped;
    }

    // Outside the pool: query the OS for ground-truth page state.
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi)) == 0 || mbi.State == MEM_FREE) {
        return Status::NotMapped;
    }

    std::lock_guard<std::mutex> lock(g_regions_mutex);
    const Region* matched_region = nullptr;
    for (const auto& r : g_regions) {
        if (r.base <= address && address < r.base + r.size) {
            matched_region = &r;
            break;
        }
    }

    out_info->win32_protection = mbi.Protect;
    out_info->is_committed     = (mbi.State == MEM_COMMIT);
    out_info->is_reserved      = (mbi.State == MEM_RESERVE);

    if (matched_region) {
        out_info->base_address = matched_region->base;
        out_info->size         = matched_region->size;
        if (out_info->is_committed) {
            u32 host_prot = TranslateFromWin32(mbi.Protect);
            out_info->protection = (matched_region->protection != PROT_NONE &&
                                    !matched_region->mixed_protection)
                                       ? (host_prot & matched_region->protection)
                                       : host_prot;
            if (out_info->protection == PROT_NONE && host_prot != PROT_NONE) {
                out_info->protection = host_prot;
            }
        } else {
            out_info->protection = PROT_NONE;
        }
    } else {
        out_info->base_address = reinterpret_cast<guest_addr_t>(mbi.BaseAddress);
        out_info->size         = mbi.RegionSize;
        out_info->protection   = out_info->is_committed ? TranslateFromWin32(mbi.Protect) : PROT_NONE;
    }
    return Status::Ok;
}

bool IsReadable(guest_addr_t address, u64 size) {
    if (size == 0) return true;
    constexpr u64 kHostPageSize = 4096;
    u64 start_page = address & ~(kHostPageSize - 1);
    u64 end_page   = (address + size - 1) & ~(kHostPageSize - 1);
    for (u64 p = start_page; p <= end_page; p += kHostPageSize) {
        MemoryInfo info{};
        if (Query(p, &info) != Status::Ok) return false;
        if (!info.is_committed) return false;
        if (!(info.protection & PROT_READ)) return false;
    }
    return true;
}

bool IsWritable(guest_addr_t address, u64 size) {
    if (size == 0) return true;
    constexpr u64 kHostPageSize = 4096;
    u64 start_page = address & ~(kHostPageSize - 1);
    u64 end_page   = (address + size - 1) & ~(kHostPageSize - 1);
    for (u64 p = start_page; p <= end_page; p += kHostPageSize) {
        MemoryInfo info{};
        if (Query(p, &info) != Status::Ok) return false;
        if (!info.is_committed) return false;
        if (!(info.protection & PROT_WRITE)) return false;
    }
    return true;
}

bool IsExecutable(guest_addr_t address, u64 size) {
    if (size == 0) return true;
    constexpr u64 kHostPageSize = 4096;
    u64 start_page = address & ~(kHostPageSize - 1);
    u64 end_page   = (address + size - 1) & ~(kHostPageSize - 1);
    for (u64 p = start_page; p <= end_page; p += kHostPageSize) {
        MemoryInfo info{};
        if (Query(p, &info) != Status::Ok) return false;
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

// ---------------------------------------------------------------------------
// Stage 2: guest VA range ownership contract.
// ---------------------------------------------------------------------------

namespace {

// True when [base, base+size) overlaps any tracked region or any known
// host allocation we must treat as taken (the direct-mapped pool interior,
// which stays committed after PoolFree by design).
bool RangeOverlapsOwnedLocked(guest_addr_t base, u64 size) {
    const guest_addr_t end = base + size;
    for (const auto& r : g_regions) {
        if (r.base < end && base < r.base + r.size) return true;
    }
    // Pool interior is owned even where untracked (freed pool blocks keep
    // their pages for reuse; handing the VA out again would alias live data).
    if (g_pool_ok) {
        const guest_addr_t pool_start = reinterpret_cast<guest_addr_t>(g_pool_base);
        if (base < pool_start + kPoolSize && pool_start < end) return true;
    }
    return false;
}

} // namespace

Status AllocateRange(u64 size, u64 alignment, Owner owner,
                     const char* name, guest_addr_t* out_addr) {
    if (!out_addr || size == 0) return Status::InvalidArgument;
    *out_addr = 0;
    if (alignment == 0) alignment = PAGE_SIZE;

    // Windows allocation granularity: every reservation is 64 KiB aligned.
    u64 alloc_align = (alignment > 65536) ? alignment : 65536;
    const u64 aligned_size = ALIGN_UP(size, 65536);

    std::lock_guard<std::mutex> lock(g_regions_mutex);

    // Deterministic low-address-first scan of the guest module window.
    guest_addr_t cursor = ALIGN_UP(kGuestVaBase, alloc_align);
    while (cursor + aligned_size <= kGuestVaEnd) {
        if (!RangeOverlapsOwnedLocked(cursor, aligned_size)) {
            // Reserve at the host level.  We hold g_regions_mutex — same lock
            // TrackRegion takes — and VirtualAlloc never re-enters us.
            void* reserved = VirtualAlloc(reinterpret_cast<void*>(cursor),
                                          aligned_size, MEM_RESERVE, PAGE_NOACCESS);
            if (reserved) {
                Region r;
                r.base = cursor; r.size = aligned_size;
                r.protection = PROT_NONE; r.win32_prot = PAGE_NOACCESS;
                r.committed = false; r.owner = owner;
                if (name) r.name = name;
                g_regions.push_back(std::move(r));
                *out_addr = cursor;
                LOG_DEBUG(Memory, "AllocateRange: [0x%llx-0x%llx] owner=%s name=%s",
                          cursor, cursor + aligned_size, OwnerAsString(owner),
                          name ? name : "");
                return Status::Ok;
            }
            // Host refused this specific address (should not happen given the
            // overlap check, but stay safe): skip one granule and retry.
            LOG_WARN(Memory, "AllocateRange: VirtualAlloc refused 0x%llx (err=%lu)",
                     cursor, GetLastError());
        }
        const guest_addr_t next = cursor + alloc_align;
        if (next <= cursor) break;  // overflow guard
        cursor = next;
    }
    LOG_INFO(Memory, "AllocateRange: no free %llu-byte range in window "
             "[0x%llx-0x%llx) (owner=%s name=%s)",
             (unsigned long long)aligned_size,
             (unsigned long long)kGuestVaBase, (unsigned long long)kGuestVaEnd,
             OwnerAsString(owner), name ? name : "");
    return Status::OutOfMemory;
}

Status ReleaseRange(guest_addr_t base) {
    std::lock_guard<std::mutex> lock(g_regions_mutex);
    auto it = std::find_if(g_regions.begin(), g_regions.end(),
        [&](const Region& r) { return r.base == base; });
    if (it == g_regions.end()) {
        return Status::NotMapped;
    }
    Region r = *it;
    g_regions.erase(it);

    // Release host backing.  MEM_RELEASE works for whole reservations; a
    // partially-committed reservation releases both commit and reserve state.
    if (r.managed) {
        if (IsInPool(base)) {
            PoolFree(base, r.size);  // pool ranges: rewind/free-list, no VirtualFree
        } else {
            if (!VirtualFree(reinterpret_cast<void*>(base), 0, MEM_RELEASE)) {
                LOG_WARN(Memory, "ReleaseRange: VirtualFree failed at 0x%llx (err=%lu) — "
                         "tracking removed anyway", base, GetLastError());
            }
        }
    }

    // Drop write-tracking that lives entirely inside the released span.
    const guest_addr_t end = base + r.size;
    g_write_ranges.erase(std::remove_if(g_write_ranges.begin(), g_write_ranges.end(),
        [&](const TrackedWriteRange& w) {
            return w.start >= base && w.start + w.length <= end;
        }),
        g_write_ranges.end());

    g_free_ranges.push_back({base, r.size});
    LOG_DEBUG(Memory, "ReleaseRange: freed [0x%llx-0x%llx] (was owner=%s name=%s)",
              base, end, OwnerAsString(r.owner), r.name.c_str());
    return Status::Ok;
}

Status ForgetResource(guest_addr_t address) {
    std::lock_guard<std::mutex> lock(g_regions_mutex);
    auto it = std::find_if(g_regions.begin(), g_regions.end(),
        [&](const Region& r) { return r.base == address; });
    if (it == g_regions.end()) {
        return Status::NotMapped;
    }
    if (it->managed) {
        // Forgetting a manager-owned range would leak its host backing —
        // callers must use ReleaseRange for those.  Refuse loudly.
        LOG_WARN(Memory, "ForgetResource: [0x%llx-0x%llx] is manager-owned; "
                 "use ReleaseRange (refused)", it->base, it->base + it->size);
        return Status::AccessDenied;
    }
    LOG_DEBUG(Memory, "ForgetResource: untracked [0x%llx-0x%llx] (owner=%s, host pages stay)",
              it->base, it->base + it->size, OwnerAsString(it->owner));
    g_regions.erase(it);
    return Status::Ok;
}

Owner QueryOwner(guest_addr_t address) {
    std::lock_guard<std::mutex> lock(g_regions_mutex);
    for (const auto& r : g_regions) {
        if (r.base <= address && address < r.base + r.size) return r.owner;
    }
    return Owner::None;
}

bool IsRangeFree(guest_addr_t address, u64 size) {
    if (size == 0) return false;
    // Page 0 is never allocatable: guest null-pointer dereferences must
    // fault, never silently succeed against an allocation at 0.
    if (address < PAGE_SIZE) return false;
    std::lock_guard<std::mutex> lock(g_regions_mutex);
    return !RangeOverlapsOwnedLocked(address, size);
}

Status AdoptRange(guest_addr_t address, u64 size, u32 protection,
                  bool committed, Owner owner, const char* name) {
    if (size == 0) return Status::InvalidArgument;
    const u64 aligned_size = ALIGN_UP(size, PAGE_SIZE);
    {
        std::lock_guard<std::mutex> lock(g_regions_mutex);
        // Idempotent: re-adopting an exactly-matching tracked range updates
        // its record (owner/name/state) instead of double-tracking.  Used
        // e.g. to retag Memory::Reserve output as game-requested.
        for (auto& r : g_regions) {
            if (r.base == address && r.size == aligned_size) {
                r.protection = protection;
                r.win32_prot = TranslateProtection(protection);
                r.committed  = committed;
                r.owner      = owner;
                if (name) r.name = name;
                r.managed    = false;  // host memory not ours to free
                LOG_DEBUG(Memory, "AdoptRange: retagged [0x%llx-0x%llx] owner=%s",
                          address, address + aligned_size, OwnerAsString(owner));
                return Status::Ok;
            }
        }
    }
    TrackRegionOwned(address, aligned_size, protection,
                     TranslateProtection(protection), committed, owner, name);
    {
        std::lock_guard<std::mutex> lock(g_regions_mutex);
        for (auto& r : g_regions) {
            if (r.base == address && r.size == aligned_size) r.managed = false;
        }
    }
    LOG_DEBUG(Memory, "AdoptRange: [0x%llx-0x%llx] owner=%s name=%s committed=%d",
              address, address + aligned_size, OwnerAsString(owner),
              name ? name : "", (int)committed);
    return Status::Ok;
}

bool IsValidGuestPointer(guest_addr_t address) {
    MemoryInfo info{};
    return Query(address, &info) == Status::Ok;
}

void ReadBuffer(guest_addr_t addr, void* dest, u64 size) {
    if (addr >= 0x200000000ULL && addr < 0x202000000ULL) {
        LOG_DEBUG(Memory, "Framebuffer read at guest 0x%llx (size=%llu)", addr, size);
    }
    GuardedRead(dest, addr, size);
}

void WriteBuffer(guest_addr_t addr, const void* src, u64 size) {
    if (addr >= 0x200000000ULL && addr < 0x202000000ULL) {
        LOG_DEBUG(Memory, "Framebuffer write at guest 0x%llx (size=%llu)", addr, size);
    }
    GuardedWrite(addr, src, size);
}

bool GuardedRead(void* dest_host, guest_addr_t src_guest, u64 size, u64* out_bytes_read) {
    if (out_bytes_read) *out_bytes_read = 0;
    if (size == 0) return true;
    if (!dest_host) return false;

    // Fast path: if entirely readable upfront, single memcpy
    if (IsReadable(src_guest, size)) {
        std::memcpy(dest_host, reinterpret_cast<const void*>(src_guest), size);
        if (out_bytes_read) *out_bytes_read = size;
        return true;
    }

    constexpr u64 kHostPage = 4096;
    u64 copied = 0;
    guest_addr_t current_src = src_guest;
    u8* current_dst = reinterpret_cast<u8*>(dest_host);
    u64 remaining = size;

    while (remaining > 0) {
        u64 page_offset = current_src & (kHostPage - 1);
        u64 chunk = kHostPage - page_offset;
        if (chunk > remaining) chunk = remaining;

        if (!IsReadable(current_src, chunk)) {
            CommitOnFault(current_src);
            if (!IsReadable(current_src, chunk)) {
                LOG_WARN(Memory, "GuardedRead: invalid read at 0x%llx (copied %llu of %llu bytes)",
                         current_src, copied, size);
                if (out_bytes_read) *out_bytes_read = copied;
                return false;
            }
        }

        std::memcpy(current_dst, reinterpret_cast<const void*>(current_src), chunk);
        current_src += chunk;
        current_dst += chunk;
        copied += chunk;
        remaining -= chunk;
    }

    if (out_bytes_read) *out_bytes_read = copied;
    return true;
}

bool GuardedWrite(guest_addr_t dest_guest, const void* src_host, u64 size, u64* out_bytes_written) {
    if (out_bytes_written) *out_bytes_written = 0;
    if (size == 0) return true;
    if (!src_host) return false;

    // Fast path: if entirely writable upfront, single memcpy
    if (IsWritable(dest_guest, size)) {
        std::memcpy(reinterpret_cast<void*>(dest_guest), src_host, size);
        if (out_bytes_written) *out_bytes_written = size;
        return true;
    }

    constexpr u64 kHostPage = 4096;
    u64 copied = 0;
    guest_addr_t current_dest = dest_guest;
    const u8* current_src = reinterpret_cast<const u8*>(src_host);
    u64 remaining = size;

    while (remaining > 0) {
        u64 page_offset = current_dest & (kHostPage - 1);
        u64 chunk = kHostPage - page_offset;
        if (chunk > remaining) chunk = remaining;

        if (!IsWritable(current_dest, chunk)) {
            CommitOnFault(current_dest);
            if (!IsWritable(current_dest, chunk)) {
                LOG_WARN(Memory, "GuardedWrite: invalid write at 0x%llx (copied %llu of %llu bytes)",
                         current_dest, copied, size);
                if (out_bytes_written) *out_bytes_written = copied;
                return false;
            }
        }

        std::memcpy(reinterpret_cast<void*>(current_dest), current_src, chunk);
        current_dest += chunk;
        current_src  += chunk;
        copied       += chunk;
        remaining    -= chunk;
    }

    if (out_bytes_written) *out_bytes_written = copied;
    return true;
}

bool GuardedCopy(guest_addr_t dest_guest, guest_addr_t src_guest, u64 size, u64* out_bytes_copied) {
    if (out_bytes_copied) *out_bytes_copied = 0;
    if (size == 0) return true;
    if (dest_guest == src_guest) {
        if (out_bytes_copied) *out_bytes_copied = size;
        return true;
    }

    // H4.6: reject sign-extended SCE error codes masquerading as pointers
    constexpr u64 kBadPtrMask  = 0xFFFFFFFF80000000ULL;
    constexpr u64 kBadPtrMatch = 0xFFFFFFFF80000000ULL;
    if ((dest_guest & kBadPtrMask) == kBadPtrMatch || (src_guest & kBadPtrMask) == kBadPtrMatch) {
        LOG_WARN(Memory, "GuardedCopy: bad pointer (looks like sign-extended error code): dest=0x%llx src=0x%llx size=%llu",
                 dest_guest, src_guest, size);
        return false;
    }

    // Fast path: if both ranges are valid upfront, single memmove
    if (IsReadable(src_guest, size) && IsWritable(dest_guest, size)) {
        std::memmove(reinterpret_cast<void*>(dest_guest),
                     reinterpret_cast<const void*>(src_guest), size);
        if (out_bytes_copied) *out_bytes_copied = size;
        return true;
    }

    // Pre-pass: try demand-committing all touched pages across both ranges
    auto DemandCommitSpan = [](guest_addr_t addr, u64 len, bool is_write) {
        constexpr u64 kHostPage = 4096;
        guest_addr_t cur = addr & ~(kHostPage - 1);
        guest_addr_t end = (addr + len - 1) & ~(kHostPage - 1);
        for (guest_addr_t p = cur; p <= end; p += kHostPage) {
            if (is_write ? !IsWritable(p, 1) : !IsReadable(p, 1)) {
                CommitOnFault(p);
            }
        }
    };
    DemandCommitSpan(src_guest, size, false);
    DemandCommitSpan(dest_guest, size, true);

    if (IsReadable(src_guest, size) && IsWritable(dest_guest, size)) {
        std::memmove(reinterpret_cast<void*>(dest_guest),
                     reinterpret_cast<const void*>(src_guest), size);
        if (out_bytes_copied) *out_bytes_copied = size;
        return true;
    }

    // Partial/Fault path: chunk-by-chunk copy stopping at fault boundary
    constexpr u64 kHostPage = 4096;
    u64 copied = 0;
    u64 remaining = size;
    bool success = true;

    // Determine direction for overlapping ranges (memmove semantics)
    const bool overlap_backward = (dest_guest > src_guest && dest_guest < src_guest + size);

    if (!overlap_backward) {
        guest_addr_t cur_src  = src_guest;
        guest_addr_t cur_dest = dest_guest;
        while (remaining > 0) {
            u64 src_off  = cur_src & (kHostPage - 1);
            u64 dest_off = cur_dest & (kHostPage - 1);
            u64 chunk = std::min(kHostPage - src_off, kHostPage - dest_off);
            if (chunk > remaining) chunk = remaining;

            if (!IsReadable(cur_src, chunk)) {
                CommitOnFault(cur_src);
                if (!IsReadable(cur_src, chunk)) {
                    LOG_WARN(Memory, "GuardedCopy: read fault at 0x%llx (copied %llu of %llu bytes)", cur_src, copied, size);
                    success = false;
                    break;
                }
            }
            if (!IsWritable(cur_dest, chunk)) {
                CommitOnFault(cur_dest);
                if (!IsWritable(cur_dest, chunk)) {
                    LOG_WARN(Memory, "GuardedCopy: write fault at 0x%llx (copied %llu of %llu bytes)", cur_dest, copied, size);
                    success = false;
                    break;
                }
            }

            std::memmove(reinterpret_cast<void*>(cur_dest),
                         reinterpret_cast<const void*>(cur_src), chunk);
            cur_src   += chunk;
            cur_dest  += chunk;
            copied    += chunk;
            remaining -= chunk;
        }
    } else {
        // Backward copy for overlapping dest > src
        guest_addr_t cur_src  = src_guest + size;
        guest_addr_t cur_dest = dest_guest + size;
        while (remaining > 0) {
            u64 src_off  = cur_src & (kHostPage - 1);
            u64 dest_off = cur_dest & (kHostPage - 1);
            if (src_off == 0) src_off = kHostPage;
            if (dest_off == 0) dest_off = kHostPage;
            u64 chunk = std::min(src_off, dest_off);
            if (chunk > remaining) chunk = remaining;

            guest_addr_t chunk_src  = cur_src - chunk;
            guest_addr_t chunk_dest = cur_dest - chunk;

            if (!IsReadable(chunk_src, chunk)) {
                CommitOnFault(chunk_src);
                if (!IsReadable(chunk_src, chunk)) {
                    LOG_WARN(Memory, "GuardedCopy: backward read fault at 0x%llx (copied %llu of %llu bytes)", chunk_src, copied, size);
                    success = false;
                    break;
                }
            }
            if (!IsWritable(chunk_dest, chunk)) {
                CommitOnFault(chunk_dest);
                if (!IsWritable(chunk_dest, chunk)) {
                    LOG_WARN(Memory, "GuardedCopy: backward write fault at 0x%llx (copied %llu of %llu bytes)", chunk_dest, copied, size);
                    success = false;
                    break;
                }
            }

            std::memmove(reinterpret_cast<void*>(chunk_dest),
                         reinterpret_cast<const void*>(chunk_src), chunk);
            cur_src   -= chunk;
            cur_dest  -= chunk;
            copied    += chunk;
            remaining -= chunk;
        }
    }

    if (out_bytes_copied) *out_bytes_copied = copied;
    return success;
}

bool GuardedSet(guest_addr_t dest_guest, int value, u64 size, u64* out_bytes_set) {
    if (out_bytes_set) *out_bytes_set = 0;
    if (size == 0) return true;

    // Fast path: if entirely writable upfront, single memset
    if (IsWritable(dest_guest, size)) {
        std::memset(reinterpret_cast<void*>(dest_guest), value, size);
        if (out_bytes_set) *out_bytes_set = size;
        return true;
    }

    constexpr u64 kHostPage = 4096;
    u64 set_count = 0;
    guest_addr_t cur_dest = dest_guest;
    u64 remaining = size;

    while (remaining > 0) {
        u64 dest_off = cur_dest & (kHostPage - 1);
        u64 chunk = kHostPage - dest_off;
        if (chunk > remaining) chunk = remaining;

        if (!IsWritable(cur_dest, chunk)) {
            CommitOnFault(cur_dest);
            if (!IsWritable(cur_dest, chunk)) {
                LOG_WARN(Memory, "GuardedSet: write fault at 0x%llx (set %llu of %llu bytes)", cur_dest, set_count, size);
                if (out_bytes_set) *out_bytes_set = set_count;
                return false;
            }
        }

        std::memset(reinterpret_cast<void*>(cur_dest), value, chunk);
        cur_dest  += chunk;
        set_count += chunk;
        remaining -= chunk;
    }

    if (out_bytes_set) *out_bytes_set = set_count;
    return true;
}

u64 GuardedStrlen(guest_addr_t str_guest, u64 max_len) {
    if (!str_guest || max_len == 0) return 0;

    constexpr u64 kHostPage = 4096;
    guest_addr_t cur = str_guest;
    u64 len = 0;

    while (len < max_len) {
        u64 page_offset = cur & (kHostPage - 1);
        u64 chunk = kHostPage - page_offset;
        if (len + chunk > max_len) chunk = max_len - len;

        if (!IsReadable(cur, chunk)) {
            CommitOnFault(cur);
            if (!IsReadable(cur, chunk)) {
                u64 valid_bytes = 0;
                while (valid_bytes < chunk && IsReadable(cur + valid_bytes, 1)) {
                    u8 b = *reinterpret_cast<const u8*>(cur + valid_bytes);
                    if (b == 0) return len + valid_bytes;
                    ++valid_bytes;
                }
                LOG_DEBUG(Memory, "GuardedStrlen: reached unmapped boundary at 0x%llx (len=%llu)", cur + valid_bytes, len + valid_bytes);
                return len + valid_bytes;
            }
        }

        const char* p = reinterpret_cast<const char*>(cur);
        const void* hit = std::memchr(p, 0, chunk);
        if (hit) {
            return len + (reinterpret_cast<const char*>(hit) - p);
        }

        cur += chunk;
        len += chunk;
    }

    return len;
}

guest_addr_t GuardedStrcpy(guest_addr_t dest_guest, guest_addr_t src_guest, u64 max_len) {
    if (!dest_guest || !src_guest || max_len == 0) return dest_guest;

    constexpr u64 kHostPage = 4096;
    guest_addr_t cur_src  = src_guest;
    guest_addr_t cur_dest = dest_guest;
    u64 copied = 0;

    while (copied < max_len) {
        u64 src_off  = cur_src & (kHostPage - 1);
        u64 dest_off = cur_dest & (kHostPage - 1);
        u64 chunk = std::min(kHostPage - src_off, kHostPage - dest_off);
        if (copied + chunk > max_len) chunk = max_len - copied;

        if (!IsReadable(cur_src, chunk)) {
            CommitOnFault(cur_src);
            if (!IsReadable(cur_src, chunk)) {
                LOG_WARN(Memory, "GuardedStrcpy: read fault at src 0x%llx (copied %llu bytes)", cur_src, copied);
                break;
            }
        }
        if (!IsWritable(cur_dest, chunk)) {
            CommitOnFault(cur_dest);
            if (!IsWritable(cur_dest, chunk)) {
                LOG_WARN(Memory, "GuardedStrcpy: write fault at dest 0x%llx (copied %llu bytes)", cur_dest, copied);
                break;
            }
        }

        const char* s = reinterpret_cast<const char*>(cur_src);
        char* d       = reinterpret_cast<char*>(cur_dest);
        const void* hit = std::memchr(s, 0, chunk);
        if (hit) {
            u64 bytes_to_null = (reinterpret_cast<const char*>(hit) - s) + 1; // include \0
            std::memcpy(d, s, bytes_to_null);
            return dest_guest;
        }

        std::memcpy(d, s, chunk);
        cur_src  += chunk;
        cur_dest += chunk;
        copied   += chunk;
    }

    return dest_guest;
}

guest_addr_t GuardedStrncpy(guest_addr_t dest_guest, guest_addr_t src_guest, u64 count) {
    if (!dest_guest || count == 0) return dest_guest;
    if (!src_guest) {
        GuardedSet(dest_guest, 0, count);
        return dest_guest;
    }

    constexpr u64 kHostPage = 4096;
    guest_addr_t cur_src  = src_guest;
    guest_addr_t cur_dest = dest_guest;
    u64 copied = 0;
    bool found_null = false;

    while (copied < count) {
        u64 src_off  = cur_src & (kHostPage - 1);
        u64 dest_off = cur_dest & (kHostPage - 1);
        u64 chunk = std::min(kHostPage - src_off, kHostPage - dest_off);
        if (copied + chunk > count) chunk = count - copied;

        if (!found_null) {
            if (!IsReadable(cur_src, chunk)) {
                CommitOnFault(cur_src);
                if (!IsReadable(cur_src, chunk)) {
                    LOG_WARN(Memory, "GuardedStrncpy: read fault at src 0x%llx (copied %llu of %llu bytes)", cur_src, copied, count);
                    GuardedSet(cur_dest, 0, count - copied);
                    return dest_guest;
                }
            }
        }
        if (!IsWritable(cur_dest, chunk)) {
            CommitOnFault(cur_dest);
            if (!IsWritable(cur_dest, chunk)) {
                LOG_WARN(Memory, "GuardedStrncpy: write fault at dest 0x%llx (copied %llu of %llu bytes)", cur_dest, copied, count);
                return dest_guest;
            }
        }

        if (!found_null) {
            const char* s = reinterpret_cast<const char*>(cur_src);
            char* d       = reinterpret_cast<char*>(cur_dest);
            const void* hit = std::memchr(s, 0, chunk);
            if (hit) {
                u64 bytes_to_null = (reinterpret_cast<const char*>(hit) - s) + 1; // includes \0
                std::memcpy(d, s, bytes_to_null);
                if (chunk > bytes_to_null) {
                    std::memset(d + bytes_to_null, 0, chunk - bytes_to_null);
                }
                found_null = true;
            } else {
                std::memcpy(d, s, chunk);
            }
        } else {
            std::memset(reinterpret_cast<void*>(cur_dest), 0, chunk);
        }

        cur_src  += chunk;
        cur_dest += chunk;
        copied   += chunk;
    }

    return dest_guest;
}

int GuardedStrcmp(guest_addr_t a_guest, guest_addr_t b_guest, u64 max_len) {
    if (a_guest == b_guest) return 0;
    if (!a_guest) return -1;
    if (!b_guest) return 1;

    constexpr u64 kHostPage = 4096;
    guest_addr_t cur_a = a_guest;
    guest_addr_t cur_b = b_guest;
    u64 checked = 0;

    while (checked < max_len) {
        u64 a_off = cur_a & (kHostPage - 1);
        u64 b_off = cur_b & (kHostPage - 1);
        u64 chunk = std::min(kHostPage - a_off, kHostPage - b_off);
        if (checked + chunk > max_len) chunk = max_len - checked;

        if (!IsReadable(cur_a, chunk)) {
            CommitOnFault(cur_a);
            if (!IsReadable(cur_a, chunk)) return -1;
        }
        if (!IsReadable(cur_b, chunk)) {
            CommitOnFault(cur_b);
            if (!IsReadable(cur_b, chunk)) return 1;
        }

        const u8* pa = reinterpret_cast<const u8*>(cur_a);
        const u8* pb = reinterpret_cast<const u8*>(cur_b);
        for (u64 i = 0; i < chunk; ++i) {
            if (pa[i] != pb[i]) {
                return (pa[i] < pb[i]) ? -1 : 1;
            }
            if (pa[i] == 0) return 0;
        }

        cur_a   += chunk;
        cur_b   += chunk;
        checked += chunk;
    }

    return 0;
}

int GuardedStrncmp(guest_addr_t a_guest, guest_addr_t b_guest, u64 count) {
    if (count == 0 || a_guest == b_guest) return 0;
    if (!a_guest) return -1;
    if (!b_guest) return 1;

    constexpr u64 kHostPage = 4096;
    guest_addr_t cur_a = a_guest;
    guest_addr_t cur_b = b_guest;
    u64 checked = 0;

    while (checked < count) {
        u64 a_off = cur_a & (kHostPage - 1);
        u64 b_off = cur_b & (kHostPage - 1);
        u64 chunk = std::min(kHostPage - a_off, kHostPage - b_off);
        if (checked + chunk > count) chunk = count - checked;

        if (!IsReadable(cur_a, chunk)) {
            CommitOnFault(cur_a);
            if (!IsReadable(cur_a, chunk)) return -1;
        }
        if (!IsReadable(cur_b, chunk)) {
            CommitOnFault(cur_b);
            if (!IsReadable(cur_b, chunk)) return 1;
        }

        const u8* pa = reinterpret_cast<const u8*>(cur_a);
        const u8* pb = reinterpret_cast<const u8*>(cur_b);
        for (u64 i = 0; i < chunk; ++i) {
            if (pa[i] != pb[i]) {
                return (pa[i] < pb[i]) ? -1 : 1;
            }
            if (pa[i] == 0) return 0;
        }

        cur_a   += chunk;
        cur_b   += chunk;
        checked += chunk;
    }

    return 0;
}

int GuardedMemcmp(guest_addr_t a_guest, guest_addr_t b_guest, u64 count) {
    if (count == 0 || a_guest == b_guest) return 0;
    if (!a_guest) return -1;
    if (!b_guest) return 1;

    // Fast path: if both are readable, single memcmp
    if (IsReadable(a_guest, count) && IsReadable(b_guest, count)) {
        return std::memcmp(reinterpret_cast<const void*>(a_guest),
                           reinterpret_cast<const void*>(b_guest), count);
    }

    constexpr u64 kHostPage = 4096;
    guest_addr_t cur_a = a_guest;
    guest_addr_t cur_b = b_guest;
    u64 checked = 0;

    while (checked < count) {
        u64 a_off = cur_a & (kHostPage - 1);
        u64 b_off = cur_b & (kHostPage - 1);
        u64 chunk = std::min(kHostPage - a_off, kHostPage - b_off);
        if (checked + chunk > count) chunk = count - checked;

        if (!IsReadable(cur_a, chunk)) {
            CommitOnFault(cur_a);
            if (!IsReadable(cur_a, chunk)) return -1;
        }
        if (!IsReadable(cur_b, chunk)) {
            CommitOnFault(cur_b);
            if (!IsReadable(cur_b, chunk)) return 1;
        }

        const int res = std::memcmp(reinterpret_cast<const void*>(cur_a),
                                    reinterpret_cast<const void*>(cur_b), chunk);
        if (res != 0) return res;

        cur_a   += chunk;
        cur_b   += chunk;
        checked += chunk;
    }

    return 0;
}

bool CommitOnFault(guest_addr_t address) {
    constexpr u64 kGranularity = 65536; // Windows allocation granularity
    const guest_addr_t base = address & ~(kGranularity - 1);
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi)) == 0) {
        LOG_WARN(Memory, "CommitOnFault: VirtualQuery failed at 0x%llx", address);
        return false;
    }
    if (mbi.State == MEM_COMMIT) {
        return true; // Already committed
    }
    if (mbi.State != MEM_RESERVE) {
        LOG_DEBUG(Memory, "CommitOnFault: page at 0x%llx not MEM_RESERVE (state=%u) — skipping", address, (u32)mbi.State);
        return false;
    }
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
    // O1.4: coalesce with adjacent ranges to reduce VirtualProtect calls.
    for (auto& r : g_write_ranges) {
        if (r.start + r.length == start) {
            // Adjacent after existing range — extend.
            r.length += length;
            if (r.armed) {
                DisarmWriteRangeLocked(r);
                ArmWriteRangeLocked(r);
            }
            return;
        }
        if (start + length == r.start) {
            // Adjacent before existing range — extend backward.
            const u64 new_len = r.length + length;
            r.start = start;
            r.length = new_len;
            if (r.armed) {
                DisarmWriteRangeLocked(r);
                ArmWriteRangeLocked(r);
            }
            return;
        }
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
