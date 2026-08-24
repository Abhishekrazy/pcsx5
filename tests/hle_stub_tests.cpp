// Unit tests for the HLE auto-stub generation and crash behavior.
//
// Build target: hle_stub_tests (see CMakeLists.txt).

#include "hle/hle.h"
#include "memory/memory.h"
#include "common/log.h"

#include <cstdio>
#include <csetjmp>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

extern "C" u64 HleDispatch(u64, u64, u64, u64, u64, u64, u64, u64, u64);

namespace {

int g_failures = 0;

#define EXPECT(cond, msg)                                                        \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, msg); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

#define EXPECT_EQ(a, b, msg)                                                                   \
    do {                                                                                       \
        auto _lhs = (a);                                                                       \
        auto _rhs = (b);                                                                       \
        if (!(_lhs == _rhs)) {                                                                 \
            std::fprintf(stderr, "[FAIL] %s:%d: %s  (lhs=%lld rhs=%lld)\n",                    \
                         __FILE__, __LINE__, msg,                                              \
                         (long long)_lhs, (long long)_rhs);                                    \
            ++g_failures;                                                                      \
        }                                                                                      \
    } while (0)

// Extracts symbol ID from thunk.
u64 ReadSymbolIdFromThunk(guest_addr_t thunk_addr) {
    if (!thunk_addr) return 0;
    u8 buf[10] = {};
    std::memcpy(buf, reinterpret_cast<const void*>(thunk_addr), sizeof(buf));
    u64 id = 0;
    for (int i = 0; i < 8; ++i) {
        id |= static_cast<u64>(buf[2 + i]) << (8 * i);
    }
    return id;
}

void TestUnknownStubCrash() {
    std::fprintf(stdout, "[TEST] Unknown stub crash\n");

    HLE::SetStrictImportMode(false); // Enable auto-stubs

    // Force an unknown symbol to be resolved and auto-stubbed
    guest_addr_t thunk = HLE::Resolve("unknown_module", "unknown_function");
    EXPECT(thunk != 0, "Resolve should return a valid thunk for unknown symbols when not strict");
    
    u64 sym_id = ReadSymbolIdFromThunk(thunk);
    EXPECT(sym_id != 0, "Symbol ID should be encoded in thunk");

    bool crashed = false;
    HLE::SetMainGuestThreadId(::GetCurrentThreadId());
    HLE::ArmGuestExitEnv(true);
#pragma warning(push)
#pragma warning(disable: 4611)
    if (setjmp(HLE::GuestExitEnv()) == 0) {
#pragma warning(pop)
        // Attempt to invoke the auto-stub.
        // It should longjmp because it's UNKNOWN.
        HleDispatch(sym_id, 0, 0, 0, 0, 0, 0, 0, 0);
        EXPECT(false, "HleDispatch should not return for UNKNOWN stubs");
    } else {
        crashed = true;
    }
    HLE::ArmGuestExitEnv(false);

    EXPECT(crashed, "Invoking an UNKNOWN stub should trigger ExitGuestProcess");
    EXPECT_EQ(HLE::GuestExitCode(), (u32)1, "ExitGuestProcess code should be 1 for UNKNOWN stubs");
}

void TestStubContracts() {
    std::fprintf(stdout, "[TEST] Stub contracts\n");

    HLE::SetStrictImportMode(false); // Enable auto-stubs

    // Register test contracts
    HLE::StubContract safe_contract{};
    safe_contract.module_name = "test_module";
    safe_contract.library_name = "test_lib";
    safe_contract.name = "safe_function";
    safe_contract.classification = HLE::StubClass::VOID_OR_SIDE_EFFECT;
    safe_contract.default_return_policy = 42;
    safe_contract.is_dangerous = false;
    HLE::RegisterStubContract(safe_contract);

    HLE::StubContract never_safe_contract{};
    never_safe_contract.module_name = "test_module";
    never_safe_contract.library_name = "test_lib";
    never_safe_contract.name = "never_safe_function";
    never_safe_contract.classification = HLE::StubClass::NEVER_SAFE;
    never_safe_contract.default_return_policy = 0;
    never_safe_contract.is_dangerous = true;
    HLE::RegisterStubContract(never_safe_contract);

    // 1. SAFE contract test
    guest_addr_t safe_thunk = HLE::Resolve("test_module", "safe_function");
    EXPECT(safe_thunk != 0, "Resolve should return a valid thunk for SAFE symbol");
    u64 safe_sym_id = ReadSymbolIdFromThunk(safe_thunk);
    
    // SAFE stub should execute successfully and return the default policy
    u64 safe_ret = HleDispatch(safe_sym_id, 0, 0, 0, 0, 0, 0, 0, 0);
    EXPECT_EQ(safe_ret, (u64)42, "SAFE stub should return default_return_policy");

    // 2. NEVER_SAFE contract test
    guest_addr_t never_safe_thunk = HLE::Resolve("test_module", "never_safe_function");
    EXPECT(never_safe_thunk != 0, "Resolve should return a valid thunk for NEVER_SAFE symbol at link time");
    u64 never_safe_sym_id = ReadSymbolIdFromThunk(never_safe_thunk);

    bool crashed = false;
    HLE::SetMainGuestThreadId(::GetCurrentThreadId());
    HLE::ArmGuestExitEnv(true);
#pragma warning(push)
#pragma warning(disable: 4611)
    if (setjmp(HLE::GuestExitEnv()) == 0) {
#pragma warning(pop)
        // Attempt to invoke the NEVER_SAFE stub. It should longjmp.
        HleDispatch(never_safe_sym_id, 0, 0, 0, 0, 0, 0, 0, 0);
        EXPECT(false, "HleDispatch should not return for NEVER_SAFE stubs");
    } else {
        crashed = true;
    }
    HLE::ArmGuestExitEnv(false);
    
    EXPECT(crashed, "Invoking a NEVER_SAFE stub should trigger ExitGuestProcess");
    EXPECT_EQ(HLE::GuestExitCode(), (u32)1, "ExitGuestProcess code should be 1 for NEVER_SAFE stubs");
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    if (!Memory::Initialize()) {
        std::fprintf(stderr, "FATAL: Memory::Initialize failed\n");
        return 2;
    }
    if (!HLE::Initialize()) {
        std::fprintf(stderr, "FATAL: HLE::Initialize failed\n");
        Memory::Shutdown();
        return 2;
    }

    TestUnknownStubCrash();
    TestStubContracts();

    HLE::Shutdown();
    Memory::Shutdown();

    if (g_failures > 0) {
        std::fprintf(stderr, "\nFAILED: %d tests failed.\n", g_failures);
        return 1;
    }

    std::fprintf(stdout, "\nSUCCESS: All hle_stub_tests passed.\n");
    return 0;
}
