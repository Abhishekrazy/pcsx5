#include "windows_timing_api.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

namespace pcsx5::runtime {
namespace {
bool query_frequency(void*, std::int64_t& output) noexcept {
    LARGE_INTEGER native{};
    if (QueryPerformanceFrequency(&native) == 0) return false;
    output = native.QuadPart;
    return true;
}
bool query_counter(void*, std::int64_t& output) noexcept {
    LARGE_INTEGER native{};
    if (QueryPerformanceCounter(&native) == 0) return false;
    output = native.QuadPart;
    return true;
}
} // namespace

timing_result<tick_frequency> detail::counter_frequency_from(
    counter_query query, void* context) noexcept {
    std::int64_t value{};
    if (query == nullptr || !query(context, value)) return std::unexpected(timing_error::host_failure);
    if (value <= 0) return std::unexpected(timing_error::invalid_frequency);
    return tick_frequency::make(static_cast<std::uint64_t>(value));
}

timing_result<tick_stamp> detail::counter_now_from(counter_query query, void* context) noexcept {
    std::int64_t value{};
    if (query == nullptr || !query(context, value) || value < 0)
        return std::unexpected(timing_error::host_failure);
    return tick_stamp{static_cast<std::uint64_t>(value)};
}

timing_result<tick_frequency> windows_counter_frequency() noexcept {
    return detail::counter_frequency_from(query_frequency, nullptr);
}
timing_result<tick_stamp> windows_counter_now() noexcept {
    return detail::counter_now_from(query_counter, nullptr);
}
} // namespace pcsx5::runtime
