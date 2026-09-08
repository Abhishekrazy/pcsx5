#include <pcsx5/runtime/timing.h>
#include "../runtime/src/linux_timing_api.h"

#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
using namespace pcsx5::runtime;
void check(bool value, const char* label) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", label); std::exit(1); }
}
struct sample { timespec stamp; int status; };
int query(void* raw, timespec& stamp) noexcept {
    const auto& value = *static_cast<const sample*>(raw);
    stamp = value.stamp;
    return value.status;
}
void error(sample value, timing_error expected, const char* label) {
    const auto result = detail::linux_counter_now_from(query, &value);
    check(!result && result.error() == expected, label);
}
} // namespace

int main() {
    const auto missing = detail::linux_counter_now_from(nullptr, nullptr);
    check(!missing && missing.error() == timing_error::host_failure, "null native callback");
    error({{7, 9}, -1}, timing_error::host_failure, "native failure");
    error({{-1, 0}, 0}, timing_error::host_failure, "negative seconds");
    error({{0, -1}, 0}, timing_error::host_failure, "negative nanoseconds");
    error({{0, 1'000'000'000}, 0}, timing_error::host_failure, "invalid nanosecond component");
    sample zero{{0, 0}, 0};
    const auto origin = detail::linux_counter_now_from(query, &zero);
    check(origin && origin->count == 0, "zero epoch valid");
    sample exact{{12, 345}, 0};
    const auto converted = detail::linux_counter_now_from(query, &exact);
    check(converted && converted->count == 12'000'000'345ULL, "seconds and nanoseconds conversion");
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    // Linux x64's signed 64-bit time_t can represent the uint64-nanosecond edge.
    static_assert(std::numeric_limits<time_t>::max() >= maximum / 1'000'000'000ULL + 1);
    sample edge{{static_cast<time_t>(maximum / 1'000'000'000ULL),
                 static_cast<long>(maximum % 1'000'000'000ULL)}, 0};
    const auto largest = detail::linux_counter_now_from(query, &edge);
    check(largest && largest->count == maximum, "last representable stamp");
    ++edge.stamp.tv_nsec;
    error(edge, timing_error::overflow, "fraction overflow");
    error({{static_cast<time_t>(maximum / 1'000'000'000ULL + 1), 0}, 0},
          timing_error::overflow, "seconds overflow");
    const auto frequency = linux_counter_frequency();
    check(frequency && frequency->count() == 1'000'000'000ULL, "nanosecond frequency");
    auto previous = linux_counter_now();
    check(previous.has_value(), "real native sample");
    for (unsigned index = 0; index < 4096; ++index) {
        const auto current = linux_counter_now();
        check(current.has_value(), "real repeated sample");
        check(current->count >= previous->count, "nondecreasing monotonic clock");
        check(elapsed_nanoseconds(*previous, *current, *frequency).has_value(), "real elapsed conversion");
        previous = current;
    }
    std::puts("Linux timing tests passed");
}
