#include <pcsx5/runtime/worker.h>
#if defined(PCSX5_TEST_LINUX)
#include "../runtime/src/linux_worker_api.h"
#else
#include "../runtime/src/windows_worker_api.h"
#endif

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <latch>
#include <stdexcept>
#include <system_error>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {
using namespace pcsx5::runtime;

#if defined(PCSX5_TEST_LINUX)
using test_worker_api = detail::linux_worker_api;
constexpr auto start_worker = start_linux_worker;
constexpr auto start_worker_with_api = detail::start_linux_worker_with_api;
#else
using test_worker_api = detail::worker_api;
constexpr auto start_worker = start_windows_worker;
constexpr auto start_worker_with_api = detail::start_windows_worker_with_api;
#endif

void check(bool condition, const char* label) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", label);
        std::exit(1);
    }
}

struct failure_context {
    bool fail_start{false};
    bool fail_join{false};
    unsigned joins{0};
    bool mark_join_failure{false};
};

std::thread injected_start(void* raw, void (*entry)(void*) noexcept, void* context) {
    if (static_cast<failure_context*>(raw)->fail_start) {
        throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
    }
    return std::thread(entry, context);
}

void injected_join(void* raw, std::thread& thread) {
    auto& state = *static_cast<failure_context*>(raw);
    ++state.joins;
    if (state.fail_join) {
        if (state.mark_join_failure) {
            std::puts("WORKER_JOIN_FAILURE_INJECTED");
            std::fflush(stdout);
        }
        throw std::system_error(std::make_error_code(std::errc::invalid_argument));
    }
    thread.join();
}

test_worker_api api(failure_context& context) {
    return {&context, injected_start, injected_join};
}

void increment(void* context) { ++*static_cast<unsigned*>(context); }
void throws(void*) { throw std::runtime_error("synthetic callback failure"); }
void no_context(void* context) { if (context) throw std::runtime_error("non-null context"); }

struct concurrent_context {
    std::latch entered{2};
    std::latch release{1};
};
void concurrent(void* raw) {
    auto& context = *static_cast<concurrent_context*>(raw);
    context.entered.count_down();
    context.release.wait();
}

struct self_context {
    std::latch published{1};
    std::latch attempted{1};
    host_worker* worker{nullptr};
    bool rejected{false};
};
void self_join(void* raw) {
    auto& context = *static_cast<self_context*>(raw);
    context.published.wait();
    const auto result = context.worker->join();
    context.rejected = !result && result.error() == worker_error::self_join;
    context.attempted.count_down();
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--terminate-join") {
        failure_context failure{false, true, 0};
        failure.mark_join_failure = true;
        auto doomed = start_worker_with_api(no_context, nullptr, api(failure));
        check(doomed && *doomed, "termination owner start");
        std::set_terminate([] {
            std::puts("WORKER_JOIN_TERMINATED");
            std::fflush(stdout);
            std::_Exit(73);
        });
        doomed->reset();
        return 2;
    }
    check(argc == 1, "known test mode");
    static_assert(!std::is_copy_constructible_v<host_worker>);
    static_assert(!std::is_move_constructible_v<host_worker>);
    const auto invalid = start_worker(nullptr, nullptr);
    check(!invalid && invalid.error() == worker_error::invalid_callback, "null callback");

    unsigned writes{0};
    auto started = start_worker(increment, &writes);
    check(started && *started, "start owner");
    auto owner = std::move(*started);
    check(!*started, "unique ownership transfer");
    check(owner->join() == worker_completion::returned, "join normal callback");
    check(writes == 1, "join publishes callback writes");
    check(owner->join() == worker_completion::returned && writes == 1, "idempotent join");
    owner.reset();

    {
        auto automatic = start_worker(increment, &writes);
        check(automatic && *automatic, "automatic cleanup start");
    }
    check(writes == 2, "destructor joins and publishes writes");

    auto throwing = start_worker(throws, nullptr);
    check(throwing && *throwing, "throwing start");
    check((*throwing)->join() == worker_completion::callback_threw, "exception normalized");
    check((*throwing)->join() == worker_completion::callback_threw, "exception result retained");
    auto empty = start_worker(no_context, nullptr);
    check(empty && (*empty)->join() == worker_completion::returned, "null context supported");

    concurrent_context together;
    auto first = start_worker(concurrent, &together);
    auto second = start_worker(concurrent, &together);
    check(first && second, "two workers start");
    together.entered.wait();
    together.release.count_down();
    check((*first)->join() == worker_completion::returned, "first concurrent completion");
    check((*second)->join() == worker_completion::returned, "second concurrent completion");

    self_context self;
    auto self_owner = start_worker(self_join, &self);
    check(self_owner && *self_owner, "self-join start");
    self.worker = self_owner->get();
    self.published.count_down();
    self.attempted.wait();
    check(self.rejected, "self-join rejected");
    check((*self_owner)->join() == worker_completion::returned, "owner retained after self-join");

    failure_context failures{true, false, 0};
    auto failed = start_worker_with_api(increment, &writes, api(failures));
    check(!failed && failed.error() == worker_error::resource_unavailable, "start failure normalized");
    check(writes == 2 && failures.joins == 0, "failed start did not invoke callback or join");
    auto incomplete = api(failures);
    incomplete.join = nullptr;
    const auto invalid_api = start_worker_with_api(increment, &writes, incomplete);
    check(!invalid_api && invalid_api.error() == worker_error::host_failure, "invalid seam rejected");
    failures.fail_start = false;
    failures.fail_join = true;
    auto retry = start_worker_with_api(increment, &writes, api(failures));
    check(retry && *retry, "retry owner start");
    const auto join_failed = (*retry)->join();
    check(!join_failed && join_failed.error() == worker_error::host_failure, "join failure normalized");
    failures.fail_join = false;
    check((*retry)->join() == worker_completion::returned, "join retry retains worker");
    check(writes == 3 && failures.joins == 2, "retry exactly once callback");
    retry->reset();
    check(failures.joins == 2, "joined destructor does not rejoin");
    std::puts("worker lifecycle tests passed");
}
