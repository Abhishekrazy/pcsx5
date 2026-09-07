// Regression test for the TLS stub's thread-pointer fallback (2026-09-07).
//
// A patched TLS site loads the guest thread pointer from a host TLS slot. On a
// host thread that reached guest code without ever being bound, that slot is
// zero. The stub used to return the zero unchanged, so `mov rax, fs:[0]`
// produced 0 and the guest's next instruction dereferenced a null-based
// address: PPSA02929 crashed at guest RIP 0x80015ff6d executing
// `mov r12, [rax - 0x15b8]` with RAX = 0.
//
// The exception-handler path this stub replaced resolves the same access
// through a three-level chain ending at the shared thread pointer, so the two
// disagreed. This test pins the stub's half of that agreement.
//
// What is not covered: executing the emitted code, which needs a live process
// with a real TLS slot. Only the emitted bytes are asserted here.

#include "kernel/tls_patch.h"

#include <cstdio>
#include <cstring>

namespace {
int g_failures = 0;

void Expect(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

// Finds `needle` in the first `size` bytes of `stub`.
bool Contains(const unsigned char* stub, unsigned size,
              const unsigned char* needle, unsigned needle_size) {
    if (needle_size > size) return false;
    for (unsigned i = 0; i + needle_size <= size; ++i) {
        if (std::memcmp(stub + i, needle, needle_size) == 0) return true;
    }
    return false;
}
} // namespace

int main() {
    using Kernel::TlsPatch::AccessInfo;
    using Kernel::TlsPatch::TestEmitStub;

    constexpr unsigned kSlot = 0x1488;
    constexpr unsigned long long kFallback = 0x1d680120000ull;

    AccessInfo read{};
    read.opcode = 0x8B;        // mov reg, fs:[disp]
    read.is_64bit = true;
    read.reg = 0;              // rax: the simplest form
    read.displacement = 0;     // fs:[0], the site that crashed
    read.instr_len = 12;

    unsigned char stub[64] = {};
    const unsigned size = TestEmitStub(stub, read, 0x800160384ull, kSlot, kFallback);
    Expect(size != 0, "a 64-bit fs:[0] read emits a stub");
    Expect(size <= sizeof(stub), "the stub fits the region");

    // The zero test and the branch over the fallback must both be present, or
    // a zero slot flows through to the guest again.
    const unsigned char test_rax[] = {0x48, 0x85, 0xC0};   // test rax, rax
    Expect(Contains(stub, size, test_rax, sizeof(test_rax)),
           "the stub tests the loaded thread pointer for zero");

    const unsigned char mov_imm64[] = {0x48, 0xB8};        // mov rax, imm64
    Expect(Contains(stub, size, mov_imm64, sizeof(mov_imm64)),
           "the stub carries a fallback thread pointer");

    unsigned char fallback_bytes[8];
    std::memcpy(fallback_bytes, &kFallback, 8);
    Expect(Contains(stub, size, fallback_bytes, 8),
           "the fallback is the thread pointer it was given");

    // Every other stub form is flag-transparent, and the zero test writes
    // flags, so the pair that preserves them has to be there.
    Expect(stub[0] == 0x9C, "the stub saves EFLAGS before testing");
    bool restores = false;
    for (unsigned i = 0; i < size; ++i) if (stub[i] == 0x9D) restores = true;
    Expect(restores, "the stub restores EFLAGS");

    // A zero fallback would reintroduce the null thread pointer the stub
    // exists to prevent, so the site must be left unpatched instead.
    unsigned char stub2[64] = {};
    Expect(TestEmitStub(stub2, read, 0x800160384ull, kSlot, 0) == 0,
           "a zero fallback refuses to emit and leaves the site unpatched");

    if (g_failures == 0) {
        std::fprintf(stdout, "tls stub fallback tests: all passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
