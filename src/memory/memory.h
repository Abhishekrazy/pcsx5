#pragma once
//
// Guest virtual memory manager.
//
// The current implementation maps guest virtual addresses directly to host
// virtual addresses via Win32 VirtualAlloc.  This keeps the data path
// pointer-fast (no shadow page tables) at the cost of tying the guest VA
// space to whatever the host kernel is willing to give us.  All addresses
// returned by these functions are valid for direct pointer use in the
// emulator core.
//
// Every public function returns a `Status` value so callers can distinguish
// between "out of memory", "argument was bogus", and "the OS said no".
// The legacy bool/address-returning API below remains for backwards
// compatibility with existing HLE / loader / kernel call sites.
//
#include "../common/types.h"
#include <cstddef>

namespace Memory {

// ---------------------------------------------------------------------------
// Status / info
// ---------------------------------------------------------------------------
enum class Status {
    Ok              = 0,
    InvalidArgument = 1,  // 0 size, unaligned, null out pointer, ...
    OutOfMemory     = 2,  // VirtualAlloc returned NULL
    NotMapped       = 3,  // Query/Unmap/Protect on an unmapped range
    AccessDenied    = 4,  // VirtualProtect failed (e.g. page is a guard)
    Win32Error      = 5,  // Unspecified VirtualAlloc / VirtualFree failure
};

// Returns a human-readable name.  Named "StatusAsString" to avoid a parse
// ambiguity with the "Memory::Status" enum class prefix under MSVC.
const char* StatusAsString(Status s);

struct MemoryInfo {
    guest_addr_t base_address    = 0;  // page-aligned base of the containing region
    u64          size            = 0;  // size of the containing region in bytes
    u32          protection      = 0;  // PROT_* bitmask
    bool         is_committed    = false; // true if pages are MEM_COMMIT, false if just reserved
    bool         is_reserved     = false; // true if MEM_RESERVE
    u32          win32_protection = 0;    // raw PAGE_* value (for diagnostics)
};

struct MemoryStats {
    u64 total_reserved  = 0; // bytes of MEM_RESERVE
    u64 total_committed = 0; // bytes of MEM_COMMIT
    u64 region_count    = 0; // number of tracked regions
};

// PS5 / FreeBSD memory protection flags
constexpr u32 PROT_NONE  = 0x0;
constexpr u32 PROT_READ  = 0x1;
constexpr u32 PROT_WRITE = 0x2;
constexpr u32 PROT_EXEC  = 0x4;

// Guest module window — where PIE images and PRX modules live.  Mirrors the
// loader's constants (elf.cpp kPieBaseHint/kGuestWindowEnd); kept here so
// AllocateRange and fault classification agree with module placement.
// The full guest VA space is NOT limited to this window (framebuffer at
// 0x200000000, kernel-chosen pool/heap addresses outside it), but module
// auto-placement only ever picks from inside it.
constexpr guest_addr_t kGuestVaBase = 0x800000000ULL;
constexpr guest_addr_t kGuestVaEnd  = 0x900000000ULL;

// ---------------------------------------------------------------------------
// Init / shutdown
// ---------------------------------------------------------------------------
bool Initialize();
void Shutdown();

// ---------------------------------------------------------------------------
// Primary API (returns Status; out_addr receives the actual base address)
// ---------------------------------------------------------------------------
Status Map    (guest_addr_t address, u64 size, u32 protection, guest_addr_t* out_addr);
Status Reserve(guest_addr_t address, u64 size, guest_addr_t* out_addr);
Status Commit (guest_addr_t address, u64 size, u32 protection);
Status Unmap  (guest_addr_t address, u64 size);
Status Protect(guest_addr_t address, u64 size, u32 protection);

// Query the page-aligned region containing `address`.  Returns NotMapped if
// the address falls outside every known region.
Status Query(guest_addr_t address, MemoryInfo* out_info);

// Convenience predicates.  Walk every page in the range, return false on
// the first page that is not committed with the requested access.
bool IsReadable (guest_addr_t address, u64 size);
bool IsWritable (guest_addr_t address, u64 size);
bool IsExecutable(guest_addr_t address, u64 size);

// Aggregated view of the regions currently tracked by the manager.
MemoryStats GetStats();

// ---------------------------------------------------------------------------
// Guest VA range ownership (Stage 2 contract).
//
// Every guest VA range is owned by exactly one named subsystem.  Ownership is
// explicit so that "who allocated this address" is queryable and so that a
// released range can be reused deterministically.  The loader expresses a
// preferred address via Reserve/AllocateRange; Memory decides availability —
// it never silently relocates (the caller walks fallback hints and logs why).
//
//   Reserve(address, size)      — reserve VA, no host commit (PROT_NONE).
//   Commit(address, size, prot) — back reserved pages / adjust protection.
//   Map(address, size, prot)    — reserve + commit in one step.
//   Unmap(address, size)        — release host backing AND the reservation
//                                 (range becomes free for reuse).
//   AllocateRange(size, align, owner, name, out) — owner-agnostic placement:
//                                 pick any free range in the guest module
//                                 window.  Used when no preferred address.
//   ReleaseRange(base)          — Unmap by ownership record; frees for reuse.
//
// Windows enforces physical single-ownership under the hood (VirtualAlloc
// refuses overlapping reservations), but only ranges that go through this
// API are *visible* to Query/IsValidGuestPointer.  Host-side allocations made
// outside this API (e.g. legacy HLE pools) must call AdoptRange to register.
// ---------------------------------------------------------------------------
enum class Owner : u32 {
    None    = 0,  // untracked / unknown
    Loader  = 1,  // guest module images (eboot, PRX)
    Kernel  = 2,  // kernel-managed guest allocations (stacks, TLS, mmap)
    Hle     = 3,  // HLE-owned (thunk page, phys pool, trampolines)
    Guest   = 4,  // guest-requested (ReserveVirtualRange / MapDirectMemory)
};

const char* OwnerAsString(Owner o);

// Find and reserve a free range of `size` bytes (aligned up to `alignment`,
// minimum 64 KiB allocation granularity).  Deterministic low-address-first
// policy inside [kGuestVaBase, kGuestVaEnd).  Returns OutOfMemory when no
// gap fits — the CALLER decides whether/how to fall back.
Status AllocateRange(u64 size, u64 alignment, Owner owner,
                     const char* name, guest_addr_t* out_addr);

// Release a previously reserved/mapped range by base address: unmaps host
// backing (if any), removes tracking, and marks the VA span reusable.
// Returns NotMapped when `base` does not start a tracked region.
Status ReleaseRange(guest_addr_t base);

// Who owns the containing region of `address`?  None when unmapped/untracked.
Owner QueryOwner(guest_addr_t address);

// True when [address, address+size) lies entirely outside every tracked
// region AND outside all host allocations we know of (pool interior counts
// as owned by Memory itself).  Cheap check for "can I Reserve here?"
bool IsRangeFree(guest_addr_t address, u64 size);

// Register a range that was allocated through Win32 directly (outside this
// API) so Query/IsValidGuestPointer/QueryOwner can see it.  Does NOT change
// host page state.  Use Owner::Hle or Owner::Kernel as appropriate.
Status AdoptRange(guest_addr_t address, u64 size, u32 protection,
                  bool committed, Owner owner, const char* name);

// Drop the tracking record for an adopted range WITHOUT touching host pages
// — the adopter keeps sole responsibility for freeing the host allocation
// (e.g. a thread stack VirtualFree'd by the CPU thread registry).  After this
// returns Ok, Query reports NotMapped for the range.  Returns NotMapped when
// no exactly-matching tracked region exists.
Status ForgetResource(guest_addr_t address);

// ---------------------------------------------------------------------------
// Returns true when `address` falls inside any tracked memory region
// (reserved or committed).  Fast check suitable for validating guest-
// supplied pointers in HLE before writing them to guest memory.
bool IsValidGuestPointer(guest_addr_t address);

// Read/write helpers (pointer-fast; no fault recovery)
// ---------------------------------------------------------------------------
template <typename T>
inline T Read(guest_addr_t addr) {
    return *reinterpret_cast<T*>(addr);
}

template <typename T>
inline void Write(guest_addr_t addr, T value) {
    *reinterpret_cast<T*>(addr) = value;
}

void ReadBuffer (guest_addr_t addr, void* dest, u64 size);
void WriteBuffer(guest_addr_t addr, const void* src, u64 size);

// ---------------------------------------------------------------------------
// Page-aware, fault-resilient guest memory primitives (Task 25).
//
// These functions validate guest memory page-by-page (4 KiB page boundaries),
// demand-committing any encountered MEM_RESERVE pages via CommitOnFault.
// If an unmapped (MEM_FREE) or invalidly protected page is encountered, the
// operation cleanly halts at the boundary, records the byte count successfully
// processed, and returns false (or the processed length) without raising a
// host Access Violation (0xC0000005) or relying on host SEH on guest stacks.
// ---------------------------------------------------------------------------
bool GuardedRead(void* dest_host, guest_addr_t src_guest, u64 size, u64* out_bytes_read = nullptr);
bool GuardedWrite(guest_addr_t dest_guest, const void* src_host, u64 size, u64* out_bytes_written = nullptr);
bool GuardedCopy(guest_addr_t dest_guest, guest_addr_t src_guest, u64 size, u64* out_bytes_copied = nullptr);
bool GuardedSet(guest_addr_t dest_guest, int value, u64 size, u64* out_bytes_set = nullptr);

u64          GuardedStrlen(guest_addr_t str_guest, u64 max_len = UINT64_MAX);
guest_addr_t GuardedStrcpy(guest_addr_t dest_guest, guest_addr_t src_guest, u64 max_len = UINT64_MAX);
guest_addr_t GuardedStrncpy(guest_addr_t dest_guest, guest_addr_t src_guest, u64 count);
int          GuardedStrcmp(guest_addr_t a_guest, guest_addr_t b_guest, u64 max_len = UINT64_MAX);
int          GuardedStrncmp(guest_addr_t a_guest, guest_addr_t b_guest, u64 count);
int          GuardedMemcmp(guest_addr_t a_guest, guest_addr_t b_guest, u64 count);

// ---------------------------------------------------------------------------
// Pointer <-> guest address translation
// ---------------------------------------------------------------------------
inline void* Translate(guest_addr_t addr) {
    return reinterpret_cast<void*>(addr);
}

inline guest_addr_t GetGuestAddress(void* host_ptr) {
    return reinterpret_cast<guest_addr_t>(host_ptr);
}

// ---------------------------------------------------------------------------
// Guest fault handling
//
// The emulator registers a top-of-chain handler for access violations that
// originate from guest code (i.e. the guest tried to read/write/exec an
// unmapped or wrongly-protected page).  The handler is invoked with the
// fault address and the Win32 exception code; returning true means the
// fault was handled and execution should resume, false means the original
// SEH filter should take over.
// ---------------------------------------------------------------------------
using GuestFaultHandler = bool (*)(guest_addr_t fault_address,
                                   u64 exception_code,
                                   void* user_data);

void SetGuestFaultHandler(GuestFaultHandler handler, void* user_data);
GuestFaultHandler GetGuestFaultHandler();
void* GetGuestFaultHandlerUserData();

// Demand-commit support: if `address` lies inside a tracked reserved
// (not yet committed) region, commit the 64 KiB block covering it as
// read/write and return true.  Returns false when the address is not
// covered by any reserved region.
bool CommitOnFault(guest_addr_t address);

// ---------------------------------------------------------------------------
// Guest image write tracking (port of SharpEmu's GuestImageWriteTracker,
// commit 04557fd "Refresh CPU-rewritten guest textures by write generation").
//
// A tracked range is armed read-only; the first CPU write faults through the
// guest VEH, which disarms the range and bumps its monotonic write
// generation.  Cache owners (the vk_draw texture upload path) record the
// generation they uploaded against and re-upload only when it changes, so a
// guest CPU rewrite of texture memory (video planes, streamed atlases) can
// never leave a stale VkImage behind.  Unlike a consumable dirty flag the
// generation survives one owner consuming and re-arming the range, which is
// what lets a second cache owner still observe the rewrite.
// ---------------------------------------------------------------------------

// Registers (or re-arms) write tracking for [address, address+byte_count).
// Re-tracking with a different size replaces the range but carries its
// generation, so resizes do not hide rewrites from cache owners.
void TrackGuestWrites(guest_addr_t address, u64 byte_count);
void UntrackGuestWrites(guest_addr_t address);

// Returns the monotonic first-write generation for a tracked range; false
// when the range is untracked (caller must treat content as always stale).
bool TryGetGuestWriteGeneration(guest_addr_t address, u64* generation_out);

// Re-arms write protection after the owner finished reading the guest
// bytes, so the next CPU write faults and bumps the generation again.
void RearmGuestWrites(guest_addr_t address);

} // namespace Memory
