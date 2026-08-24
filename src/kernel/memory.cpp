#include "memory.h"
#include "memory/memory.h"
#include "../common/log.h"
#include <windows.h>
#include <algorithm>

// ---------------------------------------------------------------------------
// Kernel guest-memory front end.
//
// Stage 2: this module no longer owns a guest VA allocator.  All placement,
// reservation, commit, protection and host-backing decisions live in
// Memory:: (src/memory) — the single guest VA authority.  What remains here
// is the syscall-facing translation layer (POSIX mmap/munmap/brk semantics
// onto the Memory contract) plus kernel-visible metadata.
//
// The previous implementation bumped a static pointer through a private
// 4 TB window (0x400000000000..) with direct VirtualAlloc calls — invisible
// to Memory's region table and unreachable from the fault classifier.  No
// shipped title exercises these syscalls (they are Linux-number syscalls;
// PS5 guests use the sceKernel* HLE), so delegation changes no boot behavior
// while removing the second allocator.
// ---------------------------------------------------------------------------

namespace Kernel {

namespace {
guest_addr_t g_brk_base = 0;   // first brk allocation
guest_addr_t g_brk_current = 0;
constexpr u64 kBrkMaxSize = 256ULL * 1024 * 1024;  // sane ceiling for brk growth
} // namespace

// Initialize/shutdown: Memory::Initialize/Shutdown own the actual state; the
// Lua subsystem registry initializes Memory before Kernel, so these are just
// liveness checks with a clear error if ordering is ever violated.
void InitializeGuestMemory() {
    // Touch the manager cheaply to confirm it is live (Query on page 0 is
    // NotMapped but proves the region lock works).
    Memory::MemoryInfo info{};
    (void)Memory::Query(0, &info);
}

void ShutdownGuestMemory() {
    // Memory::Shutdown releases all manager-owned ranges.
    // Reset kernel-visible BRK heap cursors.
    g_brk_base = 0;
    g_brk_current = 0;
}

// Allocate guest memory — anonymous mmap-style allocation via the manager.
guest_addr_t AllocGuestMemory(u64 size, u64 alignment, int prot, int flags, int fd, s64 offset) {
    (void)flags; (void)fd; (void)offset;

    u32 mprot = Memory::PROT_NONE;
    if (prot & PROT_READ)  mprot |= Memory::PROT_READ;
    if (prot & PROT_WRITE) mprot |= Memory::PROT_WRITE;
    if (prot & PROT_EXEC)  mprot |= Memory::PROT_EXEC;
    if (mprot == Memory::PROT_NONE) mprot = Memory::PROT_READ | Memory::PROT_WRITE;

    guest_addr_t addr = 0;
    // MAP_FIXED callers go through MapGuestMemory; this entry always lets the
    // manager place.  Alignment below page granularity is meaningless to the
    // host (64 KiB allocation granularity); larger alignments pass through.
    const u64 align = (alignment > 0x1000) ? alignment : 0x1000;
    if (Memory::Map(0, size, mprot, &addr) != Memory::Status::Ok && align > 0x1000) {
        // Retry honoring an explicit large alignment via Reserve+Commit at a
        // manager-chosen aligned address.
        guest_addr_t reserved = 0;
        const u64 padded = size + align;
        if (Memory::Reserve(0, padded, &reserved) == Memory::Status::Ok) {
            const guest_addr_t aligned = (reserved + align - 1) & ~(align - 1);
            if (Memory::Commit(aligned, size, mprot) == Memory::Status::Ok &&
                Memory::Unmap(reserved, aligned - reserved) == Memory::Status::Ok) {
                return aligned;
            }
            Memory::Unmap(reserved, padded);
            return 0;
        }
        return 0;
    }
    return addr;
}

bool FreeGuestMemory(guest_addr_t addr, u64 size) {
    return Memory::Unmap(addr, size) == Memory::Status::Ok ||
           Memory::ReleaseRange(addr) == Memory::Status::Ok;
}

bool ProtectGuestMemory(guest_addr_t addr, u64 size, int prot) {
    u32 mprot = Memory::PROT_NONE;
    if (prot & PROT_READ)  mprot |= Memory::PROT_READ;
    if (prot & PROT_WRITE) mprot |= Memory::PROT_WRITE;
    if (prot & PROT_EXEC)  mprot |= Memory::PROT_EXEC;
    return Memory::Protect(addr, size, mprot) == Memory::Status::Ok;
}

bool GetGuestMemoryInfo(guest_addr_t addr, MEMORY_BASIC_INFORMATION* info) {
    // The caller wants raw host VM state (used by diagnostics paths).
    // VirtualQuery is the ground truth for committed/reserved/free; the
    // region table refines ownership but MBI is what this API promises.
    if (!info) return false;
    return VirtualQuery(reinterpret_cast<void*>(static_cast<uintptr_t>(addr)),
                        info, sizeof(MEMORY_BASIC_INFORMATION)) != 0;
}

bool IsValidGuestAddress(guest_addr_t addr, u64 size) {
    return Memory::IsReadable(addr, size) || Memory::IsRangeFree(addr, size) == false;
}

// mmap syscall front end.
guest_addr_t MapGuestMemory(guest_addr_t addr, u64 length, int prot, int flags, int fd, s64 offset) {
    (void)fd; (void)offset;

    u32 mprot = Memory::PROT_NONE;
    if (prot & PROT_READ)  mprot |= Memory::PROT_READ;
    if (prot & PROT_WRITE) mprot |= Memory::PROT_WRITE;
    if (prot & PROT_EXEC)  mprot |= Memory::PROT_EXEC;
    if (mprot == Memory::PROT_NONE) mprot = Memory::PROT_READ | Memory::PROT_WRITE;

    const bool fixed = (flags & MAP_FIXED) != 0;
    if (addr == 0 || !fixed) {
        // Manager places when no fixed address requested.  A non-zero
        // non-fixed addr is a hint: try it first, fall back to placement.
        if (addr != 0) {
            guest_addr_t out = 0;
            if (Memory::Map(addr, length, mprot, &out) == Memory::Status::Ok) {
                return out;
            }
        }
        guest_addr_t out = 0;
        if (Memory::Map(0, length, mprot, &out) != Memory::Status::Ok) {
            return 0;
        }
        return out;
    }

    // Fixed mapping: deterministic failure when unavailable.
    guest_addr_t out = 0;
    if (Memory::Map(addr, length, mprot, &out) != Memory::Status::Ok) {
        return 0;
    }
    return out;
}

bool UnmapGuestMemory(guest_addr_t addr, u64 length) {
    const Memory::Status st = Memory::Unmap(addr, length);
    return st == Memory::Status::Ok || st == Memory::Status::NotMapped;
}

// brk support: the classic bump-cursor semantics over the manager.  The base
// is established once from a real manager allocation rather than a hardcoded
// magic address, so it participates in the same address space as everything
// else.
guest_addr_t GetBreak() {
    return g_brk_current;
}

guest_addr_t SetBreak(guest_addr_t new_break) {
    if (g_brk_base == 0) {
        // First brk call: establish the heap through the manager.
        guest_addr_t heap = 0;
        if (Memory::Map(0, kBrkMaxSize, Memory::PROT_READ | Memory::PROT_WRITE, &heap)
                != Memory::Status::Ok) {
            LOG_ERROR(Kernel, "SetBreak: failed to establish brk heap");
            return 0;
        }
        g_brk_base = heap;
        g_brk_current = heap;
        LOG_DEBUG(Kernel, "brk heap established at 0x%llx (%llu MB)",
                  heap, kBrkMaxSize / (1024 * 1024));
    }

    if (new_break < g_brk_base) return g_brk_current;
    if (new_break > g_brk_base + kBrkMaxSize) return g_brk_current;

    // The heap is one committed manager mapping; moving the cursor needs no
    // host calls in either direction (pages beyond the old break were never
    // guaranteed zero anyway under this simplified model).
    g_brk_current = new_break;
    return g_brk_current;
}

} // namespace Kernel
