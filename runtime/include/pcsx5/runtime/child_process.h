#pragma once
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <stop_token>
#include <string>

namespace pcsx5::runtime {
enum class process_error { invalid_argument, unavailable, out_of_memory, host_failure };
enum class process_outcome { exited, signaled, timed_out, cancelled };
struct process_result {
    process_outcome outcome{};
    // Native exit status (DWORD on Windows, low 8 bits on Linux) or Linux signal.
    // Zero for timeout/cancellation. Not a guest exit code or exception record.
    std::uint32_t code{};
    friend bool operator==(const process_result&, const process_result&) = default;
};
using process_run_result = std::expected<process_result, process_error>;
// Launch one trusted helper by absolute native path, without a shell or PATH
// lookup. Arguments exclude argv[0]; strings are UTF-8 on Windows, bytes on Linux.
// Reject NULs, >32 arguments, >8192 total path/argument units, timeout outside
// 1..60000ms. Input buffers are borrowed and immutable until return.
// Validation precedes pre-cancellation. Observe exit first, then cancellation,
// then timeout. Timeout starts after spawn; scheduling/OS cleanup can exceed it.
// Returns only after its child has exited and handles/PID have been reaped.
// Cleanup failure terminates the caller rather than abandon a live child.
// No signal-handler overrides or global child reaping allowed in the caller.
// No descendant containment, I/O capture, filesystem/network restriction or
// hostile-code sandbox. CWD/environment inherited; Linux non-CLOEXEC FDs may be
// inherited. Windows starts hidden with handle inheritance disabled.
[[nodiscard]] process_run_result run_windows_child(const std::filesystem::path&,
    std::span<const std::string> arguments, std::uint32_t timeout_ms,
    std::stop_token cancellation = {}) noexcept;
[[nodiscard]] process_run_result run_linux_child(const std::filesystem::path&,
    std::span<const std::string> arguments, std::uint32_t timeout_ms,
    std::stop_token cancellation = {}) noexcept;
} // namespace pcsx5::runtime
