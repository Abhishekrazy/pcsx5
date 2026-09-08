#include <pcsx5/runtime/page_range.h>
#include <pcsx5/runtime/memory.h>

#include <cstdint>
#include <cstdio>
#include <limits>

using pcsx5::runtime::memory_geometry;
using pcsx5::runtime::page_range;

static_assert(!memory_geometry::make(0, 4096));
static_assert(!memory_geometry::make(4096, 0));
static_assert(!memory_geometry::make(4096, 4097));
static_assert(memory_geometry::make(4096, 65536)->reservation_alignment() == 65536);
static_assert(!page_range::make(*memory_geometry::make(4096, 65536), 8192, 0, 0));

int main() {
    std::uint64_t checks = 0;
    for (std::uint64_t page = 1; page <= 8; ++page) {
        const auto geometry = *memory_geometry::make(page, page * 3);
        for (std::uint64_t capacity = 0; capacity <= 32; ++capacity) {
            for (std::uint64_t offset = 0; offset <= 40; ++offset) {
                for (std::uint64_t size = 0; size <= 40; ++size) {
                    // Small-domain addition is deliberately independent of implementation.
                    const bool expected = capacity != 0 && capacity % page == 0 &&
                        size != 0 && offset % page == 0 && size % page == 0 &&
                        offset + size <= capacity;
                    const auto result = page_range::make(geometry, capacity, offset, size);
                    ++checks;
                    if (result.has_value() != expected ||
                        (result && (result->offset() != offset || result->size() != size))) {
                        std::fprintf(stderr, "page range enumeration failed\n");
                        return 1;
                    }
                }
            }
        }
    }
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    constexpr auto bytes = *memory_geometry::make(1, 1);
    if (!page_range::make(bytes, max, max - 1, 1) ||
        page_range::make(bytes, max, max, 1) ||
        page_range::make(bytes, max, max - 1, 2) ||
        !page_range::make(bytes, max, 0, max) ||
        page_range::make(bytes, 16, max, 2)) {
        std::fprintf(stderr, "page range overflow checks failed\n");
        return 1;
    }
    std::printf("runtime page ranges: %llu enumeration checks and overflow cases passed\n",
        static_cast<unsigned long long>(checks));
    return 0;
}
