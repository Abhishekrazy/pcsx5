// libkernel file-I/O tests — drives the real HLE implementations in
// src/hle/libkernel.cpp directly with GuestArgs structs (no thunk
// round-trip), via the declarations in src/hle/libkernel_file.h.
//
// Covered (SharpEmu ports bb3318a / d7f6e3f):
//   - POSIX open/close/read/write/fstat/stat return -1 with the guest errno
//     set on failure (never the raw 0x8002xxxx Orbis sentinel a libc caller
//     would store as a valid fd), and fd/count/0 on success.
//   - sceKernelMapDirectMemory2's shifted argument layout (memoryType at rdx,
//     alignment passed as the 7th/stack argument).
//
// Build target: libkernel_file_tests (see CMakeLists.txt).

#define _CRT_SECURE_NO_WARNINGS
#include "hle/libkernel_file.h"
#include "hle/hle.h"
#include "memory/memory.h"
#include "common/log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <io.h>

// The Release build defines NDEBUG, which would compile every assert() away
// and silently neuter this suite.  Redefine assert to an always-on check.
#undef assert
#define assert(cond)                                                            \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::printf("CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::fflush(stdout);                                                \
            std::exit(1);                                                       \
        }                                                                       \
    } while (0)

using namespace HLE;

namespace {

constexpr u64 kMinusOne = ~0ULL;
// The Orbis not-found sentinel a guest might mistake for an fd (see the
// SharpEmu bb3318a regression: 0x80020002 stored as an fd, then closed).
constexpr u64 kSentinelFd = 0x80020002;

const char* kTestFile = "pcsx5_libkernel_file_test.tmp";

guest_addr_t g_page = 0; // 64 KiB scratch page mapped in main()

GuestArgs Args(u64 a1 = 0, u64 a2 = 0, u64 a3 = 0, u64 a4 = 0, u64 a5 = 0, u64 a6 = 0) {
    GuestArgs a;
    a.arg1 = a1; a.arg2 = a2; a.arg3 = a3;
    a.arg4 = a4; a.arg5 = a5; a.arg6 = a6;
    return a;
}

guest_addr_t PutString(const char* s, u64 offset) {
    const guest_addr_t at = g_page + offset;
    std::strcpy(reinterpret_cast<char*>(at), s);
    return at;
}

s32 GuestErrno() { return *HLE::GuestErrnoPtr(); }

// POSIX open of an absent path must be -1 with errno=ENOENT, not the raw
// 0x80020002 sentinel the guest would otherwise keep as a "valid" fd.
void TestPosixOpenMissing() {
    HLE::SetGuestErrno(0);
    const guest_addr_t path = PutString("/__pcsx5_test_missing__/il2cpp.usym", 0x100);
    assert(PosixOpen(Args(path, O_RDONLY, 0)) == kMinusOne);
    assert(GuestErrno() == ENOENT);
    std::printf("  posix open missing: OK\n");
}

// Successful open returns the fd; fstat on it succeeds; closing it succeeds;
// a second close of the same fd is -1/EBADF (not the raw sentinel).
void TestPosixOpenFstatClose() {
    const guest_addr_t path = PutString(kTestFile, 0x100);
    const u64 fd = PosixOpen(Args(path, O_RDONLY, 0));
    assert(static_cast<s64>(fd) >= 0);

    const guest_addr_t statbuf = g_page + 0x400;
    assert(PosixFstat(Args(fd, statbuf)) == 0);
    // st_size (field 8 of the Orbis stat layout) must match the file written
    // by main() ("payload" = 7 bytes).
    assert(Memory::Read<s64>(statbuf + 7 * 8) == 7);

    assert(PosixClose(Args(fd)) == 0);
    HLE::SetGuestErrno(0);
    assert(PosixClose(Args(fd)) == kMinusOne);
    assert(GuestErrno() == EBADF);
    std::printf("  posix open/fstat/close: OK\n");
}

// fstat on the never-opened sentinel fd is -1/EBADF.
void TestPosixFstatSentinelFd() {
    HLE::SetGuestErrno(0);
    assert(PosixFstat(Args(kSentinelFd, g_page + 0x400)) == kMinusOne);
    assert(GuestErrno() == EBADF);
    std::printf("  posix fstat sentinel fd: OK\n");
}

// read/write on the sentinel fd are -1/EBADF.
void TestPosixReadWriteBadFd() {
    HLE::SetGuestErrno(0);
    assert(PosixRead(Args(kSentinelFd, g_page + 0x200, 0x40)) == kMinusOne);
    assert(GuestErrno() == EBADF);
    HLE::SetGuestErrno(0);
    assert(PosixWrite(Args(kSentinelFd, g_page + 0x200, 0x7)) == kMinusOne);
    assert(GuestErrno() == EBADF);
    std::printf("  posix read/write bad fd: OK\n");
}

// A real read through the POSIX wrapper returns the byte count.
void TestPosixReadRoundtrip() {
    const guest_addr_t path = PutString(kTestFile, 0x100);
    const u64 fd = PosixOpen(Args(path, O_RDONLY, 0));
    assert(static_cast<s64>(fd) >= 0);
    char* buf = reinterpret_cast<char*>(g_page + 0x200);
    std::memset(buf, 0, 0x40);
    assert(PosixRead(Args(fd, g_page + 0x200, 0x40)) == 7);
    assert(std::memcmp(buf, "payload", 7) == 0);
    assert(PosixClose(Args(fd)) == 0);
    std::printf("  posix read roundtrip: OK\n");
}

// stat on an existing file succeeds; a missing path is -1/ENOENT.
void TestPosixStat() {
    const guest_addr_t path = PutString(kTestFile, 0x100);
    const guest_addr_t statbuf = g_page + 0x400;
    assert(PosixStat(Args(path, statbuf)) == 0);
    assert(Memory::Read<s64>(statbuf + 7 * 8) == 7); // st_size

    HLE::SetGuestErrno(0);
    const guest_addr_t missing = PutString("/__pcsx5_test_missing__/nope.bin", 0x180);
    assert(PosixStat(Args(missing, statbuf)) == kMinusOne);
    assert(GuestErrno() == ENOENT);
    std::printf("  posix stat: OK\n");
}

// v2 takes memoryType at rdx (arg3) and alignment as the 7th (stack)
// argument; the mapped VA must come back through the in/out pointer.
void TestMapDirectMemory2() {
    const guest_addr_t addr_ptr = g_page + 0x800;
    Memory::Write<u64>(addr_ptr, 0); // no hint

    u64 stack_alignment = 0x10000;
    GuestArgs a = Args(addr_ptr, 0x10000, /*memoryType=*/0,
                       /*prot=*/0x3 /*RW*/, /*flags=*/0, /*directMemoryStart=*/0);
    a.stack_args = reinterpret_cast<u64>(&stack_alignment);

    assert(SceKernelMapDirectMemory2(a) == 0);
    const u64 mapped = Memory::Read<u64>(addr_ptr);
    assert(mapped != 0);
    // The mapping is writable guest memory (PROT_READ|PROT_WRITE).
    Memory::Write<u64>(mapped, 0x1122334455667788ULL);
    assert(Memory::Read<u64>(mapped) == 0x1122334455667788ULL);
    std::printf("  sceKernelMapDirectMemory2: OK\n");
}

// v1 still reads its unshifted register layout (alignment in r9/arg6).
void TestMapDirectMemory1() {
    const guest_addr_t addr_ptr = g_page + 0x808;
    Memory::Write<u64>(addr_ptr, 0);
    GuestArgs a = Args(addr_ptr, 0x10000, /*prot=*/0x3, /*flags=*/0,
                       /*directMemoryStart=*/0, /*alignment=*/0x10000);
    assert(SceKernelMapDirectMemory(a) == 0);
    assert(Memory::Read<u64>(addr_ptr) != 0);
    std::printf("  sceKernelMapDirectMemory: OK\n");
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    LogConfig::SetLevel(LogCategory::HLE, LogLevel::Warn);

    if (!Memory::Initialize()) {
        std::fprintf(stderr, "Memory::Initialize failed\n");
        return 1;
    }
    if (Memory::Map(0, 0x10000, Memory::PROT_READ | Memory::PROT_WRITE, &g_page) != Memory::Status::Ok) {
        std::fprintf(stderr, "scratch page map failed\n");
        return 1;
    }

    // Host-side fixture file the POSIX wrappers operate on.
    {
        const int fd = _open(kTestFile, _O_CREAT | _O_WRONLY | _O_BINARY | _O_TRUNC, 0644);
        assert(fd >= 0);
        assert(_write(fd, "payload", 7) == 7);
        assert(_close(fd) == 0);
    }

    TestPosixOpenMissing();
    TestPosixOpenFstatClose();
    TestPosixFstatSentinelFd();
    TestPosixReadWriteBadFd();
    TestPosixReadRoundtrip();
    TestPosixStat();
    TestMapDirectMemory2();
    TestMapDirectMemory1();

    std::remove(kTestFile);
    std::printf("libkernel_file_tests: ALL OK\n");
    return 0;
}
