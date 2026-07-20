// Guest image write-tracker tests (port of SharpEmu's
// GuestImageWriteTrackerTests, commit 04557fd "Refresh CPU-rewritten guest
// textures by write generation").
//
// The write generation lets GPU caches detect guest CPU rewrites even after
// another cache owner consumed and re-armed the range: the generation is
// monotonic and survives re-arm cycles and range replacement.  These
// invariants back the vk_draw texture-cache skip for CPU-rewritten images
// (video planes, streamed font atlases).

#include "memory/memory.h"
#include "common/log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// See memory_query_tests.cpp: keep windows.h macros from colliding with the
// Memory::PAGE_SIZE constant, then restore the PS5 guest page size.
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

#define EXPECT_EQ(a, b, msg)                                                        \
    do {                                                                            \
        ++g_checks;                                                                 \
        auto _lhs = (a);                                                            \
        auto _rhs = (b);                                                            \
        if (!(_lhs == _rhs)) {                                                      \
            std::fprintf(stderr, "[FAIL] %s:%d: %s  (lhs=%lld rhs=%lld)\n",         \
                         __FILE__, __LINE__, msg,                                   \
                         (long long)_lhs, (long long)_rhs);                         \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

// Scratch allocation the tracker can protect (64 KiB, host-page backed).
void* AllocPages() {
    return VirtualAlloc(nullptr, 65536, MEM_RESERVE | MEM_COMMIT,
                        PAGE_READWRITE);
}

void FreePages(void* p) {
    if (p) VirtualFree(p, 0, MEM_RELEASE);
}

void WriteByte(void* p, size_t offset) {
    *reinterpret_cast<volatile unsigned char*>(
        static_cast<unsigned char*>(p) + offset) = 0xAB;
}

// The first CPU write faults through the VEH, disarms the range and bumps
// the generation; reading the generation afterwards does not roll it back,
// which is what lets a second cache owner still observe the rewrite.
void TestGenerationBumpsOnFirstWrite() {
    void* pages = AllocPages();
    EXPECT(pages != nullptr, "scratch pages allocated");
    if (!pages) return;
    const auto address = reinterpret_cast<guest_addr_t>(pages);

    Memory::TrackGuestWrites(address, 4096);
    u64 generation = 0xFFFF;
    EXPECT(Memory::TryGetGuestWriteGeneration(address, &generation),
           "tracked range has a generation");
    EXPECT_EQ(generation, 0ull, "generation starts at zero");

    WriteByte(pages, 0);
    EXPECT(Memory::TryGetGuestWriteGeneration(address, &generation),
           "generation readable after the write");
    EXPECT_EQ(generation, 1ull, "first write bumps the generation");

    Memory::UntrackGuestWrites(address);
    FreePages(pages);
}

// The first fault disarms the range; later writes are free-running and must
// not inflate the generation until the owner re-arms.
void TestGenerationIncrementsOncePerArmedLifetime() {
    void* pages = AllocPages();
    EXPECT(pages != nullptr, "scratch pages allocated");
    if (!pages) return;
    const auto address = reinterpret_cast<guest_addr_t>(pages);

    Memory::TrackGuestWrites(address, 4096);
    WriteByte(pages, 0);
    WriteByte(pages, 1);
    u64 generation = 0;
    EXPECT(Memory::TryGetGuestWriteGeneration(address, &generation),
           "generation readable");
    EXPECT_EQ(generation, 1ull, "disarmed writes do not bump the generation");

    Memory::RearmGuestWrites(address);
    WriteByte(pages, 2);
    EXPECT(Memory::TryGetGuestWriteGeneration(address, &generation),
           "generation readable after re-arm");
    EXPECT_EQ(generation, 2ull, "re-armed write bumps the generation again");

    Memory::UntrackGuestWrites(address);
    FreePages(pages);
}

// Re-registering the same allocation with a different size replaces the
// range but must carry the generation, otherwise a resize would hide the
// rewrite from cache owners.
void TestGenerationCarriesAcrossRangeReplacement() {
    void* pages = AllocPages();
    EXPECT(pages != nullptr, "scratch pages allocated");
    if (!pages) return;
    const auto address = reinterpret_cast<guest_addr_t>(pages);

    // Guest-page alignment is 16 KiB; cross it so the re-track really
    // covers a different page span.
    Memory::TrackGuestWrites(address, 0x4000);
    WriteByte(pages, 0);
    Memory::TrackGuestWrites(address, 0x8000);
    u64 generation = 0;
    EXPECT(Memory::TryGetGuestWriteGeneration(address, &generation),
           "generation survives replacement");
    EXPECT_EQ(generation, 1ull, "replacement carries the generation");

    // The resized range is armed over its whole new span.
    WriteByte(pages, 0x4000 + 8);
    EXPECT(Memory::TryGetGuestWriteGeneration(address, &generation),
           "generation readable after resized-span write");
    EXPECT_EQ(generation, 2ull, "write in the extended span bumps");

    Memory::UntrackGuestWrites(address);
    FreePages(pages);
}

void TestUntrackedAddressHasNoGeneration() {
    u64 generation = 0xFFFF;
    EXPECT(!Memory::TryGetGuestWriteGeneration(0xDEAD00000000ull, &generation),
           "untracked address has no generation");

    void* pages = AllocPages();
    EXPECT(pages != nullptr, "scratch pages allocated");
    if (!pages) return;
    const auto address = reinterpret_cast<guest_addr_t>(pages);
    Memory::TrackGuestWrites(address, 4096);
    Memory::UntrackGuestWrites(address);
    EXPECT(!Memory::TryGetGuestWriteGeneration(address, &generation),
           "untracked range loses its generation");
    FreePages(pages);
}

} // namespace

int main() {
    if (!Memory::Initialize()) {
        std::fprintf(stderr, "Memory::Initialize failed\n");
        return 1;
    }
    TestGenerationBumpsOnFirstWrite();
    TestGenerationIncrementsOncePerArmedLifetime();
    TestGenerationCarriesAcrossRangeReplacement();
    TestUntrackedAddressHasNoGeneration();
    Memory::Shutdown();

    std::printf("memory_write_tracker_tests: %d checks, %d failures\n",
                g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
