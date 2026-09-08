#include <pcsx5/runtime/timing.h>
#include "linux_timing_api.h"

#include <ctime>
#include <limits>

namespace pcsx5::runtime {
namespace {
int native_query(void*, timespec& stamp) noexcept {
    return clock_gettime(CLOCK_MONOTONIC, &stamp);
}
} // namespace

timing_result<tick_stamp> detail::linux_counter_now_from(
    linux_clock_query query, void* context) noexcept {
    if (!query) return std::unexpected(timing_error::host_failure);
    timespec stamp{};
    if (query(context, stamp) != 0 || stamp.tv_sec < 0 ||
        stamp.tv_nsec < 0 || stamp.tv_nsec >= 1'000'000'000) {
        return std::unexpected(timing_error::host_failure);
    }
    constexpr std::uint64_t scale = 1'000'000'000;
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto seconds = static_cast<std::uint64_t>(stamp.tv_sec);
    const auto fraction = static_cast<std::uint64_t>(stamp.tv_nsec);
    if (seconds > maximum / scale) return std::unexpected(timing_error::overflow);
    const auto whole = seconds * scale;
    if (fraction > maximum - whole) return std::unexpected(timing_error::overflow);
    return tick_stamp{whole + fraction};
}

timing_result<tick_frequency> linux_counter_frequency() noexcept {
    return tick_frequency::make(1'000'000'000);
}

timing_result<tick_stamp> linux_counter_now() noexcept {
    return detail::linux_counter_now_from(native_query, nullptr);
}

} // namespace pcsx5::runtime
