#include <pcsx5/runtime/timing.h>
#include "windows_timing_api.h"

#include <iostream>
#include <limits>

namespace rt = pcsx5::runtime;
namespace {
struct synthetic_counter { std::int64_t value; bool success; };
bool query(void* context, std::int64_t& output) noexcept {
    const auto& state = *static_cast<synthetic_counter*>(context);
    output = state.value;
    return state.success;
}
}

int main() {
    unsigned failures{};
    const auto check = [&](bool condition) { if (!condition) ++failures; };
    const auto error_is = [](const auto& result, rt::timing_error error) {
        return !result && result.error() == error;
    };
    const auto stamp_is = [](const auto& result, std::uint64_t count) {
        return result && result->count == count;
    };
    synthetic_counter state{10, false};
    check(error_is(rt::detail::counter_frequency_from(query, &state), rt::timing_error::host_failure));
    check(error_is(rt::detail::counter_now_from(query, &state), rt::timing_error::host_failure));
    check(error_is(rt::detail::counter_frequency_from(nullptr, nullptr), rt::timing_error::host_failure));
    check(error_is(rt::detail::counter_now_from(nullptr, nullptr), rt::timing_error::host_failure));
    state = {-1, true};
    check(error_is(rt::detail::counter_frequency_from(query, &state), rt::timing_error::invalid_frequency));
    check(error_is(rt::detail::counter_now_from(query, &state), rt::timing_error::host_failure));
    state.value = 0;
    check(error_is(rt::detail::counter_frequency_from(query, &state), rt::timing_error::invalid_frequency));
    check(stamp_is(rt::detail::counter_now_from(query, &state), 0));
    state.value = std::numeric_limits<std::int64_t>::max();
    const auto maximum_frequency = rt::detail::counter_frequency_from(query, &state);
    check(maximum_frequency && maximum_frequency->count() == static_cast<std::uint64_t>(state.value));
    check(stamp_is(rt::detail::counter_now_from(query, &state), static_cast<std::uint64_t>(state.value)));

    const auto frequency = rt::windows_counter_frequency();
    auto previous = rt::windows_counter_now();
    if (!frequency || !previous) return 1;
    for (unsigned i = 0; i < 4096; ++i) {
        const auto current = rt::windows_counter_now();
        const auto repeated_frequency = rt::windows_counter_frequency();
        if (!current || !repeated_frequency) return 1;
        check(current->count >= previous->count);
        check(repeated_frequency->count() == frequency->count());
        check(rt::elapsed_nanoseconds(*previous, *current, *frequency).has_value());
        previous = current;
    }
    std::cout << "Windows counter source and injected failures: " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
