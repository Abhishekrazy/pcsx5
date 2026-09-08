#pragma once
#include <pcsx5/execution/native_step.h>

namespace pcsx5::execution::testing {
// Private, per-invocation failure seam. Injected failures occur after real
// resources exist and use normal teardown. This does not simulate OS exhaustion.
enum class native_failure_stage { none, after_images, after_context, before_snapshot, hold_after_context };
[[nodiscard]] native_result step_linux_native_injected(const std::filesystem::path&,
    const native_request&, std::uint32_t, native_failure_stage, bool* reached = nullptr) noexcept;
[[nodiscard]] native_result step_windows_native_injected(const std::filesystem::path&,
    const native_request&, std::uint32_t, native_failure_stage, bool* reached = nullptr) noexcept;
} // namespace pcsx5::execution::testing
