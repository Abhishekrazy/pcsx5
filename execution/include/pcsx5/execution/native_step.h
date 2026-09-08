#pragma once
#include <pcsx5/execution/interpreter.h>
#include <filesystem>
#include <expected>
namespace pcsx5::execution {
// Native bring-up addresses, not a PS5 virtual-memory policy. Exact reservation
// only; providers must never replace an existing child mapping.
inline constexpr std::uint64_t native_code_base = 0x20000000;
inline constexpr std::uint64_t native_data_base = 0x20010000;
inline constexpr std::size_t native_page_bytes = 4096;
struct native_request {
    cpu_state initial{};
    std::array<std::byte,native_page_bytes> code{};
    std::array<std::byte,native_page_bytes> data{};
};
enum class native_stop { stepped, breakpoint, memory_fault, illegal_instruction };
struct native_snapshot {
    native_stop stop{};
    cpu_state state{};
    std::array<std::byte,native_page_bytes> data{};
    std::uint64_t fault_address{};
};
enum class native_error { invalid, unavailable, host_failure, timed_out, out_of_memory };
using native_result = std::expected<native_snapshot,native_error>;
// Trusted authored instructions ONLY; not a security sandbox. One actual
// hardware step in a fresh disposable child. No guest syscall filtering yet.
// Flags input/output are arithmetic bits plus reserved bit1 only; TF/IF and
// privileged host flags never enter the modeled snapshot. All 16 GPRs/RIP are
// hardware observations, including faulting RSP. Arithmetic definedness remains
// the interpreter/caller's responsibility. Timeout 1..60000 ms after spawn.
// Success/error returns only after child termination/reaping and handle cleanup.
[[nodiscard]] native_result step_windows_native(const std::filesystem::path& helper,
    const native_request&,std::uint32_t timeout_ms) noexcept;
[[nodiscard]] native_result step_linux_native(const std::filesystem::path& helper,
    const native_request&,std::uint32_t timeout_ms) noexcept;
// Linux dedicated helper only; sets up exact RX/RW images and ptrace handshake.
// No caller in the emulator parent may invoke this function.
int linux_native_probe() noexcept;
} // namespace pcsx5::execution
