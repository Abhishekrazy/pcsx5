#pragma once
#include <pcsx5/core/guest_memory.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace pcsx5::execution {
inline constexpr std::uint64_t arithmetic_flags = 0x8d5;
// GPR encoding order: RAX RCX RDX RBX RSP RBP RSI RDI R8 ... R15.
struct cpu_state {
    std::array<std::uint64_t, 16> gpr{};
    std::uint64_t rip{};
    std::uint64_t rflags{2};
    std::uint64_t known_flags{arithmetic_flags};
    friend bool operator==(const cpu_state&, const cpu_state&) = default;
};
enum class stop_reason : std::uint8_t {
    completed, unsupported, memory_failure, invalid_address, unknown_flags,
    breakpoint, syscall, budget_exhausted, trace_full
};
enum class access_kind : std::uint8_t { none, fetch, read, write };
struct step_result {
    stop_reason reason{stop_reason::completed};
    access_kind access{access_kind::none};
    core::guest_memory_error memory_error{core::guest_memory_error::invalid_state};
    std::uint64_t fault_address{};
    bool memory_uncertain{};
    std::array<std::byte, 15> instruction{};
    std::uint8_t length{};
    bool wrote_memory{};
    std::uint64_t write_address{};
    std::array<std::byte, 8> write_bytes{};
    std::uint8_t write_size{};
    friend bool operator==(const step_result&, const step_result&) = default;
};
// No allocation; memory/state are borrowed and externally serialized. Stops do
// not retire the instruction. A failing backing write may mutate memory only.
[[nodiscard]] step_result step(cpu_state&, core::guest_memory&) noexcept;
struct trace_record {
    std::uint64_t sequence{};
    cpu_state before{};
    cpu_state after{};
    step_result result{};
    friend bool operator==(const trace_record&, const trace_record&) = default;
};
struct run_result {
    stop_reason reason{};
    std::size_t records{};
    std::uint64_t retired{};
};
// Sequence starts at zero per call; includes a final non-retiring step stop.
// Zero budget performs no fetch. Exhausted trace space performs no next step.
[[nodiscard]] run_result run(cpu_state&, core::guest_memory&, std::uint64_t budget,
    std::span<trace_record>) noexcept;
// Trace v1: exact fixed-size little-endian wire record, independent of padding
// and host endianness. Returns false for invalid records or undersized output.
inline constexpr std::size_t trace_wire_size = 362;
[[nodiscard]] bool encode_trace(const trace_record&, std::span<std::byte>) noexcept;
[[nodiscard]] bool decode_trace(std::span<const std::byte>, trace_record&) noexcept;
} // namespace pcsx5::execution
