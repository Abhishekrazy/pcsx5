#pragma once

#include <pcsx5/core/dynamic_info.h>
#include <pcsx5/core/hle_registry.h>
#include <pcsx5/core/loader.h>
#include <pcsx5/execution/arm64_step.h>

#include <cstdint>
#include <expected>
#include <optional>
#include <span>

namespace pcsx5::frontend {

enum class session_error {
    invalid_input,
    out_of_memory,
    runtime_failure
};

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
[[nodiscard]] std::expected<session_result, session_error> run_program(
    std::span<const std::byte> input,
    std::uint64_t budget,
    execution::code_factory factory = nullptr) noexcept;

struct elf_session_config {
    std::uint64_t budget{1'000'000};
    execution::code_factory code_factory{nullptr};
    core::backing_factory backing_factory{nullptr};
    core::hle_registry* registry{nullptr};
    std::optional<std::uint64_t> base_address_slide{std::nullopt};
    std::uint64_t stack_size{0x20000}; // 128 KiB
    std::uint64_t stack_base{0x70000000};
    std::uint64_t thunk_base{0x7FFF0000};
    bool strict_import_mode{false};
};

struct elf_session_result {
    session_result session;
    core::loaded_module module;
    std::optional<core::dynamic_info> dynamic;
    std::int64_t exit_code{0};
    bool exited{false};
};

// Loads and executes a 64-bit ELF or Sony PS5 SELF binary in guest memory.
// Handles segment allocation, BSS zeroing, TLS template extraction,
// dynamic table parsing, relocation processing, HLE dispatch, and direct guest syscalls.
[[nodiscard]] std::expected<elf_session_result, session_error> run_elf(
    std::span<const std::byte> elf_or_self_bytes,
    const elf_session_config& config = {}) noexcept;

} // namespace pcsx5::frontend
