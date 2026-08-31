// Guest Memory Access Correctness & Page-Crossing Recovery Tests (Task 25)
//
// Exercises page-boundary crossing, demand-commit recovery, unmapped fault boundaries,
// protection violations, string primitives, overlapping memmove semantics, and
// multi-worker thread concurrency stress tests (1..32 threads, 100 cycles).

#include "memory/memory.h"
#include "common/log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

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
#include <vector>
#include <thread>
#include <atomic>
#include <numeric>
#include <random>

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
    } while (0)

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

// 1. Single-byte read/write/copy across various page offsets
void TestSingleByteAndPageOffsets() {
    std::fprintf(stdout, "[TEST] Single-byte read/write/copy and page offsets\n");
    EXPECT(Memory::Initialize(), "Initialize");

    guest_addr_t page = 0;
    CheckStatus(Memory::Map(0, 4096, Memory::PROT_READ | Memory::PROT_WRITE, &page),
                Memory::Status::Ok, __FILE__, __LINE__, "Map 4KB page");

    // Start of page
    u8 val = 0xAB;
    EXPECT(Memory::GuardedWrite(page, &val, 1), "Write at start of page");
    u8 read_val = 0;
    EXPECT(Memory::GuardedRead(&read_val, page, 1), "Read at start of page");
    EXPECT_EQ(read_val, 0xAB, "Value matches at start of page");

    // Middle of page
    val = 0xCD;
    EXPECT(Memory::GuardedWrite(page + 2048, &val, 1), "Write at middle of page");
    EXPECT(Memory::GuardedRead(&read_val, page + 2048, 1), "Read at middle of page");
    EXPECT_EQ(read_val, 0xCD, "Value matches at middle of page");

    // End of page (offset 4095)
    val = 0xEF;
    EXPECT(Memory::GuardedWrite(page + 4095, &val, 1), "Write at end of page");
    EXPECT(Memory::GuardedRead(&read_val, page + 4095, 1), "Read at end of page");
    EXPECT_EQ(read_val, 0xEF, "Value matches at end of page");

    Memory::Unmap(page, 4096);
    Memory::Shutdown();
}

// 2. 2-Page Boundary Crossing
void TestTwoPageCrossing() {
    std::fprintf(stdout, "[TEST] 2-Page Boundary Crossing\n");
    EXPECT(Memory::Initialize(), "Initialize");

    guest_addr_t base = 0;
    CheckStatus(Memory::Map(0, 8192, Memory::PROT_READ | Memory::PROT_WRITE, &base),
                Memory::Status::Ok, __FILE__, __LINE__, "Map 8KB span");

    // Fill pattern across boundary
    std::vector<u8> pattern(64);
    for (size_t i = 0; i < pattern.size(); ++i) pattern[i] = static_cast<u8>(i + 1);

    // Cross boundary: offset 4096 - 32 = 4064 (spans 4064 to 4128)
    guest_addr_t cross_addr = base + 4064;
    u64 written = 0;
    EXPECT(Memory::GuardedWrite(cross_addr, pattern.data(), pattern.size(), &written), "GuardedWrite across boundary");
    EXPECT_EQ(written, pattern.size(), "Written full size across boundary");

    std::vector<u8> read_back(64, 0);
    u64 read_bytes = 0;
    EXPECT(Memory::GuardedRead(read_back.data(), cross_addr, read_back.size(), &read_bytes), "GuardedRead across boundary");
    EXPECT_EQ(read_bytes, pattern.size(), "Read full size across boundary");
    EXPECT(std::memcmp(pattern.data(), read_back.data(), pattern.size()) == 0, "Data matches across 2-page boundary");

    // GuardedCopy across pages
    guest_addr_t copy_dst = base + 100;
    u64 copied = 0;
    EXPECT(Memory::GuardedCopy(copy_dst, cross_addr, pattern.size(), &copied), "GuardedCopy across boundary");
    EXPECT_EQ(copied, pattern.size(), "Copied full size");
    std::vector<u8> copy_read(64, 0);
    Memory::GuardedRead(copy_read.data(), copy_dst, 64);
    EXPECT(std::memcmp(pattern.data(), copy_read.data(), pattern.size()) == 0, "Copied data matches");

    Memory::Unmap(base, 8192);
    Memory::Shutdown();
}

// 3. Multi-page Crossing (64KB across 16 host 4KB pages)
void TestMultiPageCrossing() {
    std::fprintf(stdout, "[TEST] Multi-Page Crossing (64KB across 16 pages)\n");
    EXPECT(Memory::Initialize(), "Initialize");

    guest_addr_t src_base = 0, dst_base = 0;
    CheckStatus(Memory::Map(0, 128 * 1024, Memory::PROT_READ | Memory::PROT_WRITE, &src_base),
                Memory::Status::Ok, __FILE__, __LINE__, "Map src 128KB");
    CheckStatus(Memory::Map(0, 128 * 1024, Memory::PROT_READ | Memory::PROT_WRITE, &dst_base),
                Memory::Status::Ok, __FILE__, __LINE__, "Map dst 128KB");

    constexpr u64 kSize = 65536;
    std::vector<u8> buffer(kSize);
    for (size_t i = 0; i < kSize; ++i) buffer[i] = static_cast<u8>((i * 7 + 13) & 0xFF);

    // Unaligned start across pages: offset 123
    guest_addr_t src = src_base + 123;
    guest_addr_t dst = dst_base + 456;

    EXPECT(Memory::GuardedWrite(src, buffer.data(), kSize), "GuardedWrite 64KB unaligned");
    u64 copied = 0;
    EXPECT(Memory::GuardedCopy(dst, src, kSize, &copied), "GuardedCopy 64KB unaligned");
    EXPECT_EQ(copied, kSize, "Full 64KB copied");

    std::vector<u8> read_back(kSize, 0);
    EXPECT(Memory::GuardedRead(read_back.data(), dst, kSize), "GuardedRead 64KB");
    EXPECT(std::memcmp(buffer.data(), read_back.data(), kSize) == 0, "64KB Multi-page copy verified");

    // GuardedSet
    EXPECT(Memory::GuardedSet(dst, 0x5A, kSize), "GuardedSet 64KB");
    Memory::GuardedRead(read_back.data(), dst, kSize);
    bool all_5a = true;
    for (size_t i = 0; i < kSize; ++i) {
        if (read_back[i] != 0x5A) { all_5a = false; break; }
    }
    EXPECT(all_5a, "GuardedSet filled 64KB with 0x5A");

    Memory::Unmap(src_base, 128 * 1024);
    Memory::Unmap(dst_base, 128 * 1024);
    Memory::Shutdown();
}

// 4. Demand-Commit Across Reserved Pages
void TestDemandCommitCrossing() {
    std::fprintf(stdout, "[TEST] Demand-Commit Across Reserved Pages\n");
    EXPECT(Memory::Initialize(), "Initialize");

    guest_addr_t reserved = 0;
    // Reserve 1MB (16 x 64KB blocks)
    CheckStatus(Memory::Reserve(0, 1024 * 1024, &reserved),
                Memory::Status::Ok, __FILE__, __LINE__, "Reserve 1MB");

    // Commit ONLY the first 64KB block
    CheckStatus(Memory::Commit(reserved, 64 * 1024, Memory::PROT_READ | Memory::PROT_WRITE),
                Memory::Status::Ok, __FILE__, __LINE__, "Commit first 64KB");

    // Page 0 is committed. Page 1 (offset 64KB) is MEM_RESERVE.
    // Query on page 1 must report NOT committed!
    Memory::MemoryInfo info{};
    CheckStatus(Memory::Query(reserved + 65536, &info), Memory::Status::Ok,
                __FILE__, __LINE__, "Query uncommitted block");
    EXPECT(!info.is_committed, "Uncommitted block is not committed in Query");
    EXPECT(info.is_reserved, "Uncommitted block is reserved");

    // Write spanning from committed block across into uncommitted block (offset 65500 to 65600)
    std::vector<u8> pattern(200, 0x77);
    guest_addr_t span_addr = reserved + 65500;
    u64 written = 0;
    EXPECT(Memory::GuardedWrite(span_addr, pattern.data(), pattern.size(), &written), "Demand commit write spanning boundary");
    EXPECT_EQ(written, pattern.size(), "Written full span");

    // Re-query second block: should now be committed via demand commit!
    CheckStatus(Memory::Query(reserved + 65536, &info), Memory::Status::Ok,
                __FILE__, __LINE__, "Query after demand commit");
    EXPECT(info.is_committed, "Block is now committed after demand-commit");

    std::vector<u8> read_back(200, 0);
    EXPECT(Memory::GuardedRead(read_back.data(), span_addr, 200), "Read back demand committed bytes");
    EXPECT(std::memcmp(pattern.data(), read_back.data(), 200) == 0, "Demand commit data matches");

    Memory::Unmap(reserved, 1024 * 1024);
    Memory::Shutdown();
}

// 5. Fault Boundary Recovery on Unmapped Memory
void TestUnmappedFaultBoundary() {
    std::fprintf(stdout, "[TEST] Fault Boundary Recovery on Unmapped Memory\n");
    EXPECT(Memory::Initialize(), "Initialize");

    // Reserve a 64KB chunk whose FOLLOWING page is genuinely free.
    //
    // The read below deliberately runs off the end of this reservation, so the
    // test is only meaningful if the address after it is unmapped. That was
    // previously assumed: VirtualQuery was called on it and its answer thrown
    // away. When another allocation happened to sit there the read succeeded
    // for the full 8192 bytes and the suite failed intermittently -- about one
    // full run in three, while passing in isolation, which reads like a defect
    // in the memory subsystem and is not one.
    //
    // Reserving until the neighbour is observably MEM_FREE makes the
    // precondition hold rather than hoping for it. Reservations that fail the
    // check are kept, not released, so the retry does not simply land on the
    // same address again; they are released once a usable one is found.
    void* host_res = nullptr;
    guest_addr_t base = 0;
    std::vector<void*> rejected;
    for (int attempt = 0; attempt < 64 && !host_res; ++attempt) {
        void* candidate = VirtualAlloc(nullptr, 65536, MEM_RESERVE, PAGE_NOACCESS);
        if (!candidate) break;
        const guest_addr_t cand_base = reinterpret_cast<guest_addr_t>(candidate);
        MEMORY_BASIC_INFORMATION probe{};
        VirtualQuery(reinterpret_cast<LPCVOID>(cand_base + 65536), &probe, sizeof(probe));
        if (probe.State == MEM_FREE) {
            host_res = candidate;
            base = cand_base;
        } else {
            rejected.push_back(candidate);
        }
    }
    for (void* r : rejected) VirtualFree(r, 0, MEM_RELEASE);
    EXPECT(host_res != nullptr,
           "found a 64KB reservation followed by free address space");
    if (!host_res) return;
    CheckStatus(Memory::AdoptRange(base, 65536, Memory::PROT_READ | Memory::PROT_WRITE, false,
                                  Memory::Owner::Kernel, "unmapped_test"),
                Memory::Status::Ok, __FILE__, __LINE__, "Adopt 64KB reservation");

    // Commit only the last 4KB page of the reservation: [base + 61440, base + 65536)
    guest_addr_t valid_page = base + 65536 - 4096;
    VirtualAlloc(reinterpret_cast<void*>(valid_page), 4096, MEM_COMMIT, PAGE_READWRITE);

    // Initialize the valid 4KB page
    std::vector<u8> valid_data(4096, 0x42);
    EXPECT(Memory::GuardedWrite(valid_page, valid_data.data(), 4096), "Write valid page");

    // base + 65536 was confirmed MEM_FREE when the reservation was chosen, so
    // the read below genuinely crosses into unmapped space.

    // Request reading 8192 bytes from valid_page (4096 valid + 4096 unmapped/free)
    std::vector<u8> dest(8192, 0);
    u64 bytes_read = 0;
    bool ok = Memory::GuardedRead(dest.data(), valid_page, 8192, &bytes_read);
    EXPECT(!ok, "GuardedRead returns false on unmapped boundary");
    EXPECT_EQ(bytes_read, 4096, "GuardedRead stopped exactly at valid 4096 boundary");
    EXPECT(std::memcmp(dest.data(), valid_data.data(), 4096) == 0, "Valid 4096 bytes read accurately");

    // GuardedCopy into valid destination
    guest_addr_t dest_page = 0;
    CheckStatus(Memory::Map(0, 8192, Memory::PROT_READ | Memory::PROT_WRITE, &dest_page),
                Memory::Status::Ok, __FILE__, __LINE__, "Map dest 8KB");

    u64 bytes_copied = 0;
    ok = Memory::GuardedCopy(dest_page, valid_page, 8192, &bytes_copied);
    EXPECT(!ok, "GuardedCopy returns false on unmapped src boundary");
    EXPECT_EQ(bytes_copied, 4096, "GuardedCopy stopped at boundary");

    VirtualFree(host_res, 0, MEM_RELEASE);
    Memory::Unmap(dest_page, 8192);
    Memory::Shutdown();
}

// 6. Protection Violation Recovery
void TestProtectionViolation() {
    std::fprintf(stdout, "[TEST] Protection Violation Recovery\n");
    EXPECT(Memory::Initialize(), "Initialize");

    guest_addr_t ro_page = 0;
    CheckStatus(Memory::Map(0, 4096, Memory::PROT_READ, &ro_page),
                Memory::Status::Ok, __FILE__, __LINE__, "Map 4KB Read-Only");

    EXPECT(Memory::IsReadable(ro_page, 4096), "IsReadable true for PROT_READ");
    EXPECT(!Memory::IsWritable(ro_page, 4096), "IsWritable false for PROT_READ");

    u8 val = 0x99;
    u64 written = 0;
    EXPECT(!Memory::GuardedWrite(ro_page, &val, 1, &written), "GuardedWrite fails on PROT_READ");
    EXPECT_EQ(written, 0, "0 bytes written on PROT_READ");

    Memory::Unmap(ro_page, 4096);
    Memory::Shutdown();
}

// 7. Overlapping Copies (memmove semantics)
void TestOverlappingCopies() {
    std::fprintf(stdout, "[TEST] Overlapping Copies (memmove semantics)\n");
    EXPECT(Memory::Initialize(), "Initialize");

    guest_addr_t buf = 0;
    CheckStatus(Memory::Map(0, 8192, Memory::PROT_READ | Memory::PROT_WRITE, &buf),
                Memory::Status::Ok, __FILE__, __LINE__, "Map 8KB");

    // Forward overlap: copy [0..100) to [50..150)
    std::vector<u8> src_data(100);
    for (size_t i = 0; i < 100; ++i) src_data[i] = static_cast<u8>(i);
    Memory::GuardedWrite(buf, src_data.data(), 100);

    u64 copied = 0;
    EXPECT(Memory::GuardedCopy(buf + 50, buf, 100, &copied), "Forward overlap copy");
    EXPECT_EQ(copied, 100, "Copied 100 bytes");

    std::vector<u8> check(100);
    Memory::GuardedRead(check.data(), buf + 50, 100);
    EXPECT(std::memcmp(src_data.data(), check.data(), 100) == 0, "Forward overlap matches source");

    // Backward overlap: copy [50..150) to [0..100)
    EXPECT(Memory::GuardedCopy(buf, buf + 50, 100, &copied), "Backward overlap copy");
    EXPECT_EQ(copied, 100, "Copied 100 bytes");
    Memory::GuardedRead(check.data(), buf, 100);
    EXPECT(std::memcmp(src_data.data(), check.data(), 100) == 0, "Backward overlap matches source");

    Memory::Unmap(buf, 8192);
    Memory::Shutdown();
}

// 8. String Primitives across page boundaries
void TestStringPrimitives() {
    std::fprintf(stdout, "[TEST] String Primitives Across Page Boundaries\n");
    EXPECT(Memory::Initialize(), "Initialize");

    guest_addr_t base = 0;
    CheckStatus(Memory::Map(0, 8192, Memory::PROT_READ | Memory::PROT_WRITE, &base),
                Memory::Status::Ok, __FILE__, __LINE__, "Map 8KB");

    // Place a string crossing the 4096 page boundary: starts at 4090, 20 chars long
    std::string test_str = "0123456789ABCDEFGHIJ"; // 20 chars
    guest_addr_t str_addr = base + 4090;
    Memory::GuardedWrite(str_addr, test_str.c_str(), test_str.size() + 1);

    u64 len = Memory::GuardedStrlen(str_addr);
    EXPECT_EQ(len, 20, "GuardedStrlen across page boundary");

    guest_addr_t dst_addr = base + 100;
    Memory::GuardedStrcpy(dst_addr, str_addr);
    char buf[32] = {};
    Memory::GuardedRead(buf, dst_addr, 21);
    EXPECT(test_str == buf, "GuardedStrcpy across boundary matches");

    // GuardedStrcmp
    EXPECT_EQ(Memory::GuardedStrcmp(str_addr, dst_addr), 0, "GuardedStrcmp equal");

    // GuardedStrncmp
    EXPECT_EQ(Memory::GuardedStrncmp(str_addr, dst_addr, 10), 0, "GuardedStrncmp prefix equal");

    Memory::Unmap(base, 8192);
    Memory::Shutdown();
}

// 9. Multi-threaded Concurrency Stress Test (1, 2, 4, 8, 16, 32 workers, 100 cycles)
void TestConcurrencyStress() {
    std::fprintf(stdout, "[TEST] Multi-threaded Concurrency Stress Test\n");
    EXPECT(Memory::Initialize(), "Initialize");

    const std::vector<int> worker_counts = {1, 2, 4, 8, 16, 32};

    for (int num_threads : worker_counts) {
        std::fprintf(stdout, "  -> Stress testing with %d worker threads (100 cycles each)...\n", num_threads);

        std::atomic<bool> start_flag{false};
        std::atomic<int> error_count{0};
        std::vector<std::thread> workers;
        workers.reserve(num_threads);

        for (int t = 0; t < num_threads; ++t) {
            workers.emplace_back([&, t]() {
                while (!start_flag.load(std::memory_order_relaxed)) {
                    std::this_thread::yield();
                }

                std::mt19937_64 rng(1337 + t);
                for (int cycle = 0; cycle < 100; ++cycle) {
                    // Map a 64KB region
                    guest_addr_t local_mem = 0;
                    if (Memory::Map(0, 65536, Memory::PROT_READ | Memory::PROT_WRITE, &local_mem) != Memory::Status::Ok) {
                        ++error_count;
                        continue;
                    }

                    // Test page-crossing write, copy, read
                    u64 offset = (rng() % 3000) + 3000; // around 4096 boundary
                    std::vector<u8> pattern(1024, static_cast<u8>((t ^ cycle) & 0xFF));

                    if (!Memory::GuardedWrite(local_mem + offset, pattern.data(), pattern.size())) {
                        ++error_count;
                    }

                    if (!Memory::GuardedCopy(local_mem + 100, local_mem + offset, pattern.size())) {
                        ++error_count;
                    }

                    std::vector<u8> verify(1024, 0);
                    if (!Memory::GuardedRead(verify.data(), local_mem + 100, pattern.size())) {
                        ++error_count;
                    }

                    if (std::memcmp(pattern.data(), verify.data(), pattern.size()) != 0) {
                        ++error_count;
                    }

                    Memory::Unmap(local_mem, 65536);
                }
            });
        }

        start_flag.store(true, std::memory_order_release);
        for (auto& w : workers) {
            if (w.joinable()) w.join();
        }

        EXPECT_EQ(error_count.load(), 0, "Multi-threaded concurrency stress test completed with 0 errors");
    }

    Memory::Shutdown();
}

} // namespace

int main() {
    std::fprintf(stdout, "============================================================\n");
    std::fprintf(stdout, " PCSX5 Guest Memory Access Correctness & Concurrency Suite\n");
    std::fprintf(stdout, "============================================================\n");

    TestSingleByteAndPageOffsets();
    TestTwoPageCrossing();
    TestMultiPageCrossing();
    TestDemandCommitCrossing();
    TestUnmappedFaultBoundary();
    TestProtectionViolation();
    TestOverlappingCopies();
    TestStringPrimitives();
    TestConcurrencyStress();

    std::fprintf(stdout, "============================================================\n");
    std::fprintf(stdout, " Checks: %d, Failures: %d\n", g_checks, g_failures);
    std::fprintf(stdout, " Result: %s\n", g_failures == 0 ? "PASSED" : "FAILED");
    std::fprintf(stdout, "============================================================\n");

    return g_failures == 0 ? 0 : 1;
}
