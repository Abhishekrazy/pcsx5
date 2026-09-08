#include <pcsx5/runtime/memory.h>

#include <array>
#include <cstdio>
#include <limits>
#include <type_traits>
#include <utility>

using namespace pcsx5::runtime;
static_assert(!std::is_copy_constructible_v<memory_reservation>);
static_assert(!std::is_move_constructible_v<memory_reservation>);
static_assert(std::has_virtual_destructor_v<memory_reservation>);

#define CHECK(expression) do { if (!(expression)) { \
    std::fprintf(stderr, "line %d: %s\n", __LINE__, #expression); return 1; } } while (false)

template<class T>
bool fails(const memory_result<T>& result, memory_error error) {
    return !result && result.error() == error;
}

bool has_state(const memory_reservation& owner, std::uint64_t offset, page_state state) {
    const auto info = owner.query(offset);
    return info && info->state == state;
}

bool has_access(const memory_reservation& owner, std::uint64_t offset, memory_access access) {
    const auto info = owner.query(offset);
    return info && info->access == access;
}

int main() {
    const auto geometry = windows_memory_geometry();
    CHECK(geometry);
    const auto page = geometry->page_size();
    CHECK(page > 1 && page <= std::numeric_limits<std::uint64_t>::max() / 4);
    CHECK(geometry->reservation_alignment() % page == 0);
    CHECK(fails(reserve_windows_memory(0), memory_error::invalid_range));
    CHECK(fails(reserve_windows_memory(page + 1), memory_error::invalid_range));
    const auto largest_aligned = std::numeric_limits<std::uint64_t>::max() -
        std::numeric_limits<std::uint64_t>::max() % page;
    CHECK(fails(reserve_windows_memory(largest_aligned), memory_error::invalid_range));
    auto allocation = reserve_windows_memory(page * 4);
    CHECK(allocation && *allocation);
    auto owner = std::move(*allocation);
    CHECK(!*allocation);
    CHECK(owner->size() == page * 4);
    CHECK(owner->geometry().page_size() == page);
    for (std::uint64_t offset = 0; offset < page * 4; offset += page) {
        const auto info = owner->query(offset);
        CHECK(info && info->state == page_state::reserved && info->access == memory_access::none);
    }
    std::array<std::byte, 4> pattern{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    std::array<std::byte, 4> output{};
    CHECK(fails(owner->read(0, output), memory_error::invalid_state));
    CHECK(fails(owner->write(0, pattern), memory_error::invalid_state));
    CHECK(fails(owner->protect(0, page, memory_access::read_write), memory_error::invalid_state));
    CHECK(fails(owner->decommit(0, page), memory_error::invalid_state));
    CHECK(fails(owner->commit(1, page, memory_access::read_write), memory_error::invalid_range));
    CHECK(fails(owner->commit(0, 0, memory_access::read_write), memory_error::invalid_range));
    CHECK(fails(owner->commit(page * 4, page, memory_access::read_write), memory_error::invalid_range));
    CHECK(fails(owner->commit(std::numeric_limits<std::uint64_t>::max(), page,
        memory_access::read_write), memory_error::invalid_range));
    CHECK(fails(owner->commit(0, page, static_cast<memory_access>(99)), memory_error::unsupported));
    CHECK(owner->commit(page, page * 2, memory_access::read_write));
    CHECK(fails(owner->protect(page, page, static_cast<memory_access>(99)), memory_error::unsupported));
    CHECK(has_access(*owner, page, memory_access::read_write));
    CHECK(owner->read(page, output) && output == (std::array<std::byte, 4>{}));
    CHECK(owner->write(page * 2 - 2, pattern));
    CHECK(owner->read(page * 2 - 2, output) && output == pattern);
    CHECK(fails(owner->commit(0, page * 2, memory_access::read_only), memory_error::invalid_state));
    CHECK(has_state(*owner, 0, page_state::reserved));
    CHECK(has_access(*owner, page, memory_access::read_write));
    CHECK(fails(owner->protect(0, page * 2, memory_access::none), memory_error::invalid_state));
    CHECK(has_access(*owner, page, memory_access::read_write));
    CHECK(fails(owner->decommit(0, page * 2), memory_error::invalid_state));
    CHECK(has_state(*owner, page, page_state::committed));
    CHECK(owner->protect(page * 2, page, memory_access::read_only));
    CHECK(has_access(*owner, page * 2 + 1, memory_access::read_only));
    const auto replacement = std::array<std::byte, 4>{std::byte{9}, std::byte{9}, std::byte{9}, std::byte{9}};
    CHECK(fails(owner->write(page * 2 - 2, replacement), memory_error::access_denied));
    CHECK(owner->read(page * 2 - 2, output) && output == pattern); // no partial write
    CHECK(owner->protect(page * 2, page, memory_access::none));
    output = replacement;
    CHECK(fails(owner->read(page * 2 - 2, output), memory_error::access_denied));
    CHECK(output == replacement); // no partial read
    CHECK(owner->decommit(page, page * 2));
    CHECK(has_state(*owner, page, page_state::reserved));
    CHECK(owner->commit(page, page * 2, memory_access::read_write));
    CHECK(owner->read(page * 2 - 2, output) && output == (std::array<std::byte, 4>{}));
    CHECK(fails(owner->query(page * 4), memory_error::invalid_range));
    CHECK(fails(owner->read(page * 4 - 1, output), memory_error::invalid_range));
    CHECK(fails(owner->write(0, {}), memory_error::invalid_range));
    CHECK(fails(owner->read(0, {}), memory_error::invalid_range));
    auto second = reserve_windows_memory(page);
    CHECK(second && *second);
    CHECK((*second)->commit(0, page, memory_access::read_write));
    CHECK((*second)->write(0, pattern));
    CHECK(fails(owner->write(owner->size(), replacement), memory_error::invalid_range));
    CHECK((*second)->read(0, output) && output == pattern);
    CHECK(owner->commit(0, page, memory_access::none));
    CHECK(has_access(*owner, 0, memory_access::none));
    CHECK(fails(owner->read(0, output), memory_error::access_denied));
    CHECK(owner->commit(page * 3, page, memory_access::read_only));
    CHECK(has_access(*owner, page * 3, memory_access::read_only));
    CHECK(owner->read(page * 3, output) && output == (std::array<std::byte, 4>{}));
    CHECK(fails(owner->write(page * 3, pattern), memory_error::access_denied));
    CHECK(owner->release());
    CHECK(owner->release());
    CHECK(owner->size() == page * 4 && owner->geometry().page_size() == page);
    CHECK(fails(owner->query(0), memory_error::invalid_state));
    CHECK(fails(owner->commit(0, page, memory_access::read_write), memory_error::invalid_state));
    CHECK(fails(owner->protect(0, page, memory_access::none), memory_error::invalid_state));
    CHECK(fails(owner->decommit(0, page), memory_error::invalid_state));
    CHECK(fails(owner->read(0, output), memory_error::invalid_state));
    CHECK(fails(owner->write(0, pattern), memory_error::invalid_state));
    CHECK((*second)->read(0, output) && output == pattern);
    // The second owner exercises automatic cleanup; native cleanup observation
    // and injected native failures are separately gated in P2.3.
    std::puts("owned memory lifecycle, range, state and copy protection checks passed");
    return 0;
}
