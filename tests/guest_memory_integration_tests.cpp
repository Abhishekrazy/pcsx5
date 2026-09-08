#include <pcsx5/runtime/guest_memory_backing.h>
#include <pcsx5/runtime/worker.h>
#include <pcsx5/runtime/timing.h>
#include "host_memory_provider.h"
#include <array>
#include <cstdio>

namespace core = pcsx5::core;
namespace rt = pcsx5::runtime;
#if defined(PCSX5_TEST_LINUX)
constexpr auto start_worker = rt::start_linux_worker;
constexpr auto counter_now = rt::linux_counter_now;
constexpr auto counter_frequency = rt::linux_counter_frequency;
#else
constexpr auto start_worker = rt::start_windows_worker;
constexpr auto counter_now = rt::windows_counter_now;
constexpr auto counter_frequency = rt::windows_counter_frequency;
#endif
struct worker_context { core::guest_memory& guest; bool success{}; };
void worker_write(void* raw) {
    auto& context = *static_cast<worker_context*>(raw);
    const std::array pattern{std::byte{9}};
    context.success = context.guest.write(0x10000, pattern).has_value();
}
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (false)
template<class T> bool fails(const core::guest_memory_result<T>& r, core::guest_memory_error e) { return !r && r.error() == e; }
rt::memory_error injected_error{};
rt::memory_result<std::unique_ptr<rt::memory_reservation>> failure(std::uint64_t) noexcept {
    return std::unexpected(injected_error);
}
rt::memory_result<std::unique_ptr<rt::memory_reservation>> null_owner(std::uint64_t) noexcept { return nullptr; }
int main() {
    const auto geometry = test_host::memory_geometry(); CHECK(geometry);
    const auto page = geometry->page_size();
    CHECK(fails(rt::make_guest_memory_backing(page, nullptr), core::guest_memory_error::unsupported));
    CHECK(fails(rt::make_guest_memory_backing(page, null_owner), core::guest_memory_error::host_failure));
    constexpr std::array errors{
        std::pair{rt::memory_error::invalid_range, core::guest_memory_error::invalid_range},
        std::pair{rt::memory_error::invalid_state, core::guest_memory_error::invalid_state},
        std::pair{rt::memory_error::unsupported, core::guest_memory_error::unsupported},
        std::pair{rt::memory_error::access_denied, core::guest_memory_error::access_denied},
        std::pair{rt::memory_error::out_of_memory, core::guest_memory_error::out_of_memory},
        std::pair{rt::memory_error::host_failure, core::guest_memory_error::host_failure}};
    for (const auto& [native, normalized] : errors) {
        injected_error = native;
        CHECK(fails(rt::make_guest_memory_backing(page, failure), normalized));
    }
    CHECK(fails(rt::make_guest_memory_backing(0, test_host::reserve_memory), core::guest_memory_error::invalid_range));
    auto a = rt::make_guest_memory_backing(page, test_host::reserve_memory);
    auto b = rt::make_guest_memory_backing(page, test_host::reserve_memory);
    CHECK(a && *a && b && *b);
    core::guest_memory guest;
    CHECK(guest.map(0x10000, page, core::guest_memory_access::read_write, *a));
    CHECK(!*a);
    CHECK(fails(guest.map(0x10000, page, core::guest_memory_access::read_write, *b), core::guest_memory_error::overlap));
    CHECK(*b);
    CHECK(guest.map(0x10000 + page, page, core::guest_memory_access::read_write, *b));
    std::array<std::byte, 4> pattern{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}}, out{};
    CHECK(guest.read(0x10000, out)); CHECK((out == std::array<std::byte, 4>{}));
    CHECK(guest.write(0x10000 + page - 4, pattern));
    CHECK(guest.read(0x10000 + page - 4, out)); CHECK(out == pattern);
    CHECK(fails(guest.write(0x10000 + page - 2, pattern), core::guest_memory_error::invalid_range));
    CHECK(guest.read(0x10000 + page - 4, out)); CHECK(out == pattern);
    CHECK(guest.protect(0x10000, core::guest_memory_access::read_only));
    CHECK(fails(guest.write(0x10000, pattern), core::guest_memory_error::access_denied));
    CHECK(guest.read(0x10000 + page - 4, out)); CHECK(out == pattern);
    CHECK(guest.protect(0x10000, core::guest_memory_access::none));
    CHECK(fails(guest.read(0x10000, out), core::guest_memory_error::access_denied));
    CHECK(guest.unmap(0x10000)); CHECK(guest.mapping_count() == 1);
    CHECK(fails(guest.read(0x10000, out), core::guest_memory_error::unmapped));
    CHECK(guest.write(0x10000 + page, pattern));
    CHECK(guest.read(0x10000 + page, out)); CHECK(out == pattern);
    CHECK(guest.unmap(0x10000 + page)); CHECK(guest.mapping_count() == 0);
    // Remap uses a new real reservation: old data must not survive.
    auto fresh = rt::make_guest_memory_backing(page, test_host::reserve_memory); CHECK(fresh);
    CHECK(guest.map(0x10000, page, core::guest_memory_access::read_write, *fresh));
    CHECK(guest.read(0x10000 + page - 4, out)); CHECK((out == std::array<std::byte, 4>{}));
    // No simultaneous guest-memory access: join publishes the worker's writes.
    const auto frequency = counter_frequency(); CHECK(frequency);
    const auto before = counter_now(); CHECK(before);
    worker_context context{guest};
    auto worker = start_worker(worker_write, &context); CHECK(worker && *worker);
    CHECK((*worker)->join() == rt::worker_completion::returned); CHECK(context.success);
    const auto after = counter_now(); CHECK(after);
    CHECK(rt::elapsed_nanoseconds(*before, *after, *frequency));
    CHECK(guest.read(0x10000, out)); CHECK(out[0] == std::byte{9});
    // Final mapping exercises owned destruction as well as explicit unmap above.
    std::puts("guest-memory-integration-v1: zero write read bounds permissions isolation remap worker clock PASS");
}
