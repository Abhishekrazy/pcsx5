// Authored synthetic fixture: no firmware, game, or retail executable input.
// Observations below freeze legacy behavior, including known containment gaps.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "memory/memory.h"
#include <cstdio>

namespace {
int failures = 0;
void Check(bool condition, const char* label) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", label);
    if (!condition) ++failures;
}

// Keep SEH in a leaf without objects requiring C++ unwinding.
bool TryStore(volatile unsigned char* address, unsigned char value) {
    __try {
        *address = value;
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                    ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

struct FaultObservation {
    guest_addr_t permitted_address = 0;
    guest_addr_t observed_address = 0;
    u64 observed_code = 0;
    unsigned calls = 0;
};

bool ObserveFault(guest_addr_t address, u64 code, void* opaque) {
    auto& observation = *static_cast<FaultObservation*>(opaque);
    ++observation.calls;
    observation.observed_address = address;
    observation.observed_code = code;
    // The fixture authorizes recovery only for its own one reserved page.
    return address == observation.permitted_address &&
           Memory::CommitOnFault(address);
}
}

int main() {
    Check(Memory::Initialize(), "memory initialization");
    FaultObservation observation;
    Memory::SetGuestFaultHandler(ObserveFault, &observation);
    Check(Memory::GetGuestFaultHandler() == ObserveFault &&
              Memory::GetGuestFaultHandlerUserData() == &observation,
          "callback registration and user data");

    guest_addr_t reserved = 0;
    Check(Memory::Reserve(0, 65536, &reserved) == Memory::Status::Ok,
          "reserve tracked fixture pages");
    if (reserved != 0) {
        observation.permitted_address = reserved;
        const bool resumed =
            TryStore(reinterpret_cast<volatile unsigned char*>(reserved), 0x5a);
        Check(resumed, "actual write AV is recovered through demand-commit callback");
        Check(observation.calls == 1 && observation.observed_address == reserved &&
                  observation.observed_code == EXCEPTION_ACCESS_VIOLATION,
              "callback receives exact address, code, and user data");
        Check(resumed && *reinterpret_cast<volatile unsigned char*>(reserved) == 0x5a,
              "faulting instruction resumes and completes its write");

        Memory::TrackGuestWrites(reserved, 4096);
        u64 before = 0;
        Check(Memory::TryGetGuestWriteGeneration(reserved, &before),
              "tracked-write generation exists");
        observation.calls = 0;
        Check(TryStore(reinterpret_cast<volatile unsigned char*>(reserved), 0x6b),
              "tracked-write AV resumes");
        u64 after = 0;
        Check(Memory::TryGetGuestWriteGeneration(reserved, &after) && after == before + 1,
              "tracked-write AV increments generation once");
        Check(observation.calls == 0,
              "tracked-write handler takes priority over registered callback");
        Memory::UntrackGuestWrites(reserved);
        Check(Memory::Unmap(reserved, 65536) == Memory::Status::Ok,
              "release tracked fixture pages");
    }

    observation.permitted_address = 0;
    auto* untracked = static_cast<unsigned char*>(
        VirtualAlloc(nullptr, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS));
    Check(untracked != nullptr, "allocate fixture-owned untracked noaccess pages");
    if (untracked) {
        observation.calls = 0;
        const auto address = reinterpret_cast<guest_addr_t>(untracked);
        Check(Memory::QueryOwner(address) == Memory::Owner::None,
              "host allocation is not in guest ownership registry");
        Check(!TryStore(untracked, 1), "callback rejection propagates AV to SEH");
        Check(observation.calls == 1 && observation.observed_address == address,
              "LEGACY GAP: untracked host AV is nevertheless forwarded to callback");
        Memory::SetGuestFaultHandler(nullptr, nullptr);
        observation.calls = 0;
        Check(!TryStore(untracked, 1) && observation.calls == 0,
              "without callback unhandled AV reaches SEH");
        Check(VirtualFree(untracked, 0, MEM_RELEASE) != 0,
              "release fixture-owned untracked noaccess pages");
    }

    void* host_reserved = VirtualAlloc(nullptr, 65536, MEM_RESERVE, PAGE_NOACCESS);
    Check(host_reserved != nullptr, "reserve fixture-owned untracked host pages");
    if (host_reserved) {
        const auto address = reinterpret_cast<guest_addr_t>(host_reserved);
        Check(Memory::QueryOwner(address) == Memory::Owner::None &&
                  Memory::CommitOnFault(address),
              "LEGACY GAP: demand commit accepts reservation outside guest registry");
        MEMORY_BASIC_INFORMATION info{};
        Check(VirtualQuery(host_reserved, &info, sizeof(info)) != 0 &&
                  info.State == MEM_COMMIT && info.Protect == PAGE_READWRITE,
              "untracked reservation was actually committed read/write");
        Check(VirtualFree(host_reserved, 0, MEM_RELEASE) != 0,
              "release fixture-owned untracked reservation");
    }
    Memory::SetGuestFaultHandler(nullptr, nullptr);
    Memory::Shutdown();
    std::printf("memory fault-route characterization: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
