// Original synthetic fixture. No firmware, game, keys, or retail assets.
// Pins legacy lifecycle/TLS observations, not a proposed cross-platform contract.
#include "hle/guest_lifecycle.h"
#include <array>
#include <barrier>
#include <cstdio>
#include <cstring>
#include <thread>

namespace {
int failures = 0;

void Check(bool condition, const char* label) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", label);
    if (!condition) ++failures;
}

void CheckCrashAndResetScopes() {
    HLE::ResetGuestLifecycleState();
    Check(!HLE::StopRequested() && !HLE::IsGuestCrashed() &&
              HLE::GuestExitCode() == 0,
          "lifecycle reset clears observable stop, crash, and exit code");

    u32 exception_code = 0x1234;
    guest_addr_t rip = 0x5678;
    char description[128] = "unchanged";
    Check(!HLE::GetLastGuestCrashInfo(&exception_code, &rip, description,
                                    static_cast<int>(sizeof(description))) &&
              exception_code == 0x1234 && rip == 0x5678 &&
              std::strcmp(description, "unchanged") == 0,
          "absent crash leaves caller outputs untouched");

    HLE::RequestStop();
    HLE::RequestStop();
    Check(HLE::StopRequested(), "request-stop is observable and idempotent");

    constexpr u32 expected_code = 0xc0000005;
    constexpr guest_addr_t expected_rip = 0x123456789abcdef0;
    {
        std::jthread publisher([] {
            HLE::SetGuestCrashed(expected_code, expected_rip);
        });
    }
    Check(HLE::IsGuestCrashed() &&
              HLE::GetLastGuestCrashInfo(&exception_code, &rip, description,
                                         static_cast<int>(sizeof(description))) &&
              exception_code == expected_code && rip == expected_rip &&
              std::strcmp(description,
                  "GUEST_CRASH: code=0xC0000005 at RIP=0x123456789abcdef0") == 0,
          "joined worker crash publication preserves code, RIP, and message");
    Check(HLE::GetLastGuestCrashInfo(nullptr, nullptr, nullptr, 0),
          "crash query accepts omitted outputs");

    char tiny[6] = {};
    Check(HLE::GetLastGuestCrashInfo(nullptr, nullptr, tiny,
                                    static_cast<int>(sizeof(tiny))) &&
              std::strcmp(tiny, "GUEST") == 0,
          "short crash message buffer is truncated and terminated");
    char untouched = 'x';
    Check(HLE::GetLastGuestCrashInfo(nullptr, nullptr, &untouched, 0) &&
              HLE::GetLastGuestCrashInfo(nullptr, nullptr, &untouched, -1) &&
              untouched == 'x',
          "non-positive crash message size does not write");

    HLE::SetGuestCrashed(0x80000003, 0x42);
    Check(HLE::GetLastGuestCrashInfo(&exception_code, &rip, nullptr, 0) &&
              exception_code == 0x80000003 && rip == 0x42,
          "later crash replaces earlier metadata");

    HLE::ResetGuestCrashState();
    Check(!HLE::IsGuestCrashed() && HLE::StopRequested() &&
              !HLE::GetLastGuestCrashInfo(nullptr, nullptr, nullptr, 0),
          "run-statistics crash reset leaves stop request set");
    HLE::SetGuestCrashed(0x80000003, 0x42);
    HLE::ResetGuestLifecycleState();
    Check(!HLE::StopRequested() && !HLE::IsGuestCrashed() &&
              HLE::GuestExitCode() == 0,
          "shutdown lifecycle reset clears stop and crash together");
}

struct ThreadObservation {
    bool initially_zero = false;
    bool own_stack = false;
    bool own_block = false;
    bool null_preserves_block = false;
    bool scalar_is_independent = false;
};

void CheckThreadLocalCallbacks() {
    constexpr uintptr_t main_stack = 0x1010;
    constexpr std::array<u64, 8> main_block{10, 11, 12, 13, 14, 15, 16, 17};
    SetHostStackPointer(main_stack);
    SetIncomingXmmBlock(main_block.data());
    std::array<ThreadObservation, 2> observed{};
    std::barrier rendezvous(3);

    auto run = [&](std::size_t index) {
        auto& result = observed[index];
        const std::array<u64, 8> zero{};
        result.initially_zero = GetHostStackPointer() == 0 &&
            HLE::GetIncomingXmm0() == 0 &&
            std::memcmp(HLE::GetIncomingXmmBlock(), zero.data(), sizeof(zero)) == 0;

        const uintptr_t expected_stack = 0x2020 + index;
        // Represents the sixteen qwords of a full XMM0-XMM7 spill.
        // VERIFIED legacy behavior copies its first eight CONTIGUOUS qwords.
        // This does not claim correct low-lane extraction for eight registers.
        std::array<u64, 16> spill{};
        for (std::size_t lane = 0; lane < spill.size(); ++lane) {
            spill[lane] = 0x1000 * (index + 1) + lane;
        }
        SetHostStackPointer(expected_stack);
        SetIncomingXmmBlock(spill.data());
        rendezvous.arrive_and_wait();
        rendezvous.arrive_and_wait();
        result.own_stack = GetHostStackPointer() == expected_stack;
        result.own_block = HLE::GetIncomingXmm0() == spill[0] &&
            std::memcmp(HLE::GetIncomingXmmBlock(), spill.data(), 8 * sizeof(u64)) == 0;
        SetIncomingXmmBlock(nullptr);
        result.null_preserves_block = HLE::GetIncomingXmm0() == spill[0] &&
            std::memcmp(HLE::GetIncomingXmmBlock(), spill.data(), 8 * sizeof(u64)) == 0;
        SetIncomingXmm0(0xbeef + index);
        result.scalar_is_independent = HLE::GetIncomingXmm0() == 0xbeef + index &&
            std::memcmp(HLE::GetIncomingXmmBlock(), spill.data(), 8 * sizeof(u64)) == 0;
    };

    {
        std::jthread first(run, 0);
        std::jthread second(run, 1);
        rendezvous.arrive_and_wait();
        Check(GetHostStackPointer() == main_stack &&
                  HLE::GetIncomingXmm0() == main_block[0] &&
                  std::memcmp(HLE::GetIncomingXmmBlock(), main_block.data(),
                              sizeof(main_block)) == 0,
              "concurrent worker callbacks do not overwrite main-thread TLS");
        rendezvous.arrive_and_wait();
    }

    for (const auto& result : observed) {
        Check(result.initially_zero, "fresh worker TLS starts zero initialized");
        Check(result.own_stack, "worker host stack pointer is thread-local");
        Check(result.own_block, "worker XMM block retains eight contiguous spill qwords");
        Check(result.null_preserves_block, "null XMM block setter leaves TLS untouched");
        Check(result.scalar_is_independent, "scalar XMM0 setter does not update captured block");
    }

    HLE::ResetGuestLifecycleState();
    Check(GetHostStackPointer() == main_stack &&
              HLE::GetIncomingXmm0() == main_block[0] &&
              std::memcmp(HLE::GetIncomingXmmBlock(), main_block.data(),
                          sizeof(main_block)) == 0,
          "legacy lifecycle reset does not clear caller host-stack or XMM TLS");
    SetHostStackPointer(0);
    constexpr std::array<u64, 8> zero{};
    SetIncomingXmmBlock(zero.data());
}
}

int main() {
    CheckCrashAndResetScopes();
    CheckThreadLocalCallbacks();
    return failures == 0 ? 0 : 1;
}
