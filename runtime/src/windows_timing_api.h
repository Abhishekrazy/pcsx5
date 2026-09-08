#pragma once

#include <pcsx5/runtime/timing.h>

namespace pcsx5::runtime::detail {

// Private synchronous injection seam; no global override or retained context.
using counter_query = bool (*)(void*, std::int64_t&) noexcept;
[[nodiscard]] timing_result<tick_frequency> counter_frequency_from(
    counter_query query, void* context) noexcept;
[[nodiscard]] timing_result<tick_stamp> counter_now_from(
    counter_query query, void* context) noexcept;

} // namespace pcsx5::runtime::detail
