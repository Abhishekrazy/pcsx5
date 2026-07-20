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
