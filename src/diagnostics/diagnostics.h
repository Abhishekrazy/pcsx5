#pragma once
#include "../common/types.h"
#include <functional>
#include <string>
#include <vector>

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

// I6.4: Callback types for extended diagnostic data.
// Hang snapshot callback: receives a wide-character directory path + user data.
// Used to capture the last rendered frame from the GPU.
typedef void (*HangSnapshotCallback)(const std::wstring& dir, void* user);

// Boot timeline callback: returns a JSON array of boot stage strings.
typedef std::string (*BootTimelineCallback)();

// Config snapshot callback: returns a JSON object of current config values.
typedef std::string (*ConfigSnapshotCallback)();

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

// I2.3: Write a diagnostic snapshot bundle (not crash-triggered).  Useful for
// capturing periodic process state.  Returns the path to the created directory,
// or the empty string on failure.
std::string WriteDiagnosticSnapshot(const std::string& label);

// Convenience: the directory the bundle is written to.
const std::string& BundleDirectory();

// I6.4: Register a callback that captures the last rendered frame on hang/crash.
void SetHangSnapshotCallback(HangSnapshotCallback cb, void* user = nullptr);

// I6.4: Register a callback that returns the boot timeline as a JSON array.
void SetBootTimelineCallback(BootTimelineCallback cb);

// I6.4: Register a callback that returns a config snapshot as a JSON object.
void SetConfigSnapshotCallback(ConfigSnapshotCallback cb);

// I6.4: Record a flip timestamp for hang detection diagnostics.
void RecordFlipTimestamp(uint64_t frame_counter);

// I6.4: Get the last flip frame counter.
uint64_t GetLastFlipFrame();

// I6.4: Get the timestamp (steady_clock us) of the last flip.
uint64_t GetLastFlipTimestampUs();

// I6.4: Capture a snapshot of all running threads (toolhelp).
std::vector<std::string> CaptureThreadSnapshot();

// I6.4: Write a hang snapshot bundle (called when the hang detector fires).
// Returns the path to the created directory, or empty string on failure.
std::string WriteHangSnapshotBundle(const std::string& label);

// Process uptime in microseconds (steady clock).
uint64_t ProcessUptimeMicros();

// I2.3: ISO-8601 timestamp helper.
std::string NowIso8601();

// JSON-escape a string.
std::string JsonEscape(const std::string& s);

// Hex formatting helper.
std::string Hex(u64 v, int width = 0);

} // namespace Diagnostics
