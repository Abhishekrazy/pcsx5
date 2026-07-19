#include "guest_clock.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace Kernel {

namespace {
    struct GuestClockState {
        u64 freq   = 0; // QPC frequency (Hz)
        u64 origin = 0; // QPC value at first use
    };

    const GuestClockState& State() {
        // Function-local static: initialized exactly once, thread-safe (C++11).
        static const GuestClockState s = [] {
            GuestClockState st;
            LARGE_INTEGER f, c;
            QueryPerformanceFrequency(&f);
            QueryPerformanceCounter(&c);
            st.freq   = static_cast<u64>(f.QuadPart);
            st.origin = static_cast<u64>(c.QuadPart);
            return st;
        }();
        return s;
    }
} // namespace

u64 GuestClockCounter() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<u64>(c.QuadPart);
}

u64 GuestClockCounterFrequency() {
    return State().freq;
}

u64 GuestClockMicros() {
    const GuestClockState& s = State();
    const u64 delta = GuestClockCounter() - s.origin;
    // Split into sec/rem to avoid overflow on long uptimes.
    const u64 sec = delta / s.freq;
    const u64 rem = delta % s.freq;
    return sec * 1000000ULL + (rem * 1000000ULL) / s.freq;
}

void GuestClockRealtime(s64* out_sec, s64* out_nsec) {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER ul;
    ul.LowPart  = ft.dwLowDateTime;
    ul.HighPart = ft.dwHighDateTime;
    const u64 unix_100ns = ul.QuadPart - 116444736000000000ULL;
    if (out_sec)  *out_sec  = static_cast<s64>(unix_100ns / 10000000ULL);
    if (out_nsec) *out_nsec = static_cast<s64>((unix_100ns % 10000000ULL) * 100ULL);
}

} // namespace Kernel
