#pragma once
#include "hle.h"

namespace HLE {

// ---------------------------------------------------------------------------
// Individual handlers from libkernel.cpp, exported so unit tests can drive
// them directly with a GuestArgs struct (no thunk/dispatcher round-trip
// needed) — same pattern as libkernel_sync.h.
// ---------------------------------------------------------------------------

// POSIX-named file exports (libc ABI): 0/fd/byte-count on success, -1 with
// the guest errno set (SetGuestErrno) on failure.
u64 PosixOpen(const GuestArgs& args);
u64 PosixClose(const GuestArgs& args);
u64 PosixRead(const GuestArgs& args);
u64 PosixWrite(const GuestArgs& args);
u64 PosixFstat(const GuestArgs& args);
u64 PosixStat(const GuestArgs& args);

// sceKernelAllocateDirectMemory / sceKernelMapDirectMemory / sceKernelMapDirectMemory2.
u64 SceKernelAllocateDirectMemory(const GuestArgs& args);
u64 SceKernelMapDirectMemory(const GuestArgs& args);
u64 SceKernelMapDirectMemory2(const GuestArgs& args);

} // namespace HLE
