#pragma once
//
// guest_tracer.h — default-off guest CPU tracer (diagnostic).
//
// PCSX5 executes guest x64 natively.  This module logs every call to the
// Construct runtime's JSON string reader (guest 0x81012f790) in Dreaming Sarah
// (PPSA02929) — the parser's token start/cursor/string length + content, tagged
// with the OS thread id — to guest_trace.log.  It exists to isolate why a
// rebuilt binary frame record's image path is truncated from 33 bytes
// ("images/precious_stones-sheet0.png") to 5 ("image") on the background-loader
// worker threads.
//
// Enabled ONLY when the environment variable `PCSX5_GUEST_TRACE` is non-empty.
// When disabled every function is an inert no-op, so normal boot / other games
// are completely unaffected.
//

#include "../common/types.h"
#include <windows.h> // PCONTEXT, DWORD

namespace Kernel {

struct GuestTracer {
    // Object format is plain; all state is function-local / static to keep the
    // integration surface tiny and thread-safe enough for a diagnostic tool.

    // Called from the kernel VEH for every exception.  Returns true if the
    // exception was a single-step handled by the tracer (caller should then
    // return EXCEPTION_CONTINUE_EXECUTION), false to let the kernel continue
    // with normal handling.
    static bool HandleTrap(DWORD exception_code, PCONTEXT ctx);

    // Called from the VEH on the main guest-crash path (guest RIP) so the
    // tracer can arm TF / decide to trace for the current thread.
    static void NotifyGuestRip(u64 rip, PCONTEXT ctx);

    // Convenience: true iff tracing is currently enabled for this run.
    static bool Enabled();
};

} // namespace Kernel
