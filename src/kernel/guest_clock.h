#pragma once
#include "../common/types.h"

namespace Kernel {

// Shared QueryPerformanceCounter-based guest clock.
//
// A single monotonic origin and frequency are captured lazily on first use
// and shared by every clock consumer (the sceKernelGetProcessTime family in
// HLE, sys_clock_gettime/sys_clock_getres, and HLE timeout bookkeeping), so
// all reported counters agree with each other and never drift due to
// per-call re-originating.

// Raw QPC tick count (monotonic, never re-originated).
u64 GuestClockCounter();

// QPC frequency in Hz (constant for the lifetime of the process).
u64 GuestClockCounterFrequency();

// Microseconds elapsed since the shared origin (process-start analogue).
u64 GuestClockMicros();

// Realtime (UTC) as seconds + nanoseconds, from GetSystemTimePreciseAsFileTime.
void GuestClockRealtime(s64* out_sec, s64* out_nsec);

} // namespace Kernel
