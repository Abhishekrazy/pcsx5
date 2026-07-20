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

// sceKernelMapDirectMemory / sceKernelMapDirectMemory2.  The "2" variant
// takes an extra memoryType argument (rdx) that shifts protection/flags/
// directMemoryStart down one register each and passes alignment as the 7th
// (stack) argument via GuestArgs::stack_args.
u64 SceKernelMapDirectMemory(const GuestArgs& args);
u64 SceKernelMapDirectMemory2(const GuestArgs& args);

} // namespace HLE
