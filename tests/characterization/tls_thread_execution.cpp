// Authored synthetic x86-64 fixture: no firmware/game/retail executable input.
// Exercises the real legacy patcher, not a replacement implementation.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "kernel/tls_patch.h"
#include "memory/memory.h"
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
using ReadTls = u64 (*)();
}

int main() {
    Check(Memory::Initialize(), "memory initialization");
    if (!Kernel::TlsPatch::Initialize()) {
        Check(false, "full legacy TLS patcher initialization (required; not skipped)");
        Memory::Shutdown();
        return 1;
    }
    const u64 fallback = 0x1020304050607080ULL;
    Kernel::TlsPatch::SetDefaultThreadPointer(reinterpret_cast<guest_addr_t>(&fallback));

    guest_addr_t site = 0;
    Check(Memory::AllocateRange(65536, 65536, Memory::Owner::Kernel,
                               "synthetic-tls-fixture", &site) == Memory::Status::Ok,
          "reserve synthetic code in legacy module window");
    if (site == 0 || Memory::Commit(site, 65536, Memory::PROT_READ | Memory::PROT_WRITE)
                         != Memory::Status::Ok) {
        Check(false, "commit synthetic code pages");
        Kernel::TlsPatch::Shutdown();
        Memory::Shutdown();
        return 1;
    }
    // mov rax, qword ptr fs:[0]; ret. Never execute before patching.
    constexpr std::array<u8, 10> original{0x64, 0x48, 0x8b, 0x04, 0x25,
                                         0, 0, 0, 0, 0xc3};
    std::memcpy(reinterpret_cast<void*>(site), original.data(), original.size());
    // A leaf guest can keep data at rsp-8. Save a sentinel there, execute
    // the same TLS load, then return the value found in that red-zone slot.
    constexpr u64 sentinel = 0x1122334455667788ULL;
    constexpr std::array<u8, 30> red_zone_probe{
        0x48, 0xb8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
        0x48, 0x89, 0x44, 0x24, 0xf8,
        0x64, 0x48, 0x8b, 0x04, 0x25, 0, 0, 0, 0,
        0x48, 0x8b, 0x44, 0x24, 0xf8, 0xc3};
    std::memcpy(reinterpret_cast<void*>(site + 128), red_zone_probe.data(), red_zone_probe.size());
    Check(Memory::Protect(site, 65536, Memory::PROT_READ | Memory::PROT_EXEC)
              == Memory::Status::Ok, "make synthetic code executable/read-only");
    const Kernel::TlsPatch::AccessInfo access{0x8b, true, 0, 0, 0, 9};
    const bool patched = Kernel::TlsPatch::TryPatchSite(site, access);
    Check(patched, "real patcher rewrites synthetic TLS load");
    if (patched) {
        const auto read_tls = reinterpret_cast<ReadTls>(site);
        constexpr std::array<u64, 2> expected{0xaabbccdd00112233ULL, 0x5566778899aabbccULL};
        std::array<u64, 2> first{};
        std::array<u64, 2> rebound{};
        std::barrier rendezvous(2);
        auto run_bound = [&](std::size_t index) {
            const u64 value = expected[index];
            const u64 changed = value ^ 0xffffULL;
            Kernel::TlsPatch::BindCurrentThread(reinterpret_cast<guest_addr_t>(&value));
            rendezvous.arrive_and_wait();
            first[index] = read_tls();
            rendezvous.arrive_and_wait();
            Kernel::TlsPatch::BindCurrentThread(reinterpret_cast<guest_addr_t>(&changed));
            rendezvous.arrive_and_wait();
            rebound[index] = read_tls();
            Kernel::TlsPatch::BindCurrentThread(0);
        };
        std::jthread first_thread(run_bound, 0);
        std::jthread second_thread(run_bound, 1);
        first_thread.join();
        second_thread.join();
        Check(first == expected, "same patched site reads distinct simultaneous thread bindings");
        Check(rebound[0] == (expected[0] ^ 0xffffULL) &&
                  rebound[1] == (expected[1] ^ 0xffffULL),
              "rebinding each thread changes its own next executed load");

        u64 unbound_result = 0;
        std::jthread unbound([&] { unbound_result = read_tls(); });
        unbound.join();
        Check(unbound_result == fallback,
              "new thread with no binding executes the emitted fallback path");
        Kernel::TlsPatch::BindCurrentThread(0);
        Check(read_tls() == fallback, "explicit zero binding selects fallback");
        if (Kernel::TlsPatch::TryPatchSite(site + 128 + 15, access)) {
            const auto probe = reinterpret_cast<ReadTls>(site + 128);
            Check(probe() != sentinel,
                  "LEGACY GAP: executed TLS stub overwrites leaf red-zone slot at rsp-8");
        } else {
            Check(false, "patch red-zone characterization site");
        }
        Kernel::TlsPatch::InvalidateAll();
        Check(std::memcmp(reinterpret_cast<void*>(site), original.data(), original.size()) == 0,
              "invalidation restores exact original synthetic bytes");
        Check(std::memcmp(reinterpret_cast<void*>(site + 128), red_zone_probe.data(),
                          red_zone_probe.size()) == 0,
              "invalidation restores exact red-zone probe bytes");
    }
    Kernel::TlsPatch::Shutdown();
    Check(!Kernel::TlsPatch::IsInitialized(), "TLS patcher shutdown");
    Check(Memory::ReleaseRange(site) == Memory::Status::Ok, "release synthetic code");
    Memory::Shutdown();
    std::printf("per-thread TLS execution characterization: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
