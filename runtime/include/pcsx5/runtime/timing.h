#pragma once

#include <cstdint>
#include <expected>
#include <limits>

namespace pcsx5::runtime {

enum class timing_error { invalid_frequency, reversed_ticks, overflow, host_failure };
template<class T> using timing_result = std::expected<T, timing_error>;
struct tick_stamp { std::uint64_t count; };
struct nanosecond_duration { std::uint64_t count; };

class tick_frequency {
public:
    [[nodiscard]] static constexpr timing_result<tick_frequency>
    make(std::uint64_t ticks_per_second) noexcept {
        if (ticks_per_second == 0) return std::unexpected(timing_error::invalid_frequency);
        return tick_frequency(ticks_per_second);
    }
    [[nodiscard]] constexpr std::uint64_t count() const noexcept { return count_; }
private:
    explicit constexpr tick_frequency(std::uint64_t count) noexcept : count_(count) {}
    std::uint64_t count_;
};

// Same clock source and boot are a caller precondition; timestamps carry no
// identity or UTC epoch. Floor to whole nanoseconds, never wrap or saturate.
// Reversed samples (including counter wrap) are rejected, not repaired.
[[nodiscard]] constexpr timing_result<nanosecond_duration> elapsed_nanoseconds(
    tick_stamp start, tick_stamp end, tick_frequency frequency) noexcept {
    if (end.count < start.count) return std::unexpected(timing_error::reversed_ticks);
    constexpr std::uint64_t scale = 1'000'000'000;
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto ticks = end.count - start.count;
    const auto divisor = frequency.count();
    const auto seconds = ticks / divisor;
    if (seconds > maximum / scale) return std::unexpected(timing_error::overflow);
    const auto numerator = ticks % divisor;

    // Compute floor(numerator * scale / divisor) without a wide integer or
    // overflowing product. Process scale's 30 bits; after each bit remainder
    // is less than divisor and quotient is at most the processed scale prefix.
    std::uint64_t quotient{};
    std::uint64_t remainder{};
    for (std::uint64_t bit = std::uint64_t{1} << 29; bit != 0; bit >>= 1) {
        quotient *= 2;
        if (remainder >= divisor - remainder) {
            remainder -= divisor - remainder;
            ++quotient;
        } else {
            remainder += remainder;
        }
        if ((scale & bit) != 0) {
            if (remainder >= divisor - numerator) {
                remainder -= divisor - numerator;
                ++quotient;
            } else {
                remainder += numerator;
            }
        }
    }
    const auto whole = seconds * scale;
    if (quotient > maximum - whole) return std::unexpected(timing_error::overflow);
    return nanosecond_duration{whole + quotient};
}

// Windows-only leaf. Frequency may be cached for this boot. Sequential samples
// on one thread are nondecreasing; cross-thread near-tick ordering is not a
// synchronization primitive. No guest timing, wall-time or accuracy guarantee.
[[nodiscard]] timing_result<tick_frequency> windows_counter_frequency() noexcept;
[[nodiscard]] timing_result<tick_stamp> windows_counter_now() noexcept;

// Linux CLOCK_MONOTONIC leaf, with nanosecond ticks (not a resolution promise).
// The same-source/boot and ordering preconditions above also apply here.
[[nodiscard]] timing_result<tick_frequency> linux_counter_frequency() noexcept;
[[nodiscard]] timing_result<tick_stamp> linux_counter_now() noexcept;

} // namespace pcsx5::runtime
