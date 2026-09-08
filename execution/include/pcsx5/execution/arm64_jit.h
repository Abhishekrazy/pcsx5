#pragma once
#include <pcsx5/execution/interpreter.h>
#include <expected>
#include <vector>
namespace pcsx5::execution {
enum class scalar_operation { nop, move, add, subtract, compare, bit_xor, bit_and, bit_or, test };
struct scalar_instruction {
    scalar_operation operation{};
    std::uint8_t width{64}; // 32 or 64 only
    std::uint8_t destination{},source{}; // Architectural x86 GPR indexes.
    bool immediate{};
    std::uint64_t value{};
    std::uint64_t rip{};
    std::uint8_t length{};
};
enum class jit_error { unsupported, truncated, invalid, out_of_memory };
inline constexpr std::size_t arm64_max_instructions=64;
inline constexpr std::size_t arm64_max_code_bytes=32768;
// Bounded frontend for the existing register-only scalar interpreter subset.
// Memory/branches/stack/traps/syscalls remain explicit interpreter work.
[[nodiscard]] std::expected<scalar_instruction,jit_error> lower_scalar(
    std::span<const std::byte> bytes,std::uint64_t guest_rip) noexcept;
// Emits a leaf AAPCS64 function void(cpu_state*) with only caller-saved scratch
// registers (never platform-reserved X18). All arithmetic flag work is emitted
// ARM64 code, not a call into the interpreter. No host addresses embedded.
// Input instructions must form one contiguous straight-line block.
// Invocation precondition: the valid cpu_state's RIP equals the block's first
// RIP, and its guest bytes still match the bytes that were lowered. The caller
// must invalidate stale compiled code; this emitter is not a dispatch/cache policy.
[[nodiscard]] std::expected<std::vector<std::byte>,jit_error> emit_arm64(
    std::span<const scalar_instruction>) noexcept;
} // namespace pcsx5::execution
