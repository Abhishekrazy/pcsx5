#include "linux_fault.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>
#include <type_traits>

namespace {
using namespace pcsx5::runtime;
using namespace pcsx5::runtime::detail;
static_assert(std::is_trivially_copyable_v<linux_signal_observation>);
static_assert(std::is_standard_layout_v<linux_signal_observation>);
int failures = 0;
void check(bool condition, const char* name) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", name); ++failures; }
}

// Provider identity must not be inferred from public geometry/size or callbacks.
// Any method invocation here is an error: capture only identifies native owners.
class foreign_reservation final : public memory_reservation {
public:
    memory_geometry geometry() const noexcept override { std::abort(); }
    std::uint64_t size() const noexcept override { std::abort(); }
    memory_result<void> commit(std::uint64_t, std::uint64_t, memory_access) noexcept override { std::abort(); }
    memory_result<void> protect(std::uint64_t, std::uint64_t, memory_access) noexcept override { std::abort(); }
    memory_result<void> decommit(std::uint64_t, std::uint64_t) noexcept override { std::abort(); }
    memory_result<page_info> query(std::uint64_t) const noexcept override { std::abort(); }
    memory_result<void> read(std::uint64_t, std::span<std::byte>) const noexcept override { std::abort(); }
    memory_result<void> write(std::uint64_t, std::span<const std::byte>) noexcept override { std::abort(); }
    memory_result<void> release() noexcept override { std::abort(); }
};

// These globals are private to the single-threaded test child. They are fully
// initialized before handler installation and never mutated while installed.
linux_fault_region child_region{};
fault_access child_access = fault_access::read;
fault_route child_route = fault_route::owned_memory;
struct sigaction previous_handler {};
volatile sig_atomic_t observation_valid = 0;
void terminal_handler(int signal_number, siginfo_t*, void*) {
    // Reaching this pre-existing handler proves the adapter did not consume the
    // fault. Exact child status distinguishes success from an arbitrary crash.
    _exit(signal_number == SIGSEGV && observation_valid == 1 ? 73 : 74);
}
void observation_handler(int signal_number, siginfo_t* info, void* native_context) {
    if (info == nullptr || native_context == nullptr) _exit(75);
    linux_signal_observation output{fault_route::unsupported, false, {}};
    const bool handled = linux_fault_filter(child_region, signal_number, *info,
        *static_cast<const ucontext_t*>(native_context), output);
    const bool valid_record = child_route == fault_route::unowned
        ? !output.has_record
        : output.has_record && output.record.offset == 7
            && output.record.access == child_access
            && output.record.cause == fault_cause::access_violation;
    observation_valid = !handled && output.route == child_route && valid_record ? 1 : 0;
    previous_handler.sa_sigaction(signal_number, info, native_context);
    _exit(76);
}

void real_fault_case(bool write_access, bool other_owner) {
    const pid_t child = fork();
    check(child >= 0, "fork fault child");
    if (child < 0) return;
    if (child == 0) {
        const auto geometry = linux_memory_geometry();
        if (!geometry) _exit(77);
        auto owner = reserve_linux_memory(geometry->page_size());
        auto other = reserve_linux_memory(geometry->page_size());
        if (!owner || !other) _exit(78);
        const auto first = capture_linux_fault_region(**owner);
        const auto second = capture_linux_fault_region(**other);
        if (!first || !second) _exit(79);
        child_region = *first;
        child_access = write_access ? fault_access::write : fault_access::read;
        child_route = other_owner ? fault_route::unowned : fault_route::owned_memory;
        struct sigaction terminal {};
        terminal.sa_sigaction = terminal_handler;
        terminal.sa_flags = SA_SIGINFO;
        if (sigemptyset(&terminal.sa_mask) != 0 || sigaction(SIGSEGV, &terminal, nullptr) != 0)
            _exit(80);
        struct sigaction observer {};
        observer.sa_sigaction = observation_handler;
        observer.sa_flags = SA_SIGINFO;
        if (sigemptyset(&observer.sa_mask) != 0 || sigaction(SIGSEGV, &observer, &previous_handler) != 0)
            _exit(81);
        const auto address = (other_owner ? second->base : first->base) + 7;
        auto* pointer = reinterpret_cast<volatile unsigned char*>(address);
        if (write_access) *pointer = 0x5a;
        else { const unsigned char value = *pointer; (void)value; }
        _exit(82); // Inaccessible synthetic reservation must actually fault.
    }
    int status = 0;
    pid_t result;
    do { result = waitpid(child, &status, 0); } while (result < 0 && errno == EINTR);
    check(result == child && WIFEXITED(status) && WEXITSTATUS(status) == 73,
        "real fault observed then forwarded to prior handler");
}
} // namespace

int main() {
    using namespace pcsx5::runtime;
    using namespace pcsx5::runtime::detail;
    const foreign_reservation foreign;
    check(!capture_linux_fault_region(foreign), "foreign provider cannot create snapshot");
    const auto geometry = linux_memory_geometry();
    check(geometry.has_value(), "native geometry");
    if (!geometry) return 1;
    auto owner = reserve_linux_memory(geometry->page_size());
    auto other = reserve_linux_memory(geometry->page_size());
    check(owner.has_value() && other.has_value(), "native reservations");
    if (!owner || !other) return 1;
    const auto region = capture_linux_fault_region(**owner);
    const auto other_region = capture_linux_fault_region(**other);
    check(region.has_value() && other_region.has_value(), "capture live owners");
    if (!region || !other_region) return 1;
    siginfo_t info{};
    info.si_signo = SIGSEGV;
    info.si_code = SEGV_ACCERR;
    info.si_addr = reinterpret_cast<void*>(region->base + 7);
    ucontext_t context{};
    context.uc_mcontext.gregs[REG_TRAPNO] = 14;
    for (const auto access : {fault_access::read, fault_access::write, fault_access::execute}) {
        context.uc_mcontext.gregs[REG_ERR] = access == fault_access::read ? 4
            : access == fault_access::write ? 6 : 20;
        const auto original_info = info;
        const auto original_context = context;
        linux_signal_observation output{fault_route::unsupported, false, {}};
        check(!linux_fault_filter(*region, SIGSEGV, info, context, output), "always unhandled");
        check(output.route == fault_route::owned_memory && output.has_record
            && output.record.offset == 7 && output.record.access == access
            && output.record.cause == fault_cause::access_violation, "normalize access");
        check(std::memcmp(&info, &original_info, sizeof(info)) == 0
            && std::memcmp(&context, &original_context, sizeof(context)) == 0, "native inputs unchanged");
    }
    context.uc_mcontext.gregs[REG_ERR] = 4;
    info.si_signo = SIGBUS;
    for (const int code : {BUS_ADRERR, BUS_OBJERR}) {
        info.si_code = code;
        const auto output = observe_linux_fault(*region, SIGBUS, info, context);
        check(output.has_record && output.record.cause == fault_cause::backing_store_error,
            "synthetic backing error");
    }
    info.si_code = BUS_ADRALN;
    check(observe_linux_fault(*region, SIGBUS, info, context).route == fault_route::unsupported,
        "alignment fault unsupported");
    info.si_signo = SIGSEGV;
    info.si_code = SEGV_MAPERR;
    check(observe_linux_fault(*region, SIGSEGV, info, context).route == fault_route::owned_memory,
        "synthetic map error");
    info.si_addr = reinterpret_cast<void*>(other_region->base);
    check(observe_linux_fault(*region, SIGSEGV, info, context).route == fault_route::unowned,
        "other owner not adopted");
    info.si_addr = reinterpret_cast<void*>(region->base + region->size);
    check(observe_linux_fault(*region, SIGSEGV, info, context).route == fault_route::unowned,
        "one past owner rejected");
    info.si_addr = reinterpret_cast<void*>(region->base - 1);
    check(observe_linux_fault(*region, SIGSEGV, info, context).route == fault_route::unowned,
        "below owner rejected");
    info.si_addr = reinterpret_cast<void*>(region->base);
    for (const int code : {static_cast<int>(SI_USER), static_cast<int>(SI_QUEUE), static_cast<int>(SEGV_PKUERR)}) {
        info.si_code = code;
        check(observe_linux_fault(*region, SIGSEGV, info, context).route == fault_route::unsupported,
            "unmodeled or user-delivered signal unsupported");
    }
    info.si_code = SEGV_ACCERR;
    check(observe_linux_fault(*region, SIGILL, info, context).route == fault_route::unsupported,
        "mismatched signal unsupported");
    context.uc_mcontext.gregs[REG_TRAPNO] = 13;
    check(observe_linux_fault(*region, SIGSEGV, info, context).route == fault_route::unsupported,
        "non page-fault context unsupported");
    context.uc_mcontext.gregs[REG_TRAPNO] = 14;
    for (const int error : {8, 32, 64, 22}) {
        context.uc_mcontext.gregs[REG_ERR] = error;
        check(observe_linux_fault(*region, SIGSEGV, info, context).route == fault_route::unsupported,
            "unmodeled or contradictory page-fault bits unsupported");
    }
    check((*owner)->release().has_value(), "release captured owner");
    check(!capture_linux_fault_region(**owner), "closed owner cannot create snapshot");
    // No use of region after release: retained snapshots are invalid by contract.
    struct sigaction before {};
    check(sigaction(SIGSEGV, nullptr, &before) == 0, "read parent disposition");
    real_fault_case(false, false);
    real_fault_case(true, false);
    real_fault_case(false, true);
    real_fault_case(true, true);
    struct sigaction after {};
    check(sigaction(SIGSEGV, nullptr, &after) == 0
        && before.sa_sigaction == after.sa_sigaction && before.sa_flags == after.sa_flags,
        "parent handler disposition unchanged");
    for (int signal_number = 1; signal_number < NSIG; ++signal_number)
        check(sigismember(&before.sa_mask, signal_number) == sigismember(&after.sa_mask, signal_number),
            "parent handler mask unchanged");
    if (failures == 0) std::puts("Linux fault observation: synthetic cases and four real forwarding children passed");
    return failures == 0 ? 0 : 1;
}
