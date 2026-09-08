#pragma once
#include <pcsx5/execution/arm64_jit.h>
#include <pcsx5/runtime/executable_code.h>
namespace pcsx5::execution {
using code_factory=runtime::code_result<std::unique_ptr<runtime::executable_code>> (*)(
    std::span<const std::byte>,runtime::code_isa) noexcept;
struct arm64_step_result {
    step_result result{};
    bool translated{};
};
// Execute compiled register scalars; other encodings use the interpreter and
// explicitly report translated=false. Code-cache errors propagate without
// attempting a second execution. Calls and guest instruction bytes must remain
// externally serialized/stable. Each call compiles a fresh immutable image, so
// no stale guest-code cache is retained. No oracle arithmetic on compiled paths.
[[nodiscard]] runtime::code_result<arm64_step_result> step_arm64(
    cpu_state&,core::guest_memory&,code_factory) noexcept;
} // namespace pcsx5::execution
