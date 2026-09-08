#include <pcsx5/execution/native_run.h>
#include <algorithm>
#include <memory>
#include <new>

namespace pcsx5::execution {
namespace {
class image_backing final : public core::memory_backing {
public:
    explicit image_backing(const std::array<std::byte, native_page_bytes>& image) : image_(image) {}
    std::uint64_t size() const noexcept override { return image_.size(); }
    core::guest_memory_result<void> read(std::uint64_t offset, std::span<std::byte> out) const noexcept override {
        if (!live_) return std::unexpected(core::guest_memory_error::invalid_state);
        if (out.size() > image_.size() || offset > image_.size() - out.size())
            return std::unexpected(core::guest_memory_error::invalid_range);
        std::copy_n(image_.begin() + static_cast<std::size_t>(offset), out.size(), out.begin());
        return {};
    }
    core::guest_memory_result<void> write(std::uint64_t offset, std::span<const std::byte> in) noexcept override {
        if (!live_) return std::unexpected(core::guest_memory_error::invalid_state);
        if (in.size() > image_.size() || offset > image_.size() - in.size())
            return std::unexpected(core::guest_memory_error::invalid_range);
        std::copy(in.begin(), in.end(), image_.begin() + static_cast<std::size_t>(offset));
        return {};
    }
    core::guest_memory_result<void> release() noexcept override { live_ = false; return {}; }
private:
    std::array<std::byte, native_page_bytes> image_{};
    bool live_{true};
};
bool valid_state(const cpu_state& state) noexcept {
    return (state.rflags & 2) != 0 && (state.rflags & ~(arithmetic_flags | 2ULL)) == 0 &&
        (state.known_flags & ~arithmetic_flags) == 0;
}
bool equal_defined(const cpu_state& actual, const cpu_state& expected) noexcept {
    return valid_state(actual) && actual.gpr == expected.gpr && actual.rip == expected.rip &&
        ((actual.rflags ^ expected.rflags) & expected.known_flags) == 0;
}
native_error memory_error(core::guest_memory_error error) noexcept {
    return error == core::guest_memory_error::out_of_memory ? native_error::out_of_memory : native_error::host_failure;
}
} // namespace

std::expected<native_run_response, native_error> run_native(const std::filesystem::path& helper,
    const native_run_request& request, native_step_provider provider, std::uint32_t timeout_ms) noexcept {
    if (!provider || helper.empty() || !helper.is_absolute() ||
        helper.native().find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos ||
        timeout_ms == 0 || timeout_ms > 60000 || request.budget == 0 || request.budget > 256 ||
        request.return_address < native_code_base ||
        request.return_address - native_code_base >= native_page_bytes || !valid_state(request.machine.initial))
        return std::unexpected(native_error::invalid);
    try {
        native_run_response response{};
        response.final = request.machine.initial;
        response.data = request.machine.data;
        response.records.reserve(request.budget);
        for (;;) {
            if (response.final.rip == request.return_address) {
                response.stop = native_run_stop::returned; return response;
            }
            if (response.retired == request.budget) {
                response.stop = native_run_stop::budget_exhausted; return response;
            }
            // The interpreter memory port models reads, not execute permission.
            // Only the owned code image is eligible for native instruction fetch.
            if (response.final.rip < native_code_base ||
                response.final.rip - native_code_base >= native_page_bytes) {
                response.stop = native_run_stop::unsupported; return response;
            }
            core::guest_memory memory;
            std::unique_ptr<core::memory_backing> code = std::make_unique<image_backing>(request.machine.code);
            std::unique_ptr<core::memory_backing> data = std::make_unique<image_backing>(response.data);
            if (const auto mapped = memory.map(native_code_base, native_page_bytes,
                core::guest_memory_access::read_only, code); !mapped)
                return std::unexpected(memory_error(mapped.error()));
            if (const auto mapped = memory.map(native_data_base, native_page_bytes,
                core::guest_memory_access::read_write, data); !mapped)
                return std::unexpected(memory_error(mapped.error()));
            auto expected = response.final;
            const auto preflight = step(expected, memory);
            // A failed fetch may leave an incomplete, unclassified instruction.
            // Only fully decoded data faults may proceed to hardware observation.
            if (preflight.reason == stop_reason::memory_failure &&
                preflight.access != access_kind::read && preflight.access != access_kind::write) {
                response.stop = native_run_stop::unsupported; return response;
            }
            if (preflight.reason != stop_reason::completed && preflight.reason != stop_reason::breakpoint &&
                preflight.reason != stop_reason::memory_failure) {
                response.stop = native_run_stop::unsupported; return response;
            }
            auto expected_data = response.data;
            if (const auto read = memory.read(native_data_base, expected_data); !read)
                return std::unexpected(memory_error(read.error()));
            native_request machine{};
            machine.initial = response.final; machine.code = request.machine.code; machine.data = response.data;
            const auto observed = provider(helper, machine, timeout_ms);
            if (!observed) return std::unexpected(observed.error());
            response.records.push_back({response.final, *observed, preflight});
            response.final = observed->state;
            response.data = observed->data;
            if (preflight.reason == stop_reason::memory_failure) {
                response.stop = observed->stop == native_stop::memory_fault &&
                    equal_defined(observed->state, expected) && observed->data == expected_data ?
                    native_run_stop::guest_fault : native_run_stop::divergence;
                return response;
            }
            if (preflight.reason == stop_reason::breakpoint) {
                expected.rip += preflight.length; // Includes any decoded REX prefix.
                response.stop = observed->stop == native_stop::breakpoint &&
                    equal_defined(observed->state, expected) && observed->data == expected_data ?
                    native_run_stop::requested_exit : native_run_stop::divergence;
                return response;
            }
            if (observed->stop != native_stop::stepped || !equal_defined(observed->state, expected) ||
                observed->data != expected_data) {
                response.stop = native_run_stop::divergence; return response;
            }
            // Undefined arithmetic values stay actual; only their definedness
            // comes from the oracle after successful comparison. Annotate both
            // the retained observation and next input so complete modeled-state
            // traces chain. Never substitute expected physical register values.
            response.records.back().observed.state.known_flags = expected.known_flags;
            response.final.known_flags = expected.known_flags;
            ++response.retired;
        }
    } catch (const std::bad_alloc&) { return std::unexpected(native_error::out_of_memory); }
    catch (...) { return std::unexpected(native_error::host_failure); }
}
} // namespace pcsx5::execution
