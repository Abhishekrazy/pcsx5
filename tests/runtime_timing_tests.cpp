#include <pcsx5/runtime/timing.h>

#include <cstdint>
#include <iostream>
#include <limits>

namespace rt = pcsx5::runtime;

int main() {
    unsigned failures{};
    std::uint64_t checks{};
    const auto check = [&](bool condition) {
        ++checks;
        if (!condition) ++failures;
    };
    const auto convert = [](std::uint64_t ticks, std::uint64_t frequency) {
        return rt::elapsed_nanoseconds({0}, {ticks}, *rt::tick_frequency::make(frequency));
    };
    const auto count_is = [](const rt::timing_result<rt::nanosecond_duration>& value,
                             std::uint64_t expected) {
        return value && value->count == expected;
    };
    check(!rt::tick_frequency::make(0));
    check(rt::tick_frequency::make(0) == std::unexpected(rt::timing_error::invalid_frequency));
    for (std::uint64_t frequency = 1; frequency <= 127; ++frequency) {
        for (std::uint64_t ticks = 0; ticks <= 511; ++ticks) {
            const auto result = convert(ticks, frequency);
            check(result && result->count == ticks * 1'000'000'000 / frequency);
        }
    }
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    check(count_is(convert(maximum, maximum), 1'000'000'000));
    check(count_is(convert(maximum - 1, maximum), 999'999'999));
    check(count_is(convert(maximum / 2, maximum), 499'999'999));
    check(count_is(convert(maximum / 2 + 1, maximum), 500'000'000));
    check(count_is(convert(maximum, 1'000'000'000), maximum));
    check(convert(maximum, 1) == std::unexpected(rt::timing_error::overflow));
    check(convert(maximum, 999'999'999) == std::unexpected(rt::timing_error::overflow));
    check(count_is(convert(maximum / 1'000'000'000, 1),
          (maximum / 1'000'000'000) * 1'000'000'000));
    check(convert(maximum / 1'000'000'000 + 1, 1) == std::unexpected(rt::timing_error::overflow));
    for (std::uint64_t distance = 0; distance < 1024; ++distance) {
        const auto frequency = maximum - distance;
        check(count_is(convert(frequency - 1, frequency), 999'999'999));
        check(count_is(convert(frequency / 2, frequency),
                       frequency % 2 == 0 ? 500'000'000 : 499'999'999));
    }
    const auto one = *rt::tick_frequency::make(1);
    check(count_is(rt::elapsed_nanoseconds({maximum}, {maximum}, one), 0));
    check(count_is(rt::elapsed_nanoseconds({maximum - 1}, {maximum}, one), 1'000'000'000));
    check(rt::elapsed_nanoseconds({1}, {0}, one) == std::unexpected(rt::timing_error::reversed_ticks));
    static_assert(rt::elapsed_nanoseconds({7}, {8}, *rt::tick_frequency::make(3))->count == 333'333'333);
    std::cout << checks << " timing checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
