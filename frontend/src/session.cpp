#include <pcsx5/frontend/session.h>
#include <algorithm>
#include <new>
namespace pcsx5::frontend {
namespace {
class image final : public core::memory_backing {
public:
    explicit image(std::span<const std::byte> input) { std::copy(input.begin(),input.end(),data_.begin()); }
    std::uint64_t size() const noexcept override { return data_.size(); }
    core::guest_memory_result<void> release() noexcept override { return {}; }
    core::guest_memory_result<void> read(std::uint64_t offset,std::span<std::byte> output) const noexcept override {
        if (output.size()>data_.size() || offset>data_.size()-output.size()) return std::unexpected(core::guest_memory_error::invalid_range);
        std::copy_n(data_.begin()+static_cast<std::size_t>(offset),output.size(),output.begin()); return {};
    }
    core::guest_memory_result<void> write(std::uint64_t offset,std::span<const std::byte> input) noexcept override {
        if (input.size()>data_.size() || offset>data_.size()-input.size()) return std::unexpected(core::guest_memory_error::invalid_range);
        std::copy(input.begin(),input.end(),data_.begin()+static_cast<std::size_t>(offset)); return {};
    }
private:
    std::array<std::byte,4096> data_{};
};
}
std::expected<session_result,session_error> run_program(std::span<const std::byte> input,
    std::uint64_t budget,execution::code_factory factory) noexcept {
    if (input.empty() || input.size()>4096 || !budget || budget>1'000'000) return std::unexpected(session_error::invalid_input);
    try {
        core::guest_memory memory;
        std::unique_ptr<core::memory_backing> backing=std::make_unique<image>(input);
        if (!memory.map(0x1000,4096,core::guest_memory_access::read_write,backing)) return std::unexpected(session_error::out_of_memory);
        session_result result{}; result.state.rip=0x1000; result.state.gpr[4]=0x2000;
        for (std::uint64_t i=0;i<budget;++i) {
            execution::step_result step{};
            bool translated{};
            if (factory) {
                auto next=execution::step_arm64(result.state,memory,factory);
                if (!next) return std::unexpected(session_error::runtime_failure);
                step=next->result; translated=next->translated;
            } else step=execution::step(result.state,memory);
            result.stop=step.reason;
            if (step.reason!=execution::stop_reason::completed) return result;
            ++result.retired;
            translated ? ++result.translated : ++result.interpreted;
        }
        result.stop=execution::stop_reason::budget_exhausted; return result;
    } catch (const std::bad_alloc&) { return std::unexpected(session_error::out_of_memory); }
}
}
