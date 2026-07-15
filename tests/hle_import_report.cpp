// Unit tests for the HLE import reporting and strict-imports API.
// These tests exercise the registry, Resolve/ResolveAny, the auto-stub
// classification, and the per-symbol statistics without booting the full
// emulator.
//
// Build target: hle_import_report_tests (see CMakeLists.txt).

#include "hle/hle.h"
#include "memory/memory.h"
#include "common/log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" u64 HleDispatch(u64, u64, u64, u64, u64, u64, u64, u64);

namespace {

// Read the symbol_id encoded in a thunk.  The thunk machine code is:
//   49 BA <8-byte symbol_id>   ; mov r10, symbol_id
//   48 B8 <8-byte dispatcher>  ; mov rax, HleCommonDispatcher
//   FF E0                       ; jmp rax
u64 ReadSymbolIdFromThunk(guest_addr_t thunk_addr) {
    if (!thunk_addr) return 0;
    u8 buf[10] = {};
    std::memcpy(buf, reinterpret_cast<const void*>(thunk_addr), sizeof(buf));
    // Skip 49 BA (2 bytes) and read the next 8 as little-endian u64
    u64 id = 0;
    for (int i = 0; i < 8; ++i) {
        id |= static_cast<u64>(buf[2 + i]) << (8 * i);
    }
    return id;
}

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

void ResetCounters() {
    HLE::ResetRunStatistics();
    EXPECT_EQ(HLE::GetUnresolvedImportCount(), (u64)0, "fresh unresolved count should be zero");
    EXPECT(HLE::GetImportReport().empty(), "fresh import report should be empty");
}

void TestResolveAndReport() {
    std::fprintf(stdout, "[TEST] Resolve, report, strict mode\n");

    HLE::SetStrictImportMode(false);
    ResetCounters();

    HLE::RegisterSymbol("testlib", "alpha#T#T", [](const HLE::GuestArgs& /*a*/) -> u64 { return 0xAA; });
    HLE::RegisterSymbol("testlib", "beta#T#T",  [](const HLE::GuestArgs& /*a*/) -> u64 { return 0xBB; });

    // Exact match returns a non-zero thunk address
    guest_addr_t a = HLE::Resolve("testlib", "alpha#T#T");
    EXPECT(a != 0, "Resolve alpha#T#T should succeed");

    // Base-NID variant match
    guest_addr_t a_var = HLE::Resolve("testlib", "alpha#j#j");
    EXPECT(a_var == a, "Resolve alpha#j#j should NID-variant match alpha#T#T");

    // Cross-module search
    guest_addr_t b_any = HLE::ResolveAny("beta#T#T");
    EXPECT(b_any != 0, "ResolveAny beta#T#T should succeed");

    // Read the actual symbol ids from the thunks (libraries registered before us)
    u64 alpha_id = ReadSymbolIdFromThunk(a);
    u64 beta_id  = ReadSymbolIdFromThunk(b_any);
    EXPECT(alpha_id != 0 && beta_id != 0, "thunks should encode non-zero symbol ids");
    EXPECT(alpha_id != beta_id, "alpha and beta must have different ids");

    // Dispatch to bump call counts.  HleDispatch is a real entrypoint — we
    // call it directly with the IDs we just read from the thunks.
    HleDispatch(/*symbol_id=*/alpha_id, /*rdi=*/0x11, /*rsi=*/0x22, /*rdx=*/0x33,
                /*rcx=*/0x44, /*r8=*/0x55, /*r9=*/0x66, /*guest_rip=*/0xDEADBEEF);
    HleDispatch(alpha_id, 0, 0, 0, 0, 0, 0, 0xCAFE0001);
    HleDispatch(beta_id,  0, 0, 0, 0, 0, 0, 0xCAFE0002);

    auto report = HLE::GetImportReport();
    EXPECT_EQ(report.size(), (size_t)2, "report should contain both registered symbols");

    bool saw_alpha = false, saw_beta = false;
    for (const auto& s : report) {
        if (s.name == "alpha#T#T") {
            saw_alpha = true;
            EXPECT_EQ(s.call_count, (u64)2, "alpha call count");
            EXPECT_EQ(s.last_caller_rip, (u64)0xCAFE0001, "alpha last caller");
            EXPECT(s.module_name == "testlib", "alpha module");
        }
        if (s.name == "beta#T#T") {
            saw_beta = true;
            EXPECT_EQ(s.call_count, (u64)1, "beta call count");
            EXPECT_EQ(s.last_caller_rip, (u64)0xCAFE0002, "beta last caller");
        }
    }
    EXPECT(saw_alpha, "alpha in report");
    EXPECT(saw_beta,  "beta in report");
}

void TestStrictModeFailsLoudly() {
    std::fprintf(stdout, "[TEST] Strict-import mode refuses to stub\n");

    // Disable auto-stubbing.  A request for an unknown NID must return 0.
    HLE::SetStrictImportMode(true);
    ResetCounters();

    guest_addr_t r = HLE::ResolveAny("this_is_definitely_not_registered#T#T");
    EXPECT_EQ(r, (guest_addr_t)0, "strict-mode unknown NID should resolve to 0");

    // A known NID still works under strict mode.
    HLE::RegisterSymbol("testlib", "gamma#T#T", [](const HLE::GuestArgs& /*a*/) -> u64 { return 0xCC; });
    guest_addr_t g = HLE::Resolve("testlib", "gamma#T#T");
    EXPECT(g != 0, "strict-mode known NID should resolve");

    HLE::SetStrictImportMode(false);
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

    TestResolveAndReport();
    TestStrictModeFailsLoudly();

    HLE::Shutdown();
    Memory::Shutdown();

    if (g_failures == 0) {
        std::fprintf(stdout, "HLE import-report tests passed.\n");
        return 0;
    }
    std::fprintf(stderr, "HLE import-report tests: %d failure(s).\n", g_failures);
    return 1;
}
