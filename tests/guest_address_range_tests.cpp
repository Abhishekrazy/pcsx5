#include <pcsx5/core/guest_address_range.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>
#include <type_traits>

namespace {
using pcsx5::core::guest_address_range;
constexpr auto max_address = std::numeric_limits<std::uint64_t>::max();

static_assert(!std::is_default_constructible_v<guest_address_range>);
static_assert(!std::is_constructible_v<guest_address_range, std::uint64_t, std::uint64_t>);
static_assert(std::is_trivially_copyable_v<guest_address_range>);
static_assert(noexcept(guest_address_range::make(0, 1)));
static_assert(!guest_address_range::make(0, 0));
static_assert(!guest_address_range::make(max_address, 2));
static_assert(guest_address_range::make(max_address, 1)->contains(max_address));
static_assert(guest_address_range::make(max_address, 1)->offset_of(max_address) == 0);

int failures = 0;
std::uint64_t checks = 0;

void check(bool result, std::string_view label) {
    ++checks;
    if (!result) {
        if (failures < 10) {
            std::cerr << "FAIL: " << label << '\n';
        }
        ++failures;
    }
}

void edge_cases() {
    check(!guest_address_range::make(0, 0), "empty at zero rejected");
    check(!guest_address_range::make(max_address, 0), "empty at maximum rejected");
    check(!guest_address_range::make(max_address, 2), "one-byte overflow rejected");
    check(!guest_address_range::make(2, max_address), "large overflow rejected");
    const auto low = guest_address_range::make(0, max_address);
    const auto high = guest_address_range::make(1, max_address);
    const auto last = guest_address_range::make(max_address, 1);
    check(low.has_value() && high.has_value() && last.has_value(), "large valid ranges");
    if (!low || !high || !last) {
        return;
    }
    check(low->base() == 0 && low->size() == max_address, "accessors preserve values");
    check(low->contains(max_address - 1) && !low->contains(max_address), "upper edge excluded");
    check(high->contains(max_address) && !high->contains(0), "final byte included");
    check(high->contains(*last) && !low->contains(*last), "range containment at final byte");
    check(!low->overlaps(*last) && !last->overlaps(*low), "adjacency at maximum");
    check(low->overlaps(*high) && high->overlaps(*low), "large overlap symmetric");
    check(high->offset_of(max_address) == max_address - 1, "large offset");
    check(!high->offset_of(0), "below-base offset rejected");
    check(last->offset_of(max_address) == 0, "final-byte offset");
    check(!last->offset_of(0), "wrapped offset rejected");
    check(!low->contains(*high) && !high->contains(*low), "partial overlap is not containment");
}

// Independent small-domain oracle: enumerate the occupied bytes instead of
// reproducing the production subtraction-based interval formulas.
void enumerate(std::uint64_t origin) {
    constexpr std::size_t domain = 16;
    for (std::size_t base = 0; base < domain; ++base) {
        for (std::size_t size = 1; size <= domain - base; ++size) {
            const auto range = guest_address_range::make(origin + base, size);
            check(range.has_value(), "enumerated range valid");
            if (!range) {
                continue;
            }
            std::array<bool, domain> occupied{};
            for (std::size_t byte = base; byte < base + size; ++byte) {
                occupied[byte] = true;
            }
            for (std::size_t byte = 0; byte < domain; ++byte) {
                check(range->contains(origin + byte) == occupied[byte], "address membership oracle");
                const auto offset = range->offset_of(origin + byte);
                check(offset.has_value() == occupied[byte], "offset validity oracle");
                if (offset) {
                    check(*offset == byte - base, "offset value oracle");
                }
            }
            for (std::size_t other_base = 0; other_base < domain; ++other_base) {
                for (std::size_t other_size = 1; other_size <= domain - other_base; ++other_size) {
                    const auto other = guest_address_range::make(origin + other_base, other_size);
                    check(other.has_value(), "second enumerated range valid");
                    if (!other) {
                        continue;
                    }
                    bool any = false;
                    bool all = true;
                    for (std::size_t byte = other_base; byte < other_base + other_size; ++byte) {
                        any = any || occupied[byte];
                        all = all && occupied[byte];
                    }
                    check(range->overlaps(*other) == any, "overlap oracle");
                    check(other->overlaps(*range) == any, "symmetric overlap oracle");
                    check(range->contains(*other) == all, "range containment oracle");
                }
            }
        }
    }
}

void factory_near_maximum() {
    // There are exactly 16 - base occupied byte positions left in this window.
    // Small integer counting is independent of the production uint64_t check.
    for (std::uint64_t base = 0; base < 16; ++base) {
        for (std::uint64_t size = 0; size <= 17; ++size) {
            const bool expected = size != 0 && size <= 16 - base;
            check(guest_address_range::make(max_address - 15 + base, size).has_value()
                      == expected,
                  "near-maximum factory oracle");
        }
    }
}
} // namespace

int main() {
    edge_cases();
    factory_near_maximum();
    enumerate(0);
    enumerate(max_address - 15);
    std::cout << "guest_address_range: " << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
