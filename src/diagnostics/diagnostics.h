#pragma once
#include "../common/types.h"
#include <string>

namespace Diagnostics {

// State captured by the crash handler.  Mirrors a subset of the Win32
// CONTEXT structure that the diagnostic bundle writes out.
struct CrashContext {
    u64 timestamp_us    = 0;
    u32 thread_id       = 0;
    u32 exc_code        = 0;   // Renamed from `exception_code` to avoid MSVC's
                                // SEH-context identifier mangling.
    u64 fault_address   = 0;   // ExceptionInformation[1] (target of read/write/exec)
    u64 rip             = 0;
    u64 rsp             = 0;
    u64 rbp             = 0;
    u64 rax = 0, rbx = 0, rcx = 0, rdx = 0;
    u64 rsi = 0, rdi = 0, r8  = 0, r9  = 0;
    u64 r10 = 0, r11 = 0, r12 = 0, r13 = 0, r14 = 0, r15 = 0;
};

// True if a crash has been captured since process start (or last Reset).
bool HasCrashReport();

// Snapshot of the captured crash context (only valid if HasCrashReport is true).
const CrashContext& GetCrashContext();

// Reset the captured crash report so a future crash can be recorded again.
void ResetCrashReport();

// Install the top-of-chain unhandled-exception filter.  Idempotent.  When
// the process later crashes, the filter writes a crash-report bundle to
// `bundle_dir` and writes a MiniDump next to it.
void InstallCrashHandler(const std::string& bundle_dir);

// Write the crash report bundle to the configured directory even when no
// crash has been captured (useful for periodic snapshots / debugging).  When
// `force` is true, writes a stub bundle so callers can verify the layout
// without a real crash.  Returns the path to the created directory, or the
// empty string on failure.
std::string WriteCrashReportBundle(bool force = false);

// Convenience: the directory the bundle is written to.
const std::string& BundleDirectory();

} // namespace Diagnostics
