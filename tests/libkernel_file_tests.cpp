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
#include "kernel/kernel.h"
#include "kernel/fd_table.h"
#include "kernel/memory.h"
#include "cpu/cpu.h"
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

// FD table lifecycle, handle close symmetry, duplicate, and re-initialization.
void TestFdTableLifecycleAndTeardown() {
    // 1. Initialize FD table and verify standard descriptors (0, 1, 2).
    Kernel::InitializeFdTable();
    assert(Kernel::GetOpenFdCount() == 3);
    assert(Kernel::IsValidFd(0));
    assert(Kernel::IsValidFd(1));
    assert(Kernel::IsValidFd(2));

    // 2. Allocate multiple descriptors with mock/real file handles.
    HANDLE h1 = CreateFileA(kTestFile, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(h1 != INVALID_HANDLE_VALUE);
    HANDLE h2 = CreateFileA(kTestFile, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(h2 != INVALID_HANDLE_VALUE);

    int fd1 = Kernel::AllocateFd(Kernel::FD_TYPE_FILE, h1, O_RDONLY, 0644, "test_file_1");
    int fd2 = Kernel::AllocateFd(Kernel::FD_TYPE_FILE, h2, O_RDONLY, 0644, "test_file_2");
    assert(fd1 == 3);
    assert(fd2 == 4);
    assert(Kernel::GetOpenFdCount() == 5);
    assert(Kernel::IsValidFd(fd1));
    assert(Kernel::IsValidFd(fd2));

    // 3. Test duplication without recursive lock failure.
    int dup_fd = Kernel::DuplicateFd(fd1, -1);
    assert(dup_fd == 5);
    assert(Kernel::GetOpenFdCount() == 6);
    assert(Kernel::CloseFd(dup_fd));
    assert(Kernel::GetOpenFdCount() == 5);

    // 4. Teardown FD table while descriptors are still open:
    // MUST NOT DEADLOCK on non-recursive mutex and MUST close open handles.
    Kernel::ShutdownFdTable();
    assert(Kernel::GetOpenFdCount() == 0);
    assert(!Kernel::IsValidFd(0));
    assert(!Kernel::IsValidFd(fd1));
    assert(!Kernel::IsValidFd(fd2));

    // 5. Re-initialize FD table: must start clean with stdin/stdout/stderr.
    Kernel::InitializeFdTable();
    assert(Kernel::GetOpenFdCount() == 3);
    assert(Kernel::IsValidFd(0));

    HANDLE h3 = CreateFileA(kTestFile, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(h3 != INVALID_HANDLE_VALUE);
    int fd3 = Kernel::AllocateFd(Kernel::FD_TYPE_FILE, h3, O_RDONLY, 0644, "test_file_3");
    assert(fd3 == 3); // Reused lowest available non-standard slot
    assert(Kernel::GetOpenFdCount() == 4);

    Kernel::ShutdownFdTable();
    assert(Kernel::GetOpenFdCount() == 0);

    // Re-initialize for subsequent test fixtures
    Kernel::InitializeFdTable();
    std::printf("  fd_table lifecycle and teardown symmetry: OK\n");
}

// Complete Kernel subsystem lifecycle, module registry, threads, BRK, and path symmetry.
void TestKernelLifecycleAndTeardownSymmetry() {
    // 1. Initialize Kernel subsystem
    assert(Kernel::Initialize());

    // 2. Register module metadata
    Loader::LoadedModule mod;
    mod.name = "libtest.prx";
    mod.base_address = 0x880000000;
    mod.image_size = 0x20000;
    Loader::MappedSegment seg{};
    seg.address = 0x880000000;
    seg.size = 0x20000;
    mod.segments.push_back(seg);
    Kernel::RegisterLoadedModule(mod);
    assert(Kernel::FindModuleForAddr(0x880001000) != nullptr);

    // 3. Set path mappings
    Kernel::SetApp0Directory("C:/games/test_app0");
    Kernel::SetSaveDataDirectory("C:/savedata/test_save");
    assert(Kernel::TranslateGuestPath("/app0/data.bin") == "C:/games/test_app0/data.bin");

    // 4. Configure module resolver
    Kernel::ConfigureModuleResolver("C:/games/test_app0", "");
    assert(!Kernel::GetModuleResolver().SearchDirectories().empty());

    // 5. Establish BRK cursor
    guest_addr_t brk_val = Kernel::SetBreak(0);
    assert(brk_val != 0);
    assert(Kernel::GetBreak() == brk_val);

    // 6. Register a secondary thread
    Kernel::ThreadContext tctx;
    tctx.thread_id = 2;
    tctx.tls_base = 0x1234000;
    Kernel::RegisterThread(tctx);
    assert(Kernel::ResolveGuestThreadPointer(2) == 0x1234000);

    // 7. Shutdown Kernel subsystem
    Kernel::Shutdown();

    // 8. Verify all state is reset cleanly
    assert(Kernel::FindModuleForAddr(0x880001000) == nullptr);
    assert(Kernel::TranslateGuestPath("/app0/data.bin") == "/app0/data.bin");
    assert(Kernel::GetModuleResolver().SearchDirectories().empty());
    assert(Kernel::GetBreak() == 0);
    assert(Kernel::ResolveGuestThreadPointer(2) == 0);
    assert(CpuCore::ActiveThreadCount() == 0);

    // 9. Re-initialize Kernel subsystem for a fresh session
    assert(Kernel::Initialize());
    assert(Kernel::FindModuleForAddr(0x880001000) == nullptr);
    assert(Kernel::TranslateGuestPath("/app0/data.bin") == "/app0/data.bin");
    assert(Kernel::GetBreak() == 0);

    Kernel::Shutdown();
    std::printf("  kernel subsystem lifecycle and teardown symmetry: OK\n");
}

// HLE 2 GB direct memory physical pool reservation, commit, access, shutdown deallocation, and reinitialization.
void TestHlePhysicalPoolLifecycleAndTeardown() {
    // 1. Allocate from physical pool via SceKernelAllocateDirectMemory
    const guest_addr_t phys_out_ptr = g_page + 0x820;
    Memory::Write<u64>(phys_out_ptr, 0);
    GuestArgs alloc_args = Args(0, 0, 0x20000, 0x10000, 0, phys_out_ptr);
    assert(SceKernelAllocateDirectMemory(alloc_args) == 0);
    const u64 phys_offset = Memory::Read<u64>(phys_out_ptr);
    assert(phys_offset != 0);

    // 2. Map direct memory backing via SceKernelMapDirectMemory2
    const guest_addr_t addr_ptr = g_page + 0x810;
    Memory::Write<u64>(addr_ptr, 0);
    u64 stack_alignment = 0x10000;
    GuestArgs map_args = Args(addr_ptr, 0x20000, /*memoryType=*/0,
                              /*prot=*/0x3 /*RW*/, /*flags=*/0, /*directMemoryStart=*/phys_offset);
    map_args.stack_args = reinterpret_cast<u64>(&stack_alignment);

    assert(SceKernelMapDirectMemory2(map_args) == 0);
    const u64 mapped = Memory::Read<u64>(addr_ptr);
    assert(mapped != 0);
    assert(HLE::IsPhysPoolAddress(mapped));
    assert(Memory::QueryOwner(mapped) == Memory::Owner::Hle);

    // Verify read/write
    Memory::Write<u64>(mapped, 0xCAFEBABE12345678ULL);
    assert(Memory::Read<u64>(mapped) == 0xCAFEBABE12345678ULL);

    // 3. Shut down HLE physical pool
    HLE::ResetPhysPool();

    // 4. Verify pool is released and untracked
    assert(!HLE::IsPhysPoolAddress(mapped));
    assert(Memory::QueryOwner(mapped) == Memory::Owner::None);

    // Verify host memory is MEM_FREE via VirtualQuery
    MEMORY_BASIC_INFORMATION mbi{};
    SIZE_T query_res = VirtualQuery(reinterpret_cast<void*>(mapped), &mbi, sizeof(mbi));
    assert(query_res != 0);
    assert(mbi.State == MEM_FREE);

    // 5. Re-allocate in a fresh session to ensure clean re-initialization
    Memory::Write<u64>(phys_out_ptr, 0);
    assert(SceKernelAllocateDirectMemory(alloc_args) == 0);
    const u64 phys_offset2 = Memory::Read<u64>(phys_out_ptr);
    assert(phys_offset2 != 0);

    Memory::Write<u64>(addr_ptr, 0);
    GuestArgs map_args2 = Args(addr_ptr, 0x20000, /*memoryType=*/0,
                               /*prot=*/0x3 /*RW*/, /*flags=*/0, /*directMemoryStart=*/phys_offset2);
    map_args2.stack_args = reinterpret_cast<u64>(&stack_alignment);
    assert(SceKernelMapDirectMemory2(map_args2) == 0);
    const u64 mapped2 = Memory::Read<u64>(addr_ptr);
    assert(mapped2 != 0);
    assert(HLE::IsPhysPoolAddress(mapped2));
    assert(Memory::QueryOwner(mapped2) == Memory::Owner::Hle);
    Memory::Write<u64>(mapped2, 0xDEADBEEF87654321ULL);
    assert(Memory::Read<u64>(mapped2) == 0xDEADBEEF87654321ULL);

    // Final shutdown of pool
    HLE::ResetPhysPool();
    assert(!HLE::IsPhysPoolAddress(mapped2));
    assert(Memory::QueryOwner(mapped2) == Memory::Owner::None);

    std::printf("  hle physical pool lifecycle and teardown symmetry: OK\n");
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
    TestFdTableLifecycleAndTeardown();
    TestKernelLifecycleAndTeardownSymmetry();
    TestHlePhysicalPoolLifecycleAndTeardown();

    std::remove(kTestFile);
    std::printf("libkernel_file_tests: ALL OK\n");
    return 0;
}
