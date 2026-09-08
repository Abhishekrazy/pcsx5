#pragma once

#include <pcsx5/runtime/timing.h>
#include <ctime>

namespace pcsx5::runtime::detail {

// Private Linux seam. The callback reports clock_gettime success by returning
// zero and initializing the complete timespec. Output is ignored on failure.
using linux_clock_query = int (*)(void*, timespec&) noexcept;
[[nodiscard]] timing_result<tick_stamp> linux_counter_now_from(
    linux_clock_query query, void* context) noexcept;

} // namespace pcsx5::runtime::detail
