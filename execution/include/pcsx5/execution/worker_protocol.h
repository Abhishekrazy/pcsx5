#pragma once
#include <pcsx5/execution/interpreter.h>
#include <expected>
#include <vector>
namespace pcsx5::execution {
inline constexpr std::uint64_t worker_memory_base = 0x1000;
inline constexpr std::size_t worker_memory_size = 4096;
inline constexpr std::size_t worker_max_steps = 256;
struct worker_request {
    std::uint64_t id{}; // Caller must use a fresh, nonzero ID per invocation.
    std::uint32_t budget{1};
    cpu_state initial{};
    std::array<std::byte,worker_memory_size> memory{};
};
struct worker_response {
    std::uint64_t id{};
    run_result result{};
    cpu_state final{};
    std::array<std::byte,worker_memory_size> memory{};
    std::vector<trace_record> trace;
};
enum class protocol_error { invalid, out_of_memory };
template<class T> using protocol_result = std::expected<T,protocol_error>;
inline constexpr std::size_t request_wire_size = 4264;
inline constexpr std::size_t response_header_size = 4269;
inline constexpr std::size_t max_response_wire_size = response_header_size + worker_max_steps * trace_wire_size;
// PXQ1: magic[4], id[8], budget[4], initial[152], memory[4096].
// PXR1: magic[4], id[8], reason[1], records[4], retired[4], final[152],
// memory[4096], records * PXI1 trace[362]. All integers little-endian.
// Exact lengths only; output objects remain unchanged on malformed input.
[[nodiscard]] bool encode_request(const worker_request&,std::span<std::byte>) noexcept;
[[nodiscard]] bool decode_request(std::span<const std::byte>,worker_request&) noexcept;
[[nodiscard]] protocol_result<std::vector<std::byte>> encode_response(const worker_response&) noexcept;
[[nodiscard]] protocol_result<worker_response> decode_response(std::span<const std::byte>) noexcept;
// Validates identity, trace chain, instruction bytes, memory writes and final
// image against this request. Not authentication or proof of ISA correctness.
[[nodiscard]] bool matches_request(const worker_request&,const worker_response&) noexcept;
// Initial worker executes the interpreter only; no native guest execution.
[[nodiscard]] protocol_result<worker_response> execute_request(const worker_request&) noexcept;
} // namespace pcsx5::execution
