#include <pcsx5/execution/interpreter.h>

namespace pcsx5::execution {
namespace {
bool valid(const trace_record& record) noexcept {
    const auto& r = record.result;
    if (r.reason > stop_reason::syscall || r.access > access_kind::write ||
        static_cast<unsigned>(r.memory_error) > static_cast<unsigned>(core::guest_memory_error::host_failure) || r.length > 15 ||
        (record.before.known_flags & ~arithmetic_flags) != 0 ||
        (record.after.known_flags & ~arithmetic_flags) != 0) return false;
    for (std::size_t i = r.length; i < r.instruction.size(); ++i)
        if (r.instruction[i] != std::byte{}) return false;
    if (r.reason == stop_reason::completed) {
        if (r.length == 0) return false;
    } else if (record.before != record.after || r.wrote_memory) return false;
    if (r.reason == stop_reason::memory_failure || r.reason == stop_reason::invalid_address) {
        if (r.access == access_kind::none) return false;
    } else if (r.access != access_kind::none || r.fault_address != 0) return false;
    if (r.reason != stop_reason::memory_failure &&
        r.memory_error != core::guest_memory_error::invalid_state) return false;
    if (r.memory_uncertain && (r.reason != stop_reason::memory_failure || r.access != access_kind::write)) return false;
    if (r.wrote_memory) {
        if (r.write_size != 4 && r.write_size != 8) return false;
    } else if (r.write_size != 0 || r.write_address != 0) return false;
    for (std::size_t i = r.write_size; i < r.write_bytes.size(); ++i)
        if (r.write_bytes[i] != std::byte{}) return false;
    return true;
}
struct writer {
    std::span<std::byte> out;
    std::size_t position{};
    void scalar(std::uint64_t value, unsigned size) noexcept {
        for (unsigned i = 0; i < size; ++i) out[position++] = static_cast<std::byte>((value >> (8*i)) & 255);
    }
    void state(const cpu_state& value) noexcept {
        for (const auto reg : value.gpr) scalar(reg, 8);
        scalar(value.rip, 8); scalar(value.rflags, 8); scalar(value.known_flags, 8);
    }
};
struct reader {
    std::span<const std::byte> in;
    std::size_t position{};
    std::uint64_t scalar(unsigned size) noexcept {
        std::uint64_t value{};
        for (unsigned i = 0; i < size; ++i) value |= std::to_integer<std::uint64_t>(in[position++]) << (8*i);
        return value;
    }
    cpu_state state() noexcept {
        cpu_state value{};
        for (auto& reg : value.gpr) reg = scalar(8);
        value.rip = scalar(8); value.rflags = scalar(8); value.known_flags = scalar(8);
        return value;
    }
};
constexpr std::uint64_t magic = 0x31495850; // PXI1
}
run_result run(cpu_state& state, core::guest_memory& memory, std::uint64_t budget,
    std::span<trace_record> trace) noexcept {
    run_result result{stop_reason::budget_exhausted};
    while (result.retired < budget) {
        if (result.records == trace.size()) { result.reason = stop_reason::trace_full; return result; }
        auto& record = trace[result.records];
        record = {};
        record.sequence = result.records;
        record.before = state;
        record.result = step(state, memory);
        record.after = state;
        ++result.records;
        if (record.result.reason != stop_reason::completed) {
            result.reason = record.result.reason; return result;
        }
        ++result.retired;
    }
    return result;
}
bool encode_trace(const trace_record& record, std::span<std::byte> out) noexcept {
    if (out.size() < trace_wire_size || !valid(record)) return false;
    writer w{out};
    w.scalar(magic, 4); w.scalar(record.sequence, 8);
    w.state(record.before); w.state(record.after);
    const auto& r = record.result;
    w.scalar(static_cast<std::uint8_t>(r.reason), 1);
    w.scalar(static_cast<std::uint8_t>(r.access), 1);
    w.scalar(static_cast<std::uint8_t>(r.memory_error), 1);
    w.scalar(r.fault_address, 8); w.scalar(r.memory_uncertain, 1);
    for (const auto byte : r.instruction) w.scalar(std::to_integer<unsigned>(byte), 1);
    w.scalar(r.length, 1); w.scalar(r.wrote_memory, 1); w.scalar(r.write_address, 8);
    for (const auto byte : r.write_bytes) w.scalar(std::to_integer<unsigned>(byte), 1);
    w.scalar(r.write_size, 1);
    return w.position == trace_wire_size;
}
bool decode_trace(std::span<const std::byte> in, trace_record& record) noexcept {
    if (in.size() != trace_wire_size) return false;
    reader rd{in};
    if (rd.scalar(4) != magic) return false;
    trace_record candidate{};
    candidate.sequence = rd.scalar(8); candidate.before = rd.state(); candidate.after = rd.state();
    auto& r = candidate.result;
    r.reason = static_cast<stop_reason>(rd.scalar(1));
    r.access = static_cast<access_kind>(rd.scalar(1));
    const auto memory_error = rd.scalar(1);
    if (memory_error > static_cast<unsigned>(core::guest_memory_error::host_failure)) return false;
    r.memory_error = static_cast<core::guest_memory_error>(memory_error);
    r.fault_address = rd.scalar(8);
    const auto uncertain = rd.scalar(1);
    if (uncertain > 1) return false;
    r.memory_uncertain = uncertain != 0;
    for (auto& byte : r.instruction) byte = static_cast<std::byte>(rd.scalar(1));
    r.length = static_cast<std::uint8_t>(rd.scalar(1));
    const auto wrote = rd.scalar(1);
    if (wrote > 1) return false;
    r.wrote_memory = wrote != 0; r.write_address = rd.scalar(8);
    for (auto& byte : r.write_bytes) byte = static_cast<std::byte>(rd.scalar(1));
    r.write_size = static_cast<std::uint8_t>(rd.scalar(1));
    if (!valid(candidate)) return false;
    record = candidate;
    return true;
}
} // namespace pcsx5::execution
