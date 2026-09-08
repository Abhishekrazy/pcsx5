#include <pcsx5/execution/worker_protocol.h>
#include <algorithm>
#include <memory>
#include <new>

namespace pcsx5::execution {
namespace {
constexpr std::uint64_t request_magic = 0x31515850;
constexpr std::uint64_t response_magic = 0x31525850;

struct writer {
    std::span<std::byte> bytes;
    std::size_t position{};
    void scalar(std::uint64_t value, unsigned count) noexcept {
        for (unsigned i = 0; i < count; ++i)
            bytes[position++] = static_cast<std::byte>((value >> (8*i)) & 255);
    }
    void state(const cpu_state& value) noexcept {
        for (const auto reg : value.gpr) scalar(reg, 8);
        scalar(value.rip, 8); scalar(value.rflags, 8); scalar(value.known_flags, 8);
    }
    void image(const std::array<std::byte, worker_memory_size>& value) noexcept {
        for (const auto byte : value) bytes[position++] = byte;
    }
};
struct reader {
    std::span<const std::byte> bytes;
    std::size_t position{};
    std::uint64_t scalar(unsigned count) noexcept {
        std::uint64_t value{};
        for (unsigned i = 0; i < count; ++i)
            value |= std::to_integer<std::uint64_t>(bytes[position++]) << (8*i);
        return value;
    }
    cpu_state state() noexcept {
        cpu_state value{};
        for (auto& reg : value.gpr) reg = scalar(8);
        value.rip = scalar(8); value.rflags = scalar(8); value.known_flags = scalar(8);
        return value;
    }
    void image(std::array<std::byte, worker_memory_size>& value) noexcept {
        for (auto& byte : value) byte = bytes[position++];
    }
};
bool valid_request(const worker_request& request) noexcept {
    return request.id != 0 && request.budget != 0 && request.budget <= worker_max_steps &&
        (request.initial.known_flags & ~arithmetic_flags) == 0;
}
bool inside_image(std::uint64_t address, std::size_t count) noexcept {
    return address >= worker_memory_base && count <= worker_memory_size &&
        address - worker_memory_base <= worker_memory_size - count;
}
bool valid_response(const worker_response& response) noexcept {
    const auto& result = response.result;
    if (response.id == 0 || response.trace.empty() || response.trace.size() > worker_max_steps ||
        result.records != response.trace.size() || result.retired > worker_max_steps ||
        (response.final.known_flags & ~arithmetic_flags) != 0) return false;
    const bool exhausted = result.reason == stop_reason::budget_exhausted;
    if (!exhausted && (result.reason == stop_reason::completed || result.reason > stop_reason::syscall))
        return false;
    std::uint64_t completed{};
    std::array<std::byte, trace_wire_size> encoded{};
    for (std::size_t i = 0; i < response.trace.size(); ++i) {
        const auto& record = response.trace[i];
        if (record.sequence != i || !encode_trace(record, encoded) ||
            (i != 0 && record.before != response.trace[i-1].after)) return false;
        if (record.result.reason == stop_reason::completed) ++completed;
        else if (exhausted || i + 1 != response.trace.size() || record.result.reason != result.reason)
            return false;
    }
    if (completed != result.retired || response.final != response.trace.back().after) return false;
    return exhausted ? completed == result.records : completed + 1 == result.records;
}

// Owns data only: instruction fetches still go through the interpreter's core
// memory port. No executable allocation or host pointer is transported.
class array_backing final : public core::memory_backing {
public:
    explicit array_backing(const std::array<std::byte, worker_memory_size>& image) : bytes_(image) {}
    std::uint64_t size() const noexcept override { return bytes_.size(); }
    core::guest_memory_result<void> read(std::uint64_t offset, std::span<std::byte> out) const noexcept override {
        if (!live_) return std::unexpected(core::guest_memory_error::invalid_state);
        if (out.size() > bytes_.size() || offset > bytes_.size() - out.size())
            return std::unexpected(core::guest_memory_error::invalid_range);
        std::copy_n(bytes_.begin() + static_cast<std::size_t>(offset), out.size(), out.begin());
        return {};
    }
    core::guest_memory_result<void> write(std::uint64_t offset, std::span<const std::byte> in) noexcept override {
        if (!live_) return std::unexpected(core::guest_memory_error::invalid_state);
        if (in.size() > bytes_.size() || offset > bytes_.size() - in.size())
            return std::unexpected(core::guest_memory_error::invalid_range);
        std::copy(in.begin(), in.end(), bytes_.begin() + static_cast<std::size_t>(offset));
        return {};
    }
    core::guest_memory_result<void> release() noexcept override { live_ = false; return {}; }
private:
    std::array<std::byte, worker_memory_size> bytes_{};
    bool live_{true};
};
} // namespace

bool encode_request(const worker_request& request, std::span<std::byte> out) noexcept {
    if (out.size() != request_wire_size || !valid_request(request)) return false;
    writer w{out};
    w.scalar(request_magic, 4); w.scalar(request.id, 8); w.scalar(request.budget, 4);
    w.state(request.initial); w.image(request.memory);
    return w.position == out.size();
}
bool decode_request(std::span<const std::byte> in, worker_request& request) noexcept {
    if (in.size() != request_wire_size) return false;
    reader rd{in};
    if (rd.scalar(4) != request_magic) return false;
    worker_request candidate{};
    candidate.id = rd.scalar(8); candidate.budget = static_cast<std::uint32_t>(rd.scalar(4));
    candidate.initial = rd.state(); rd.image(candidate.memory);
    if (!valid_request(candidate)) return false;
    request = candidate;
    return true;
}
protocol_result<std::vector<std::byte>> encode_response(const worker_response& response) noexcept {
    if (!valid_response(response)) return std::unexpected(protocol_error::invalid);
    try {
        std::vector<std::byte> bytes(response_header_size + response.trace.size() * trace_wire_size);
        writer w{bytes};
        w.scalar(response_magic, 4); w.scalar(response.id, 8);
        w.scalar(static_cast<std::uint8_t>(response.result.reason), 1);
        w.scalar(response.result.records, 4); w.scalar(response.result.retired, 4);
        w.state(response.final); w.image(response.memory);
        for (const auto& record : response.trace) {
            if (!encode_trace(record, std::span{bytes}.subspan(w.position, trace_wire_size)))
                return std::unexpected(protocol_error::invalid);
            w.position += trace_wire_size;
        }
        return bytes;
    } catch (const std::bad_alloc&) { return std::unexpected(protocol_error::out_of_memory); }
}
protocol_result<worker_response> decode_response(std::span<const std::byte> bytes) noexcept {
    if (bytes.size() < response_header_size || bytes.size() > max_response_wire_size)
        return std::unexpected(protocol_error::invalid);
    reader rd{bytes};
    if (rd.scalar(4) != response_magic) return std::unexpected(protocol_error::invalid);
    worker_response response{};
    response.id = rd.scalar(8); response.result.reason = static_cast<stop_reason>(rd.scalar(1));
    response.result.records = static_cast<std::size_t>(rd.scalar(4));
    response.result.retired = rd.scalar(4);
    if (response.result.records == 0 || response.result.records > worker_max_steps ||
        bytes.size() != response_header_size + response.result.records * trace_wire_size)
        return std::unexpected(protocol_error::invalid);
    response.final = rd.state(); rd.image(response.memory);
    try {
        response.trace.resize(response.result.records);
        for (auto& record : response.trace) {
            if (!decode_trace(bytes.subspan(rd.position, trace_wire_size), record))
                return std::unexpected(protocol_error::invalid);
            rd.position += trace_wire_size;
        }
        if (!valid_response(response)) return std::unexpected(protocol_error::invalid);
        return response;
    } catch (const std::bad_alloc&) { return std::unexpected(protocol_error::out_of_memory); }
}
bool matches_request(const worker_request& request, const worker_response& response) noexcept {
    if (!valid_request(request) || !valid_response(response) || request.id != response.id ||
        request.initial != response.trace.front().before || response.result.retired > request.budget ||
        response.result.records > request.budget ||
        (response.result.reason == stop_reason::budget_exhausted) != (response.result.retired == request.budget))
        return false;
    auto image = request.memory;
    for (const auto& record : response.trace) {
        const auto& result = record.result;
        // The interpreter conservatively marks every failed write uncertain,
        // including a core rejection before the backing is called. Preserve
        // that trace marker; this transactional backing has no partial-write
        // path, and the final image check still requires unchanged failed writes.
        if (result.length != 0) {
            if (!inside_image(record.before.rip, result.length)) return false;
            const auto offset = static_cast<std::size_t>(record.before.rip - worker_memory_base);
            for (std::size_t i = 0; i < result.length; ++i)
                if (image[offset+i] != result.instruction[i]) return false;
        }
        if (result.wrote_memory) {
            if (!inside_image(result.write_address, result.write_size)) return false;
            const auto offset = static_cast<std::size_t>(result.write_address - worker_memory_base);
            std::copy_n(result.write_bytes.begin(), result.write_size, image.begin() + offset);
        }
    }
    return image == response.memory;
}
protocol_result<worker_response> execute_request(const worker_request& request) noexcept {
    if (!valid_request(request)) return std::unexpected(protocol_error::invalid);
    try {
        core::guest_memory memory;
        std::unique_ptr<core::memory_backing> backing = std::make_unique<array_backing>(request.memory);
        const auto mapped = memory.map(worker_memory_base, worker_memory_size,
            core::guest_memory_access::read_write, backing);
        if (!mapped) return std::unexpected(mapped.error() == core::guest_memory_error::out_of_memory ?
            protocol_error::out_of_memory : protocol_error::invalid);
        worker_response response{};
        response.id = request.id; response.final = request.initial;
        response.trace.resize(request.budget);
        response.result = run(response.final, memory, request.budget, response.trace);
        response.trace.resize(response.result.records);
        if (!memory.read(worker_memory_base, response.memory) || !matches_request(request, response))
            return std::unexpected(protocol_error::invalid);
        return response;
    } catch (const std::bad_alloc&) { return std::unexpected(protocol_error::out_of_memory); }
}
} // namespace pcsx5::execution
