#pragma once
#include <pcsx5/execution/arm64_step.h>
namespace pcsx5::frontend {
enum class session_error { invalid_input, out_of_memory, runtime_failure };
struct session_result {
    execution::cpu_state state;
    execution::stop_reason stop;
    std::uint64_t retired{}, translated{}, interpreted{};
};
// Bounded synthetic raw-code runner, not a PS5 ELF loader. Input 1..4096 bytes;
// initial RIP=0x1000/RSP=0x2000, one private 4096-byte RW guest image. No imports,
// host syscalls, firmware, threads, files or external guest resources exposed.
// Null factory selects interpreter. Non-null selects the hybrid ARM64 backend;
// provider failure returns runtime_failure, never silently switches strategy.
[[nodiscard]] std::expected<session_result,session_error> run_program(
    std::span<const std::byte>,std::uint64_t budget,execution::code_factory = nullptr) noexcept;
}
