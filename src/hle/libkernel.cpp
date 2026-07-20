#define _CRT_SECURE_NO_WARNINGS
#include "hle.h"
#include "libkernel_file.h"
#include "guest_printf.h"
#include "../kernel/kernel.h"
#include "../kernel/thread.h"
#include "../kernel/guest_clock.h"
#include "../cpu/cpu.h"
#include "../memory/memory.h"
#include "../common/log.h"
#include "../gpu/gpu.h"
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <windows.h>
#include <cstring>
#include <chrono>
#include <thread>
#include <iostream>
#include <unordered_set>
#include <mutex>
#include <io.h>
#include <fcntl.h>
#include <cstdio>
#include <cerrno>
#include <string>
#include <sys/stat.h>

namespace HLE {

    // SysVAmd64VaList / GetNextVaListArg / SafeReadCharacter / FormatGuestString
    // moved to src/hle/guest_printf.{h,cpp} (shared with the libc sprintf
    // family in liblibc.cpp).
    // -----------------------------------------------------------------------
    // Physical memory pool (emulates PS5 direct memory / GPU-visible memory)
    //
    // On the real PS5:
    //   AllocateDirectMemory  -> returns a sequential PHYSICAL OFFSET (0-based)
    //   MapDirectMemory       -> maps [physOffset, physOffset+len) into VA space
    //
    // We emulate this with a single large VirtualAlloc reservation.
    // Physical offsets are just byte offsets into that reservation, and the
    // reservation is committed in 16 MiB chunks ahead of the bump pointer
    // (see EnsurePhysCommitted).  Kept at file scope so the demand-commit
    // fault handler can reach it.
    // -----------------------------------------------------------------------
    namespace {
        constexpr u64  PHYS_POOL_SIZE = 2ULL * 1024 * 1024 * 1024; // 2 GB pool
        // Commit-ahead granularity.  Allocation requests bump the phys offset
        // without touching pages; we commit the pool in large chunks ahead of
        // the bump pointer so that neither sceKernelMapDirectMemory nor a
        // guest first-touch has to pay a per-64KiB commit (the latter used to
        // go through the demand-commit VEH fault path: one exception + one
        // Info log line per 64 KiB block — Dreaming Sarah's content load
        // issues 450+ such blocks).
        constexpr u64  PHYS_COMMIT_CHUNK = 16ULL * 1024 * 1024; // 16 MB
        guest_addr_t   g_phys_pool_base = 0;
        u64            g_phys_pool_offset = 0x10000; // start past offset 0
        u64            g_phys_pool_committed = 0;    // bytes committed from base
        std::mutex     g_phys_mutex;

        // Lazily initialise the physical pool on first AllocateDirectMemory
        bool EnsurePhysPool() {
            if (g_phys_pool_base) return true;
            // Reserve (but don't commit) 2 GB.  We'll commit individual pages on Map.
            void* p = VirtualAlloc(nullptr, PHYS_POOL_SIZE, MEM_RESERVE, PAGE_NOACCESS);
            if (!p) {
                LOG_ERROR(HLE, "PhysPool: VirtualAlloc reserve failed!");
                return false;
            }
            g_phys_pool_base = reinterpret_cast<guest_addr_t>(p);
            LOG_INFO(HLE, "PhysPool: reserved 2 GB at base 0x%llx", g_phys_pool_base);
            return true;
        }

        // Ensure pool bytes [0, end_offset) are committed, committing whole
        // PHYS_COMMIT_CHUNK blocks.  Caller must hold g_phys_mutex.
        bool EnsurePhysCommitted(u64 end_offset) {
            while (g_phys_pool_committed < end_offset) {
                const guest_addr_t at = g_phys_pool_base + g_phys_pool_committed;
                if (!VirtualAlloc(reinterpret_cast<void*>(at), PHYS_COMMIT_CHUNK,
                                  MEM_COMMIT, PAGE_READWRITE)) {
                    LOG_ERROR(HLE, "PhysPool: chunk commit failed at 0x%llx (err=%lu)",
                              at, GetLastError());
                    return false;
                }
                g_phys_pool_committed += PHYS_COMMIT_CHUNK;
                LOG_DEBUG(HLE, "PhysPool: committed chunk at +0x%llx (%llu MiB total)",
                          g_phys_pool_committed - PHYS_COMMIT_CHUNK,
                          g_phys_pool_committed >> 20);
            }
            return true;
        }
    } // namespace

    bool IsPhysPoolAddress(guest_addr_t addr) {
        return g_phys_pool_base != 0 &&
               addr >= g_phys_pool_base && addr < g_phys_pool_base + PHYS_POOL_SIZE;
    }

    bool CommitPhysPool(guest_addr_t addr) {
        if (!IsPhysPoolAddress(addr)) return false;
        constexpr u64 kGranularity = 65536; // Windows allocation granularity
        const guest_addr_t base = addr & ~(kGranularity - 1);
        std::lock_guard<std::mutex> lk(g_phys_mutex);
        if (!VirtualAlloc(reinterpret_cast<void*>(base), kGranularity, MEM_COMMIT, PAGE_READWRITE)) {
            LOG_WARN(HLE, "PhysPool: demand-commit failed at 0x%llx (err=%lu)", base, GetLastError());
            return false;
        }
        LOG_DEBUG(HLE, "PhysPool: demand-committed 64 KiB at 0x%llx (fault at 0x%llx)", base, addr);
        return true;
    }

    // PS5 protection values may carry CPU/GPU visibility bits (0x10/0x20) in
    // the high nibble on top of PROT_READ/WRITE/EXEC (0x1/0x2/0x4).  Strip
    // everything except the RWX bits for host protection translation and warn
    // only on bits we genuinely do not recognize.
    static u32 SanitizeGuestProt(u32 prot) {
        constexpr u32 kCpuGpuFlags = 0x30;
        constexpr u32 kRwx = Memory::PROT_READ | Memory::PROT_WRITE | Memory::PROT_EXEC;
        const u32 unknown = prot & ~(kCpuGpuFlags | kRwx);
        if (unknown) {
            LOG_WARN(HLE, "SanitizeGuestProt: unknown prot bits 0x%X in 0x%X (stripped)", unknown, prot);
        }
        return prot & kRwx;
    }

    // -----------------------------------------------------------------------
    // sceKernelMapDirectMemory / sceKernelMapDirectMemory2
    //
    // MapDirectMemoryCore carries the shared body (SharpEmu d7f6e3f): the "2"
    // variant inserts a memoryType argument (rdx) ahead of v1's protection,
    // shifting protection/flags/directMemoryStart down one register each and
    // pushing alignment onto the stack (the 7th argument, captured by the
    // dispatcher as GuestArgs::stack_args).  memoryType only selects cache/GPU
    // attributes this HLE does not model per mapping, so it is accepted but
    // does not affect placement.
    // -----------------------------------------------------------------------
    static u64 MapDirectMemoryCore(guest_addr_t addr_ptr, u64 length, u32 prot,
                                   u32 flags, u64 phys_offset, u64 alignment) {
        (void)flags;

        LOG_INFO(HLE, "sceKernelMapDirectMemory(addr_ptr: 0x%llx, len: 0x%llx, prot: 0x%X, physOff: 0x%llx, align: 0x%llx)",
                 addr_ptr, length, prot, phys_offset, alignment);

        if (!addr_ptr || !length) {
            LOG_WARN(HLE, "sceKernelMapDirectMemory: null addr_ptr or zero length");
            return 0x800D0004;
        }
        if (alignment < 0x1000) alignment = 0x1000;

        std::lock_guard<std::mutex> lk(g_phys_mutex);
        if (!EnsurePhysPool()) return 0x800D0006;

        // Sanitize prot: PS5 prot values legitimately include CPU/GPU flag
        // bits (0x10/0x20) above the RWX nibble (e.g. 0x33); strip them.
        const u32 rwx = SanitizeGuestProt(prot);

        // Determine Windows protection
        DWORD win_prot = PAGE_READWRITE;
        bool r = (rwx & 1), w = (rwx & 2), x = (rwx & 4);
        if (x)      win_prot = w ? PAGE_EXECUTE_READWRITE : (r ? PAGE_EXECUTE_READ : PAGE_EXECUTE_READ); // Always Exec+Read for safety
        else if (w) win_prot = PAGE_READWRITE;
        else if (r) win_prot = PAGE_READONLY;
        else        win_prot = PAGE_NOACCESS;

        u64 rounded = (length + 0xFFF) & ~0xFFFULL;
        guest_addr_t hint = Memory::Read<u64>(addr_ptr);
        void* target = nullptr;
        bool alloc_ok = false;
        if (hint != 0) {
            target = reinterpret_cast<void*>(hint);
            // If the hint lies inside an existing reservation (the phys
            // pool or a sceKernelReserveVirtualRange region), a
            // RESERVE|COMMIT fails with ERROR_INVALID_ADDRESS — commit in
            // place instead.
            Memory::MemoryInfo qinfo{};
            const bool already_reserved =
                (Memory::Query(hint, &qinfo) == Memory::Status::Ok) && qinfo.is_reserved;
            if (already_reserved) {
                if (VirtualAlloc(target, rounded, MEM_COMMIT, win_prot)) {
                    alloc_ok = true;
                } else {
                    DWORD err = GetLastError();
                    LOG_ERROR(HLE, "MapDirectMemoryCore: commit-in-place failed at 0x%llx size=0x%llx (err=%lu)",
                              hint, rounded, err);
                }
            } else {
                // Reserve and commit at the hint address
                if (VirtualAlloc(target, rounded, MEM_RESERVE | MEM_COMMIT, win_prot)) {
                    alloc_ok = true;
                } else {
                    // Try to commit only in case it's already reserved
                    if (VirtualAlloc(target, rounded, MEM_COMMIT, win_prot)) {
                        alloc_ok = true;
                    } else {
                        DWORD err = GetLastError();
                        LOG_ERROR(HLE, "MapDirectMemoryCore: VirtualAlloc failed at 0x%llx size=0x%llx (err=%lu)",
                                  hint, rounded, err);
                    }
                }
            }
            if (!alloc_ok) {
                LOG_WARN(HLE, "MapDirectMemoryCore: hint 0x%llx unusable; falling back to phys pool mapping", hint);
            }
        }
        if (!alloc_ok) {
            if (phys_offset == 0) {
                // No physical backing requested.  This is how games
                // reserve distinct VA windows (prot=0, committed later
                // via sceKernelMprotect) or grab anonymous committed
                // memory.  An earlier revision fell back to
                // pool_base+0 for every such call, aliasing ALL of the
                // game's independent mappings onto one address; the
                // resulting heap corruption crashed LOST EPIC's dlmalloc
                // (chunk headers overlapping unrelated buffers).
                guest_addr_t va = 0;
                if (rwx == 0) {
                    alloc_ok = (Memory::Reserve(0, rounded, &va) == Memory::Status::Ok);
                } else {
                    alloc_ok = (Memory::Map(0, rounded, rwx, &va) == Memory::Status::Ok);
                }
                target = reinterpret_cast<void*>(va);
                if (!alloc_ok) {
                    LOG_ERROR(HLE, "MapDirectMemoryCore: failed to allocate distinct VA (size=0x%llx, prot=0x%X)",
                              rounded, prot);
                }
            } else {
                target = reinterpret_cast<void*>(g_phys_pool_base + phys_offset);
                if (VirtualAlloc(target, rounded, MEM_COMMIT, win_prot)) {
                    alloc_ok = true;
                } else {
                    DWORD old;
                    if (VirtualProtect(target, rounded, win_prot, &old)) {
                        alloc_ok = true;
                    } else {
                        DWORD err = GetLastError();
                        LOG_ERROR(HLE, "MapDirectMemoryCore: Phys pool commit failed at 0x%llx size=0x%llx (err=%lu)",
                                  (u64)target, rounded, err);
                    }
                }
            }
        }

        if (!alloc_ok || !target) {
            LOG_ERROR(HLE, "MapDirectMemoryCore: Allocation failed!");
            return 0x800D0006;
        }

        guest_addr_t mapped_va = reinterpret_cast<guest_addr_t>(target);
        Memory::Write<u64>(addr_ptr, mapped_va);
        LOG_INFO(HLE, "sceKernelMapDirectMemory -> va: 0x%llx", mapped_va);
        return 0;
    }

    u64 SceKernelMapDirectMemory(const GuestArgs& args) {
        return MapDirectMemoryCore(args.arg1, args.arg2, static_cast<u32>(args.arg3),
                                   static_cast<u32>(args.arg4), args.arg5, args.arg6);
    }

    u64 SceKernelMapDirectMemory2(const GuestArgs& args) {
        const u64 alignment = args.stack_args ? Memory::Read<u64>(args.stack_args) : 0;
        return MapDirectMemoryCore(args.arg1, args.arg2, static_cast<u32>(args.arg4),
                                   static_cast<u32>(args.arg5), args.arg6, alignment);
    }

    // -----------------------------------------------------------------------
    // Kernel file operations (sceKernel* cores + POSIX exports)
    //
    // The raw sceKernel* handlers follow the Orbis convention: fd / byte
    // count / 0 on success, SCE_KERNEL_ERROR_E* (0x80020000|errno) on
    // failure.  The POSIX-named exports (open/close/read/write/fstat/stat)
    // are called by guest libc code that follows the POSIX ABI — -1 with
    // errno set through __error() — so they wrap the cores through
    // PosixFailure.  Leaking the raw 0x8002xxxx sentinel to a libc caller
    // makes it store the sentinel as a valid fd and later dereference it
    // (SharpEmu bb3318a: Unity's IL2CPP file layer probing an absent
    // il2cpp.usym crashed exactly this way).  fd-based calls map a bad handle
    // to EBADF; path-based calls default to ENOENT.
    // -----------------------------------------------------------------------
    namespace {
        constexpr s32 SCE_KERNEL_ERROR_EBADF  = static_cast<s32>(0x80020009);
        constexpr s32 SCE_KERNEL_ERROR_EFAULT = static_cast<s32>(0x8002000E);
        constexpr s32 SCE_KERNEL_ERROR_EINVAL = static_cast<s32>(0x80020016);

        // CRT fds handed out by KernelOpenCore.  The POSIX wrappers must
        // reject a bad/closed/sentinel fd with EBADF before it reaches the
        // UCRT: closing an already-closed fd (exactly the SharpEmu bb3318a
        // scenario) terminates the process via the invalid-parameter handler
        // instead of returning EBADF.  Host stdio fds 0-2 are always
        // considered valid (and are never actually closed — that would kill
        // the emulator's own stdout).
        std::mutex g_guest_fd_mutex;
        std::unordered_set<int> g_guest_fds;

        bool IsGuestFd(int fd) {
            if (fd >= 0 && fd <= 2) return true;
            std::lock_guard<std::mutex> lk(g_guest_fd_mutex);
            return g_guest_fds.count(fd) != 0;
        }

        void TrackGuestFd(int fd) {
            std::lock_guard<std::mutex> lk(g_guest_fd_mutex);
            g_guest_fds.insert(fd);
        }

        bool UntrackGuestFd(int fd) {
            std::lock_guard<std::mutex> lk(g_guest_fd_mutex);
            return g_guest_fds.erase(fd) != 0;
        }

        // The host CRT errno values match the Orbis/FreeBSD numbering for
        // every code produced below, so the Orbis error is 0x80020000 + errno
        // (sign-extended into s64 so failure is simply result < 0).
        s64 OrbisErrno() {
            return static_cast<s64>(static_cast<s32>(0x80020000 + errno));
        }

        std::string ReadGuestPath(guest_addr_t path_ptr) {
            std::string path;
            if (path_ptr) {
                for (u64 i = 0; i < 4096; ++i) {
                    const u8 c = Memory::Read<u8>(path_ptr + i);
                    if (!c) break;
                    path += static_cast<char>(c);
                }
            }
            return path;
        }

        // Orbis struct stat layout (matches src/kernel/syscalls.cpp SysStat).
        struct OrbisStat {
            u64 st_dev;
            u64 st_ino;
            u64 st_mode;
            u64 st_nlink;
            u64 st_uid;
            u64 st_gid;
            u64 st_rdev;
            s64 st_size;
            s64 st_blksize;
            s64 st_blocks;
            s64 st_atime;
            s64 st_atimensec;
            s64 st_mtime;
            s64 st_mtimensec;
            s64 st_ctime;
            s64 st_ctimensec;
            s64 __unused[3];
        };

        void FillOrbisStat(guest_addr_t statbuf, const struct _stat64& hs) {
            OrbisStat st{};
            st.st_dev     = hs.st_dev;
            st.st_ino     = hs.st_ino;
            // _S_IFREG (0100000) / _S_IFDIR (0040000) match the Orbis values.
            st.st_mode    = hs.st_mode;
            st.st_nlink   = hs.st_nlink;
            st.st_uid     = hs.st_uid;
            st.st_gid     = hs.st_gid;
            st.st_rdev    = hs.st_rdev;
            st.st_size    = hs.st_size;
            st.st_blksize = 4096;
            st.st_blocks  = (st.st_size + 511) / 512;
            st.st_atime   = hs.st_atime;
            st.st_mtime   = hs.st_mtime;
            st.st_ctime   = hs.st_ctime;
            Memory::WriteBuffer(statbuf, &st, sizeof(st));
        }

        s64 KernelOpenCore(guest_addr_t path_ptr, int flags, int mode) {
            if (!path_ptr) return SCE_KERNEL_ERROR_EINVAL;
            const std::string path = ReadGuestPath(path_ptr);
            const int fd = _open(Kernel::TranslateGuestPath(path).c_str(), flags | _O_BINARY, mode);
            if (fd < 0) {
                const s64 err = OrbisErrno();
                LOG_INFO(HLE, "sceKernelOpen('%s', flags=0x%X, mode=0x%X) -> error 0x%X",
                         path.c_str(), flags, mode, static_cast<u32>(err));
                return err;
            }
            LOG_INFO(HLE, "sceKernelOpen('%s', flags=0x%X, mode=0x%X) -> %d", path.c_str(), flags, mode, fd);
            TrackGuestFd(fd);
            return fd;
        }

        s64 KernelReadCore(int fd, guest_addr_t buf, u64 count) {
            if (!buf || count == 0) return 0;
            if (!IsGuestFd(fd)) return SCE_KERNEL_ERROR_EBADF;
            const int n = _read(fd, reinterpret_cast<void*>(buf), static_cast<unsigned>(count));
            if (n < 0) return OrbisErrno();
            LOG_DEBUG(HLE, "sceKernelRead(fd=%d, buf=0x%llx, count=%llu) -> %d", fd, buf, count, n);
            return n;
        }

        s64 KernelWriteCore(int fd, guest_addr_t buf, u64 count) {
            if (!buf || count == 0) return 0;
            if (!IsGuestFd(fd)) return SCE_KERNEL_ERROR_EBADF;
            const int n = _write(fd, reinterpret_cast<const void*>(buf), static_cast<unsigned>(count));
            if (n < 0) return OrbisErrno();
            LOG_DEBUG(HLE, "sceKernelWrite(fd=%d, buf=0x%llx, count=%llu) -> %d", fd, buf, count, n);
            return n;
        }

        s64 KernelCloseCore(int fd) {
            // Never close host stdio behind the emulator's back.
            if (fd >= 0 && fd <= 2) return 0;
            if (!UntrackGuestFd(fd)) return SCE_KERNEL_ERROR_EBADF;
            const int r = _close(fd);
            if (r != 0) return OrbisErrno();
            LOG_DEBUG(HLE, "sceKernelClose(fd=%d) -> 0", fd);
            return 0;
        }

        s64 KernelFstatCore(int fd, guest_addr_t statbuf) {
            if (!statbuf) return SCE_KERNEL_ERROR_EFAULT;
            if (!IsGuestFd(fd)) return SCE_KERNEL_ERROR_EBADF;
            struct _stat64 hs {};
            if (_fstat64(fd, &hs) != 0) return OrbisErrno();
            FillOrbisStat(statbuf, hs);
            LOG_DEBUG(HLE, "sceKernelFstat(fd=%d) -> 0 (size=%lld)", fd, static_cast<s64>(hs.st_size));
            return 0;
        }

        s64 KernelStatCore(guest_addr_t path_ptr, guest_addr_t statbuf) {
            if (!path_ptr || !statbuf) return SCE_KERNEL_ERROR_EFAULT;
            const std::string path = ReadGuestPath(path_ptr);
            struct _stat64 hs {};
            if (_stat64(Kernel::TranslateGuestPath(path).c_str(), &hs) != 0) {
                const s64 err = OrbisErrno();
                LOG_INFO(HLE, "sceKernelStat('%s') -> error 0x%X", path.c_str(), static_cast<u32>(err));
                return err;
            }
            FillOrbisStat(statbuf, hs);
            LOG_DEBUG(HLE, "sceKernelStat('%s') -> 0 (size=%lld)", path.c_str(), static_cast<s64>(hs.st_size));
            return 0;
        }
    } // namespace

    // Translates a failed raw Orbis kernel result into the libc/POSIX ABI:
    // return -1 with errno set (via the __error() cell, SetGuestErrno).
    static u64 PosixFailure(s64 orbis_result, int not_found_errno = ENOENT) {
        int e;
        switch (static_cast<u32>(orbis_result)) {
            case 0x80020016: e = EINVAL; break; // SCE_KERNEL_ERROR_EINVAL
            case 0x8002000E: e = EFAULT; break; // SCE_KERNEL_ERROR_EFAULT
            case 0x8002000D: e = EACCES; break; // SCE_KERNEL_ERROR_EACCES
            default:         e = not_found_errno; break;
        }
        SetGuestErrno(e);
        return ~0ull; // -1
    }

    u64 PosixOpen(const GuestArgs& args) {
        // Our HLE return value lands in RAX directly, so on success the fd
        // itself is returned (SharpEmu returns 0 and lets its import bridge
        // prefer the RAX written by the core — same net effect).
        const s64 r = KernelOpenCore(args.arg1, static_cast<int>(args.arg2), static_cast<int>(args.arg3));
        return r < 0 ? PosixFailure(r) : static_cast<u64>(r);
    }

    u64 PosixClose(const GuestArgs& args) {
        const s64 r = KernelCloseCore(static_cast<int>(args.arg1));
        return r < 0 ? PosixFailure(r, EBADF) : 0;
    }

    u64 PosixRead(const GuestArgs& args) {
        const s64 r = KernelReadCore(static_cast<int>(args.arg1), args.arg2, args.arg3);
        return r < 0 ? PosixFailure(r, EBADF) : static_cast<u64>(r);
    }

    u64 PosixWrite(const GuestArgs& args) {
        const s64 r = KernelWriteCore(static_cast<int>(args.arg1), args.arg2, args.arg3);
        return r < 0 ? PosixFailure(r, EBADF) : static_cast<u64>(r);
    }

    u64 PosixFstat(const GuestArgs& args) {
        const s64 r = KernelFstatCore(static_cast<int>(args.arg1), args.arg2);
        return r < 0 ? PosixFailure(r, EBADF) : 0;
    }

    u64 PosixStat(const GuestArgs& args) {
        const s64 r = KernelStatCore(args.arg1, args.arg2);
        return r < 0 ? PosixFailure(r) : 0;
    }

    // Helper to register standard stubs
    void RegisterLibKernel() {
        LOG_INFO(HLE, "Registering libkernel HLE symbols...");

        // =====================================================================
        // XKRegsFpEpk#T#T  ===  PS5 __libc_start_main / sceLibcInitialize
        // Called by _start after TLS setup and DT_INIT:
        //   XKRegsFpEpk(argc, argv, envp)  -> should NEVER return to _start
        // We find the game's main() via HLE::GetGuestMainAddress() (set at load time
        // by scanning the symbol table for "main"), then call it via InvokeGuestFunction.
        // =====================================================================
        RegisterSymbol("libkernel", "XKRegsFpEpk#T#T", [](const GuestArgs& args) -> u64 {
            // XKRegsFpEpk is "catchReturnFromMain" — the PS5 libc calls this to wrap
            // a call to main() and catch its return value. The CPU dispatcher already
            // calls main() directly from Execute(); by the time XKRegsFpEpk is hit from
            // inside _start, main() has returned and this should just log the exit status
            // and signal a clean process exit. Calling InvokeGuestFunction(main_va) here
            // would cause stack corruption on secondary guest threads (SharpEmu insight).
            u64 exit_status = args.arg1;  // rdi = exit status from main()

            LOG_INFO(HLE, "XKRegsFpEpk (catchReturnFromMain): exit_status=%llu — signalling process exit", exit_status);

            // Signal the guest dispatch loop to terminate cleanly by jumping
            // back into Kernel::Execute (HLE::ExitGuestProcess longjmps to the
            // armed setjmp buffer); the main thread's window loop then
            // proceeds through the normal shutdown path.
            HLE::ExitGuestProcess(static_cast<u32>(exit_status));
        });

        // =====================================================================
        // XwLA5cTHjt4#T#T  ===  sceKernelGetProcessType
        // Returns the process type (1 = SceKernelMainProc for the main process).
        // Called at startup to determine execution context.
        // =====================================================================
        RegisterSymbol("libkernel", "XwLA5cTHjt4#T#T", [](const GuestArgs& /*args*/) -> u64 {
            LOG_DEBUG(HLE, "sceKernelGetProcessType() -> 1 (SceKernelMainProc)");
            return 1; // SCE_KERNEL_MAIN_PROC
        });

        // =====================================================================
        // scePthreadAttrInit  /  scePthreadAttrDestroy
        // Minimal attr init: write the struct size as a sentinel, return 0.
        // =====================================================================
        RegisterSymbol("libkernel", "scePthreadAttrInit", [](const GuestArgs& args) -> u64 {
            guest_addr_t attr_ptr = args.arg1;
            if (attr_ptr) Memory::Write<u64>(attr_ptr, 0x38); // attr struct sentinel size
            LOG_DEBUG(HLE, "scePthreadAttrInit(0x%llx) -> OK", attr_ptr);
            return 0;
        });
        RegisterSymbol("libkernel", "scePthreadAttrDestroy", [](const GuestArgs& args) -> u64 {
            LOG_DEBUG(HLE, "scePthreadAttrDestroy(0x%llx) -> OK", args.arg1);
            return 0;
        });
        RegisterSymbol("libkernel", "62KCwEMmzcM#S#N", [](const GuestArgs& args) -> u64 {
            LOG_DEBUG(HLE, "scePthreadAttrDestroy(NID)(0x%llx) -> OK", args.arg1);
            return 0;
        });
        RegisterSymbol("libkernel", "scePthreadAttrSetstacksize", [](const GuestArgs& args) -> u64 {
            LOG_DEBUG(HLE, "scePthreadAttrSetstacksize(attr=0x%llx, size=0x%llx) -> OK", args.arg1, args.arg2);
            return 0;
        });
        RegisterSymbol("libkernel", "scePthreadAttrSetdetachstate", [](const GuestArgs& args) -> u64 {
            LOG_DEBUG(HLE, "scePthreadAttrSetdetachstate(attr=0x%llx, state=%llu) -> OK", args.arg1, args.arg2);
            return 0;
        });
        RegisterSymbol("libkernel", "scePthreadAttrSetschedparam", [](const GuestArgs& args) -> u64 {
            LOG_DEBUG(HLE, "scePthreadAttrSetschedparam(attr=0x%llx) -> OK", args.arg1);
            return 0;
        });
        RegisterSymbol("libkernel", "scePthreadAttrSetinheritsched", [](const GuestArgs& args) -> u64 {
            LOG_DEBUG(HLE, "scePthreadAttrSetinheritsched(attr=0x%llx, inherit=%llu) -> OK", args.arg1, args.arg2);
            return 0;
        });
        RegisterSymbol("libkernel", "scePthreadAttrSetschedpolicy", [](const GuestArgs& args) -> u64 {
            LOG_DEBUG(HLE, "scePthreadAttrSetschedpolicy(attr=0x%llx, policy=%llu) -> OK", args.arg1, args.arg2);
            return 0;
        });

        // =====================================================================
        // scePthreadCreate / scePthreadJoin are registered by NID below
        // (6UgtwV+0zb4#T#T and onNY9Byn-W8#S#N); the name-keyed duplicates
        // were dropped during the CpuCore thread-registry consolidation.
        // =====================================================================

        // =====================================================================
        // scePthreadDetach  ===  Detach a thread (auto-cleanup on exit).
        // =====================================================================
        RegisterSymbol("libkernel", "scePthreadDetach", [](const GuestArgs& args) -> u64 {
            u64 tid = args.arg1;
            LOG_INFO(HLE, "scePthreadDetach(tid=%llu)", tid);

            if (!CpuCore::DetachThread(tid)) {
                LOG_ERROR(HLE, "scePthreadDetach: thread %llu not found", tid);
                return 3; // ESRCH
            }
            return 0;
        });

        // =====================================================================
        // scePthreadExit  ===  Exit the current thread.
        // =====================================================================
        RegisterSymbol("libkernel", "scePthreadExit", [](const GuestArgs& args) -> u64 {
            u64 exit_value = args.arg1;
            LOG_INFO(HLE, "scePthreadExit(value=0x%llx)", exit_value);
            ExitThread(static_cast<DWORD>(exit_value));
        });

        // scePthreadSelf is registered by NID below (aI+OeCz8xrQ#T#T).
        RegisterSymbol("libkernel", "scePthreadGetprio", [](const GuestArgs& args) -> u64 {
            LOG_DEBUG(HLE, "scePthreadGetprio(thread=0x%llx) -> 700", args.arg1);
            if (args.arg2) Memory::Write<s32>(args.arg2, 700); // Default PS5 priority
            return 0;
        });
        RegisterSymbol("libkernel", "scePthreadSetprio", [](const GuestArgs& args) -> u64 {
            LOG_DEBUG(HLE, "scePthreadSetprio(thread=0x%llx, prio=%lld)", args.arg1, (s64)args.arg2);
            return 0;
        });

        // =====================================================================
        // Pthread mutex/cond/rwlock/once/TLS-key, sceKernel mutex/sema/event
        // flag/equeue and process-clock symbols are now REAL implementations
        // registered from src/hle/libkernel_sync.cpp (HLE::RegisterLibKernelSync,
        // called right after RegisterLibKernel in HLE::Initialize).
        // =====================================================================

        // =====================================================================
        // sceKernelGetProcessType (also registered via NID alias)
        // =====================================================================
        RegisterSymbol("libkernel", "sceKernelGetProcessType", [](const GuestArgs& /*args*/) -> u64 {
            return 1; // SCE_KERNEL_MAIN_PROC
        });

        // =====================================================================
        // Usleep
        RegisterSymbol("libkernel", "sceKernelUsleep", [](const GuestArgs& args) -> u64 {
            u32 microseconds = static_cast<u32>(args.arg1);

            LOG_DEBUG(HLE, "sceKernelUsleep(%u us)", microseconds);
            std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
            return 0;
        });

        // Gettimeofday
        RegisterSymbol("libkernel", "sceKernelGettimeofday", [](const GuestArgs& args) -> u64 {
            guest_addr_t tv_ptr = args.arg1;
            // timeval structure: 8-byte tv_sec, 8-byte tv_usec
            if (tv_ptr) {
                auto now = std::chrono::system_clock::now();
                auto duration = now.time_since_epoch();
                auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
                auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(duration).count() % 1000000;

                Memory::Write<u64>(tv_ptr, seconds);
                Memory::Write<u64>(tv_ptr + 8, microseconds);
            }
            return 0;
        });

        // clock_gettime (lLMT9vJAck0) — mirrors sys_clock_gettime
        // (src/kernel/syscalls.cpp) over the shared guest clock:
        // clock_id 0 = CLOCK_REALTIME, anything else = monotonic QPC origin.
        // timespec layout: 8-byte tv_sec, 8-byte tv_nsec.
        auto ClockGettime = [](const GuestArgs& args) -> u64 {
            const u32 clock_id = static_cast<u32>(args.arg1);
            const guest_addr_t tp = args.arg2;
            if (tp == 0) {
                return static_cast<u64>(-1);
            }
            s64 sec = 0, nsec = 0;
            if (clock_id == 0) { // CLOCK_REALTIME
                Kernel::GuestClockRealtime(&sec, &nsec);
            } else { // CLOCK_MONOTONIC etc. — shared QPC origin (guest_clock.cpp)
                const u64 qpc  = Kernel::GuestClockCounter();
                const u64 freq = Kernel::GuestClockCounterFrequency();
                sec  = static_cast<s64>(qpc / freq);
                nsec = static_cast<s64>(((qpc % freq) * 1000000000ULL) / freq);
            }
            Memory::Write<u64>(tp, static_cast<u64>(sec));
            Memory::Write<u64>(tp + 8, static_cast<u64>(nsec));
            return 0;
        };
        RegisterSymbol("libkernel", "clock_gettime", ClockGettime);
        RegisterSymbol("libkernel", "lLMT9vJAck0", ClockGettime);

        // sceKernelCreateMutex / LockMutex / UnlockMutex / DeleteMutex are real
        // implementations in src/hle/libkernel_sync.cpp.

        // Direct memory allocations (mocking virtual heap allocations)
        RegisterSymbol("libkernel", "sceKernelAllocateMainDirectMemory", [](const GuestArgs& args) -> u64 {
            u64 size = args.arg1;
            u64 alignment = args.arg2;
            u32 type = static_cast<u32>(args.arg3);
            guest_addr_t phys_addr_out = args.arg4;
            (void)type;

            LOG_INFO(HLE, "sceKernelAllocateMainDirectMemory(size: %llu, align: %llu, type: %u)", size, alignment, type);

            // On real hardware this returns a physical direct-memory OFFSET
            // (0-based), exactly like sceKernelAllocateDirectMemory.  An
            // earlier revision mapped a separate host buffer and returned its
            // VA as the "phys addr"; games then passed that VA as physOff to
            // sceKernelMapDirectMemory, so the mapped VA and the backing
            // buffer were disjoint and data written through one view was
            // invisible through the other.  Carve from the phys pool instead
            // so physOff always refers to pool memory (LOST EPIC).
            if (!phys_addr_out || !size) return 0x800D0004; // EINVAL
            if (alignment < 0x1000) alignment = 0x1000;

            std::lock_guard<std::mutex> lk(g_phys_mutex);
            if (!EnsurePhysPool()) return 0x800D0006;

            const u64 phys_offset = (g_phys_pool_offset + alignment - 1) & ~(alignment - 1);
            if (phys_offset + size > PHYS_POOL_SIZE) {
                LOG_ERROR(HLE, "sceKernelAllocateMainDirectMemory: out of physical pool space!");
                return 0x800D0006;
            }
            g_phys_pool_offset = phys_offset + size;
            if (!EnsurePhysCommitted(phys_offset + size)) return 0x800D0006;

            Memory::Write<u64>(phys_addr_out, phys_offset);
            LOG_INFO(HLE, "sceKernelAllocateMainDirectMemory -> physOffset: 0x%llx (size: 0x%llx)", phys_offset, size);
            return 0; // Success
        });

        RegisterSymbol("libkernel", "sceKernelMapDirectMemory", [](const GuestArgs& args) -> u64 {
            guest_addr_t start = args.arg1;
            u64 size = args.arg2;
            u32 prot = static_cast<u32>(args.arg3);
            u32 flags = static_cast<u32>(args.arg4);
            u64 phys_addr = args.arg5;
            u64 alignment = args.arg6;
            (void)flags;
            (void)alignment;

            LOG_INFO(HLE, "sceKernelMapDirectMemory(start: 0x%llx, size: %llu, phys: 0x%llx)", start, size, phys_addr);
            // Protect direct memory range (strip PS5 CPU/GPU prot flag bits)
            if (Memory::Protect(start, size, SanitizeGuestProt(prot)) != Memory::Status::Ok) {
                LOG_WARN(HLE, "sceKernelMapDirectMemory: Protect failed");
            }
            return 0; // Success
        });

        // _init_env (bzQExy189ZI#T#T) — libc environment init, called at startup
        RegisterSymbol("libkernel", "bzQExy189ZI#T#T", [](const GuestArgs& /*args*/) -> u64 {
            LOG_DEBUG(HLE, "libkernel::_init_env() -> 0 (success)");
            return 0;
        });

        // atexit (8G2LB+A3rzg#T#T) — register process-exit callback
        RegisterSymbol("libkernel", "8G2LB+A3rzg#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t func = args.arg1;
            LOG_DEBUG(HLE, "libkernel::atexit(func: 0x%llx) -> 0", func);
            return 0; // success
        });

        RegisterSymbol("libkernel", "sceKernelLoadStartModule", [](const GuestArgs& args) -> u64 {
            guest_addr_t path_ptr = args.arg1;
            u32 argc = static_cast<u32>(args.arg2);
            guest_addr_t argv = args.arg3;
            u32 flags = static_cast<u32>(args.arg4);
            guest_addr_t opts = args.arg5;
            guest_addr_t res = args.arg6;
            (void)argc;
            (void)argv;
            (void)opts;
            (void)res;

            const char* path = path_ptr ? reinterpret_cast<const char*>(path_ptr) : "unknown";
            LOG_INFO(HLE, "sceKernelLoadStartModule(path: '%s', flags: 0x%X)", path, flags);

            // Consult the module resolver first: if the guest asked for a
            // module by name (or guest path) and the user supplied a real
            // PRX for it, load that instead of falling back to HLE.
            std::string filepath = path;
            if (auto resolved = Kernel::GetModuleResolver().ResolveModuleFile(path)) {
                LOG_INFO(HLE, "sceKernelLoadStartModule: resolved '%s' to PRX '%s'",
                         path, resolved->string().c_str());
                filepath = resolved->string();
            } else {
                // Guest paths ("/app0/...") must be translated to host paths
                // before hitting the filesystem (LOST EPIC's Unity modules).
                filepath = Kernel::TranslateGuestPath(path);
            }

            // Attempt to load the module using our kernel loader
            Loader::LoadedModule loaded_lib;

            if (Kernel::LoadModule(filepath, loaded_lib)) {
                static u32 mock_module_id = 0x2000;
                u32 mod_id = mock_module_id++;
                LOG_INFO(HLE, "Successfully loaded PRX module '%s' (assigned ID: 0x%X)", filepath.c_str(), mod_id);
                return mod_id;
            }

            LOG_ERROR(HLE, "Failed to load PRX module: %s", filepath.c_str());
            return 0x80020001; // Standard Sony error code for module not found
        });

        // memset (8zTFvBIAIN8#T#T)
        RegisterSymbol("libkernel", "8zTFvBIAIN8#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t dest = args.arg1;
            u32 ch = static_cast<u32>(args.arg2);
            u64 count = args.arg3;
            
            // Guard against corrupted count values (e.g. count > 256MB is likely garbage)
            constexpr u64 MAX_MEMSET = 256ULL * 1024 * 1024;
            if (count > MAX_MEMSET) {
                LOG_WARN(HLE, "libkernel::memset: count 0x%llx exceeds 256MB limit, clamping to 0", count);
                count = 0;
            }
            
            LOG_DEBUG(HLE, "libkernel::memset(dest: 0x%llx, ch: %u, count: %llu)", dest, ch, count);
            if (dest && count > 0) {
                std::memset(reinterpret_cast<void*>(dest), static_cast<int>(ch & 0xFF), count);
            }
            return dest;
        });

        // strlen (j4ViWNHEgww#T#T)
        RegisterSymbol("libkernel", "j4ViWNHEgww#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t str = args.arg1;
            if (!str) return 0;
            
            u64 len = 0;
            while (Memory::Read<u8>(str + len) != 0) {
                len++;
            }
            
            std::string content;
            for (u64 i = 0; i < (len < 256 ? len : 256); ++i) {
                content += static_cast<char>(Memory::Read<u8>(str + i));
            }
            LOG_DEBUG(HLE, "libkernel::strlen(str: 0x%llx) -> %llu (Value: '%s')", str, len, content.c_str());
            return len;
        });

        // __cxa_atexit (tsvEmnenz48#T#T)
        RegisterSymbol("libkernel", "tsvEmnenz48#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t func = args.arg1;
            guest_addr_t arg = args.arg2;
            guest_addr_t dso = args.arg3;
            LOG_DEBUG(HLE, "libkernel::__cxa_atexit(func: 0x%llx, arg: 0x%llx, dso: 0x%llx)", func, arg, dso);
            return 0; // Success
        });

        // vsnprintf (Q2V+iqvjgC0#T#T)
        RegisterSymbol("libkernel", "Q2V+iqvjgC0#T#T", [](const GuestArgs& args) -> u64 {
            const u64 written = GuestVsnprintf(args);
            LOG_DEBUG(HLE, "libkernel::vsnprintf(dest: 0x%llx, size: %llu) -> %llu",
                      args.arg1, args.arg2, written);
            return written;
        });

        // fputs (QrZZdJ8XsX0#T#T)
        RegisterSymbol("libkernel", "QrZZdJ8XsX0#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t str_ptr = args.arg1;
            std::string msg;
            if (str_ptr) {
                u64 offset = 0;
                while (true) {
                    u8 ch = 0;
                    if (!SafeReadCharacter(str_ptr + offset, ch)) {
                        msg += "(invalid)";
                        break;
                    }
                    if (ch == 0) break;
                    msg += static_cast<char>(ch);
                    offset++;
                    if (offset > 4096) break;
                }
            }
            std::cerr << "[GUEST][FPUTS]: " << msg;
            return 0; // Success
        });

        // exit (uMei1W9uyNo#T#T)
        RegisterSymbol("libkernel", "uMei1W9uyNo#T#T", [](const GuestArgs& args) -> u64 {
            u32 code = static_cast<u32>(args.arg1);
            LOG_ERROR(Kernel, "Guest requested exit with code: %u", code);
            // Jump back into Kernel::Execute (HLE::ExitGuestProcess longjmps
            // to the armed setjmp buffer); the main thread keeps the final
            // frame visible until the window is closed.
            HLE::ExitGuestProcess(code);
        });

        // sceKernelGetDirectMemorySize (pO96TwzOm5E#S#N)
        // Returns the total size of the physical "direct" memory pool (16GB on PS5)
        RegisterSymbol("libkernel", "pO96TwzOm5E#S#N", [](const GuestArgs& /*args*/) -> u64 {
            constexpr u64 DIRECT_MEM_SIZE = 16384ULL * 1024 * 1024; // 16 GB
            LOG_DEBUG(HLE, "sceKernelGetDirectMemorySize() -> 0x%llx", DIRECT_MEM_SIZE);
            return DIRECT_MEM_SIZE;
        });

        // Physical memory pool state lives at file scope (see EnsurePhysPool /
        // IsPhysPoolAddress / CommitPhysPool above) so the demand-commit fault
        // handler can commit pool pages on first touch.

        // sceKernelAllocateDirectMemory (rTXw65xmLIA#S#N)
        RegisterSymbol("libkernel", "rTXw65xmLIA#S#N", [](const GuestArgs& args) -> u64 {
            u64 search_start = args.arg1;
            u64 search_end   = args.arg2;
            u64 length       = args.arg3;
            u64 alignment    = args.arg4;
            u32 mem_type     = static_cast<u32>(args.arg5);
            guest_addr_t out_ptr = args.arg6;
            (void)search_start; (void)search_end; (void)mem_type;

            LOG_INFO(HLE, "sceKernelAllocateDirectMemory(len: 0x%llx, align: 0x%llx, type: %u, out: 0x%llx)",
                     length, alignment, mem_type, out_ptr);

            if (!out_ptr) return 0x800D0004; // EINVAL

            u64 alloc_size = (length < 0x1000) ? 0x1000 : length;
            if (alignment < 0x1000) alignment = 0x1000;
            // Round up to alignment
            u64 aligned_size = (alloc_size + alignment - 1) & ~(alignment - 1);

            std::lock_guard<std::mutex> lk(g_phys_mutex);
            if (!EnsurePhysPool()) return 0x800D0006;

            // Align the current offset
            u64 phys_offset = (g_phys_pool_offset + alignment - 1) & ~(alignment - 1);
            if (phys_offset + aligned_size > PHYS_POOL_SIZE) {
                LOG_ERROR(HLE, "sceKernelAllocateDirectMemory: out of physical pool space!");
                return 0x800D0006;
            }
            g_phys_pool_offset = phys_offset + aligned_size;
            // Commit-ahead in large chunks so MapDirectMemory / guest first
            // touch never hit the per-64KiB demand-commit fault path.
            if (!EnsurePhysCommitted(phys_offset + aligned_size)) return 0x800D0006;

            // Write the physical OFFSET (not a host address!) back to the game
            Memory::Write<u64>(out_ptr, phys_offset);
            LOG_INFO(HLE, "sceKernelAllocateDirectMemory -> physOffset: 0x%llx (size: 0x%llx)", phys_offset, aligned_size);
            return 0;
        });

        RegisterSymbol("libkernel", "L-Q3LEjIbgA#S#N", SceKernelMapDirectMemory);
        // NOTE: NID 7oxv3PPCumo is sceKernelReserveVirtualRange (verified via
        // the PS5 name->NID SHA1 scheme); it is registered to
        // ReserveVirtualRangeImpl below.  An earlier revision also aliased it
        // to MapDirectMemoryImpl under a bogus "#y#J" tag — removed.
        // Plain-name alias — overwrites the naive Protect-only stub above,
        // which never wrote the mapped VA back to the caller.
        RegisterSymbol("libkernel", "sceKernelMapDirectMemory", SceKernelMapDirectMemory);
        // The "2" variant (extra memoryType argument, alignment on the
        // stack) — see SceKernelMapDirectMemory2 above.
        RegisterSymbol("libkernel", "BQQniolj9tQ", SceKernelMapDirectMemory2);
        RegisterSymbol("libkernel", "sceKernelMapDirectMemory2", SceKernelMapDirectMemory2);

        // sceKernelReserveVirtualRange (7oxv3PPCumo)
        auto ReserveVirtualRangeImpl = [](const GuestArgs& args) -> u64 {
            guest_addr_t addr_ptr = args.arg1; // in/out: VA hint / result
            u64 length            = args.arg2;
            u32 flags             = static_cast<u32>(args.arg3);
            u64 alignment         = args.arg4;
            (void)flags;
            (void)alignment;

            LOG_INFO(HLE, "sceKernelReserveVirtualRange(addr_ptr: 0x%llx, len: 0x%llx, flags: 0x%X, align: 0x%llx)",
                     addr_ptr, length, flags, alignment);

            if (!addr_ptr || !length) return 0x800D0004; // EINVAL

            guest_addr_t hint = Memory::Read<u64>(addr_ptr);
            guest_addr_t out = 0;
            if (Memory::Reserve(hint, length, &out) != Memory::Status::Ok) {
                if (hint == 0) return 0x800D0006; // ENOMEM
                LOG_WARN(HLE, "sceKernelReserveVirtualRange: hint 0x%llx failed; retrying without hint", hint);
                if (Memory::Reserve(0, length, &out) != Memory::Status::Ok) return 0x800D0006;
            }
            Memory::Write<u64>(addr_ptr, out);
            LOG_INFO(HLE, "sceKernelReserveVirtualRange -> va: 0x%llx", out);
            return 0;
        };
        RegisterSymbol("libkernel", "7oxv3PPCumo", ReserveVirtualRangeImpl);
        RegisterSymbol("libkernel", "sceKernelReserveVirtualRange", ReserveVirtualRangeImpl);

        // sceKernelMprotect (vSMAm3cxYTY)
        auto MprotectImpl = [](const GuestArgs& args) -> u64 {
            guest_addr_t addr = args.arg1;
            u64 length        = args.arg2;
            u32 prot          = static_cast<u32>(args.arg3);

            LOG_INFO(HLE, "sceKernelMprotect(addr: 0x%llx, len: 0x%llx, prot: 0x%X)", addr, length, prot);

            if (!addr || !length) return 0x800D0004; // EINVAL
            if (Memory::Protect(addr, length, SanitizeGuestProt(prot)) != Memory::Status::Ok) {
                return 0x800D0006; // ENOMEM
            }
            return 0;
        };
        RegisterSymbol("libkernel", "vSMAm3cxYTY", MprotectImpl);
        RegisterSymbol("libkernel", "sceKernelMprotect", MprotectImpl);

        // __cxa_guard_acquire (3GPpjQdAMTw#T#T)
        // C++ one-time static init guard. Returns 1 if caller must initialize, 0 if already done.
        // Guard layout (64-bit): bits [7:0] = initialized flag (1 = done), bits [15:8] = pending flag.
        RegisterSymbol("libkernel", "3GPpjQdAMTw#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t guard_ptr = args.arg1;
            if (!guard_ptr) return 0;

            // Read current guard state byte
            u8 state = Memory::Read<u8>(guard_ptr);
            if (state & 0x01) {
                // Already initialized
                return 0;
            }

            // Mark as pending (bit 8 = 0x01 in second byte), return 1 to signal caller to initialize
            Memory::Write<u8>(guard_ptr + 1, 0x01);
            LOG_DEBUG(HLE, "__cxa_guard_acquire(guard: 0x%llx) -> 1 (needs init)", guard_ptr);
            return 1;
        });

        // __cxa_guard_release (9rAeANT2tyE#T#T)
        // Marks the guard as initialized after the caller finishes initialization.
        RegisterSymbol("libkernel", "9rAeANT2tyE#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t guard_ptr = args.arg1;
            if (!guard_ptr) return 0;

            // Set initialized flag (byte 0 = 1) and clear pending flag (byte 1 = 0)
            Memory::Write<u8>(guard_ptr, 0x01);
            Memory::Write<u8>(guard_ptr + 1, 0x00);
            LOG_DEBUG(HLE, "__cxa_guard_release(guard: 0x%llx) -> done", guard_ptr);
            return 0;
        });

        // __cxa_guard_abort (nKCFAMmBEgQ#T#T)
        // Called if initialization fails — clears the pending flag.
        RegisterSymbol("libkernel", "nKCFAMmBEgQ#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t guard_ptr = args.arg1;
            if (guard_ptr) Memory::Write<u8>(guard_ptr + 1, 0x00);
            LOG_WARN(HLE, "__cxa_guard_abort(guard: 0x%llx)", guard_ptr);
            return 0;
        });
        // memmove (+P6FRGH4LfA#T#T) / memcpy (Q3VBxCXhUHs#T#T) and their
        // plain-name aliases share these impls.  Games occasionally call
        // them with not-yet-mapped or bogus guest pointers; probing the
        // range first avoids a first-chance AV that the dispatcher's SEH
        // guard would otherwise swallow (and the kernel VEH would log).
        auto MemmoveImpl = [](const GuestArgs& args) -> u64 {
            guest_addr_t dest = args.arg1;
            guest_addr_t src  = args.arg2;
            u64 count         = args.arg3;

            constexpr u64 MAX_MOVE = 256ULL * 1024 * 1024;
            if (count > MAX_MOVE) {
                LOG_WARN(HLE, "libkernel::memmove: count 0x%llx too large, clamping to 0", count);
                count = 0;
            }
            LOG_DEBUG(HLE, "libkernel::memmove(dest: 0x%llx, src: 0x%llx, count: %llu)", dest, src, count);
            if (dest && src && count > 0) {
                if (!Memory::IsWritable(dest, count) || !Memory::IsReadable(src, count)) {
                    LOG_WARN(HLE, "libkernel::memmove: unmapped guest range "
                             "(dest: 0x%llx, src: 0x%llx, count: %llu) — skipped", dest, src, count);
                    return dest;
                }
                std::memmove(reinterpret_cast<void*>(dest), reinterpret_cast<const void*>(src), count);
            }
            return dest;
        };
        RegisterSymbol("libkernel", "+P6FRGH4LfA#T#T", MemmoveImpl);

        // realloc (0E5HFqWCBSA#T#T)
        RegisterSymbol("libkernel", "0E5HFqWCBSA#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t old_ptr = args.arg1;
            u64 new_size         = args.arg2;
            if (new_size == 0) return 0;
            guest_addr_t mem = 0;
            if (Memory::Map(0, (new_size + 0xFFF) & ~0xFFFULL,
                            Memory::PROT_READ | Memory::PROT_WRITE, &mem) != Memory::Status::Ok) {
                return 0;
            }
            LOG_DEBUG(HLE, "libkernel::realloc(ptr: 0x%llx, size: %llu) -> 0x%llx", old_ptr, new_size, mem);
            return mem;
        });

        auto MemcpyImpl = [](const GuestArgs& args) -> u64 {
            guest_addr_t dest = args.arg1;
            guest_addr_t src  = args.arg2;
            u64 count         = args.arg3;

            constexpr u64 MAX_COPY = 256ULL * 1024 * 1024;
            if (count > MAX_COPY) {
                LOG_WARN(HLE, "libkernel::memcpy: count 0x%llx too large, clamping", count);
                count = 0;
            }

            LOG_DEBUG(HLE, "libkernel::memcpy(dest: 0x%llx, src: 0x%llx, count: %llu)", dest, src, count);
            if (dest && src && count > 0) {
                if (!Memory::IsWritable(dest, count) || !Memory::IsReadable(src, count)) {
                    LOG_WARN(HLE, "libkernel::memcpy: unmapped guest range "
                             "(dest: 0x%llx, src: 0x%llx, count: %llu) — skipped", dest, src, count);
                    return dest;
                }
                std::memmove(reinterpret_cast<void*>(dest), reinterpret_cast<const void*>(src), count);
            }
            return dest;
        };
        RegisterSymbol("libkernel", "Q3VBxCXhUHs#T#T", MemcpyImpl);

        // malloc (gQX+4GDQjpM#T#T)
        RegisterSymbol("libkernel", "gQX+4GDQjpM#T#T", [](const GuestArgs& args) -> u64 {
            u64 size = args.arg1;
            if (size == 0) size = 1;

            // Use Windows heap for guest allocations, then map them into guest space
            guest_addr_t mem = 0;
            if (Memory::Map(0, (size + 0xFFF) & ~0xFFFULL,
                            Memory::PROT_READ | Memory::PROT_WRITE, &mem) != Memory::Status::Ok) {
                return 0;
            }
            LOG_DEBUG(HLE, "libkernel::malloc(size: %llu) -> 0x%llx", size, mem);
            return mem;
        });

        // _Getptolower (1uJgoVq3bQU#T#T) — Dinkum CRT internal.
        // Returns a pointer to the persistent tolower conversion table
        // (one u16 per char, indexed directly as table[c]).  Games link
        // against it via libSceLibcInternal; returning null here caused
        // the PPSA02929 boot crash (movzx byte [rax+r12*2], RAX=0).
        // The table is built exactly once: the previous lazy check let a
        // concurrent caller observe table_addr set before the table was
        // filled, which returned a zeroed table and corrupted ctype
        // conversion on worker threads (JSON parse errors, null variants).
        RegisterSymbol("libkernel", "1uJgoVq3bQU#T#T", [](const GuestArgs& args) -> u64 {
            (void)args;
            static guest_addr_t table_addr = 0;
            static std::once_flag table_once;
            std::call_once(table_once, [] {
                if (Memory::Map(0, 0x1000,
                                Memory::PROT_READ | Memory::PROT_WRITE,
                                &table_addr) != Memory::Status::Ok) {
                    LOG_ERROR(HLE, "_Getptolower: failed to map tolower table");
                    return;
                }
                for (u32 i = 0; i < 256; ++i) {
                    const u16 v = (i >= 'A' && i <= 'Z') ? static_cast<u16>(i + 32)
                                                         : static_cast<u16>(i);
                    Memory::Write<u16>(table_addr + i * 2, v);
                }
            });
            LOG_INFO(HLE, "_Getptolower() -> 0x%llx", table_addr);
            return table_addr;
        });

        // free (tIhsqj0qsFE#T#T)
        RegisterSymbol("libkernel", "tIhsqj0qsFE#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t ptr = args.arg1;
            if (ptr) {
                LOG_DEBUG(HLE, "libkernel::free(ptr: 0x%llx) - stub (no-op)", ptr);
                // In a full impl we'd track allocation sizes and VirtualFree here.
                // For now, leak: VirtualFree needs the exact base & size.
            }
            return 0;
        });

        // calloc (2X5agFjKxMc#T#T) — malloc + zero-fill
        RegisterSymbol("libkernel", "2X5agFjKxMc#T#T", [](const GuestArgs& args) -> u64 {
            u64 nmemb = args.arg1;
            u64 size  = args.arg2;
            u64 total = nmemb * size;
            if (total == 0) total = 1;

            guest_addr_t mem = 0;
            if (Memory::Map(0, (total + 0xFFF) & ~0xFFFULL,
                            Memory::PROT_READ | Memory::PROT_WRITE, &mem) != Memory::Status::Ok) {
                return 0;
            }
            // Memory::Map already commits zeroed pages on Windows
            LOG_DEBUG(HLE, "libkernel::calloc(nmemb: %llu, size: %llu) -> 0x%llx", nmemb, size, mem);
            return mem;
        });

        // =====================================================================
        // POSIX-like file I/O (C stdio)
        // =====================================================================
        // fopen (xeYO4u7uyJ0#T#T)
        RegisterSymbol("libkernel", "xeYO4u7uyJ0#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t path_ptr = args.arg1;
            guest_addr_t mode_ptr = args.arg2;
            std::string path, mode;
            for (u64 i = 0; ; ++i) { u8 c = Memory::Read<u8>(path_ptr + i); if (!c) break; path += (char)c; }
            for (u64 i = 0; ; ++i) { u8 c = Memory::Read<u8>(mode_ptr + i); if (!c) break; mode += (char)c; }
            FILE* f = fopen(Kernel::TranslateGuestPath(path).c_str(), mode.c_str());
            LOG_INFO(HLE, "libkernel::fopen('%s', '%s') -> %p", path.c_str(), mode.c_str(), f);
            return reinterpret_cast<u64>(f);
        });

        // fclose (uodLYyUip20#T#T)
        RegisterSymbol("libkernel", "uodLYyUip20#T#T", [](const GuestArgs& args) -> u64 {
            FILE* f = reinterpret_cast<FILE*>(args.arg1);
            int r = f ? fclose(f) : -1;
            LOG_DEBUG(HLE, "libkernel::fclose(%p) -> %d", f, r);
            return (u64)(s64)r;
        });

        // fread (lbB+UlZqVG0#T#T)
        RegisterSymbol("libkernel", "lbB+UlZqVG0#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t buf = args.arg1;
            u64 size  = args.arg2;
            u64 count = args.arg3;
            FILE* f   = reinterpret_cast<FILE*>(args.arg4);
            if (!f || !buf) return 0;
            u64 n = fread(reinterpret_cast<void*>(buf), size, count, f);
            LOG_DEBUG(HLE, "libkernel::fread(buf: 0x%llx, size: %llu, count: %llu) -> %llu", buf, size, count, n);
            return n;
        });

        // fseek (rQFVBXp-Cxg#T#T)
        RegisterSymbol("libkernel", "rQFVBXp-Cxg#T#T", [](const GuestArgs& args) -> u64 {
            FILE* f    = reinterpret_cast<FILE*>(args.arg1);
            s64 offset = static_cast<s64>(args.arg2);
            int whence = static_cast<int>(args.arg3);
            int r = f ? fseek(f, (long)offset, whence) : -1;
            LOG_DEBUG(HLE, "libkernel::fseek(%p, %lld, %d) -> %d", f, offset, whence, r);
            return (u64)(s64)r;
        });

        // ftell (Qazy8LmXTvw#T#T)
        RegisterSymbol("libkernel", "Qazy8LmXTvw#T#T", [](const GuestArgs& args) -> u64 {
            FILE* f = reinterpret_cast<FILE*>(args.arg1);
            long r = f ? ftell(f) : -1;
            LOG_DEBUG(HLE, "libkernel::ftell(%p) -> %ld", f, r);
            return (u64)(s64)r;
        });

        // fwrite (unresolved, common enough to pre-register)
        RegisterSymbol("libkernel", "fwrite#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t buf = args.arg1;
            u64 size  = args.arg2;
            u64 count = args.arg3;
            FILE* f   = reinterpret_cast<FILE*>(args.arg4);
            if (!f || !buf) return 0;
            return fwrite(reinterpret_cast<const void*>(buf), size, count, f);
        });

        // printf (hcuQgD53UxM#T#T) — just log to stderr
        RegisterSymbol("libkernel", "hcuQgD53UxM#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t fmt_ptr = args.arg1;
            if (!fmt_ptr) return 0;
            std::string fmt;
            for (u64 i = 0; i < 512; ++i) { u8 c = Memory::Read<u8>(fmt_ptr + i); if (!c) break; fmt += (char)c; }
            std::cerr << "[GUEST][PRINTF]: " << fmt;
            return (u64)fmt.size();
        });

        // puts (SfQIZcqvvms stub — unknown, may be puts)
        RegisterSymbol("libkernel", "SfQIZcqvvms#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t str_ptr = args.arg1;
            if (str_ptr) {
                std::string s;
                for (u64 i = 0; i < 1024; ++i) { u8 c = Memory::Read<u8>(str_ptr + i); if (!c) break; s += (char)c; }
                std::cerr << "[GUEST][PUTS]: " << s << "\n";
            }
            return 0;
        });

        // =====================================================================
        // Kernel file operations (sceKernel* wrappers over POSIX fd)
        // Implementations live at file scope (see above): the raw sceKernel*
        // NIDs return Orbis SCE_KERNEL_ERROR_E* codes on failure, while the
        // POSIX-named exports translate failures to the libc -1/errno ABI.
        // =====================================================================
        // sceKernelOpen (1G3lF1Gg1k8#S#N)
        RegisterSymbol("libkernel", "1G3lF1Gg1k8#S#N", [](const GuestArgs& args) -> u64 {
            return static_cast<u64>(KernelOpenCore(args.arg1, static_cast<int>(args.arg2), static_cast<int>(args.arg3)));
        });

        // sceKernelRead (Cg4srZ6TKbU#S#N)
        RegisterSymbol("libkernel", "Cg4srZ6TKbU#S#N", [](const GuestArgs& args) -> u64 {
            return static_cast<u64>(KernelReadCore(static_cast<int>(args.arg1), args.arg2, args.arg3));
        });

        // sceKernelWrite — plain-name alias of the POSIX write NID below.
        RegisterSymbol("libkernel", "sceKernelWrite", [](const GuestArgs& args) -> u64 {
            return static_cast<u64>(KernelWriteCore(static_cast<int>(args.arg1), args.arg2, args.arg3));
        });

        // sceKernelClose (UK2Tl2DWUns#S#N)
        RegisterSymbol("libkernel", "UK2Tl2DWUns#S#N", [](const GuestArgs& args) -> u64 {
            return static_cast<u64>(KernelCloseCore(static_cast<int>(args.arg1)));
        });

        // sceKernelLseek (oib76F-12fk#S#N)
        RegisterSymbol("libkernel", "oib76F-12fk#S#N", [](const GuestArgs& args) -> u64 {
            int fd     = static_cast<int>(args.arg1);
            s64 offset = static_cast<s64>(args.arg2);
            int whence = static_cast<int>(args.arg3);
            s64 r = _lseeki64(fd, offset, whence);
            LOG_DEBUG(HLE, "sceKernelLseek(fd=%d, off=%lld, whence=%d) -> %lld", fd, offset, whence, r);
            return (u64)r;
        });

        // sceKernelStat (eV9wAD2riIA#S#N)
        RegisterSymbol("libkernel", "eV9wAD2riIA#S#N", [](const GuestArgs& args) -> u64 {
            return static_cast<u64>(KernelStatCore(args.arg1, args.arg2));
        });

        // sceKernelFstat — plain-name alias of the POSIX fstat NID below.
        RegisterSymbol("libkernel", "sceKernelFstat", [](const GuestArgs& args) -> u64 {
            return static_cast<u64>(KernelFstatCore(static_cast<int>(args.arg1), args.arg2));
        });

        // POSIX-named exports (libc ABI: -1 with errno set on failure).
        // Registered under both the friendly name and the bare NID; the
        // resolver bridges tagged NID requests (e.g. "wuCroIGjt2g#T#T") to
        // either form.  fstat is a libc export in the NID database.
        RegisterSymbol("libkernel", "open", PosixOpen);
        RegisterSymbol("libkernel", "wuCroIGjt2g", PosixOpen);
        RegisterSymbol("libkernel", "close", PosixClose);
        RegisterSymbol("libkernel", "bY-PO6JhzhQ", PosixClose);
        RegisterSymbol("libkernel", "read", PosixRead);
        RegisterSymbol("libkernel", "AqBioC2vF3I", PosixRead);
        RegisterSymbol("libkernel", "write", PosixWrite);
        RegisterSymbol("libkernel", "FN4gaPmuFV8", PosixWrite);
        RegisterSymbol("libkernel", "stat", PosixStat);
        RegisterSymbol("libkernel", "E6ao34wPw+U", PosixStat);
        RegisterSymbol("libkernel", "fstat", PosixFstat);
        RegisterSymbol("libkernel", "mqQMh1zPPT8", PosixFstat);
        RegisterSymbol("libc", "fstat", PosixFstat);
        RegisterSymbol("libc", "mqQMh1zPPT8", PosixFstat);

        // sceKernelMunmap (cQke9UuBQOk#S#N)
        RegisterSymbol("libkernel", "cQke9UuBQOk#S#N", [](const GuestArgs& args) -> u64 {
            guest_addr_t addr = args.arg1;
            u64 size = args.arg2;
            LOG_DEBUG(HLE, "sceKernelMunmap(addr: 0x%llx, size: 0x%llx) -> stub", addr, size);
            // No-op: we don't track allocation sizes, so we can't VirtualFree safely
            return 0;
        });

        // sceKernelVirtualQuery (rVjRvHJ0X6c#S#N)
        // Real signature: sceKernelVirtualQuery(const void* addr, int flags,
        //                                       SceKernelVirtualQueryInfo* info,
        //                                       size_t infoSize)
        // (An earlier version of this handler treated arg2 as `info`, missing
        // the flags parameter: LOST EPIC passes flags=0, got EINVAL back, and
        // then crashed dereferencing the never-written info struct.)
        RegisterSymbol("libkernel", "rVjRvHJ0X6c#S#N", [](const GuestArgs& args) -> u64 {
            guest_addr_t addr = args.arg1;
            int flags = static_cast<int>(args.arg2);
            guest_addr_t info_ptr = args.arg3;
            u64 info_size = args.arg4;
            LOG_INFO(HLE, "sceKernelVirtualQuery(addr: 0x%llx, flags: %d, info: 0x%llx, size: %llu)",
                     addr, flags, info_ptr, info_size);

            if (!info_ptr || info_size < 16) {
                return 0x80020016; // EINVAL
            }

            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) == 0) {
                LOG_ERROR(HLE, "sceKernelVirtualQuery: VirtualQuery failed (err=%lu)", GetLastError());
                return 0x80020005; // EFAULT
            }

            // Write start address
            Memory::Write<u64>(info_ptr + 0, reinterpret_cast<u64>(mbi.BaseAddress));
            // Write end address
            Memory::Write<u64>(info_ptr + 8, reinterpret_cast<u64>(mbi.BaseAddress) + mbi.RegionSize);

            // Convert Windows protection to Unix prot
            u32 prot = 0;
            if (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) {
                prot |= 1; // PROT_READ
            }
            if (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) {
                prot |= 2; // PROT_WRITE
            }
            if (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) {
                prot |= 4; // PROT_EXEC
            }

            if (info_size >= 20) {
                Memory::Write<u32>(info_ptr + 16, prot);
            }
            if (info_size >= 24) {
                Memory::Write<u32>(info_ptr + 20, mbi.State);
            }

            return 0; // Success
        });

        // =====================================================================
        // pthread stubs (single-threaded model — all calls succeed trivially)
        // =====================================================================
        // scePthreadSelf (aI+OeCz8xrQ#T#T) — current thread ID from the CpuCore registry
        RegisterSymbol("libkernel", "aI+OeCz8xrQ#T#T", [](const GuestArgs& /*args*/) -> u64 {
            u64 tid = Kernel::GetCurrentThreadId();
            LOG_DEBUG(HLE, "scePthreadSelf() -> 0x%llx", tid);
            return tid;
        });

        // scePthreadYield (T72hz6ffq08#T#T) — no-op in single-threaded mode
        RegisterSymbol("libkernel", "T72hz6ffq08#T#T", [](const GuestArgs& /*args*/) -> u64 {
            return 0;
        });

        // scePthreadCreate (6UgtwV+0zb4#T#T) — spawns a guest thread via the
        // CpuCore registry (reached through the Kernel:: thread API).
        RegisterSymbol("libkernel", "6UgtwV+0zb4#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t tid_out   = args.arg1;
            guest_addr_t attr_ptr  = args.arg2;
            guest_addr_t entry_ptr = args.arg3;
            guest_addr_t start_arg = args.arg4;
            guest_addr_t name_ptr  = args.arg5;
            (void)attr_ptr;

            std::string name = "<unnamed>";
            if (name_ptr) {
                for (u64 i = 0; i < 128; ++i) {
                    u8 c = Memory::Read<u8>(name_ptr + i);
                    if (!c) break;
                    name += static_cast<char>(c);
                }
            }

            constexpr u64 kGuestStackSize = 1024 * 1024;
            void* guest_stack = VirtualAlloc(nullptr, kGuestStackSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!guest_stack) {
                LOG_ERROR(HLE, "scePthreadCreate(NID): VirtualAlloc failed for guest stack");
                return 11; // EAGAIN
            }
            u64 stack_base = reinterpret_cast<u64>(guest_stack);

            // Orbis thread-pointer layout: fs:[0] yields the tp (self-pointer
            // stored at tp) and libc/CRT data lives at NEGATIVE offsets from
            // tp (seen at least down to tp-0x1648).  Mirror the main thread's
            // block (Kernel::Initialize), which leaves 0x10000 below the tp.
            constexpr u64 kTlsHeadroom = 0x10000;
            constexpr u64 kTlsSize = 0x4000;
            void* tls_block = VirtualAlloc(nullptr, kTlsHeadroom + kTlsSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            u64 tls_base = reinterpret_cast<u64>(tls_block) + kTlsHeadroom;
            if (tls_block) {
                *reinterpret_cast<u64*>(tls_base) = tls_base;
            }

            u64 tid = 0;
            HANDLE handle = Kernel::CreateThreadEx(entry_ptr, stack_base, kGuestStackSize,
                                                   tls_base, start_arg, &tid);
            if (!handle) {
                LOG_ERROR(HLE, "scePthreadCreate(NID): CreateThreadEx failed (err=%lu)", GetLastError());
                VirtualFree(guest_stack, 0, MEM_RELEASE);
                VirtualFree(tls_block, 0, MEM_RELEASE);
                return 11; // EAGAIN
            }

            if (tid_out) Memory::Write<u64>(tid_out, tid);
            LOG_INFO(HLE, "scePthreadCreate(NID)(entry=0x%llx, arg=0x%llx, name='%s') -> tid=%llu",
                     entry_ptr, start_arg, name.c_str(), tid);
            return 0;
        });

        // scePthreadJoin (onNY9Byn-W8#S#N) — waits on the CpuCore-registered thread
        RegisterSymbol("libkernel", "onNY9Byn-W8#S#N", [](const GuestArgs& args) -> u64 {
            u64 tid = args.arg1;
            guest_addr_t value_ptr = args.arg2;

            u64 exit_code = 0;
            if (!CpuCore::JoinThread(tid, &exit_code)) {
                LOG_ERROR(HLE, "scePthreadJoin(NID): thread %llu not found or not joinable", tid);
                return 3; // ESRCH
            }
            if (value_ptr) Memory::Write<u64>(value_ptr, exit_code);

            LOG_INFO(HLE, "scePthreadJoin(NID)(tid=%llu) -> exit_code=%llu", tid, exit_code);
            return 0;
        });

        // scePthreadMutex/Cond init/lock/unlock/destroy and the mutex-attr NIDs
        // hit by PPSA01668 (F8bUHwAG284, iMp8QpE+XO4, 1FGvU0i9saQ, cmo1RIYva9o,
        // smWEktiyyG0, 188x57JYp0g) resolve via the NID-database friendly-name
        // bridge to the real implementations in src/hle/libkernel_sync.cpp.

        // =====================================================================
        // String utilities
        // =====================================================================
        // strncpy (6sJWiWSRuqk#T#T)
        RegisterSymbol("libkernel", "6sJWiWSRuqk#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t dst = args.arg1, src = args.arg2;
            u64 n = args.arg3;
            if (dst && src && n > 0 && n < 0x10000000ULL)
                strncpy(reinterpret_cast<char*>(dst), reinterpret_cast<const char*>(src), n);
            return dst;
        });

        // strcpy
        auto StrcpyImpl = [](const GuestArgs& args) -> u64 {
            guest_addr_t dst = args.arg1, src = args.arg2;
            if (dst && src) strcpy(reinterpret_cast<char*>(dst), reinterpret_cast<const char*>(src));
            return dst;
        };
        RegisterSymbol("libkernel", "strcpy#T#T", StrcpyImpl);
        RegisterSymbol("libkernel", "kiZSXIWd9vg#T#T", StrcpyImpl); // libc strcpy NID

        // strcat (Ls4tzzhimqQ#T#T)
        auto StrcatImpl = [](const GuestArgs& args) -> u64 {
            guest_addr_t dst = args.arg1, src = args.arg2;
            if (dst && src) strcat(reinterpret_cast<char*>(dst), reinterpret_cast<const char*>(src));
            return dst;
        };
        RegisterSymbol("libkernel", "strcat#T#T", StrcatImpl);
        RegisterSymbol("libkernel", "Ls4tzzhimqQ#T#T", StrcatImpl);

        // strlen — plain-name alias of j4ViWNHEgww#T#T
        RegisterSymbol("libkernel", "strlen#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t str = args.arg1;
            if (!str) return 0;
            return strlen(reinterpret_cast<const char*>(str));
        });

        // memcpy — plain-name alias of Q3VBxCXhUHs#T#T
        RegisterSymbol("libkernel", "memcpy#T#T", MemcpyImpl);

        // memmove — plain-name alias of +P6FRGH4LfA#T#T
        RegisterSymbol("libkernel", "memmove#T#T", MemmoveImpl);

        // memset — plain-name alias of 8zTFvBIAIN8#T#T
        RegisterSymbol("libkernel", "memset#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t dst = args.arg1;
            u32 ch = static_cast<u32>(args.arg2);
            u64 n = args.arg3;
            if (dst && n > 0 && n < 0x10000000ULL) {
                if (!Memory::IsWritable(dst, n)) {
                    LOG_WARN(HLE, "libkernel::memset: unmapped guest range "
                             "(dest: 0x%llx, count: %llu) — skipped", dst, n);
                    return dst;
                }
                std::memset(reinterpret_cast<void*>(dst), static_cast<int>(ch & 0xFF), n);
            }
            return dst;
        });

        // strcmp
        RegisterSymbol("libkernel", "strcmp#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t a = args.arg1, b = args.arg2;
            if (!a || !b) return (u64)(s64)-1;
            int cmp = strcmp(reinterpret_cast<const char*>(a), reinterpret_cast<const char*>(b));
            return (u64)(s64)cmp;
        });

        // strncmp (aesyjrHVWy4#T#T)
        auto StrncmpImpl = [](const GuestArgs& args) -> u64 {
            guest_addr_t a = args.arg1, b = args.arg2;
            u64 n = args.arg3;
            if (!a || !b || n == 0 || n > 0x10000000ULL) return (u64)(s64)-1;
            int cmp = strncmp(reinterpret_cast<const char*>(a), reinterpret_cast<const char*>(b), n);
            return (u64)(s64)cmp;
        };
        RegisterSymbol("libkernel", "strncmp#T#T", StrncmpImpl);
        RegisterSymbol("libkernel", "aesyjrHVWy4#T#T", StrncmpImpl);

        // AV6ipCNa4Rw = strcasecmp
        RegisterSymbol("libkernel", "AV6ipCNa4Rw#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t a = args.arg1, b = args.arg2;
            if (!a || !b) return (u64)(s64)-1;
            int cmp = _stricmp(reinterpret_cast<const char*>(a), reinterpret_cast<const char*>(b));
            return (u64)(s64)cmp;
        });

        // sprintf (g7zzzLDYGw0#T#T) — real implementation (SysV varargs via
        // dispatcher-captured registers + guest stack overflow args).
        RegisterSymbol("libkernel", "g7zzzLDYGw0#T#T", [](const GuestArgs& args) -> u64 {
            return GuestSprintf(args);
        });

        // =====================================================================
        // Semaphore symbols (sceKernelCreate/Wait/Signal/Poll/DeleteSema) are
        // real implementations in src/hle/libkernel_sync.cpp.
        // =====================================================================

        // =====================================================================
        // Misc stubs — return success/0 for unresolved PS5-specific functions
        // These will be updated as we understand them better
        // =====================================================================
        // Unknown PS5-specific NIDs (return 0 to allow game to continue)
        for (const char* nid : {
            "Q4rRL34CEeE#T#T", "pztV4AF18iI#T#T", "8zsu04XNsZ4#T#T",
            "YQ0navp+YIc#T#T", "weDug8QD-lE#T#T",
            "RQXLbdT2lc4#T#T", "-P6FNMzk2Kc#T#T",
            // Note: cond/mutex-attr and cond NIDs (m5-2bsNfv7s, 2Tb92quprl0,
            // waPcxYiR3WA, g+PZd2hiacg) moved to real implementations in
            // src/hle/libkernel_sync.cpp — do not re-add them here.
            "4tPhsP6FpDI#H#I",
            "4wSze92BhLI#S#N",
            "-Wreprtu0Qs#S#N", "DzES9hQF4f4#S#N", "x1X76arYMxU#S#N",
            "-quPa4SEJUw#S#N", "AUXVxWeJU-A#S#N",
            "MBuItvba6z8#S#N", "hT0IAEvN+M0#E#F", "5zBnau1uIEo#E#F",
            "tpFJ8LIKvPw#E#F", "AUIHb7jUX3I#E#F", "sUXGfNMalIo#F#G",
            "Bagshr7OQ6Q#F#G", "p+GcLqwpL9M#E#F", "YE4dbtbz6OE#E#F",
            "CzkKf7ahIyU#E#F", "wG+84pnNIuo#E#F", "MfDb+4Nln64#E#F",
            "Wxbg5x3pTXA#E#F", "4llLk7YJRTE#E#F", "3kg7rT0NQIs#S#N",
            "85zul--eGXs#G#H",
            "c88Yy54Mx0w#G#H", "xk0AcarP3V4#L#M",
            "clVvL4ZDntw#L#M", "gjP9-KQzoUk#L#M", "YndgXqQVV7c#L#M",
            "rPo6tV8D9bM#Q#R", "656LMQSrg6U#Q#R", "jEIXUAr9XE8#R#S",
            "rPl0INNc-M8#R#S", "hZIg1EWGsHM#P#Q", "Vo5V8KAwCmk#Q#R",
            "hv1luiJrqQM#L#M",
            "fMP5NHUOaMk#D#E", "yKDy8S5yLA0#G#H",
            "6ncge5+l5Qs#L#M", "bwFjS+bX9mA#U#U", "eR2bZFAAU0Q#D#E",
            "d-kSG2fLrvI#P#Q"
            // The libkernel audio aliases (ekNvsT22rsY/b+uAV89IlxE/
            // QOQtbeDqsT4/s1--uE9mBFw with #N#O) are NOT stubbed here:
            // libaudioout.cpp binds them to the real paced handlers —
            // Dreaming Sarah's audio thread busy-floods 64KB direct-memory
            // allocations when sceAudioOutOutput returns instantly.
        }) {
            RegisterSymbol("libkernel", nid, [nid](const GuestArgs& /*a*/) -> u64 {
                LogStubCallOnce("libkernel", nid);
                return 0;
            });
        }
    }
}
// namespace HLE
