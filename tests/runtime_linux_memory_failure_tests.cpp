#include "linux_memory_api.h"
#include <sys/mman.h>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string_view>

using namespace pcsx5::runtime;
namespace {
int failures{};
void check(bool value, const char* label) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", label); ++failures; }
}
struct context {
    bool fail_map{}, fail_unmap{}, fail_protect{}, fail_discard{};
    bool mark_unmap_failure{};
    std::size_t partial_protect_bytes{};
    void* base{};
    int unmaps{};
};
void* map(void* opaque, std::size_t size) noexcept {
    auto& c = *static_cast<context*>(opaque);
    if (c.fail_map) { errno = ENOMEM; return MAP_FAILED; }
    c.base = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return c.base;
}
int unmap(void* opaque, void* base, std::size_t size) noexcept {
    auto& c = *static_cast<context*>(opaque);
    if (c.fail_unmap) {
        if (c.mark_unmap_failure) {
            std::puts("INJECTED_RELEASE_FAILURE"); std::fflush(stdout);
        }
        errno = EIO;
        return -1;
    }
    ++c.unmaps;
    return munmap(base, size);
}
int protect(void* opaque, void* base, std::size_t size, int protection) noexcept {
    auto& c = *static_cast<context*>(opaque);
    if (c.fail_protect) {
        // Model a native call which changed a prefix before reporting failure.
        if (c.partial_protect_bytes != 0 &&
            mprotect(base, c.partial_protect_bytes, protection) != 0) return -1;
        errno = ENOMEM;
        return -1;
    }
    return mprotect(base, size, protection);
}
int discard(void* opaque, void* base, std::size_t size) noexcept {
    auto& c = *static_cast<context*>(opaque);
    if (c.fail_discard) { errno = EIO; return -1; }
    return madvise(base, size, MADV_DONTNEED);
}
detail::linux_memory_api api(context& c) { return {&c, map, unmap, protect, discard}; }
template<class T> bool error(const memory_result<T>& result, memory_error expected) {
    return !result && result.error() == expected;
}
}
int main(int argc, char** argv) {
    const auto geometry = linux_memory_geometry();
    if (!geometry) return 1;
    const auto page = geometry->page_size();
    context c;
    if (argc == 2 && std::string_view(argv[1]) == "destructor-failure") {
        std::set_terminate([] { std::puts("EXPECTED_DESTRUCTOR_TERMINATION"); std::fflush(stdout); std::_Exit(73); });
        auto result = detail::reserve_linux_memory_with_api(page, api(c));
        if (!result) return 2;
        c.fail_unmap = true;
        c.mark_unmap_failure = true;
        result->reset();
        return 3;
    }
    check(error(detail::reserve_linux_memory_with_api(page, {}), memory_error::unsupported), "null seam");
    c.fail_map = true;
    check(error(detail::reserve_linux_memory_with_api(page, api(c)), memory_error::out_of_memory), "map failure");
    c.fail_map = false;
    auto result = detail::reserve_linux_memory_with_api(2 * page, api(c));
    if (!result) return 1;
    auto& owner = **result;
    check(owner.commit(0, page, memory_access::read_write).has_value(), "commit");
    std::array data{std::byte{42}};
    check(owner.write(0, data).has_value(), "write");
    c.fail_discard = true;
    check(error(owner.decommit(0, page), memory_error::host_failure), "discard failure");
    auto info = owner.query(0);
    check(info && info->state == page_state::committed && info->access == memory_access::none, "partial decommit state");
    check(error(owner.read(0, data), memory_error::access_denied), "partial decommit denies copy");
    c.fail_discard = false;
    check(owner.decommit(0, page).has_value(), "retry discard");
    check(owner.commit(0, page, memory_access::read_write).has_value(), "recommit");
    check(owner.read(0, data).has_value() && data[0] == std::byte{}, "discard zero fill");
    c.fail_protect = true;
    c.partial_protect_bytes = static_cast<std::size_t>(page);
    check(error(owner.protect(0, page, memory_access::read_only), memory_error::out_of_memory), "protect failure");
    check(error(owner.query(0), memory_error::host_failure), "uncertain query");
    check(error(owner.read(0, data), memory_error::host_failure), "uncertain read");
    check(owner.query(page).has_value(), "unaffected page query");
    check(error(owner.commit(page, page, memory_access::read_write), memory_error::out_of_memory), "commit native failure");
    check(error(owner.query(page), memory_error::host_failure), "commit uncertain");
    c.fail_protect = false;
    c.partial_protect_bytes = 0;
    c.fail_unmap = true;
    check(error(owner.release(), memory_error::host_failure), "release retains owner");
    c.fail_unmap = false;
    check(owner.release().has_value(), "release retry");
    unsigned char residency{};
    errno = 0;
    check(mincore(c.base, static_cast<std::size_t>(page), &residency) == -1 && errno == ENOMEM, "native unmapped");
    check(c.unmaps == 1 && owner.release().has_value(), "release idempotent");
    context other;
    { auto a = detail::reserve_linux_memory_with_api(page, api(c));
      auto b = detail::reserve_linux_memory_with_api(page, api(other));
      check(a && b && c.base != other.base, "independent owners"); }
    check(c.unmaps == 2 && other.unmaps == 1, "destructor cleanup per owner");
    {
        auto retry = detail::reserve_linux_memory_with_api(page, api(other));
        if (!retry) return 1;
        check((*retry)->commit(0, page, memory_access::read_write).has_value(), "retry owner commit");
        other.fail_protect = true;
        check(error((*retry)->decommit(0, page), memory_error::out_of_memory), "decommit protection failure");
        check(error((*retry)->query(0), memory_error::host_failure), "decommit uncertainty");
        other.fail_protect = false;
        other.fail_unmap = true;
        check(error((*retry)->release(), memory_error::host_failure), "destructor retry preparation");
        other.fail_unmap = false;
    }
    check(other.unmaps == 2, "destructor retries failed explicit release");
    std::puts("Linux memory failure checks completed");
    return failures == 0 ? 0 : 1;
}
