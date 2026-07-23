// Platform Abstraction Layer (PAL) — abstract OS services.
//
// Wraps platform-specific APIs (Win32 VirtualAlloc, Linux mmap, macOS
// mach_vm, etc.) behind a uniform interface so the emulator core can
// be compiled for any target OS without #ifdef soup.
//
// Every function is designed to be a thin, zero-overhead wrapper over
// the native syscall — the abstraction lives at the type level, not
// in the runtime.

#pragma once
#include "../../common/types.h"
#include <cstddef>
#include <cstdint>
#include <string>

namespace Platform {

// ===========================================================================
// Memory management
// ===========================================================================

enum class MemoryProtection : int {
    None     = 0,
    Read     = 1,
    Write    = 2,
    ReadWrite = 3,
    Execute  = 4,
    ReadExecute = 5,
    ReadWriteExecute = 6,
};

enum class MemoryType : int {
    Reserve = 0,      // reserve address space, no physical memory
    Commit  = 1,      // reserve + commit physical memory
    LargePages = 2,   // commit with large page support (requires SeLockMemoryPrivilege)
};

// Reserve or commit a region of virtual memory.
// `address_hint` may be 0 (let the OS choose) or a specific base address.
// Returns the base address on success, 0 on failure.
void* VirtualAlloc(void* address_hint, size_t size,
                    MemoryType type, MemoryProtection prot);

// Release a previously allocated region.
bool VirtualFree(void* address, size_t size);

// Change protection on a committed region.
bool VirtualProtect(void* address, size_t size, MemoryProtection prot);

// Query the protection and base of the region containing `address`.
// Returns false when `address` is not inside any allocated region.
bool VirtualQuery(const void* address,
                  void** out_base, size_t* out_size,
                  MemoryProtection* out_prot);

// Enable SeLockMemoryPrivilege for the current process (Windows only).
// No-op on other platforms.  Required before VirtualAlloc(LargePages).
bool EnableLargePages();

// Returns the large-page size (typically 2 MiB), or 0 when unsupported.
size_t LargePageSize();

// ===========================================================================
// Thread utilities
// ===========================================================================

// Returns a unique identifier for the calling host thread (OS thread id).
uint64_t GetCurrentThreadId();

// Set a human-readable name for the calling thread (for debugger/profiler).
bool SetThreadName(const char* name);

// ===========================================================================
// Dynamic library loading
// ===========================================================================

void* LoadLibrary(const char* path);
void* GetProcAddress(void* library, const char* name);
bool  FreeLibrary(void* library);
bool  LibraryIsLoaded(const char* path);  // check without incrementing refcount

// ===========================================================================
// Exception / signal handling
// ===========================================================================

// Opaque handle for an installed fault handler.
using FaultHandlerHandle = void*;

// Callback invoked when a fault (access violation, illegal instruction,
// etc.) occurs at an address in the guest range.  Returns true if the
// handler resolved the fault and execution should resume, false to
// pass the fault to the next handler in the chain.
using FaultHandlerCallback = bool (*)(void* fault_address,
                                       uint32_t exception_code,
                                       void* user_data);

// Install a top-of-chain fault handler.  Returns a handle that can be
// passed to RemoveFaultHandler, or nullptr on failure.
FaultHandlerHandle InstallFaultHandler(FaultHandlerCallback callback,
                                        void* user_data);

// Remove a previously installed fault handler.
bool RemoveFaultHandler(FaultHandlerHandle handle);

// ===========================================================================
// Process info
// ===========================================================================

uint64_t GetProcessId();

// ===========================================================================
// CPU feature detection
// ===========================================================================

struct CpuFeatures {
    bool has_sse4a   = false;   // AMD EXTRQ/INSERTQ
    bool has_bmi1    = false;   // ANDN, BEXTR, etc.
    bool has_bmi2    = false;   // MULX, RORX, etc.
    bool has_abm     = false;   // POPCNT, LZCNT
    bool has_avx     = false;
    bool has_avx2    = false;
    bool has_avx512  = false;
};

CpuFeatures QueryCpuFeatures();

}  // namespace Platform
