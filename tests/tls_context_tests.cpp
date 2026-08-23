// GuestTlsContext characterization tests (src/kernel/tls.cpp).
//
// Pins the CURRENT semantics of the active TLS data model:
//   - Configure validation and thread-pointer computation
//     (FreeBSD variant-II: tp sits inside the allocation)
//   - Translate: signed fs displacement -> checked guest address,
//     bounds against [allocation_base, allocation_base + size)
//   - Reset clears all state
//
// These tests characterize the existing implementation; they do NOT assert
// any PS5-documented TLS layout beyond what the code already implements.
//
// Self-contained: links only tls.cpp + log.cpp.

#include "kernel/tls.h"
#include "common/log.h"

#include <cstdio>

namespace {

int g_failures = 0;
int g_checks   = 0;

#define EXPECT(cond, msg)                                                            \
    do {                                                                             \
        ++g_checks;                                                                  \
        if (!(cond)) {                                                               \
            std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, msg);     \
            ++g_failures;                                                            \
        }                                                                            \
    } while (0)

constexpr u64 kAllocBase = 0x900000000ULL;      // arbitrary guest VA outside the module window
constexpr u64 kAllocSize = Kernel::GuestTlsContext::kDefaultAllocationSize;  // 256 KiB
constexpr u64 kTpOffset  = Kernel::GuestTlsContext::kDefaultThreadPointerOffset; // size/2

void TestConfigureValid() {
    std::fprintf(stdout, "[TEST] Configure valid\n");
    Kernel::GuestTlsContext ctx;
    EXPECT(ctx.ThreadPointer() == 0, "fresh context has null tp");

    EXPECT(ctx.Configure(kAllocBase, kAllocSize), "default-offset configure");
    EXPECT(ctx.AllocationBase() == kAllocBase, "base stored");
    EXPECT(ctx.AllocationSize() == kAllocSize, "size stored");
    EXPECT(ctx.ThreadPointer() == kAllocBase + kTpOffset, "tp = base + default offset");
}

void TestConfigureValidation() {
    std::fprintf(stdout, "[TEST] Configure validation\n");
    Kernel::GuestTlsContext ctx;

    EXPECT(!ctx.Configure(0, kAllocSize), "null base rejected");
    EXPECT(!ctx.Configure(kAllocBase, 0), "zero size rejected");
    // tp offset must point inside the allocation.
    EXPECT(!ctx.Configure(kAllocBase, kAllocSize, kAllocSize), "offset==size rejected");

    // Overflow guard: base + size wraps the address space.
    EXPECT(!ctx.Configure(0xFFFFFFFFFFFFFF00ULL, 0x200), "overflowing range rejected");

    // Failed configures reset the context to empty.
    EXPECT(ctx.ThreadPointer() == 0 && ctx.AllocationBase() == 0 &&
           ctx.AllocationSize() == 0, "failed configure resets state");
}

void TestTranslate() {
    std::fprintf(stdout, "[TEST] Translate round-trip\n");
    Kernel::GuestTlsContext ctx;
    EXPECT(ctx.Configure(kAllocBase, kAllocSize), "configure for translate");
    const u64 tp = ctx.ThreadPointer();

    // Displacement 0 -> the thread pointer itself.
    guest_addr_t addr = 0;
    EXPECT(ctx.Translate(0, 8, addr), "disp=0 translates");
    EXPECT(addr == tp, "disp=0 hits tp");

    // Positive and negative displacements stay in-bounds.
    EXPECT(ctx.Translate(16, 8, addr) && addr == tp + 16, "positive disp");
    EXPECT(ctx.Translate(-8, 8, addr) && addr == tp - 8, "negative disp (variant-II)");

    // Width must fit within the allocation after the offset.
    EXPECT(!ctx.Translate(0, 0, addr), "zero-width access rejected");
    const u64 tail = static_cast<u64>(kTpOffset);  // bytes from alloc base to tp
    EXPECT(ctx.Translate(static_cast<s64>(kAllocSize - tail - 8), 8, addr),
           "last aligned qword in allocation translates");
    EXPECT(!ctx.Translate(static_cast<s64>(kAllocSize - tail - 8) + 1, 8, addr),
           "qword crossing allocation end rejected");
    EXPECT(!ctx.Translate(0, kAllocSize + 1, addr), "width above allocation size rejected");

    // Unconfigured context cannot translate.
    Kernel::GuestTlsContext empty;
    EXPECT(!empty.Translate(0, 8, addr), "unconfigured translate rejected");
}

void TestReset() {
    std::fprintf(stdout, "[TEST] Reset clears state\n");
    Kernel::GuestTlsContext ctx;
    EXPECT(ctx.Configure(kAllocBase, kAllocSize), "configure before reset");
    ctx.Reset();
    EXPECT(ctx.ThreadPointer() == 0 && ctx.AllocationBase() == 0 &&
           ctx.AllocationSize() == 0, "reset zeroes everything");
    guest_addr_t addr = 0;
    EXPECT(!ctx.Translate(0, 8, addr), "translate after reset rejected");
}

} // namespace

int main() {
    TestConfigureValid();
    TestConfigureValidation();
    TestTranslate();
    TestReset();

    std::fprintf(stdout, "TLS context: %d check(s), %d failure(s)\n",
                 g_checks, g_failures);
    if (g_failures != 0) {
        std::fprintf(stderr, "TLS context tests FAILED with %d failure(s)\n", g_failures);
        return 1;
    }
    std::fprintf(stdout, "OK\n");
    return 0;
}
