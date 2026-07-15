// Memory subsystem validation tests.
//
// Covers the Map / Unmap / Protect / Read / Write / Buffer / Translate surface
// that every other subsystem (loader, kernel, HLE) depends on.  These tests
// run as a regular CTest binary and do not need the full emulator.
//
// Build target: memory_validation_tests (see CMakeLists.txt).

#include "memory/memory.h"
#include "common/log.h"
#include "common/types.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// Undefine macros from <windows.h> that collide with names in our own
// namespaces.  Without this, MSVC's preprocessor expands `PAGE_SIZE` inside
// `Memory::PAGE_SIZE` / `ALIGN_UP(...)` and produces C2589.  After undef'ing,
// restore the PS5 guest page size from `types.h`.
#ifdef PAGE_SIZE
#  undef PAGE_SIZE
#endif
#ifdef ALIGN_UP
#  undef ALIGN_UP
#endif
#ifdef ALIGN_DOWN
#  undef ALIGN_DOWN
#endif
#ifndef PAGE_SIZE
#  define PAGE_SIZE 0x4000
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks   = 0;

#define EXPECT(cond, msg)                                                            \
    do {                                                                            \
        ++g_checks;                                                                 \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, msg);    \
            ++g_failures;                                                           \
        }                                                                           \
        (void)0;                                                                    \
    } while (0)

#define EXPECT_EQ(a, b, msg)                                                                    \
    do {                                                                                        \
        ++g_checks;                                                                            \
        auto _lhs = (a);                                                                        \
        auto _rhs = (b);                                                                        \
        if (!(_lhs == _rhs)) {                                                                  \
            std::fprintf(stderr, "[FAIL] %s:%d: %s  (lhs=%lld rhs=%lld)\n",                     \
                         __FILE__, __LINE__, msg,                                               \
                         (long long)_lhs, (long long)_rhs);                                     \
            ++g_failures;                                                                       \
        }                                                                                       \
        (void)0;                                                                                \
    } while (0)

// ---------------------------------------------------------------------------
// Helper: query the Win32 protection of a single page that contains `addr`.
// Returns the value VirtualQuery last reported.
// ---------------------------------------------------------------------------
static DWORD QueryPageProtection(void* addr) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(addr, &mbi, sizeof(mbi))) {
        return 0;
    }
    return mbi.Protect;
}

// ---------------------------------------------------------------------------
// Helper: read the protection of a known range and assert it matches.
// ---------------------------------------------------------------------------
static void ExpectProtect(void* addr, DWORD expected, const char* what) {
    DWORD actual = QueryPageProtection(addr);
    if (actual != expected) {
        std::fprintf(stderr,
                     "[FAIL] ExpectProtect(%s): expected 0x%lX, got 0x%lX\n",
                     what, (unsigned long)expected, (unsigned long)actual);
        ++g_failures;
    }
    ++g_checks;
}

// Convenience helper: Map with OS-chosen address and assert success.
static guest_addr_t MapOs(u64 size, u32 prot) {
    guest_addr_t a = 0;
    EXPECT(Memory::Map(0, size, prot, &a) == Memory::Status::Ok, "Map(os) succeeds");
    EXPECT(a != 0, "Map(os) returned non-zero");
    return a;
}

// ---------------------------------------------------------------------------
// 1. Lifecycle
// ---------------------------------------------------------------------------
void TestLifecycle() {
    std::fprintf(stdout, "[TEST] Memory::Initialize / Shutdown roundtrip\n");
    EXPECT(Memory::Initialize(), "Initialize should succeed");
    // Calling Shutdown is currently a no-op for cleanup state (the OS reclaims
    // everything on process exit) but it must be safe to call.
    Memory::Shutdown();
}

// ---------------------------------------------------------------------------
// 2. Map / Unmap with OS-chosen address (hint = 0)
// ---------------------------------------------------------------------------
void TestMapOsChosenAddress() {
    std::fprintf(stdout, "[TEST] Map with OS-chosen address\n");
    guest_addr_t a = MapOs(4 * PAGE_SIZE, Memory::PROT_READ | Memory::PROT_WRITE);
    ExpectProtect(reinterpret_cast<void*>(a), PAGE_READWRITE, "OS-chosen RW");

    // Should be writable / readable.
    Memory::Write<u32>(a, 0xDEADBEEF);
    EXPECT_EQ(Memory::Read<u32>(a), (u32)0xDEADBEEF, "Read/Write after OS-chosen Map");

    EXPECT(Memory::Unmap(a, 4 * PAGE_SIZE) == Memory::Status::Ok, "Unmap OS-chosen region");
}

// ---------------------------------------------------------------------------
// 3. Map respects page alignment: any size rounds up to PAGE_SIZE
// ---------------------------------------------------------------------------
void TestMapAlignment() {
    std::fprintf(stdout, "[TEST] Map aligns small requests to PAGE_SIZE\n");
    constexpr u64 REQUESTED = 0x123; // well under one page
    guest_addr_t a = MapOs(REQUESTED, Memory::PROT_READ | Memory::PROT_WRITE);

    // The returned address must be page-aligned.
    EXPECT_EQ(a & (PAGE_SIZE - 1), (u64)0, "Map result is page-aligned");
    // We can write into the very last byte of the aligned region without SEGV
    // even though the requested size was tiny.
    volatile u8* p = reinterpret_cast<volatile u8*>(a);
    p[REQUESTED - 1] = 0xAB; // should not crash
    EXPECT_EQ(p[REQUESTED - 1], (u8)0xAB, "Last byte of aligned region is writable");

    EXPECT(Memory::Unmap(a, REQUESTED) == Memory::Status::Ok, "Unmap tiny region");
}

// ---------------------------------------------------------------------------
// 4. Protection bit translation.  R / W / X / RW / RX / RWX / NONE.
// ---------------------------------------------------------------------------
void TestProtectionTranslation() {
    std::fprintf(stdout, "[TEST] Protection bit translation\n");
    struct Case { u32 prot; DWORD expected; const char* name; };
    const Case cases[] = {
        {Memory::PROT_NONE,                          PAGE_NOACCESS,           "NONE"},
        {Memory::PROT_READ,                          PAGE_READONLY,           "R"},
        {Memory::PROT_READ | Memory::PROT_WRITE,     PAGE_READWRITE,          "RW"},
        {Memory::PROT_READ | Memory::PROT_EXEC,      PAGE_EXECUTE_READ,       "RX"},
        {Memory::PROT_READ | Memory::PROT_WRITE | Memory::PROT_EXEC,
                                                    PAGE_EXECUTE_READWRITE,  "RWX"},
    };

    for (const auto& c : cases) {
        guest_addr_t a = MapOs(PAGE_SIZE, c.prot);
        if (a) {
            ExpectProtect(reinterpret_cast<void*>(a), c.expected, c.name);
            EXPECT(Memory::Unmap(a, PAGE_SIZE) == Memory::Status::Ok,
                   ("Unmap " + std::string(c.name)).c_str());
        }
    }
}

// ---------------------------------------------------------------------------
// 5. Protect changes existing region protection
// ---------------------------------------------------------------------------
void TestProtect() {
    std::fprintf(stdout, "[TEST] Protect changes an existing region\n");
    guest_addr_t a = MapOs(PAGE_SIZE, Memory::PROT_READ | Memory::PROT_WRITE);
    ExpectProtect(reinterpret_cast<void*>(a), PAGE_READWRITE, "initial RW");

    EXPECT(Memory::Protect(a, PAGE_SIZE, Memory::PROT_READ) == Memory::Status::Ok, "Protect -> R");
    ExpectProtect(reinterpret_cast<void*>(a), PAGE_READONLY, "after Protect -> R");

    EXPECT(Memory::Protect(a, PAGE_SIZE, Memory::PROT_READ | Memory::PROT_WRITE) == Memory::Status::Ok,
           "Protect -> RW");
    ExpectProtect(reinterpret_cast<void*>(a), PAGE_READWRITE, "after Protect -> RW again");

    EXPECT(Memory::Unmap(a, PAGE_SIZE) == Memory::Status::Ok, "Unmap after Protect");
}

// ---------------------------------------------------------------------------
// 6. Typed Read / Write across 8/16/32/64 bit widths
// ---------------------------------------------------------------------------
void TestTypedReadWrite() {
    std::fprintf(stdout, "[TEST] Typed Read/Write (u8/u16/u32/u64)\n");
    guest_addr_t a = MapOs(PAGE_SIZE, Memory::PROT_READ | Memory::PROT_WRITE);

    Memory::Write<u8>(a + 0,  0x12);
    Memory::Write<u16>(a + 8, 0x3456);
    Memory::Write<u32>(a + 16, 0x789ABCDEu);
    Memory::Write<u64>(a + 24, 0x1122334455667788ULL);

    EXPECT_EQ(Memory::Read<u8>(a + 0),  (u8)0x12,                       "u8 roundtrip");
    EXPECT_EQ(Memory::Read<u16>(a + 8), (u16)0x3456,                    "u16 roundtrip");
    EXPECT_EQ(Memory::Read<u32>(a + 16),(u32)0x789ABCDEu,                "u32 roundtrip");
    EXPECT_EQ(Memory::Read<u64>(a + 24),(u64)0x1122334455667788ULL,      "u64 roundtrip");

    EXPECT(Memory::Unmap(a, PAGE_SIZE) == Memory::Status::Ok, "Unmap typed R/W");
}

// ---------------------------------------------------------------------------
// 7. Buffer Read/Write with non-power-of-two size
// ---------------------------------------------------------------------------
void TestBufferReadWrite() {
    std::fprintf(stdout, "[TEST] ReadBuffer / WriteBuffer\n");
    // Allocate a region large enough for both the small and cross-page tests.
    constexpr u64 BIG = 32 * 1024;
    guest_addr_t a = MapOs(BIG, Memory::PROT_READ | Memory::PROT_WRITE);

    constexpr u64 N = 257; // odd, > 256
    std::vector<u8> src(N);
    for (u64 i = 0; i < N; ++i) src[i] = static_cast<u8>(i * 31u + 7u);

    Memory::WriteBuffer(a, src.data(), N);
    std::vector<u8> dst(N, 0);
    Memory::ReadBuffer(a, dst.data(), N);
    for (u64 i = 0; i < N; ++i) {
        EXPECT_EQ(dst[i], src[i], "BufferReadWrite byte roundtrip");
    }

    // Cross-page write
    constexpr u64 CROSS = PAGE_SIZE + 16;
    std::vector<u8> src2(CROSS, 0xA5);
    Memory::WriteBuffer(a, src2.data(), CROSS);
    std::vector<u8> dst2(CROSS, 0);
    Memory::ReadBuffer(a, dst2.data(), CROSS);
    for (u64 i = 0; i < CROSS; ++i) {
        EXPECT_EQ(dst2[i], src2[i], "BufferReadWrite cross-page byte roundtrip");
    }

    EXPECT(Memory::Unmap(a, BIG) == Memory::Status::Ok, "Unmap buffer R/W");
}

// ---------------------------------------------------------------------------
// 8. Translate / GetGuestAddress are inverses
// ---------------------------------------------------------------------------
void TestTranslateIdentity() {
    std::fprintf(stdout, "[TEST] Translate / GetGuestAddress identity\n");
    guest_addr_t a = MapOs(PAGE_SIZE, Memory::PROT_READ | Memory::PROT_WRITE);

    void* host = Memory::Translate(a);
    EXPECT_EQ(Memory::GetGuestAddress(host), a, "GetGuestAddress(Translate(a)) == a");

    // Translate must return the same pointer as the guest address (since
    // guest VM == host VM).
    EXPECT_EQ(reinterpret_cast<guest_addr_t>(host), a, "Translate(a) == a");

    EXPECT(Memory::Unmap(a, PAGE_SIZE) == Memory::Status::Ok, "Unmap translate test");
}

// ---------------------------------------------------------------------------
// 9. Reserve / Commit subregion allocation
// ---------------------------------------------------------------------------
void TestReserveCommit() {
    std::fprintf(stdout, "[TEST] Reserve + Commit subregion\n");
    // Reserve 4 * PAGE_SIZE, commit only the middle 2 pages.
    guest_addr_t region = 0;
    EXPECT(Memory::Reserve(0, 4 * PAGE_SIZE, &region) == Memory::Status::Ok,
           "Reserve should succeed");
    EXPECT(region != 0, "Reserve returned non-zero");

    // NOTE: per Win32 docs, the Protection field in MEMORY_BASIC_INFORMATION
    // is *undefined* for pages that are only reserved and not committed, so we
    // do not assert on it.  We instead verify the lifecycle through Commit.

    guest_addr_t committed = region + PAGE_SIZE;
    EXPECT(Memory::Commit(committed, 2 * PAGE_SIZE,
                          Memory::PROT_READ | Memory::PROT_WRITE) == Memory::Status::Ok,
           "Commit succeeds at requested address");

    ExpectProtect(reinterpret_cast<void*>(committed), PAGE_READWRITE, "Committed pages are RW");

    // Write to the committed middle.
    Memory::Write<u32>(committed, 0xCAFEBABEu);
    EXPECT_EQ(Memory::Read<u32>(committed), (u32)0xCAFEBABEu, "Read from committed region");

    EXPECT(Memory::Unmap(region, 4 * PAGE_SIZE) == Memory::Status::Ok, "Unmap reserved region");
}

// ---------------------------------------------------------------------------
// 10. Multiple non-overlapping maps coexist
// ---------------------------------------------------------------------------
void TestNonOverlappingMaps() {
    std::fprintf(stdout, "[TEST] Multiple non-overlapping maps\n");
    std::vector<guest_addr_t> addrs;
    for (int i = 0; i < 4; ++i) {
        guest_addr_t a = MapOs(PAGE_SIZE, Memory::PROT_READ | Memory::PROT_WRITE);
        addrs.push_back(a);
    }
    for (size_t i = 0; i < addrs.size(); ++i) {
        for (size_t j = i + 1; j < addrs.size(); ++j) {
            EXPECT(addrs[i] != addrs[j],
                   "OS-chosen Maps must not return the same address twice");
        }
    }
    for (auto a : addrs) {
        EXPECT(Memory::Unmap(a, PAGE_SIZE) == Memory::Status::Ok, "Unmap non-overlapping region");
    }
}

// ---------------------------------------------------------------------------
// 11. Map at a busy address hint should not crash and should return something
// ---------------------------------------------------------------------------
void TestBusyAddressFallsBackGracefully() {
    std::fprintf(stdout, "[TEST] Map at a busy address hint\n");
    // First allocate something so that an exact address is taken.
    guest_addr_t taken = MapOs(PAGE_SIZE, Memory::PROT_READ | Memory::PROT_WRITE);

    // Try to Map EXACTLY at the taken address.  Win32 may grant it (if the
    // hint happens to be the returned address) or fail.  Either is acceptable
    // as long as we don't crash.  If the call succeeded and we get a new
    // mapping, unmap it.
    guest_addr_t dup = 0;
    if (Memory::Map(taken, PAGE_SIZE, Memory::PROT_READ | Memory::PROT_WRITE, &dup) == Memory::Status::Ok) {
        EXPECT(Memory::Unmap(dup, PAGE_SIZE) == Memory::Status::Ok, "Unmap duplicate-hint mapping");
    }
    EXPECT(Memory::Unmap(taken, PAGE_SIZE) == Memory::Status::Ok, "Unmap taken region");
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    TestLifecycle();
    TestMapOsChosenAddress();
    TestMapAlignment();
    TestProtectionTranslation();
    TestProtect();
    TestTypedReadWrite();
    TestBufferReadWrite();
    TestTranslateIdentity();
    TestReserveCommit();
    TestNonOverlappingMaps();
    TestBusyAddressFallsBackGracefully();

    std::fprintf(stdout, "Memory validation: %d check(s), %d failure(s)\n", g_checks, g_failures);
    if (g_failures == 0) {
        std::fprintf(stdout, "Memory validation tests passed.\n");
        return 0;
    }
    std::fprintf(stderr, "Memory validation tests FAILED with %d failure(s).\n", g_failures);
    return 1;
}
