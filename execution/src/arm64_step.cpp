#include <pcsx5/execution/arm64_step.h>
#include <limits>
namespace pcsx5::execution {
runtime::code_result<arm64_step_result> step_arm64(cpu_state& state,core::guest_memory& memory,code_factory factory) noexcept {
    if (!factory) return std::unexpected(runtime::code_error::invalid);
    std::array<std::byte,15> bytes{};
    std::size_t count{};
    const auto fallback=[&]() { return arm64_step_result{step(state,memory),false}; };
    for (;;) {
        if (count==bytes.size() || state.rip>std::numeric_limits<std::uint64_t>::max()-count) return fallback();
        const auto address=state.rip+count;
        if (address>0x00007fffffffffffULL && address<0xffff800000000000ULL) return fallback();
        if (!memory.read(address,std::span(bytes).subspan(count,1))) return fallback();
        ++count;
        const auto lowered=lower_scalar(std::span(bytes).first(count),state.rip);
        if (!lowered) {
            if (lowered.error()==jit_error::truncated) continue;
            return fallback();
        }
        const auto compiled=emit_arm64(std::span(&*lowered,1));
        if (!compiled) return std::unexpected(compiled.error()==jit_error::out_of_memory ?
            runtime::code_error::out_of_memory : runtime::code_error::invalid);
        auto code=factory(*compiled,runtime::code_isa::arm64);
        if (!code) return std::unexpected(code.error());
        if (!*code) return std::unexpected(runtime::code_error::host_failure);
        const auto executed=(*code)->invoke(&state);
        if (!executed) return std::unexpected(executed.error());
        step_result result{};
        result.instruction=bytes; result.length=static_cast<std::uint8_t>(count);
        return arm64_step_result{result,true};
    }
}
} // namespace pcsx5::execution
