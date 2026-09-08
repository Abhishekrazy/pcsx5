#pragma once
#include <pcsx5/runtime/fault.h>
#include <pcsx5/runtime/memory.h>
#include <cstdint>
#include <optional>
#include <signal.h>
#include <ucontext.h>

namespace pcsx5::runtime::detail {
// Private native adapter snapshot, not a public ownership token. Capture before
// installing a handler. The owner must remain alive and unreleased, with mapping
// changes externally excluded, until that handler is removed. Never reuse a
// snapshot after release, even if a new mapping occupies the same address.
struct linux_fault_region {
    std::uintptr_t base;
    std::uint64_t size;
};
// Not signal-safe: performs provider identification before signal delivery.
[[nodiscard]] std::optional<linux_fault_region> capture_linux_fault_region(
    const memory_reservation& owner) noexcept;

// Signal delivery must not construct or inspect standard-library optional
// objects. This private scalar aggregate deliberately differs from the public
// observation type; record is meaningful only when has_record is true.
struct linux_signal_observation {
    fault_route route;
    bool has_record;
    fault_record record;
};

// Read-only Linux x64 page-fault normalization. Caller supplies genuine siginfo
// and context objects (synthetic objects are allowed only in tests). No RTTI,
// native query, allocation, lock, callback, context mutation or recovery occurs.
[[nodiscard]] inline linux_signal_observation observe_linux_fault(const linux_fault_region& region,
    int signal_number, const siginfo_t& info, const ucontext_t& context) noexcept {
#if defined(__x86_64__) && defined(REG_ERR) && defined(REG_TRAPNO)
    const linux_signal_observation unsupported{fault_route::unsupported, false, {}};
    if (info.si_signo != signal_number) return unsupported;
    fault_cause cause;
    if (signal_number == SIGSEGV && (info.si_code == SEGV_MAPERR || info.si_code == SEGV_ACCERR))
        cause = fault_cause::access_violation;
    else if (signal_number == SIGBUS && (info.si_code == BUS_ADRERR || info.si_code == BUS_OBJERR))
        cause = fault_cause::backing_store_error;
    else return unsupported;
    // Linux x86 page-fault error bits: 1 = write, 4 = instruction fetch.
    // Reject other trap kinds, reserved/PK/shadow-stack/future extensions and
    // contradictory write+execute rather than inventing their access semantics.
    if (context.uc_mcontext.gregs[REG_TRAPNO] != 14) return unsupported;
    const auto error = static_cast<std::uint64_t>(context.uc_mcontext.gregs[REG_ERR]);
    if ((error & ~std::uint64_t{0x17}) != 0 || (error & 0x12) == 0x12) return unsupported;
    const auto address = reinterpret_cast<std::uintptr_t>(info.si_addr);
    if (address < region.base || address - region.base >= region.size)
        return {fault_route::unowned, false, {}};
    const auto access = (error & 0x10) != 0 ? fault_access::execute
        : (error & 2) != 0 ? fault_access::write : fault_access::read;
    return {fault_route::owned_memory, true,
        fault_record{static_cast<std::uint64_t>(address - region.base), access, cause}};
#else
    (void)region; (void)signal_number; (void)info; (void)context;
    return {fault_route::unsupported, false, {}};
#endif
}

// A caller-owned handler must forward disposition itself. False always means
// unhandled, including owned faults. This does not install any signal handler.
[[nodiscard]] inline bool linux_fault_filter(const linux_fault_region& region,
    int signal_number, const siginfo_t& info, const ucontext_t& context,
    linux_signal_observation& output) noexcept {
    output = observe_linux_fault(region, signal_number, info, context);
    return false;
}
} // namespace pcsx5::runtime::detail
