#define _CRT_SECURE_NO_WARNINGS
#include "hle.h"
#include "libkernel_file.h"
#include "guest_printf.h"
#include "../kernel/kernel.h"
#include "../kernel/thread.h"
#include "../kernel/guest_clock.h"
#include "../cpu/cpu.h"
#include "../config/config.h"
#include "../memory/memory.h"
#include "../common/log.h"
#include "../gpu/gpu.h"
#include "../loader/elf.h"
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <windows.h>
#include <cstring>
#include <chrono>
#include <thread>
#include <iostream>
#include <string>
#include <unordered_set>
#include <mutex>
#include <io.h>
#include <fcntl.h>
#include <cstdio>
#include <random>
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
        constexpr u64  PHYS_POOL_SIZE = 16ULL * 1024 * 1024 * 1024; // 16 GB pool
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
            // Register with the guest VA authority so Query/IsValidGuestPointer
            // see the pool (reserved-only; commits stay HLE-managed chunks).
            Memory::AdoptRange(g_phys_pool_base, PHYS_POOL_SIZE, Memory::PROT_NONE,
                               /*committed=*/false, Memory::Owner::Hle, "phys-pool");
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

    void ResetPhysPool() {
        guest_addr_t base = 0;
        {
            std::lock_guard<std::mutex> lk(g_phys_mutex);
            base = g_phys_pool_base;
            g_phys_pool_base = 0;
            g_phys_pool_offset = 0x10000;
            g_phys_pool_committed = 0;
        }
        if (base) {
            Memory::ForgetResource(base);
            if (!VirtualFree(reinterpret_cast<void*>(base), 0, MEM_RELEASE)) {
                LOG_WARN(HLE, "ResetPhysPool: VirtualFree failed at 0x%llx (err=%lu)",
                         base, GetLastError());
            } else {
                LOG_INFO(HLE, "PhysPool: released 2 GB pool at 0x%llx", base);
            }
        }
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
        // Track guest-visible mappings with the VA authority (guest-owned:
        // the game chose/accepted this address).  Ranges that came from
        // Memory::Reserve/Memory::Map are already tracked correctly — only
        // the raw VirtualAlloc hint path and phys-pool commits need adoption,
        // and pool interiors are covered by the pool's adopted reservation.
        if (!IsPhysPoolAddress(mapped_va) &&
            Memory::QueryOwner(mapped_va) == Memory::Owner::None) {
            const u32 mprot = (rwx & 4 ? Memory::PROT_EXEC : 0) |
                              (rwx & 2 ? Memory::PROT_WRITE : 0) |
                              (rwx & 1 ? Memory::PROT_READ : 0);
            Memory::AdoptRange(mapped_va, rounded, mprot,
                               /*committed=*/true, Memory::Owner::Guest,
                               "map-direct-hint");
        } else if (IsPhysPoolAddress(mapped_va)) {
            // Phys-backed mapping: refresh the committed state on the pool's
            // adopted record so Query reflects live commits.  The pool is one
            // region; only flip to committed once any part is committed.
            Memory::AdoptRange(g_phys_pool_base, PHYS_POOL_SIZE, Memory::PROT_NONE,
                               /*committed=*/false, Memory::Owner::Hle,
                               "phys-pool");
        }
        Memory::Write<u64>(addr_ptr, mapped_va);
        LOG_INFO(HLE, "sceKernelMapDirectMemory -> va: 0x%llx", mapped_va);
        return 0;
    }

    u64 SceKernelAllocateDirectMemory(const GuestArgs& args) {
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
        u64 aligned_size = (alloc_size + alignment - 1) & ~(alignment - 1);

        std::lock_guard<std::mutex> lk(g_phys_mutex);
        if (!EnsurePhysPool()) return 0x800D0006;

        u64 phys_offset = (g_phys_pool_offset + alignment - 1) & ~(alignment - 1);
        if (phys_offset + aligned_size > PHYS_POOL_SIZE) {
            LOG_ERROR(HLE, "sceKernelAllocateDirectMemory: out of physical pool space!");
            return 0x800D0006;
        }
        g_phys_pool_offset = phys_offset + aligned_size;
        if (!EnsurePhysCommitted(phys_offset + aligned_size)) return 0x800D0006;

        Memory::Write<u64>(out_ptr, phys_offset);
        LOG_INFO(HLE, "sceKernelAllocateDirectMemory -> physOffset: 0x%llx (size: 0x%llx)", phys_offset, aligned_size);
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

        // Orbis struct stat layout (FreeBSD 11 based, matches src/kernel/syscalls.cpp SysStat).
        struct OrbisStat {
            u32 st_dev;     // 0
            u32 st_ino;     // 4
            u16 st_mode;    // 8
            u16 st_nlink;   // 10
            u32 st_uid;     // 12
            u32 st_gid;     // 16
            u32 st_rdev;    // 20
            s64 st_atime;   // 24
            s64 st_atimensec; // 32
            s64 st_mtime;   // 40
            s64 st_mtimensec; // 48
            s64 st_ctime;   // 56
            s64 st_ctimensec; // 64
            s64 st_size;    // 72
            s64 st_blocks;  // 80
            u32 st_blksize; // 88
            u32 st_flags;   // 92
            u32 st_gen;     // 96
            s32 st_lspare;  // 100
            s64 st_birthtime; // 104
            s64 st_birthtimensec; // 112
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
            // Mirror guest stdout/stderr into the log: games report their own
            // fatal errors (il2cpp exceptions, assert text) on fd 1/2 before
            // crashing — capture that verbatim so it survives the crash.
            if (fd >= 0 && fd <= 2 && Memory::IsReadable(buf, 1)) {
                const u64 cap = count < 512 ? count : 512;
                std::string text(static_cast<size_t>(cap), '\0');
                for (u64 i = 0; i < cap; ++i) {
                    u8 c = Memory::Read<u8>(buf + i);
                    if ((c < 0x20 || c > 0x7E) && c != '\n' && c != '\t') c = '?';
                    text[static_cast<size_t>(i)] = static_cast<char>(c);
                }
                LOG_INFO(HLE, "GUEST-STDOUT[%d]: %s", fd, text.c_str());
            }
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
            LOG_ERROR(HLE, "sceKernelFstat(fd=%d) -> 0 (size=%lld)", fd, static_cast<s64>(hs.st_size));
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
            LOG_ERROR(HLE, "sceKernelStat('%s') -> 0 (size=%lld)", path.c_str(), static_cast<s64>(hs.st_size));
            return 0;
        }
    } // namespace

    // -----------------------------------------------------------------------
    // Fake FILE structs for the game's native libc.prx stdio.
    //
    // The game's real libc.prx implements fread/fseek/ftell/fclose NATIVELY
    // against its own FILE struct layout (fd at +4, flags at +0, buffer at
    // +8/+0x10/+0x18/+0x20, ungetc area at +0x7e).  Handing it a real MSVC
    // FILE* breaks every read (fd is read from +4 where MSVC stores part of
    // _ptr), which is exactly why the C2 runtime's data.js chunk reader died
    // mid-string ("image" + EOF).  We return a FAKE FILE struct in libc.prx's
    // layout, backed by a real CRT fd; libc.prx's internals then call the
    // _read/_close/lseek NIDs registered below, which route through our
    // kernel fd layer.
    // -----------------------------------------------------------------------

    // Fake FILE layout, matching libc.prx's fopen init (0x820057f90):
    //   +0x00 u16 flags        (1 = readable)
    //   +0x02 u8  mode char
    //   +0x04 s32 fd
    //   +0x08 qword buffer base
    //   +0x10 qword buffer end limit
    //   +0x18 qword current read position
    //   +0x20 qword buffer fill end
    //   +0x28 qword (same as base)
    //   +0x30 qword (same as base; ungetc ptr)
    //   +0x38 qword ptr to +0x44 (ungetc struct)
    //   +0x44 .. 0x7d  ungetc storage
    //   +0x7e .. 0x7f  ungetc buffer (initial "buffer")
    //   +0x80 qword user buffer (0)
    constexpr u64 kFakeFileSize = 0x100;

    std::mutex g_fake_file_mutex;
    std::unordered_map<u64, int> g_fake_file_fds; // fake FILE* -> fd

    u64 CreateFakeFile(int fd, char mode) {
        guest_addr_t mem = 0;
        if (Memory::Map(0, kFakeFileSize,
                        Memory::PROT_READ | Memory::PROT_WRITE, &mem) != Memory::Status::Ok) {
            return 0;
        }
        u8* f = reinterpret_cast<u8*>(mem);
        std::memset(f, 0, kFakeFileSize);
        *reinterpret_cast<u16*>(f + 0x00) = 1;           // readable
        f[0x02] = static_cast<u8>(mode);
        *reinterpret_cast<s32*>(f + 0x04) = fd;
        const u64 self = mem;
        *reinterpret_cast<u64*>(f + 0x08) = self + 0x7e; // base = ungetc
        *reinterpret_cast<u64*>(f + 0x10) = self + 0x7f; // limit
        *reinterpret_cast<u64*>(f + 0x18) = self + 0x7e; // pos
        *reinterpret_cast<u64*>(f + 0x20) = self + 0x7e; // end
        *reinterpret_cast<u64*>(f + 0x28) = self + 0x7e;
        *reinterpret_cast<u64*>(f + 0x30) = self + 0x7e;
        *reinterpret_cast<u64*>(f + 0x38) = self + 0x44;
        *reinterpret_cast<u64*>(f + 0x50) = self + 0x7e;
        *reinterpret_cast<u64*>(f + 0x58) = self + 0x7e;
        std::lock_guard<std::mutex> lk(g_fake_file_mutex);
        g_fake_file_fds[mem] = fd;
        return mem;
    }

    void DestroyFakeFile(u64 f) {
        if (!f) return;
        int fd = -1;
        {
            std::lock_guard<std::mutex> lk(g_fake_file_mutex);
            auto it = g_fake_file_fds.find(f);
            if (it != g_fake_file_fds.end()) {
                fd = it->second;
                g_fake_file_fds.erase(it);
            }
        }
        if (fd >= 0) _close(fd);
        Memory::Unmap(f, kFakeFileSize);
    }

    bool IsFakeFile(u64 f) {
        std::lock_guard<std::mutex> lk(g_fake_file_mutex);
        return g_fake_file_fds.count(f) != 0;
    }

    int FakeFileFd(u64 f) {
        std::lock_guard<std::mutex> lk(g_fake_file_mutex);
        auto it = g_fake_file_fds.find(f);
        return it != g_fake_file_fds.end() ? it->second : -1;
    }

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
        // Called by _start after TLS setup and DT_INIT (or via dynamic linker):
        //   XKRegsFpEpk(argc, argv, envp)  -> should NEVER return to _start
        // We find the game's main() via HLE::GetGuestMainAddress() (set at load time
        // by scanning the symbol table for "main"), then call it via InvokeGuestFunction.
        // =====================================================================
        RegisterSymbol("libkernel", "XKRegsFpEpk#T#T", [](const GuestArgs& args) -> u64 {
            // XKRegsFpEpk is the PS5's __libc_start_main.  It is called from
            // _start with (argc, argv, envp) and must:
            //   1. Call main(argc, argv, envp)
            //   2. Call exit() with main's return value
            //
            // The CPU dispatcher (Kernel::Execute) starts at the ELF entry
            // point (_start), NOT at main.  The previous implementation treated
            // arg1 as an exit status and immediately called ExitGuestProcess,
            // which meant main() never ran — this was the primary boot blocker.
            guest_addr_t main_va = GetGuestMainAddress();
            if (!main_va) {
                LOG_ERROR(HLE, "XKRegsFpEpk: main() not found — cannot boot.");
                HLE::ExitGuestProcess(1);
                // unreachable
            }

            // Step 1: call main(argc, argv, envp).
            // InvokeGuestFunction maps (rdi, rsi, rdx) → (arg1, arg2, arg3)
            // which matches the SysV ABI for main(int argc, char** argv, char** envp).
            LOG_INFO(HLE, "XKRegsFpEpk: calling main(argc=%llu, argv=0x%llx)", args.arg1, args.arg2);
            u64 result = InvokeGuestFunction(main_va, args.arg1, args.arg2, args.arg3);
            LOG_INFO(HLE, "main() returned with status %llu", result);

            // Step 2: exit with main's return value.
            HLE::ExitGuestProcess(static_cast<u32>(result));
            // unreachable — ExitGuestProcess is [[noreturn]]
        });

        // =====================================================================
        // Kyty-port: thread atexit tracking — counters for per-thread cleanup.
        // Games call these during thread creation/exit.  Missing implementations
        // cause the game's libc to hang waiting for atexit counts to match.
        // =====================================================================
        // KernelSetThreadAtexitCount (pB-yGZ2nQ9o) — store the count-getter fn.
        {
            static void* s_get_count = nullptr;
            RegisterSymbol("libkernel", "pB-yGZ2nQ9o#T#T", [](const GuestArgs& a) -> u64 {
                s_get_count = reinterpret_cast<void*>(static_cast<uintptr_t>(a.arg1));
                LOG_DEBUG(HLE, "KernelSetThreadAtexitCount(func=0x%llx) -> 0", a.arg1);
                return 0;
            });
        }
        // KernelSetThreadAtexitReport (WhCc1w3EhSI) — store the report fn.
        {
            static void* s_report = nullptr;
            RegisterSymbol("libkernel", "WhCc1w3EhSI#T#T", [](const GuestArgs& a) -> u64 {
                s_report = reinterpret_cast<void*>(static_cast<uintptr_t>(a.arg1));
                LOG_DEBUG(HLE, "KernelSetThreadAtexitReport(func=0x%llx) -> 0", a.arg1);
                return 0;
            });
        }
        // KernelRtldThreadAtexitIncrement (Tz4RNUCBbGI)
        RegisterSymbol("libkernel", "Tz4RNUCBbGI#T#T", [](const GuestArgs&) -> u64 {
            LOG_DEBUG(HLE, "KernelRtldThreadAtexitIncrement() -> 0");
            return 0;
        });
        // KernelRtldThreadAtexitDecrement (8OnWXlgQlvo)
        RegisterSymbol("libkernel", "8OnWXlgQlvo#T#T", [](const GuestArgs&) -> u64 {
            LOG_DEBUG(HLE, "KernelRtldThreadAtexitDecrement() -> 0");
            return 0;
        });
        // KernelSetThreadDtors (rNhWz+lvOMU) — set thread destructors.
        RegisterSymbol("libkernel", "rNhWz+lvOMU#T#T", [](const GuestArgs& a) -> u64 {
            LOG_DEBUG(HLE, "KernelSetThreadDtors(0x%llx) -> 0", a.arg1);
            return 0;
        });
        // pthread_cxa_finalize (kbw4UHHSYy0) — finalize cleanup on thread exit.
        RegisterSymbol("libkernel", "kbw4UHHSYy0#T#T", [](const GuestArgs& a) -> u64 {
            LOG_DEBUG(HLE, "pthread_cxa_finalize(0x%llx) -> 0", a.arg1);
            return 0;
        });

        RegisterSymbol("libkernel", "sceKernelDebugRaiseException#T#T", [](const GuestArgs& args) -> u64 {
            u64 rsp = args.stack_args ? (args.stack_args - 8) : 0;
            u64 guest_rip = rsp ? Memory::Read<u64>(rsp) : 0;
            
            LOG_ERROR(HLE, "==================================================");
            LOG_ERROR(HLE, "sceKernelDebugRaiseException(0x%llx) called!", args.arg1);
            LOG_ERROR(HLE, "GUEST_RIP: 0x%llx", guest_rip);
            LOG_ERROR(HLE, "GUEST_RSP: 0x%llx", rsp);
            
            // Dump stack memory for backtrace
            if (rsp) {
                LOG_ERROR(HLE, "Stack trace (return addresses):");
                for (int i = 0; i < 32; ++i) {
                    u64 addr = rsp + (i * 8);
                    u64 val = Memory::Read<u64>(addr);
                    if (val > 0x800000000 && val < 0x900000000) {
                        LOG_ERROR(HLE, "  [%02d] rsp+0x%03x = 0x%llx", i, i * 8, val);
                    }
                }
            }
            
            // Dump both PLT stubs
            if (guest_rip > 0x800000000) {
                u64 plt_stub_debug = guest_rip - 5 + 0x115333 + 5; 
                
                LOG_ERROR(HLE, "Real crashing function around RIP 0x%llx:", guest_rip);
                u8 buf[128];
                if (Memory::IsReadable(guest_rip - 64, 128)) {
                    Memory::ReadBuffer(guest_rip - 64, buf, 128);
                    char hex[512] = {0};
                    for (int i = 0; i < 128; ++i) sprintf_s(hex + i * 3, sizeof(hex) - i * 3, "%02X ", buf[i]);
                    LOG_ERROR(HLE, "  Code: %s", hex);
                }
                
                LOG_ERROR(HLE, "DebugRaiseException PLT stub at 0x%llx:", plt_stub_debug);
                if (Memory::IsReadable(plt_stub_debug, 32)) {
                    Memory::ReadBuffer(plt_stub_debug, buf, 32);
                    char hex[128] = {0};
                    for (int i = 0; i < 16; ++i) sprintf_s(hex + i * 3, sizeof(hex) - i * 3, "%02X ", buf[i]);
                    LOG_ERROR(HLE, "  Code: %s", hex);
                }
                
                if (Memory::IsReadable(plt_stub_debug + 2, 4)) {
                    u32 jmp_off = Memory::Read<u32>(plt_stub_debug + 2);
                    u64 got_debug = plt_stub_debug + 6 + jmp_off;
                    u64 got_val_debug = Memory::Read<u64>(got_debug);
                    LOG_ERROR(HLE, "GOT entry for DebugRaiseException at 0x%llx = 0x%llx", got_debug, got_val_debug);
                }
            }
            
            LOG_ERROR(HLE, "==================================================");
            
            // Do not bypass the exception, just exit like the stub would.
            HLE::SetGuestCrashed(0xE0000001, guest_rip);
            HLE::ExitGuestProcess(1);
        });

        // =====================================================================
        // Kyty-port: boot-critical stubs — games call these during _start / CRT init.
        // =====================================================================
        // elf_phdr_match_addr (Fjc4-n1+y2g) — check if addr falls in a module's phdr.
        RegisterSymbol("libkernel", "Fjc4-n1+y2g#T#T", [](const GuestArgs& a) -> u64 {
            LOG_ERROR(HLE, "elf_phdr_match_addr(addr=0x%llx) called!", a.arg1);
            
            // Allocate a dummy struct in host memory and return it. 
            // Guest pointers are 1:1 with host pointers in this emulator architecture.
            static u8* dummy_struct = nullptr;
            if (!dummy_struct) {
                dummy_struct = new u8[0x100]();
                // libc.prx checks [rax + 0x38] (tls_modid). Return a non-zero value.
                *reinterpret_cast<u64*>(dummy_struct + 0x38) = 1; 
            }
            
            return reinterpret_cast<u64>(dummy_struct);
        });
        // KernelGetProcParam (959qrazPIrg) — return the process parameter (procParam).
        RegisterSymbol("libkernel", "959qrazPIrg#T#T", [](const GuestArgs&) -> u64 {
            u64 proc_param = Kernel::GetMainModuleProcessParam();
            LOG_DEBUG(HLE, "KernelGetProcParam() -> 0x%llx", proc_param);
            return proc_param;
        });
        // tls_get_addr (vNe1w4diLCs) — __tls_get_addr(desc) where desc =
        // `{ size_t ti_module; size_t ti_offset; }` (ELF TLS descriptor, arg1=rdi).
        // Returns the address of the TLS variable for ti_module+ti_offset.
        // PCSX5 uses variant-II TLS.  Resolution is module-keyed: the MAIN
        // module (and unknown indices) uses the original
        // (thread_pointer + ti_offset) shared-block path — byte-identical to the
        // pre-DTV behaviour; a KNOWN secondary TLS-bearing module gets a
        // dedicated per-thread block seeded from its PT_TLS image.
        RegisterSymbol("libkernel", "vNe1w4diLCs#T#T", [](const GuestArgs& a) -> u64 {
            u64 module_id = 0, ti_offset = 0;
            if (a.arg1 && a.arg1 >= 0x800000000ULL && Memory::IsReadable(a.arg1, 16)) {
                module_id = Memory::Read<u64>(a.arg1);
                ti_offset = Memory::Read<u64>(a.arg1 + 8);
            }
            const u64 tid = Kernel::GetCurrentThreadId();
            const u64 tp  = Kernel::ResolveGuestThreadPointer(tid);
            const u64 addr = Kernel::ResolveDynamicTls(tid, module_id, ti_offset, tp);
            LOG_DEBUG(HLE, "__tls_get_addr(desc=0x%llx module=%llu offset=0x%llx) -> 0x%llx",
                      a.arg1, module_id, ti_offset, addr);
            return addr;
        });
        // KernelGetModuleInfoFromAddr (f7KBOafysXo) — lookup module by code address.
        RegisterSymbol("libkernel", "f7KBOafysXo#T#T", [](const GuestArgs& a) -> u64 {
            LOG_DEBUG(HLE, "KernelGetModuleInfoFromAddr(addr=0x%llx) -> ENOSYS", a.arg1);
            return 0x8002002D; // SCE_KERNEL_ERROR_ENOSYS
        });

        // sceKernelGetModuleInfoForUnwind (RpQJJVKTiFM) — returns address &
        // module metadata; used by libc's unwinder to map a code address to a
        // module and its segments.  Without it, CRT/static-init that iterates
        // the returned segment table dereferences null -> AV (ASTRO BOT,
        // "mov r14d,[r14]" at 0x3f0).  Fill a standard SceKernelModuleInfo.
        //
        // Standard PS5 SceKernelModuleInfo layout (internals):
        //   0x00 size_t st_size
        //   0x08 char  name[0x20]
        //   0x28 u32   module_id
        //   0x2c u32   module_idx
        //   0x30 uintptr module_start
        //   0x38 uintptr module_end
        //   0x40 uintptr module_offset
        //   0x48 size_t module_size
        //   0x50 size_t module_flags
        //   0x98 SceKernelModuleSegmentInfo segment_info[1] (address,size,size2,prot,flags,pad)
        RegisterSymbol("libkernel", "RpQJJVKTiFM#T#T", [](const GuestArgs& a) -> u64 {
            // ABI (per SharpEmu KernelRuntimeCompatExports): rdi=queried addr,
            // rsi=flags (0..2), rdx=out SceKernelModuleInfoForUnwind. The out
            // struct is 0x130 bytes and carries the .eh_frame fields the guest
            // libc++abi unwinder needs at 0x108/0x110/0x118.
            const guest_addr_t addr  = a.arg1;
            const int          flags = static_cast<int>(a.arg2);
            const guest_addr_t out   = a.arg3;
            if (!out) return 0x80020016; // EINVAL-ish
            if (flags < 0 || flags >= 3) return 0x80020016;
            if (!Memory::IsWritable(out, 0x130)) {
                LOG_WARN(HLE, "sceKernelGetModuleInfoForUnwind(addr=0x%llx, flags=%d, out=0x%llx) -> bad out",
                         addr, flags, out);
                return 0x80020016;
            }
            const auto* mod = Kernel::FindModuleForAddr(addr);
            if (!mod) {
                LOG_WARN(HLE, "sceKernelGetModuleInfoForUnwind(addr=0x%llx) -> module not found", addr);
                return 0x80020004; // ENOENT-ish
            }
            Memory::Write<u64>(out + 0x000, 0x130);
            for (int i = 0; i < 0x20 && i < (int)mod->name.size(); ++i)
                Memory::Write<u8>(out + 0x008 + i, static_cast<u8>(mod->name[i]));
            Memory::Write<u64>(out + 0x108, mod->eh_frame_hdr_addr);
            Memory::Write<u64>(out + 0x110, mod->eh_frame_hdr_addr);
            Memory::Write<u64>(out + 0x118, mod->eh_frame_hdr_size);
            Memory::Write<u64>(out + 0x120, mod->base_address);
            Memory::Write<u64>(out + 0x128, mod->image_size);
            LOG_DEBUG(HLE, "sceKernelGetModuleInfoForUnwind(addr=0x%llx, flags=%d, out=0x%llx) -> '%s' "
                      "base=0x%llx eh_hdr=0x%llx size=0x%llx",
                      addr, flags, out, mod->name.c_str(), mod->base_address,
                      mod->eh_frame_hdr_addr, mod->image_size);
            return 0; // SCE_OK
        });
        // KernelIsNeoMode (WslcK1FQcGI) — is this a PS4 Pro?  PS5 always false.
        RegisterSymbol("libkernel", "WslcK1FQcGI#T#T", [](const GuestArgs&) -> u64 {
            LOG_DEBUG(HLE, "KernelIsNeoMode() -> false");
            return 0;
        });
        // KernelUuidCreate (Xjoosiw+XPI) — generate a UUID.
        RegisterSymbol("libkernel", "Xjoosiw+XPI#T#T", [](const GuestArgs& a) -> u64 {
            // Write zeroed UUID (16 bytes) if a valid pointer was provided.
            if (a.arg1) {
                for (int i = 0; i < 16; ++i) Memory::Write<u8>(a.arg1 + i, 0);
            }
            LOG_DEBUG(HLE, "KernelUuidCreate(0x%llx) -> all-zeros", a.arg1);
            return 0;
        });
        // KernelSetGPO (ca7v6Cxulzs) — set general-purpose output (HW control).
        RegisterSymbol("libkernel", "ca7v6Cxulzs#T#T", [](const GuestArgs& a) -> u64 {
            LOG_DEBUG(HLE, "KernelSetGPO(val=0x%llx) -> 0", a.arg1);
            return 0;
        });
        // KernelRtldSetApplicationHeapAPI (p5EcQeEeJAE) — register heap allocators.
        RegisterSymbol("libkernel", "p5EcQeEeJAE#T#T", [](const GuestArgs& a) -> u64 {
            LOG_DEBUG(HLE, "KernelRtldSetApplicationHeapAPI(0x%llx) -> 0", a.arg1);
            return 0;
        });
        RegisterSymbol("libkernel", "TqIu8K3Q5h8#S#N", [](const GuestArgs&) -> u64 { return 0; });
        RegisterSymbol("libkernel", "O0FqD1eG0uI#S#N", [](const GuestArgs&) -> u64 { return 0; });



        // KernelGetSanitizerMallocReplaceExternal (py6L8jiVAN8) — ASan support.
        RegisterSymbol("libkernel", "py6L8jiVAN8#T#T", [](const GuestArgs&) -> u64 {
            LOG_DEBUG(HLE, "KernelGetSanitizerMallocReplaceExternal() -> 0");
            return 0;
        });
        // KernelGetSanitizerNewReplaceExternal (bnZxYgAFeA0) — ASan support.
        RegisterSymbol("libkernel", "bnZxYgAFeA0#T#T", [](const GuestArgs&) -> u64 {
            LOG_DEBUG(HLE, "KernelGetSanitizerNewReplaceExternal() -> 0");
            return 0;
        });
        // sceKernelIsAddressSanitizerEnabled (jh+8XiK4LeE) — ASan support.
        RegisterSymbol("libkernel", "jh+8XiK4LeE", [](const GuestArgs&) -> u64 {
            LOG_DEBUG(HLE, "sceKernelIsAddressSanitizerEnabled() -> 0");
            return 0;
        });
        RegisterSymbol("libkernel", "sceKernelIsAddressSanitizerEnabled", [](const GuestArgs&) -> u64 {
            LOG_DEBUG(HLE, "sceKernelIsAddressSanitizerEnabled() -> 0");
            return 0;
        });
        
        // sceKernelGetGPI (4oXYe9Xmk0Q)
        RegisterSymbol("libkernel", "4oXYe9Xmk0Q", [](const GuestArgs& args) -> u64 {
            LOG_WARN(HLE, "sceKernelGetGPI(0x%llx, 0x%llx, 0x%llx)", args.arg1, args.arg2, args.arg3);
            return 0;
        });
        RegisterSymbol("libkernel", "sceKernelGetGPI", [](const GuestArgs& args) -> u64 {
            LOG_WARN(HLE, "sceKernelGetGPI(0x%llx, 0x%llx, 0x%llx)", args.arg1, args.arg2, args.arg3);
            return 0;
        });
        
        // sceKernelConvertUtcToLocaltime (-o5uEDpN+oY)
        RegisterSymbol("libkernel", "-o5uEDpN+oY", [](const GuestArgs& args) -> u64 {
            LOG_WARN(HLE, "sceKernelConvertUtcToLocaltime(0x%llx, 0x%llx)", args.arg1, args.arg2);
            return 0;
        });
        RegisterSymbol("libkernel", "sceKernelConvertUtcToLocaltime", [](const GuestArgs& args) -> u64 {
            LOG_WARN(HLE, "sceKernelConvertUtcToLocaltime(0x%llx, 0x%llx)", args.arg1, args.arg2);
            return 0;
        });
        // PthreadEqual (3PtV6p3QNX4) — compare two thread IDs.
        RegisterSymbol("libkernel", "3PtV6p3QNX4#T#T", [](const GuestArgs& a) -> u64 {
            LOG_DEBUG(HLE, "PthreadEqual(t1=%llu, t2=%llu) -> %d", a.arg1, a.arg2, a.arg1 == a.arg2 ? 1 : 0);
            return (a.arg1 == a.arg2) ? 1 : 0;
        });
        // PthreadGetthreadid (EI-5-jlq2dE) — get OS thread id for a pthread.
        RegisterSymbol("libkernel", "EI-5-jlq2dE#T#T", [](const GuestArgs& a) -> u64 {
            LOG_DEBUG(HLE, "PthreadGetthreadid(thread=0x%llx, out=0x%llx) -> 0", a.arg1, a.arg2);
            if (a.arg2) {
                Memory::Write<u32>(a.arg2, static_cast<u32>(Kernel::GetCurrentThreadId()));
            }
            return 0;
        });
        // PthreadGetname (How7B8Oet6k) — get thread name.
        RegisterSymbol("libkernel", "How7B8Oet6k#T#T", [](const GuestArgs& a) -> u64 {
            LOG_DEBUG(HLE, "PthreadGetname(thread=%llu) -> 0", a.arg1);
            return 0;
        });
        // PthreadAttrSetaffinity / PthreadSetaffinity — CPU affinity (no-op).
        RegisterSymbol("libkernel", "3qxgM4ezETA#T#T", [](const GuestArgs& a) -> u64 {
            LOG_DEBUG(HLE, "PthreadAttrSetaffinity(attr=0x%llx) -> 0", a.arg1);
            return 0;
        });
        RegisterSymbol("libkernel", "bt3CTBKmGyI#T#T", [](const GuestArgs& a) -> u64 {
            LOG_DEBUG(HLE, "PthreadSetaffinity(thread=%llu) -> 0", a.arg1);
            return 0;
        });
        // PthreadAttrGetaffinity (8+s5BzZjxSg) — get CPU affinity mask.
        RegisterSymbol("libkernel", "8+s5BzZjxSg#T#T", [](const GuestArgs&) -> u64 {
            LOG_DEBUG(HLE, "PthreadAttrGetaffinity() -> 0");
            return 0;
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
        // scePthreadAttr / pthread_attr family
        // Uses dedicated guest memory slots so the guest receives an 8-byte pointer
        // to a valid readable/writable 64-byte guest memory struct.
        // =====================================================================
        static guest_addr_t s_attr_page_base = 0;
        static std::mutex s_attr_pool_mutex;
        static constexpr u32 kMaxAttrs = 1024;
        static bool s_attr_used[kMaxAttrs] = {};

        auto AllocAttrSlot = []() -> guest_addr_t {
            std::lock_guard<std::mutex> lock(s_attr_pool_mutex);
            if (!s_attr_page_base) {
                if (Memory::AllocateRange(64 * 1024, 64 * 1024, Memory::Owner::Hle,
                                          "pthread-attr-pool", &s_attr_page_base) != Memory::Status::Ok ||
                    Memory::Commit(s_attr_page_base, 64 * 1024, Memory::PROT_READ | Memory::PROT_WRITE) != Memory::Status::Ok) {
                    LOG_ERROR(HLE, "Failed to allocate guest pthread_attr pool page");
                    return 0;
                }
                LOG_INFO(HLE, "pthread-attr-pool allocated at 0x%llx (64KB, %u slots of 64 bytes)",
                         s_attr_page_base, kMaxAttrs);
            }
            for (u32 i = 0; i < kMaxAttrs; ++i) {
                if (!s_attr_used[i]) {
                    s_attr_used[i] = true;
                    guest_addr_t slot_va = s_attr_page_base + i * 64;
                    Memory::Write<u64>(slot_va + 0x00, 0x38);
                    Memory::Write<u64>(slot_va + 0x08, 2 * 1024 * 1024); // 2MB stack
                    Memory::Write<u64>(slot_va + 0x10, 0);               // stack base
                    Memory::Write<u64>(slot_va + 0x18, 0x1000);          // guard size
                    Memory::Write<u64>(slot_va + 0x20, 0);               // joinable
                    Memory::Write<u64>(slot_va + 0x28, 0xFF);            // cpuset
                    Memory::Write<u64>(slot_va + 0x30, 0);               // priority
                    Memory::Write<u64>(slot_va + 0x38, 0);               // inheritsched
                    return slot_va;
                }
            }
            LOG_ERROR(HLE, "PthreadAttr pool exhausted");
            return 0;
        };

        auto FreeAttrSlot = [](guest_addr_t slot_va) {
            if (!slot_va || !s_attr_page_base) return;
            std::lock_guard<std::mutex> lock(s_attr_pool_mutex);
            if (slot_va >= s_attr_page_base && slot_va < s_attr_page_base + kMaxAttrs * 64) {
                u32 i = static_cast<u32>((slot_va - s_attr_page_base) / 64);
                s_attr_used[i] = false;
            }
        };

        auto ResolveAttrPtr = [](guest_addr_t arg) -> guest_addr_t {
            if (!arg) return 0;
            if (s_attr_page_base && arg >= s_attr_page_base && arg < s_attr_page_base + kMaxAttrs * 64) {
                return arg;
            }
            u64 val = Memory::Read<u64>(arg);
            if (s_attr_page_base && val >= s_attr_page_base && val < s_attr_page_base + kMaxAttrs * 64) {
                return val;
            }
            return arg;
        };

        auto PthreadAttrInitImpl = [AllocAttrSlot](const GuestArgs& args) -> u64 {
            guest_addr_t attr = args.arg1;
            if (!attr) return 22; // EINVAL
            guest_addr_t slot_va = AllocAttrSlot();
            if (!slot_va) return 12; // ENOMEM
            Memory::Write<u64>(attr, slot_va);
            LOG_INFO(HLE, "PthreadAttrInit: wrote slot 0x%llx into guest 0x%llx", slot_va, attr);
            return 0;
        };

        auto PthreadAttrDestroyImpl = [ResolveAttrPtr, FreeAttrSlot](const GuestArgs& args) -> u64 {
            guest_addr_t attr = args.arg1;
            if (attr) {
                guest_addr_t slot_va = ResolveAttrPtr(attr);
                FreeAttrSlot(slot_va);
                Memory::Write<u64>(attr, 0);
            }
            LOG_DEBUG(HLE, "PthreadAttrDestroy(0x%llx) -> OK", attr);
            return 0;
        };

        auto PthreadAttrSetstacksizeImpl = [ResolveAttrPtr](const GuestArgs& args) -> u64 {
            guest_addr_t attr = args.arg1;
            u64 size = args.arg2;
            if (!attr) return 22; // EINVAL
            guest_addr_t slot = ResolveAttrPtr(attr);
            if (slot) Memory::Write<u64>(slot + 0x08, size);
            LOG_DEBUG(HLE, "PthreadAttrSetstacksize(attr=0x%llx, size=0x%llx) -> OK", attr, size);
            return 0;
        };

        auto PthreadAttrGetstacksizeImpl = [ResolveAttrPtr](const GuestArgs& args) -> u64 {
            guest_addr_t attr = args.arg1;
            guest_addr_t out_size = args.arg2;
            if (!attr || !out_size) return 22; // EINVAL
            guest_addr_t slot = ResolveAttrPtr(attr);
            u64 size = slot ? Memory::Read<u64>(slot + 0x08) : 2 * 1024 * 1024;
            if (size == 0) size = 2 * 1024 * 1024;
            Memory::Write<u64>(out_size, size);
            LOG_DEBUG(HLE, "PthreadAttrGetstacksize(attr=0x%llx) -> size=0x%llx", attr, size);
            return 0;
        };

        auto PthreadAttrSetstackImpl = [ResolveAttrPtr](const GuestArgs& args) -> u64 {
            guest_addr_t attr = args.arg1;
            guest_addr_t stackaddr = args.arg2;
            u64 size = args.arg3;
            if (!attr) return 22; // EINVAL
            guest_addr_t slot = ResolveAttrPtr(attr);
            if (slot) {
                Memory::Write<u64>(slot + 0x10, stackaddr);
                Memory::Write<u64>(slot + 0x08, size);
            }
            LOG_DEBUG(HLE, "PthreadAttrSetstack(attr=0x%llx, addr=0x%llx, size=0x%llx) -> OK", attr, stackaddr, size);
            return 0;
        };

        auto PthreadAttrGetstackImpl = [ResolveAttrPtr](const GuestArgs& args) -> u64 {
            guest_addr_t attr = args.arg1;
            guest_addr_t out_addr = args.arg2;
            guest_addr_t out_size = args.arg3;
            if (!attr) return 22; // EINVAL
            guest_addr_t slot = ResolveAttrPtr(attr);
            if (out_addr) Memory::Write<u64>(out_addr, slot ? Memory::Read<u64>(slot + 0x10) : 0);
            if (out_size) {
                u64 size = slot ? Memory::Read<u64>(slot + 0x08) : 2 * 1024 * 1024;
                if (size == 0) size = 2 * 1024 * 1024;
                Memory::Write<u64>(out_size, size);
            }
            LOG_DEBUG(HLE, "PthreadAttrGetstack(attr=0x%llx) -> OK", attr);
            return 0;
        };

        auto PthreadAttrSetguardsizeImpl = [ResolveAttrPtr](const GuestArgs& args) -> u64 {
            guest_addr_t attr = args.arg1;
            u64 guard = args.arg2;
            if (!attr) return 22;
            guest_addr_t slot = ResolveAttrPtr(attr);
            if (slot) Memory::Write<u64>(slot + 0x18, guard);
            LOG_DEBUG(HLE, "PthreadAttrSetguardsize(attr=0x%llx, guard=0x%llx) -> OK", attr, guard);
            return 0;
        };

        auto PthreadAttrGetguardsizeImpl = [ResolveAttrPtr](const GuestArgs& args) -> u64 {
            guest_addr_t attr = args.arg1;
            guest_addr_t out_guard = args.arg2;
            if (!attr || !out_guard) return 22;
            guest_addr_t slot = ResolveAttrPtr(attr);
            Memory::Write<u64>(out_guard, slot ? Memory::Read<u64>(slot + 0x18) : 0x1000);
            LOG_DEBUG(HLE, "PthreadAttrGetguardsize(attr=0x%llx) -> OK", attr);
            return 0;
        };

        auto PthreadAttrSetdetachstateImpl = [ResolveAttrPtr](const GuestArgs& args) -> u64 {
            guest_addr_t attr = args.arg1;
            u64 state = args.arg2;
            if (!attr) return 22;
            guest_addr_t slot = ResolveAttrPtr(attr);
            if (slot) Memory::Write<u64>(slot + 0x20, state);
            LOG_DEBUG(HLE, "PthreadAttrSetdetachstate(attr=0x%llx, state=%llu) -> OK", attr, state);
            return 0;
        };

        auto PthreadAttrGetdetachstateImpl = [ResolveAttrPtr](const GuestArgs& args) -> u64 {
            guest_addr_t attr = args.arg1;
            guest_addr_t out_state = args.arg2;
            if (!attr || !out_state) return 22;
            guest_addr_t slot = ResolveAttrPtr(attr);
            Memory::Write<u32>(out_state, slot ? static_cast<u32>(Memory::Read<u64>(slot + 0x20)) : 0);
            LOG_DEBUG(HLE, "PthreadAttrGetdetachstate(attr=0x%llx) -> OK", attr);
            return 0;
        };

        auto PthreadAttrSetaffinityImpl = [ResolveAttrPtr](const GuestArgs& args) -> u64 {
            guest_addr_t attr = args.arg1;
            u64 cpuset = args.arg2;
            if (!attr) return 22;
            guest_addr_t slot = ResolveAttrPtr(attr);
            if (slot) Memory::Write<u64>(slot + 0x28, cpuset);
            LOG_DEBUG(HLE, "PthreadAttrSetaffinity(attr=0x%llx, cpuset=0x%llx) -> OK", attr, cpuset);
            return 0;
        };

        auto PthreadAttrGetaffinityImpl = [ResolveAttrPtr](const GuestArgs& args) -> u64 {
            guest_addr_t attr = args.arg1;
            guest_addr_t out_cpuset = args.arg2;
            if (!attr || !out_cpuset) return 22;
            guest_addr_t slot = ResolveAttrPtr(attr);
            u64 mask = slot ? Memory::Read<u64>(slot + 0x28) : 0xFF;
            if (mask == 0) mask = 0xFF;
            Memory::Write<u64>(out_cpuset, mask);
            LOG_DEBUG(HLE, "PthreadAttrGetaffinity(attr=0x%llx) -> mask=0x%llx", attr, mask);
            return 0;
        };

        auto PthreadAttrSetschedparamImpl = [ResolveAttrPtr](const GuestArgs& args) -> u64 {
            guest_addr_t attr = args.arg1;
            guest_addr_t param = args.arg2;
            if (!attr) return 22;
            guest_addr_t slot = ResolveAttrPtr(attr);
            if (slot && param) Memory::Write<u64>(slot + 0x30, Memory::Read<u64>(param));
            LOG_DEBUG(HLE, "PthreadAttrSetschedparam(attr=0x%llx) -> OK", attr);
            return 0;
        };

        auto PthreadAttrGetschedparamImpl = [ResolveAttrPtr](const GuestArgs& args) -> u64 {
            guest_addr_t attr = args.arg1;
            guest_addr_t out_param = args.arg2;
            if (!attr || !out_param) return 22;
            guest_addr_t slot = ResolveAttrPtr(attr);
            Memory::Write<u64>(out_param, slot ? Memory::Read<u64>(slot + 0x30) : 0);
            LOG_DEBUG(HLE, "PthreadAttrGetschedparam(attr=0x%llx) -> OK", attr);
            return 0;
        };

        auto PthreadAttrSetschedpolicyImpl = [](const GuestArgs& args) -> u64 {
            LOG_DEBUG(HLE, "PthreadAttrSetschedpolicy(attr=0x%llx, policy=%llu) -> OK", args.arg1, args.arg2);
            return 0;
        };

        auto PthreadAttrGetschedpolicyImpl = [ResolveAttrPtr](const GuestArgs& args) -> u64 {
            guest_addr_t attr = args.arg1;
            guest_addr_t out_policy = args.arg2;
            if (!attr || !out_policy) return 22;
            guest_addr_t slot = ResolveAttrPtr(attr);
            Memory::Write<u32>(out_policy, slot ? static_cast<u32>(Memory::Read<u64>(slot + 0x38)) : 0);
            LOG_DEBUG(HLE, "PthreadAttrGetschedpolicy(attr=0x%llx) -> 0", attr);
            return 0;
        };

        auto PthreadAttrSetinheritschedImpl = [](const GuestArgs& args) -> u64 {
            LOG_DEBUG(HLE, "PthreadAttrSetinheritsched(attr=0x%llx, inherit=%llu) -> OK", args.arg1, args.arg2);
            return 0;
        };

        auto PthreadAttrGetinheritschedImpl = [ResolveAttrPtr](const GuestArgs& args) -> u64 {
            guest_addr_t attr = args.arg1;
            guest_addr_t out_inherit = args.arg2;
            if (!attr || !out_inherit) return 22;
            guest_addr_t slot = ResolveAttrPtr(attr);
            Memory::Write<u32>(out_inherit, slot ? static_cast<u32>(Memory::Read<u64>(slot + 0x38)) : 0);
            LOG_DEBUG(HLE, "PthreadAttrGetinheritsched(attr=0x%llx) -> 0", attr);
            return 0;
        };

        auto PthreadAttrGetImpl = [AllocAttrSlot, ResolveAttrPtr](const GuestArgs& args) -> u64 {
            guest_addr_t thread_id = args.arg1;
            guest_addr_t attr_ptr  = args.arg2;
            if (!attr_ptr) return 22; // EINVAL

            guest_addr_t slot = ResolveAttrPtr(attr_ptr);
            if (!slot || slot == attr_ptr) {
                slot = AllocAttrSlot();
                // This overwrites whatever the guest already had at attr_ptr.
                // If attr_ptr is not actually an attribute variable, that is a
                // destructive write into live guest data.
                LOG_INFO(HLE, "PthreadAttrGet: thread=0x%llx attr_ptr=0x%llx had 0x%llx, writing slot 0x%llx",
                         thread_id, attr_ptr, static_cast<u64>(Memory::Read<u64>(attr_ptr)), slot);
                Memory::Write<u64>(attr_ptr, slot);
            }

            u64 stack_base = 0;
            u64 stack_size = 2 * 1024 * 1024;
            u64 target_id = thread_id ? thread_id : CpuCore::GetCurrentThreadId();
            GuestThread* t = CpuCore::GetThreadById(target_id);
            if (t && t->stack_size > 0) {
                stack_base = t->stack_base;
                stack_size = t->stack_size;
            }
            if (slot) {
                Memory::Write<u64>(slot + 0x08, stack_size);
                Memory::Write<u64>(slot + 0x10, stack_base);
                Memory::Write<u64>(slot + 0x28, 0xFF);
            }
            LOG_DEBUG(HLE, "scePthreadAttrGet(thread=%llu, attr=0x%llx) -> base=0x%llx, size=0x%llx",
                      target_id, attr_ptr, stack_base, stack_size);
            return 0;
        };

        // Symbol registrations
        RegisterSymbol("libkernel", "scePthreadAttrInit", PthreadAttrInitImpl);
        RegisterSymbol("libkernel", "pthread_attr_init", PthreadAttrInitImpl);
        RegisterSymbol("libkernel", "wtkt-teR1so", PthreadAttrInitImpl);
        RegisterSymbol("libkernel", "wtkt-teR1so#T#T", PthreadAttrInitImpl);
        RegisterSymbol("libkernel", "wtkt-teR1so#S#N", PthreadAttrInitImpl);
        RegisterSymbol("libkernel", "aI+OeCz8xrQ", PthreadAttrInitImpl);
        RegisterSymbol("libkernel", "aI+OeCz8xrQ#T#T", PthreadAttrInitImpl);
        RegisterSymbol("libkernel", "aI+OeCz8xrQ#S#N", PthreadAttrInitImpl);

        RegisterSymbol("libkernel", "scePthreadAttrDestroy", PthreadAttrDestroyImpl);
        RegisterSymbol("libkernel", "pthread_attr_destroy", PthreadAttrDestroyImpl);
        RegisterSymbol("libkernel", "62KCwEMmzcM#S#N", PthreadAttrDestroyImpl);
        RegisterSymbol("libkernel", "62KCwEMmzcM#T#T", PthreadAttrDestroyImpl);
        RegisterSymbol("libkernel", "62KCwEMmzcM", PthreadAttrDestroyImpl);

        RegisterSymbol("libkernel", "scePthreadAttrSetstacksize", PthreadAttrSetstacksizeImpl);
        RegisterSymbol("libkernel", "pthread_attr_setstacksize", PthreadAttrSetstacksizeImpl);
        RegisterSymbol("libkernel", "UTXzJbWhhTE", PthreadAttrSetstacksizeImpl);
        RegisterSymbol("libkernel", "UTXzJbWhhTE#T#T", PthreadAttrSetstacksizeImpl);
        RegisterSymbol("libkernel", "UTXzJbWhhTE#S#N", PthreadAttrSetstacksizeImpl);

        RegisterSymbol("libkernel", "scePthreadAttrGetstacksize", PthreadAttrGetstacksizeImpl);
        RegisterSymbol("libkernel", "pthread_attr_getstacksize", PthreadAttrGetstacksizeImpl);

        RegisterSymbol("libkernel", "scePthreadAttrSetstack", PthreadAttrSetstackImpl);
        RegisterSymbol("libkernel", "pthread_attr_setstack", PthreadAttrSetstackImpl);
        RegisterSymbol("libkernel", "Bvn74vj6oLo", PthreadAttrSetstackImpl);
        RegisterSymbol("libkernel", "Bvn74vj6oLo#T#T", PthreadAttrSetstackImpl);
        RegisterSymbol("libkernel", "Bvn74vj6oLo#S#N", PthreadAttrSetstackImpl);

        RegisterSymbol("libkernel", "scePthreadAttrGetstack", PthreadAttrGetstackImpl);
        RegisterSymbol("libkernel", "pthread_attr_getstack", PthreadAttrGetstackImpl);

        RegisterSymbol("libkernel", "scePthreadAttrSetguardsize", PthreadAttrSetguardsizeImpl);
        RegisterSymbol("libkernel", "pthread_attr_setguardsize", PthreadAttrSetguardsizeImpl);
        RegisterSymbol("libkernel", "scePthreadAttrGetguardsize", PthreadAttrGetguardsizeImpl);
        RegisterSymbol("libkernel", "pthread_attr_getguardsize", PthreadAttrGetguardsizeImpl);

        RegisterSymbol("libkernel", "scePthreadAttrSetdetachstate", PthreadAttrSetdetachstateImpl);
        RegisterSymbol("libkernel", "pthread_attr_setdetachstate", PthreadAttrSetdetachstateImpl);
        RegisterSymbol("libkernel", "-Wreprtu0Qs#T#T", PthreadAttrSetdetachstateImpl);
        RegisterSymbol("libkernel", "-Wreprtu0Qs#S#N", PthreadAttrSetdetachstateImpl);
        RegisterSymbol("libkernel", "-Wreprtu0Qs", PthreadAttrSetdetachstateImpl);
        RegisterSymbol("libkernel", "scePthreadAttrGetdetachstate", PthreadAttrGetdetachstateImpl);
        RegisterSymbol("libkernel", "pthread_attr_getdetachstate", PthreadAttrGetdetachstateImpl);

        RegisterSymbol("libkernel", "scePthreadAttrSetaffinity", PthreadAttrSetaffinityImpl);
        RegisterSymbol("libkernel", "pthread_attr_setaffinity_np", PthreadAttrSetaffinityImpl);
        RegisterSymbol("libkernel", "3qxgM4ezETA#T#T", PthreadAttrSetaffinityImpl);
        RegisterSymbol("libkernel", "3qxgM4ezETA#S#N", PthreadAttrSetaffinityImpl);
        RegisterSymbol("libkernel", "3qxgM4ezETA", PthreadAttrSetaffinityImpl);

        RegisterSymbol("libkernel", "scePthreadAttrGetaffinity", PthreadAttrGetaffinityImpl);
        RegisterSymbol("libkernel", "pthread_attr_getaffinity_np", PthreadAttrGetaffinityImpl);
        RegisterSymbol("libkernel", "8+s5BzZjxSg#T#T", PthreadAttrGetaffinityImpl);
        RegisterSymbol("libkernel", "8+s5BzZjxSg#S#N", PthreadAttrGetaffinityImpl);
        RegisterSymbol("libkernel", "8+s5BzZjxSg", PthreadAttrGetaffinityImpl);

        RegisterSymbol("libkernel", "scePthreadAttrSetschedparam", PthreadAttrSetschedparamImpl);
        RegisterSymbol("libkernel", "pthread_attr_setschedparam", PthreadAttrSetschedparamImpl);
        RegisterSymbol("libkernel", "DzES9hQF4f4#T#T", PthreadAttrSetschedparamImpl);
        RegisterSymbol("libkernel", "DzES9hQF4f4#S#N", PthreadAttrSetschedparamImpl);
        RegisterSymbol("libkernel", "DzES9hQF4f4", PthreadAttrSetschedparamImpl);
        RegisterSymbol("libkernel", "scePthreadAttrGetschedparam", PthreadAttrGetschedparamImpl);
        RegisterSymbol("libkernel", "pthread_attr_getschedparam", PthreadAttrGetschedparamImpl);

        RegisterSymbol("libkernel", "scePthreadAttrSetschedpolicy", PthreadAttrSetschedpolicyImpl);
        RegisterSymbol("libkernel", "pthread_attr_setschedpolicy", PthreadAttrSetschedpolicyImpl);
        RegisterSymbol("libkernel", "scePthreadAttrGetschedpolicy", PthreadAttrGetschedpolicyImpl);
        RegisterSymbol("libkernel", "pthread_attr_getschedpolicy", PthreadAttrGetschedpolicyImpl);

        RegisterSymbol("libkernel", "scePthreadAttrSetinheritsched", PthreadAttrSetinheritschedImpl);
        RegisterSymbol("libkernel", "pthread_attr_setinheritsched", PthreadAttrSetinheritschedImpl);
        RegisterSymbol("libkernel", "scePthreadAttrGetinheritsched", PthreadAttrGetinheritschedImpl);
        RegisterSymbol("libkernel", "pthread_attr_getinheritsched", PthreadAttrGetinheritschedImpl);

        RegisterSymbol("libkernel", "scePthreadAttrGet", PthreadAttrGetImpl);
        RegisterSymbol("libkernel", "pthread_attr_get_np", PthreadAttrGetImpl);
        RegisterSymbol("libkernel", "x1X76arYMxU", PthreadAttrGetImpl);
        RegisterSymbol("libkernel", "x1X76arYMxU#T#T", PthreadAttrGetImpl);
        RegisterSymbol("libkernel", "x1X76arYMxU#S#N", PthreadAttrGetImpl);

        RegisterSymbol("libkernel", "pthread_setschedparam", [](const GuestArgs&) -> u64 { return 0; });
        RegisterSymbol("libkernel", "pthread_getschedparam", [](const GuestArgs&) -> u64 { return 0; });
        RegisterSymbol("libkernel", "pthread_setprio", [](const GuestArgs&) -> u64 { return 0; });
        RegisterSymbol("libkernel", "pthread_getprio", [](const GuestArgs&) -> u64 { return 0; });
        RegisterSymbol("libkernel", "pthread_rename_np", [](const GuestArgs&) -> u64 { return 0; });
        RegisterSymbol("libkernel", "__pthread_cxa_finalize", [](const GuestArgs&) -> u64 { return 0; });

        // =====================================================================
        // scePthreadCreate / scePthreadJoin are registered by NID below
        // (6UgtwV+0zb4#T#T and onNY9Byn-W8#S#N); the name-keyed duplicates
        // were dropped during the CpuCore thread-registry consolidation.
        // =====================================================================

        // =====================================================================
        // scePthreadDetach  ===  Detach a thread (auto-cleanup on exit).
        // =====================================================================
        auto PthreadDetachImpl = [](const GuestArgs& args) -> u64 {
            u64 tid = args.arg1;
            LOG_INFO(HLE, "scePthreadDetach(tid=%llu)", tid);

            if (!CpuCore::DetachThread(tid)) {
                LOG_ERROR(HLE, "scePthreadDetach: thread %llu not found", tid);
                return 3; // ESRCH
            }
            return 0;
        };
        RegisterSymbol("libkernel", "scePthreadDetach", PthreadDetachImpl);
        RegisterSymbol("libkernel", "pthread_detach", PthreadDetachImpl);

        // =====================================================================
        // scePthreadExit  ===  Exit the current thread.
        // =====================================================================
        auto PthreadExitImpl = [](const GuestArgs& args) -> u64 {
            u64 exit_value = args.arg1;
            LOG_INFO(HLE, "scePthreadExit(value=0x%llx)", exit_value);
            Kernel::ExitThread(exit_value);
            __assume(0); // ExitThread never returns
        };
        RegisterSymbol("libkernel", "scePthreadExit", PthreadExitImpl);
        RegisterSymbol("libkernel", "pthread_exit", PthreadExitImpl);

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
        // [Diagnostic] Disabled to trace real libc.prx execution of _init_env
        /* RegisterSymbol("libkernel", "bzQExy189ZI#T#T", [](const GuestArgs& args) -> u64 {
            LOG_DEBUG(HLE, "libkernel::_init_env() -> 0 (success)");
            return 0;
        }); */


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
        auto MemsetImpl = [](const GuestArgs& args) -> u64 {
            guest_addr_t dest = args.arg1;
            u32 ch = static_cast<u32>(args.arg2);
            u64 count = args.arg3;

            constexpr u64 MAX_MEMSET = 256ULL * 1024 * 1024;
            if (count > MAX_MEMSET) {
                LOG_WARN(HLE, "libkernel::memset: count 0x%llx exceeds 256MB limit, clamping to 0", count);
                count = 0;
            }

            LOG_DEBUG(HLE, "libkernel::memset(dest: 0x%llx, ch: %u, count: %llu)", dest, ch, count);
            if (dest && count > 0) {
                Memory::GuardedSet(dest, static_cast<int>(ch & 0xFF), count);
            }
            return dest;
        };
        RegisterSymbol("libkernel", "8zTFvBIAIN8#T#T", MemsetImpl);

        // strlen (j4ViWNHEgww#T#T)
        auto StrlenImpl = [](const GuestArgs& args) -> u64 {
            guest_addr_t str = args.arg1;
            if (!str) return 0;
            const u64 len = Memory::GuardedStrlen(str, UINT64_MAX);
            LOG_DEBUG(HLE, "libkernel::strlen(str: 0x%llx) -> %llu", str, len);
            return len;
        };
        RegisterSymbol("libkernel", "j4ViWNHEgww#T#T", StrlenImpl);

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
        RegisterSymbol("libkernel", "rTXw65xmLIA#S#N", SceKernelAllocateDirectMemory);
        RegisterSymbol("libkernel", "sceKernelAllocateDirectMemory", SceKernelAllocateDirectMemory);

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
        // Named variant (last argument is name string)
        RegisterSymbol("libkernel", "NcaWUxfMNIQ", SceKernelMapDirectMemory);
        RegisterSymbol("libkernel", "sceKernelMapNamedDirectMemory", SceKernelMapDirectMemory);

        // sceKernelSetVirtualRangeName (DGMG3JshrZU)
        RegisterSymbol("libkernel", "DGMG3JshrZU", [](const GuestArgs& args) -> u64 {
            guest_addr_t name_ptr = args.arg3;
            if (name_ptr) {
                std::string name = ReadGuestCString(name_ptr);
                LOG_INFO(HLE, "sceKernelSetVirtualRangeName(start: 0x%llx, len: 0x%llx, name: '%s')", args.arg1, args.arg2, name.c_str());
            } else {
                LOG_INFO(HLE, "sceKernelSetVirtualRangeName(start: 0x%llx, len: 0x%llx, name: null)", args.arg1, args.arg2);
            }
            return 0;
        });
        RegisterSymbol("libkernel", "sceKernelSetVirtualRangeName", [](const GuestArgs& args) -> u64 {
            guest_addr_t name_ptr = args.arg3;
            if (name_ptr) {
                std::string name = ReadGuestCString(name_ptr);
                LOG_INFO(HLE, "sceKernelSetVirtualRangeName(start: 0x%llx, len: 0x%llx, name: '%s')", args.arg1, args.arg2, name.c_str());
            } else {
                LOG_INFO(HLE, "sceKernelSetVirtualRangeName(start: 0x%llx, len: 0x%llx, name: null)", args.arg1, args.arg2);
            }
            return 0;
        });

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
            const u64 aligned_len = (length + 0xFFFF) & ~0xFFFULL;  // 64 KiB granularity
            guest_addr_t out = 0;
            if (Memory::Reserve(hint, length, &out) != Memory::Status::Ok) {
                if (hint == 0) return 0x800D0006; // ENOMEM
                LOG_WARN(HLE, "sceKernelReserveVirtualRange: hint 0x%llx failed; retrying without hint", hint);
                if (Memory::Reserve(0, length, &out) != Memory::Status::Ok) return 0x800D0006;
            }
            // Reserve tags Owner::Loader by default; this is a game-requested
            // reservation, so correct the ownership record.
            Memory::AdoptRange(out, aligned_len, Memory::PROT_NONE,
                               /*committed=*/false, Memory::Owner::Guest,
                               "reserve-virtual-range");
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
        // dhK16CKwhQg#T#T — appears right after strtod in the Construct runtime's
        // JSON number parser; arg1 is a destination buffer (stack/scratch), arg2
        // is the source value or format state.  The per-thread buffer is caller-
        // provided, so we return arg1 rather than a shared static (which would
        // race across the 31-thread P.Worker pool in PPSA02929).
        RegisterSymbol("libkernel", "dhK16CKwhQg#T#T", [](const GuestArgs& args) -> u64 {
            LOG_INFO(HLE, "dhK16CKwhQg#T#T called with 0x%llx, 0x%llx", args.arg1, args.arg2);
            return args.arg1;
        });

        // memmove (+P6FRGH4LfA#T#T) / memcpy (Q3VBxCXhUHs#T#T) and their
        // plain-name aliases share these impls.  Games occasionally call
        // them with not-yet-mapped or bogus guest pointers.
        //
        // SEH-guard the actual move as a TOCTOU race backstop: the
        // IsWritable/IsReadable check below runs BEFORE the CRT call,
        // and a concurrent thread could commit the page, pass the check,
        // then another thread munmaps it before memmove completes.
        //
        // IMPORTANT: __try/__except is NOT functional on the guest stack
        // because the x64 Windows unwinder validates the stack frame
        // against the TIB StackLimit/StackBase and skips SEH handlers
        // on non-primary stacks.  The explicit memory validation below
        // is the PRIMARY defence; the SEH is a best-effort fallback for
        // the narrow TOCTOU race window where it does work.
        // Shared guarded copy for memmove/memcpy.  The __try/__except SEH
        // fallback is NOT functional on the guest stack (x64 unwinder validates
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
                Memory::GuardedCopy(dest, src, count);
            }
            return dest;
        };
        RegisterSymbol("libkernel", "+P6FRGH4LfA#T#T", MemmoveImpl);

        // realloc (0E5HFqWCBSA#T#T)
        RegisterSymbol("libkernel", "0E5HFqWCBSA#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t old_ptr = args.arg1;
            u64 new_size         = args.arg2;
            if (new_size == 0) {
                return 0;
            }
            guest_addr_t mem = 0;
            if (Memory::Map(0, (new_size + 0xFFF) & ~0xFFFULL,
                            Memory::PROT_READ | Memory::PROT_WRITE, &mem) != Memory::Status::Ok) {
                return 0;
            }
            if (old_ptr) {
                Memory::GuardedCopy(mem, old_ptr, new_size);
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
                Memory::GuardedCopy(dest, src, count);
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
            LOG_DEBUG(HLE, "_Getptolower() -> 0x%llx", table_addr);
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
        //
        // The game's real libc.prx implements fread/fseek/ftell/fclose NATIVELY
        // against its own FILE struct layout (fd at +4, flags at +0, buffer at
        // +8/+0x10/+0x18/+0x20, ungetc area at +0x7e).  Handing it a real MSVC
        // FILE* breaks every read (fd is read from +4 where MSVC stores part
        // of _ptr), which is exactly why the C2 runtime's data.js chunk reader
        // died mid-string ("image" + EOF).  We therefore return a FAKE FILE
        // struct in libc.prx's layout, backed by a real CRT fd, and register
        // the _read/_close/lseek NIDs so libc.prx's internals route through
        // our kernel fd layer.
        // =====================================================================

        // fopen (xeYO4u7uyJ0#T#T)
        RegisterSymbol("libkernel", "xeYO4u7uyJ0#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t path_ptr = args.arg1;
            guest_addr_t mode_ptr = args.arg2;
            std::string path, mode;
            for (u64 i = 0; ; ++i) { u8 c = Memory::Read<u8>(path_ptr + i); if (!c) break; path += (char)c; }
            for (u64 i = 0; ; ++i) { u8 c = Memory::Read<u8>(mode_ptr + i); if (!c) break; mode += (char)c; }
            const std::string host_path = Kernel::TranslateGuestPath(path);
            // Map the mode string to open() flags (read/write/append + binary).
            int oflags = _O_BINARY;
            char mode_char = 'r';
            if (!mode.empty()) mode_char = mode[0];
            bool readable = false, writable = false, append = false, rw = false;
            for (char c : mode) {
                switch (c) {
                    case 'r': readable = true; break;
                    case 'w': writable = true; break;
                    case 'a': writable = true; append = true; break;
                    case '+': rw = true; break;
                    default: break;
                }
            }
            if (append)             oflags |= _O_APPEND | _O_CREAT;
            if (writable)           oflags |= _O_CREAT;
            if (rw)                 oflags |= _O_RDWR;
            else if (readable)      oflags |= _O_RDONLY;
            else if (writable)      oflags |= _O_WRONLY;
            const int fd = _open(host_path.c_str(), oflags, 0644);
            if (fd < 0) {
                LOG_INFO(HLE, "libkernel::fopen('%s', '%s') -> NULL (errno %d)",
                         path.c_str(), mode.c_str(), errno);
                return 0;
            }
            TrackGuestFd(fd);
            const u64 f = CreateFakeFile(fd, mode_char);
            if (!f) {
                _close(fd);
                UntrackGuestFd(fd);
                LOG_ERROR(HLE, "libkernel::fopen('%s'): fake FILE alloc failed", path.c_str());
                return 0;
            }
            LOG_INFO(HLE, "libkernel::fopen('%s', '%s') -> 0x%llx (fake FILE, fd=%d)",
                     path.c_str(), mode.c_str(), f, fd);
            return f;
        });

        // fclose (uodLYyUip20#T#T)
        RegisterSymbol("libkernel", "uodLYyUip20#T#T", [](const GuestArgs& args) -> u64 {
            const u64 f = args.arg1;
            if (!f) return ~0ull;
            if (IsFakeFile(f)) {
                const int fd = FakeFileFd(f);
                DestroyFakeFile(f);
                if (fd >= 0) UntrackGuestFd(fd);
                LOG_DEBUG(HLE, "libkernel::fclose(0x%llx) -> 0 (fake FILE)", f);
                return 0;
            }
            int r = fclose(reinterpret_cast<FILE*>(f));
            LOG_DEBUG(HLE, "libkernel::fclose(%p) -> %d", reinterpret_cast<void*>(f), r);
            return (u64)(s64)r;
        });

        // fread (lbB+UlZqVG0#T#T)
        RegisterSymbol("libkernel", "lbB+UlZqVG0#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t buf = args.arg1;
            u64 size  = args.arg2;
            u64 count = args.arg3;
            u64 f     = args.arg4;
            if (!f || !buf || !size || !count) return 0;
            const int fd = FakeFileFd(f);
            if (fd >= 0) {
                const u64 n = static_cast<u64>(KernelReadCore(fd, buf, size * count));
                const u64 items = (size > 0) ? (n / size) : 0;
                LOG_ERROR(HLE, "libkernel::fread(buf: 0x%llx, size: %llu, count: %llu) -> read bytes: %llu, returning items: %llu", buf, size, count, n, items);
                return items;
            }
            u64 n = fread(reinterpret_cast<void*>(buf), size, count, reinterpret_cast<FILE*>(f));
            LOG_ERROR(HLE, "libkernel::fread(buf: 0x%llx, size: %llu, count: %llu) -> read items: %llu", buf, size, count, n);
            return n;
        });

        // fseek (rQFVBXp-Cxg#T#T)
        RegisterSymbol("libkernel", "rQFVBXp-Cxg#T#T", [](const GuestArgs& args) -> u64 {
            u64 f      = args.arg1;
            s64 offset = static_cast<s64>(args.arg2);
            int whence = static_cast<int>(args.arg3);
            const int fd = FakeFileFd(f);
            if (fd >= 0) {
                const s64 r = _lseeki64(fd, offset, whence);
                LOG_ERROR(HLE, "libkernel::fseek(0x%llx, %lld, %d) -> %lld", f, offset, whence, r);
                return r < 0 ? ~0ull : 0;
            }
            int r = f ? fseek(reinterpret_cast<FILE*>(f), (long)offset, whence) : -1;
            LOG_ERROR(HLE, "libkernel::fseek(%p, %lld, %d) -> %d", reinterpret_cast<void*>(f), offset, whence, r);
            return (u64)(s64)r;
        });

        // ftell (Qazy8LmXTvw#T#T)
        RegisterSymbol("libkernel", "Qazy8LmXTvw#T#T", [](const GuestArgs& args) -> u64 {
            u64 f = args.arg1;
            const int fd = FakeFileFd(f);
            if (fd >= 0) {
                const s64 r = _lseeki64(fd, 0, SEEK_CUR);
                LOG_ERROR(HLE, "libkernel::ftell(0x%llx) -> %lld", f, r);
                return static_cast<u64>(r);
            }
            long r = f ? ftell(reinterpret_cast<FILE*>(f)) : -1;
            LOG_ERROR(HLE, "libkernel::ftell(%p) -> %ld", reinterpret_cast<void*>(f), r);
            return (u64)(s64)r;
        });

        // fwrite (unresolved, common enough to pre-register)
        RegisterSymbol("libkernel", "fwrite#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t buf = args.arg1;
            u64 size  = args.arg2;
            u64 count = args.arg3;
            if (!buf) return 0;
            
            // Do NOT call host fwrite with guest FILE*!
            // Just read the guest string and print it to the emulator log if it's text.
            u64 total_bytes = size * count;
            if (total_bytes > 0 && total_bytes < 10000) {
                if (Memory::IsReadable(buf, total_bytes)) {
                    std::string s(total_bytes, '\0');
                    Memory::ReadBuffer(buf, s.data(), total_bytes);
                    LOG_INFO(HLE, "[GUEST_FWRITE]: %s", s.c_str());
                }
            }
            return count; // Pretend we wrote everything
        });

        // fflush
        RegisterSymbol("libc", "fflush", [](const GuestArgs&) -> u64 { return 0; });
        RegisterSymbol("libc", "6LDBEaH-R00", [](const GuestArgs&) -> u64 { return 0; }); // libc fflush NID
        RegisterSymbol("libc", "6LDBEaH-R00#T#T", [](const GuestArgs&) -> u64 { return 0; });

        // feof (always return 1/EOF for fake files)
        RegisterSymbol("libc", "feof", [](const GuestArgs&) -> u64 { return 1; });
        RegisterSymbol("libc", "LxcEU+ICu8U", [](const GuestArgs&) -> u64 { return 1; });
        RegisterSymbol("libc", "LxcEU+ICu8U#T#T", [](const GuestArgs&) -> u64 { return 1; });

        // fgets (always return 0/NULL for fake files)
        RegisterSymbol("libc", "fgets", [](const GuestArgs&) -> u64 { return 0; });
        RegisterSymbol("libc", "KdP-nULpuGw", [](const GuestArgs&) -> u64 { return 0; });
        RegisterSymbol("libc", "KdP-nULpuGw#T#T", [](const GuestArgs&) -> u64 { return 0; });

        // fgetc (always return -1/EOF for fake files)
        RegisterSymbol("libc", "fgetc", [](const GuestArgs&) -> u64 { return (u64)-1; });
        RegisterSymbol("libc", "w3S10hD3pAA", [](const GuestArgs&) -> u64 { return (u64)-1; });
        RegisterSymbol("libc", "w3S10hD3pAA#T#T", [](const GuestArgs&) -> u64 { return (u64)-1; });

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

        // -------------------------------------------------------------------
        // libc.prx internal file primitives.  The game's real libc.prx runs
        // its OWN fread/fseek/ftell/fclose natively against the fake FILE
        // struct we hand out from fopen; those functions call _read/_close/
        // lseek through their GOT slots, which must land here so the fd-based
        // kernel layer actually performs the I/O.  Without these the slots
        // resolve to auto-stubs returning 0 and every read yields no data.
        // -------------------------------------------------------------------
        RegisterSymbol("libkernel", "DRuBt2pvICk", [](const GuestArgs& args) -> u64 {   // _read
            return static_cast<u64>(KernelReadCore(static_cast<int>(args.arg1), args.arg2, args.arg3));
        });
        RegisterSymbol("libkernel", "FxVZqBAA7ks", [](const GuestArgs& args) -> u64 {   // _write
            return static_cast<u64>(KernelWriteCore(static_cast<int>(args.arg1), args.arg2, args.arg3));
        });
        RegisterSymbol("libkernel", "_write", PosixWrite);   // name variant too
        RegisterSymbol("libkernel", "NNtFaKJbPt0", [](const GuestArgs& args) -> u64 {   // _close
            return static_cast<u64>(KernelCloseCore(static_cast<int>(args.arg1)));
        });
        RegisterSymbol("libkernel", "Oy6IpwgtYOk", [](const GuestArgs& args) -> u64 {   // lseek
            const int fd     = static_cast<int>(args.arg1);
            const s64 offset = static_cast<s64>(args.arg2);
            const int whence = static_cast<int>(args.arg3);
            const s64 r = _lseeki64(fd, offset, whence);
            return static_cast<u64>(r);
        });

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
        auto PthreadSelfImpl = [](const GuestArgs& /*args*/) -> u64 {
            u64 tid = Kernel::GetCurrentThreadId();
            LOG_DEBUG(HLE, "scePthreadSelf() -> 0x%llx", tid);
            return tid;
        };
        RegisterSymbol("libkernel", "aI+OeCz8xrQ#T#T", PthreadSelfImpl);
        RegisterSymbol("libkernel", "pthread_self", PthreadSelfImpl);
        RegisterSymbol("libkernel", "scePthreadSelf", PthreadSelfImpl);

        // scePthreadYield (T72hz6ffq08#T#T) — no-op in single-threaded mode
        // (real yield implementation registered later with pthread_yield).

        // pthread_equal (vXg9dK7/4qM#T#T) — compare thread ids.
        auto PthreadEqualImpl = [](const GuestArgs& args) -> u64 {
            return args.arg1 == args.arg2 ? 1 : 0;
        };
        RegisterSymbol("libkernel", "pthread_equal", PthreadEqualImpl);
        RegisterSymbol("libkernel", "scePthreadEqual", PthreadEqualImpl);

        // pthread_getthreadid_np (MNFAlmXy+uY#T#T) — current thread id.
        auto PthreadGetThreadIdImpl = [](const GuestArgs& /*args*/) -> u64 {
            return Kernel::GetCurrentThreadId();
        };
        RegisterSymbol("libkernel", "pthread_getthreadid_np", PthreadGetThreadIdImpl);
        RegisterSymbol("libkernel", "scePthreadGetthreadid", PthreadGetThreadIdImpl);

        // scePthreadCreate (6UgtwV+0zb4#T#T) — spawns a guest thread via the
        // CpuCore registry (reached through the Kernel:: thread API).
        // Honors the CpuConfig max_guest_threads limit to avoid race conditions
        // in the Construct runtime's multi-threaded JSON parser (PPSA02929).
        auto PthreadCreateImpl = [ResolveAttrPtr](const GuestArgs& args) -> u64 {
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

            // Thread count limit: if max_guest_threads > 0, count active
            // guest threads and reject new ones past the limit.  The game
            // may retry or continue without the thread.
            {
                const int max_threads = ConfigService::EffectiveFor("").cpu.max_guest_threads;
                if (max_threads > 0) {
                    const int active = CpuCore::ActiveThreadCount();
                    if (active >= max_threads) {
                        LOG_INFO(HLE, "scePthreadCreate(NID): thread limit %d reached (%d active), "
                                 "rejecting '%s' (EAGAIN)", max_threads, active, name.c_str());
                        return 11; // EAGAIN
                    }
                }
            }

            u64 kGuestStackSize = 2 * 1024 * 1024; // default 2MB
            if (attr_ptr) {
                guest_addr_t slot = ResolveAttrPtr(attr_ptr);
                if (slot) {
                    u64 requested = Memory::Read<u64>(slot + 8);
                    if (requested > 0 && requested < 256 * 1024 * 1024) {
                        kGuestStackSize = requested;
                    }
                }
            }
            kGuestStackSize = (kGuestStackSize + 0xFFF) & ~0xFFFULL; // align to page
            
            void* guest_stack = VirtualAlloc(nullptr, kGuestStackSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!guest_stack) {
                LOG_ERROR(HLE, "scePthreadCreate(NID): VirtualAlloc failed for guest stack");
                return 11; // EAGAIN
            }
            u64 stack_base = reinterpret_cast<u64>(guest_stack);
            // Register the thread stack so Query/fault classification see it.
            Memory::AdoptRange(stack_base, kGuestStackSize, Memory::PROT_READ | Memory::PROT_WRITE,
                               /*committed=*/true, Memory::Owner::Kernel, "thread-stack");

            // Orbis thread-pointer layout: fs:[0] yields the tp (self-pointer
            // stored at tp) and libc/CRT data lives at NEGATIVE offsets from
            // tp (seen at least down to tp-0x1648).  Mirror the main thread's
            // block (Kernel::Initialize), which leaves 0x10000 below the tp.
            constexpr u64 kTlsHeadroom = 0x10000;
            constexpr u64 kTlsSize = 0x4000;
            void* tls_block = VirtualAlloc(nullptr, kTlsHeadroom + kTlsSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (tls_block) {
                Memory::AdoptRange(reinterpret_cast<u64>(tls_block),
                                   kTlsHeadroom + kTlsSize,
                                   Memory::PROT_READ | Memory::PROT_WRITE,
                                   /*committed=*/true, Memory::Owner::Kernel,
                                   "thread-tls");
            }
            u64 tls_base = reinterpret_cast<u64>(tls_block) + kTlsHeadroom;
            if (tls_block) {
                // Self-pointer at tp[0] (FreeBSD TCB convention)
                *reinterpret_cast<u64*>(tls_base) = tls_base;
                // Seed additional TLS slots (SharpEmu SeedTlsLayout compat)
                if (*reinterpret_cast<u64*>(tls_base + 16) == 0) {
                    *reinterpret_cast<u64*>(tls_base + 16) = tls_base;
                }
                *reinterpret_cast<u64*>(tls_base + 40) = 0xC0C0C0C0C0C0C0BEULL;
                *reinterpret_cast<u64*>(tls_base + 96) = tls_base;
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
            LOG_INFO(HLE, "scePthreadCreate(NID)(entry=0x%llx, arg=0x%llx, name='%s') -> tid=%llu (stack_size=%llu)",
                     entry_ptr, start_arg, name.c_str(), tid, kGuestStackSize);
            return 0;
        };
        RegisterSymbol("libkernel", "6UgtwV+0zb4#T#T", PthreadCreateImpl);
        RegisterSymbol("libkernel", "pthread_create", PthreadCreateImpl);
        RegisterSymbol("libkernel", "pthread_create_name_np", PthreadCreateImpl);

        // scePthreadJoin (onNY9Byn-W8#S#N) — waits on the CpuCore-registered thread
        auto PthreadJoinImpl = [](const GuestArgs& args) -> u64 {
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
        };
        RegisterSymbol("libkernel", "onNY9Byn-W8#S#N", PthreadJoinImpl);
        RegisterSymbol("libkernel", "pthread_join", PthreadJoinImpl);

        // scePthreadMutex/Cond init/lock/unlock/destroy and the mutex-attr NIDs
        // hit by PPSA01668 (F8bUHwAG284, iMp8QpE+XO4, 1FGvU0i9saQ, cmo1RIYva9o,
        // smWEktiyyG0, 188x57JYp0g) resolve via the NID-database friendly-name
        // bridge to the real implementations in src/hle/libkernel_sync.cpp.

        // =====================================================================
        // String utilities
        // =====================================================================
        // strncpy (6sJWiWSRuqk#T#T)
        // strncpy (6sJWiWSRuqk#T#T)
        auto StrncpyImpl = [](const GuestArgs& args) -> u64 {
            guest_addr_t dst = args.arg1, src = args.arg2;
            u64 n = args.arg3;
            if (dst && n > 0 && n < 0x10000000ULL) {
                Memory::GuardedStrncpy(dst, src, n);
            }
            return dst;
        };
        RegisterSymbol("libkernel", "6sJWiWSRuqk#T#T", StrncpyImpl);
        RegisterSymbol("libkernel", "strncpy#T#T", StrncpyImpl);

        // strcpy
        auto StrcpyImpl = [](const GuestArgs& args) -> u64 {
            guest_addr_t dst = args.arg1, src = args.arg2;
            if (dst && src) {
                Memory::GuardedStrcpy(dst, src);
            }
            return dst;
        };
        RegisterSymbol("libkernel", "strcpy#T#T", StrcpyImpl);
        RegisterSymbol("libkernel", "kiZSXIWd9vg#T#T", StrcpyImpl); // libc strcpy NID

        // strcat (Ls4tzzhimqQ#T#T)
        auto StrcatImpl = [](const GuestArgs& args) -> u64 {
            guest_addr_t dst = args.arg1, src = args.arg2;
            if (dst && src) {
                u64 dst_len = Memory::GuardedStrlen(dst, UINT64_MAX);
                Memory::GuardedStrcpy(dst + dst_len, src);
            }
            return dst;
        };
        RegisterSymbol("libkernel", "strcat#T#T", StrcatImpl);
        RegisterSymbol("libkernel", "Ls4tzzhimqQ#T#T", StrcatImpl);

        // strlen — plain-name alias of j4ViWNHEgww#T#T
        RegisterSymbol("libkernel", "strlen#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t str = args.arg1;
            if (!str) return 0;
            return Memory::GuardedStrlen(str, UINT64_MAX);
        });

        // memcpy — plain-name alias of Q3VBxCXhUHs#T#T
        RegisterSymbol("libkernel", "memcpy#T#T", MemcpyImpl);

        // memmove — plain-name alias of +P6FRGH4LfA#T#T
        RegisterSymbol("libkernel", "memmove#T#T", MemmoveImpl);

        // memset — plain-name alias of 8zTFvBIAIN8#T#T
        RegisterSymbol("libkernel", "memset#T#T", MemsetImpl);

        // strcmp
        auto StrcmpImpl = [](const GuestArgs& args) -> u64 {
            guest_addr_t a = args.arg1, b = args.arg2;
            if (!a || !b) return (u64)(s64)-1;
            return static_cast<u64>(static_cast<s64>(Memory::GuardedStrcmp(a, b)));
        };
        RegisterSymbol("libkernel", "strcmp#T#T", StrcmpImpl);

        // strncmp (aesyjrHVWy4#T#T)
        auto StrncmpImpl = [](const GuestArgs& args) -> u64 {
            guest_addr_t a = args.arg1, b = args.arg2;
            u64 n = args.arg3;
            if (!a || !b || n == 0) return 0;
            return static_cast<u64>(static_cast<s64>(Memory::GuardedStrncmp(a, b, n)));
        };
        RegisterSymbol("libkernel", "strncmp#T#T", StrncmpImpl);
        RegisterSymbol("libkernel", "aesyjrHVWy4#T#T", StrncmpImpl);

        // AV6ipCNa4Rw = strcasecmp
        RegisterSymbol("libkernel", "AV6ipCNa4Rw#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t a = args.arg1, b = args.arg2;
            if (!a || !b) return (u64)(s64)-1;
            if (Memory::IsReadable(a, 1) && Memory::IsReadable(b, 1)) {
                int cmp = _stricmp(reinterpret_cast<const char*>(a), reinterpret_cast<const char*>(b));
                return (u64)(s64)cmp;
            }
            return (u64)(s64)-1;
        });

        // sprintf (g7zzzLDYGw0#T#T) — real implementation (SysV varargs via
        // dispatcher-captured registers + guest stack overflow args).
        RegisterSymbol("libkernel", "g7zzzLDYGw0#T#T", [](const GuestArgs& args) -> u64 {
            return GuestSprintf(args);
        });

        // pthread_yield (B5GmVDKwpn0) — yields execution to other ready threads
        auto PthreadYieldImpl = [](const GuestArgs& /*args*/) -> u64 {
            std::this_thread::yield();
            return 0;
        };
        RegisterSymbol("libkernel", "pthread_yield", PthreadYieldImpl);
        RegisterSymbol("libkernel", "B5GmVDKwpn0", PthreadYieldImpl);
        RegisterSymbol("libkernel", "T72hz6ffq08#T#T", PthreadYieldImpl);
        RegisterSymbol("libkernel", "scePthreadYield", PthreadYieldImpl);

        // RandomExports HLE (sceRandomGetRandomNumber / hardware RNG fallback)
        auto RandomGetRandomNumberImpl = [](const GuestArgs& args) -> u64 {
            const guest_addr_t buf = args.arg1;
            const u64 size = args.arg2;
            if (!buf || size == 0) return 0;
            static std::mt19937_64 rng(std::random_device{}());
            u8* p = reinterpret_cast<u8*>(buf);
            u64 offset = 0;
            while (offset < size) {
                u64 val = rng();
                size_t chunk = std::min<size_t>(sizeof(val), size - offset);
                std::memcpy(p + offset, &val, chunk);
                offset += chunk;
            }
            return 0;
        };
        for (const char* mod : {"libkernel", "libSceRandom", "libSceRng"}) {
            RegisterSymbol(mod, "sceRandomGetRandomNumber", RandomGetRandomNumberImpl);
            RegisterSymbol(mod, "sceKernelGetRandomNumber", RandomGetRandomNumberImpl);
        }

        // =====================================================================
        // Semaphore symbols (sceKernelCreate/Wait/Signal/Poll/DeleteSema) are
        // real implementations in src/hle/libkernel_sync.cpp.
        // =====================================================================

        // =====================================================================
        // Misc stubs — return success/0 for unresolved PS5-specific functions
        // These will be updated as we understand them better
        // =====================================================================
        // The fallback stubs have been removed because they maliciously shadow
        // real HLE implementations. Any truly unimplemented NIDs will naturally
        // fall through to the HLE subsystem's Resolve() auto-stub mechanism.

        auto FallbackStub = [](const GuestArgs& args) -> u64 {
            LOG_INFO(HLE, "unknown::stub(args: 0x%llx, 0x%llx, 0x%llx)", args.arg1, args.arg2, args.arg3);
            return 0;
        };
        RegisterSymbol("unknown", "amuBfI-AQc4", FallbackStub);
        RegisterSymbol("unknown", "SUEVes8gvmw", FallbackStub);
        RegisterSymbol("unknown", "6PBNpsgyaxw", FallbackStub);
        RegisterSymbol("unknown", "JT+t00a3TxA", FallbackStub);
        RegisterSymbol("unknown", "dolOmWH+huQ", FallbackStub);
        RegisterSymbol("unknown", "fd5Bp5tGTgo", FallbackStub);
        RegisterSymbol("unknown", "mPpPxv5CZt4", FallbackStub);
        RegisterSymbol("unknown", "sk54bi6FtYM", FallbackStub);
        RegisterSymbol("unknown", "pDuPEf3m4fI", FallbackStub);
        RegisterSymbol("unknown", "whrS4oksXc4", FallbackStub);
        RegisterSymbol("unknown", "oM+XCzVG3oM", FallbackStub);
        RegisterSymbol("unknown", "n590hj5Oe-k", FallbackStub);
        RegisterSymbol("unknown", "SsRbbCiWoGw", FallbackStub);
        RegisterSymbol("unknown", "mz2iTY0MK4A", FallbackStub);
        RegisterSymbol("unknown", "CUKn5pX-NVY", FallbackStub);
        RegisterSymbol("unknown", "0GnN4QCgIfs", FallbackStub);
        RegisterSymbol("unknown", "Sygnk9dr5WQ", FallbackStub);
        RegisterSymbol("unknown", "3RQ5aQfnstU", FallbackStub);
    }
}
// namespace HLE
