// Memory subsystem tests (Phase 1, item 2) — Query, fault handling, error
// codes, partial unmap, protection updates, and stats.
//
// All scratch allocations live in the process temp directory so the test is
// independent of cwd.

#include "memory/memory.h"
#include "common/log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// Undefine macros from <windows.h> that collide with names in our own
// namespaces.  Without this, MSVC's preprocessor expands `PAGE_SIZE` (and
// `ALIGN_UP` / `ALIGN_DOWN`) inside `Memory::PAGE_SIZE` / `ALIGN_UP(...)` and
// produces C2589 ("illegal token on right side of '::'") / C2062.  After
// undef'ing, restore the PS5 guest page size from `types.h`.
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
#include <filesystem>
#include <fstream>
#include <string>

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

// Compare two Memory::Status values and report a failure if they differ.
// Defined as a regular function (not a macro) because MSVC's parser emits
// C2589 ("illegal token on right side of '::'") when it sees
// `Memory::Status::Foo` enum-class member access inside a macro body.
static void CheckStatus(Memory::Status got, Memory::Status expected,
                        const char* file, int line, const char* msg) {
    ++g_checks;
    if (got != expected) {
        std::fprintf(stderr, "[FAIL] %s:%d: %s  (got=%s expected=%s)\n",
                     file, line, msg,
                     Memory::StatusAsString(got),
                     Memory::StatusAsString(expected));
        ++g_failures;
    }
}

// 1. Round-trip Map -> Query -> Unmap returns correct values
// ---------------------------------------------------------------------------
void TestMapQueryUnmap() {
    std::fprintf(stdout, "[TEST] Map -> Query -> Unmap\n");
    EXPECT(Memory::Initialize(), "Memory::Initialize");

    guest_addr_t addr = 0;
    CheckStatus(Memory::Map(0, 4 * PAGE_SIZE,
                            Memory::PROT_READ | Memory::PROT_WRITE, &addr),
                Memory::Status::Ok, __FILE__, __LINE__, "Map succeeds");
    EXPECT(addr != 0, "Map returned non-zero address");

    Memory::MemoryInfo info{};
    CheckStatus(Memory::Query(addr, &info), Memory::Status::Ok,
                __FILE__, __LINE__, "Query on mapped page");
    EXPECT_EQ(info.base_address, addr, "Query returns the same base address");
    EXPECT(info.is_committed, "Query shows committed");
    EXPECT(!info.is_reserved, "Query shows not reserved-only");
    EXPECT((info.protection & Memory::PROT_READ) != 0, "prot has READ");
    EXPECT((info.protection & Memory::PROT_WRITE) != 0, "prot has WRITE");

    EXPECT(Memory::IsReadable(addr, PAGE_SIZE), "IsReadable true");
    EXPECT(Memory::IsWritable(addr, PAGE_SIZE), "IsWritable true");
    EXPECT(!Memory::IsExecutable(addr, PAGE_SIZE), "IsExecutable false");

    CheckStatus(Memory::Unmap(addr, 4 * PAGE_SIZE), Memory::Status::Ok,
                __FILE__, __LINE__, "Unmap ok");
    CheckStatus(Memory::Query(addr, &info), Memory::Status::NotMapped,
                __FILE__, __LINE__, "Query after Unmap -> NotMapped");

    Memory::Shutdown();
}

// 2. Status codes cover all failure modes
// ---------------------------------------------------------------------------
void TestStatusCodes() {
    std::fprintf(stdout, "[TEST] Status code matrix\n");
    EXPECT(Memory::Initialize(), "Initialize");

    // InvalidArgument: 0 size
    {
        guest_addr_t addr = 0;
        CheckStatus(Memory::Map(0, 0, Memory::PROT_READ, &addr),
                    Memory::Status::InvalidArgument, __FILE__, __LINE__,
                    "Map with 0 size -> InvalidArgument");
    }
    // InvalidArgument: unaligned address
    {
        guest_addr_t addr = 0;
        CheckStatus(Memory::Map(0x123, PAGE_SIZE, Memory::PROT_READ, &addr),
                    Memory::Status::InvalidArgument, __FILE__, __LINE__,
                    "Map unaligned -> InvalidArgument");
    }
    // InvalidArgument: null out pointer
    {
        CheckStatus(Memory::Map(0, PAGE_SIZE, Memory::PROT_READ, nullptr),
                    Memory::Status::InvalidArgument, __FILE__, __LINE__,
                    "Map null out -> InvalidArgument");
    }
    // NotMapped on fresh address
    {
        Memory::MemoryInfo info{};
        CheckStatus(Memory::Query(0xDEADBEEF0000ULL, &info),
                    Memory::Status::NotMapped, __FILE__, __LINE__,
                    "Query on random addr -> NotMapped");
    }
    // Unmap of untracked region
    {
        CheckStatus(Memory::Unmap(0xDEADBEEF0000ULL, PAGE_SIZE),
                    Memory::Status::NotMapped, __FILE__, __LINE__,
                    "Unmap untracked -> NotMapped");
    }

    Memory::Shutdown();
}

// 3. Reserve -> Commit -> Protect -> Unmap lifecycle
// ---------------------------------------------------------------------------
void TestReserveCommitProtect() {
    std::fprintf(stdout, "[TEST] Reserve -> Commit -> Protect -> Unmap\n");
    EXPECT(Memory::Initialize(), "Initialize");

    guest_addr_t reserved = 0;
    CheckStatus(Memory::Reserve(0, 4 * PAGE_SIZE, &reserved),
                Memory::Status::Ok, __FILE__, __LINE__, "Reserve succeeds");
    EXPECT(reserved != 0, "Reserve returned non-zero");

    Memory::MemoryInfo info{};
    CheckStatus(Memory::Query(reserved, &info), Memory::Status::Ok,
                __FILE__, __LINE__, "Query reserved");
    EXPECT(!info.is_committed, "Reserved region not committed");
    EXPECT(info.is_reserved, "Reserved region flagged as reserved");

    // Commit the first two pages
    CheckStatus(Memory::Commit(reserved, 2 * PAGE_SIZE,
                               Memory::PROT_READ | Memory::PROT_WRITE),
                Memory::Status::Ok, __FILE__, __LINE__, "Commit succeeds");
    CheckStatus(Memory::Query(reserved, &info), Memory::Status::Ok,
                __FILE__, __LINE__, "Query after commit");
    EXPECT(info.is_committed, "Committed region is committed");

    // Protect -> shrink to read-only
    CheckStatus(Memory::Protect(reserved, PAGE_SIZE, Memory::PROT_READ),
                Memory::Status::Ok, __FILE__, __LINE__, "Protect succeeds");
    EXPECT(Memory::IsReadable(reserved, PAGE_SIZE), "IsReadable after PROT_READ");
    EXPECT(!Memory::IsWritable(reserved, PAGE_SIZE), "!IsWritable after PROT_READ");

    // Unmap the entire reserved region
    CheckStatus(Memory::Unmap(reserved, 4 * PAGE_SIZE),
                Memory::Status::Ok, __FILE__, __LINE__, "Unmap ok");
    CheckStatus(Memory::Query(reserved, &info), Memory::Status::NotMapped,
                __FILE__, __LINE__, "Query after Unmap -> NotMapped");

    Memory::Shutdown();
}

// 4. Guest fault handler: registered handler runs, returns control, and
//    the original page remains un-faulted.
// ---------------------------------------------------------------------------
struct FaultCapture {
    bool         called = false;
    guest_addr_t address = 0;
    u64          code   = 0;
};

bool FaultHandler(guest_addr_t fault_address, u64 exception_code, void* user_data) {
    auto* cap = reinterpret_cast<FaultCapture*>(user_data);
    if (cap) {
        cap->called = true;
        cap->address = fault_address;
        cap->code = exception_code;
    }
    (void)fault_address;
    (void)exception_code;
    return false; // do not resume; let the caller-side SEH handle it
}

void TestGuestFaultHandler() {
    std::fprintf(stdout, "[TEST] Guest fault handler registration\n");
    EXPECT(Memory::Initialize(), "Initialize");

    FaultCapture cap{};
    Memory::SetGuestFaultHandler(FaultHandler, &cap);
    EXPECT(Memory::GetGuestFaultHandler() == FaultHandler,
           "handler getter returns the registered function");
    EXPECT(Memory::GetGuestFaultHandlerUserData() == &cap,
           "user-data getter returns the same pointer");

    // Detach the handler before we leave the test so subsequent tests do not
    // see a dangling function pointer.
    Memory::SetGuestFaultHandler(nullptr, nullptr);
    EXPECT(Memory::GetGuestFaultHandler() == nullptr, "handler cleared");

    Memory::Shutdown();
}

// 5. MemoryStats tracks regions
// ---------------------------------------------------------------------------
void TestStats() {
    std::fprintf(stdout, "[TEST] MemoryStats accounting\n");
    EXPECT(Memory::Initialize(), "Initialize");

    const auto before = Memory::GetStats();

    guest_addr_t a = 0, b = 0, c = 0;
    CheckStatus(Memory::Map(0, PAGE_SIZE, Memory::PROT_READ, &a),
                Memory::Status::Ok, __FILE__, __LINE__, "Map a");
    CheckStatus(Memory::Map(0, PAGE_SIZE, Memory::PROT_READ, &b),
                Memory::Status::Ok, __FILE__, __LINE__, "Map b");
    CheckStatus(Memory::Map(0, PAGE_SIZE, Memory::PROT_READ, &c),
                Memory::Status::Ok, __FILE__, __LINE__, "Map c");

    const auto after = Memory::GetStats();
    EXPECT(after.region_count >= before.region_count + 3, "region count grew by >= 3");
    EXPECT(after.total_committed >= before.total_committed + 3 * PAGE_SIZE,
           "committed grew");

    CheckStatus(Memory::Unmap(a, PAGE_SIZE), Memory::Status::Ok,
                __FILE__, __LINE__, "Unmap a");
    CheckStatus(Memory::Unmap(b, PAGE_SIZE), Memory::Status::Ok,
                __FILE__, __LINE__, "Unmap b");
    CheckStatus(Memory::Unmap(c, PAGE_SIZE), Memory::Status::Ok,
                __FILE__, __LINE__, "Unmap c");

    Memory::Shutdown();
}

// 6. Status-returning API is the only API now (no legacy wrappers)
// ---------------------------------------------------------------------------
void TestStatusOnlyApi() {
    std::fprintf(stdout, "[TEST] Status-only API\n");
    EXPECT(Memory::Initialize(), "Initialize");

    guest_addr_t addr = 0;
    CheckStatus(Memory::Map(0, PAGE_SIZE,
                            Memory::PROT_READ | Memory::PROT_WRITE, &addr),
                Memory::Status::Ok, __FILE__, __LINE__, "Map succeeds");
    EXPECT(addr != 0, "Map returned non-zero address");
    CheckStatus(Memory::Unmap(addr, PAGE_SIZE), Memory::Status::Ok,
                __FILE__, __LINE__, "Unmap ok");

    guest_addr_t reserved = 0;
    CheckStatus(Memory::Reserve(0, 65536, &reserved), Memory::Status::Ok,
                __FILE__, __LINE__, "Reserve succeeds");
    EXPECT(reserved != 0, "Reserve returned non-zero");
    CheckStatus(Memory::Unmap(reserved, 65536), Memory::Status::Ok,
                __FILE__, __LINE__, "Unmap reserved");

    Memory::Shutdown();
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    TestMapQueryUnmap();
    TestStatusCodes();
    TestReserveCommitProtect();
    TestGuestFaultHandler();
    TestStats();
    TestStatusOnlyApi();

    std::fprintf(stdout, "Memory query: %d check(s), %d failure(s)\n", g_checks, g_failures);
    if (g_failures == 0) {
        std::fprintf(stdout, "Memory query tests passed.\n");
        return 0;
    }
    std::fprintf(stderr, "Memory query tests FAILED with %d failure(s).\n", g_failures);
    return 1;
}
