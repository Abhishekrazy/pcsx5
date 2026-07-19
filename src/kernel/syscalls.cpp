#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#include "syscalls.h"
#include "kernel.h"
#include "fd_table.h"
#include "guest_clock.h"
#include "memory.h"
#include "thread.h"
#include "../memory/memory.h"
#include "../hle/hle.h"
#include "../common/log.h"
#include <windows.h>
#include <iostream>
#include <vector>
#include <string>
#include <unordered_map>
#include <mutex>

// Undefine standard error macros to avoid conflict with our constexpr definitions
#undef EPERM
#undef ENOENT
#undef ESRCH
#undef EINTR
#undef EIO
#undef EBADF
#undef ENOMEM
#undef EACCES
#undef EFAULT
#undef EEXIST
#undef EINVAL
#undef EMFILE
#undef ENOSYS
#undef EAGAIN
#undef ETIMEDOUT
#undef ENOTSOCK

namespace Kernel {

// Unix compat error codes
constexpr int EPERM    = 1;
constexpr int ENOENT   = 2;
constexpr int ESRCH    = 3;
constexpr int EINTR    = 4;
constexpr int EIO      = 5;
constexpr int EBADF    = 9;
constexpr int ENOMEM   = 12;
constexpr int EACCES   = 13;
constexpr int EFAULT   = 14;
constexpr int EEXIST   = 17;
constexpr int EINVAL   = 22;
constexpr int EMFILE   = 24;
constexpr int ENOSYS   = 38;
constexpr int EAGAIN   = 35;
constexpr int ETIMEDOUT = 60;
constexpr int ENOTSOCK = 38;

struct timespec {
    s64 tv_sec;
    s64 tv_nsec;
};

static SyscallHandler g_syscall_table[512] = {};

// Safe guest memory access helpers using Structured Exception Handling (SEH)
static bool ReadGuestString(guest_addr_t addr, char* dest, size_t max_size) {
    __try {
        size_t i = 0;
        while (i < max_size - 1) {
            char c = Memory::Read<char>(addr + i);
            dest[i] = c;
            if (c == '\0') {
                return true;
            }
            i++;
        }
        dest[max_size - 1] = '\0';
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeReadBuffer(guest_addr_t addr, void* dest, u64 size) {
    __try {
        Memory::ReadBuffer(addr, dest, size);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeWriteBuffer(guest_addr_t addr, const void* src, u64 size) {
    __try {
        Memory::WriteBuffer(addr, src, size);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void InitializeSyscallTable() {
    // Initialize Winsock
    WSADATA wsaData;
    ::WSAStartup(MAKEWORD(2, 2), &wsaData);

    // FreeBSD / PS5 Guest Syscall mappings
    g_syscall_table[1] = [](CONTEXT* ctx) -> s64 { return SysExit(static_cast<s32>(ctx->Rdi), ctx); };
    g_syscall_table[3] = [](CONTEXT* ctx) -> s64 { return SysRead(ctx->Rdi, ctx->Rsi, ctx->Rdx, ctx); };
    g_syscall_table[4] = [](CONTEXT* ctx) -> s64 { return SysWrite(ctx->Rdi, ctx->Rsi, ctx->Rdx, ctx); };
    g_syscall_table[5] = [](CONTEXT* ctx) -> s64 { return SysOpen(ctx->Rdi, static_cast<u32>(ctx->Rsi), static_cast<u32>(ctx->Rdx), ctx); };
    g_syscall_table[6] = [](CONTEXT* ctx) -> s64 { return SysClose(ctx->Rdi, ctx); };
    g_syscall_table[20] = [](CONTEXT* ctx) -> s64 { return SysGetpid(ctx); };
    g_syscall_table[45] = [](CONTEXT* ctx) -> s64 { return SysBrk(ctx->Rdi, ctx); };
    g_syscall_table[54] = [](CONTEXT* ctx) -> s64 { return SysIoctl(ctx->Rdi, static_cast<u32>(ctx->Rsi), ctx->Rdx, ctx); };
    g_syscall_table[92] = [](CONTEXT* ctx) -> s64 { return SysFcntl(ctx->Rdi, static_cast<u32>(ctx->Rsi), ctx->Rdx, ctx); };
    g_syscall_table[116] = [](CONTEXT* ctx) -> s64 { return SysGettimeofday(ctx->Rdi, ctx->Rsi, ctx); };
    g_syscall_table[188] = [](CONTEXT* ctx) -> s64 { return SysStat(ctx->Rdi, ctx->Rsi, ctx); };
    g_syscall_table[189] = [](CONTEXT* ctx) -> s64 { return SysFstat(ctx->Rdi, ctx->Rsi, ctx); };
    g_syscall_table[199] = [](CONTEXT* ctx) -> s64 { return SysLseek(ctx->Rdi, static_cast<s64>(ctx->Rsi), static_cast<u32>(ctx->Rdx), ctx); };
    g_syscall_table[240] = [](CONTEXT* ctx) -> s64 { return SysNanosleep(ctx->Rdi, ctx->Rsi, ctx); };
    g_syscall_table[370] = [](CONTEXT* ctx) -> s64 { return SysThrCreate(ctx->Rdi, ctx->Rsi, ctx->Rdx, ctx->R10, static_cast<u32>(ctx->R8), ctx->R9, 0, ctx); };
    g_syscall_table[371] = [](CONTEXT* ctx) -> s64 { return SysThrExit(static_cast<s32>(ctx->Rdi), ctx); };
    g_syscall_table[372] = [](CONTEXT* ctx) -> s64 { return SysThrSelf(ctx); };
    g_syscall_table[477] = [](CONTEXT* ctx) -> s64 { return SysMmap(ctx->Rdi, ctx->Rsi, static_cast<u32>(ctx->Rdx), static_cast<u32>(ctx->R10), static_cast<s64>(ctx->R8), static_cast<s64>(ctx->R9), ctx); };
    g_syscall_table[478] = [](CONTEXT* ctx) -> s64 { return SysMunmap(ctx->Rdi, ctx->Rsi, ctx); };
    g_syscall_table[479] = [](CONTEXT* ctx) -> s64 { return SysMprotect(ctx->Rdi, ctx->Rsi, static_cast<u32>(ctx->Rdx), ctx); };

    // Thread control
    g_syscall_table[373] = [](CONTEXT* ctx) -> s64 { return SysThrKill(ctx->Rdi, static_cast<s32>(ctx->Rsi), ctx); };
    g_syscall_table[374] = [](CONTEXT* ctx) -> s64 { return SysThrSuspend(ctx->Rdi, ctx); };
    g_syscall_table[375] = [](CONTEXT* ctx) -> s64 { return SysThrWake(ctx->Rdi, ctx); };

    // Clocks
    g_syscall_table[241] = [](CONTEXT* ctx) -> s64 { return SysClockGettime(static_cast<u32>(ctx->Rdi), ctx->Rsi, ctx); };
    g_syscall_table[242] = [](CONTEXT* ctx) -> s64 { return SysClockGetres(static_cast<u32>(ctx->Rdi), ctx->Rsi, ctx); };

    // File permissions and link status
    g_syscall_table[190] = [](CONTEXT* ctx) -> s64 { return SysLstat(ctx->Rdi, ctx->Rsi, ctx); };
    g_syscall_table[191] = [](CONTEXT* ctx) -> s64 { return SysAccess(ctx->Rdi, static_cast<u32>(ctx->Rsi), ctx); };
    g_syscall_table[192] = [](CONTEXT* ctx) -> s64 { return SysChmod(ctx->Rdi, static_cast<u32>(ctx->Rsi), ctx); };
    g_syscall_table[193] = [](CONTEXT* ctx) -> s64 { return SysFchmod(ctx->Rdi, static_cast<u32>(ctx->Rsi), ctx); };

    // Sockets
    g_syscall_table[97] = [](CONTEXT* ctx) -> s64 { return SysSocket(static_cast<s32>(ctx->Rdi), static_cast<s32>(ctx->Rsi), static_cast<s32>(ctx->Rdx), ctx); };
    g_syscall_table[98] = [](CONTEXT* ctx) -> s64 { return SysBind(ctx->Rdi, ctx->Rsi, static_cast<u32>(ctx->Rdx), ctx); };
    g_syscall_table[99] = [](CONTEXT* ctx) -> s64 { return SysListen(ctx->Rdi, static_cast<s32>(ctx->Rsi), ctx); };
    g_syscall_table[100] = [](CONTEXT* ctx) -> s64 { return SysAccept(ctx->Rdi, ctx->Rsi, ctx->Rdx, ctx); };
    g_syscall_table[101] = [](CONTEXT* ctx) -> s64 { return SysConnect(ctx->Rdi, ctx->Rsi, static_cast<u32>(ctx->Rdx), ctx); };
    g_syscall_table[102] = [](CONTEXT* ctx) -> s64 { return SysSendto(ctx->Rdi, ctx->Rsi, ctx->Rdx, static_cast<s32>(ctx->R10), ctx->R8, static_cast<u32>(ctx->R9), ctx); };
    g_syscall_table[103] = [](CONTEXT* ctx) -> s64 { return SysRecvfrom(ctx->Rdi, ctx->Rsi, ctx->Rdx, static_cast<s32>(ctx->R10), ctx->R8, ctx->R9, ctx); };

    // Semaphores
    g_syscall_table[410] = [](CONTEXT* ctx) -> s64 { return SysSemInit(ctx->Rdi, static_cast<s32>(ctx->Rsi), static_cast<u32>(ctx->Rdx), ctx); };
    g_syscall_table[411] = [](CONTEXT* ctx) -> s64 { return SysSemDestroy(ctx->Rdi, ctx); };
    g_syscall_table[412] = [](CONTEXT* ctx) -> s64 { return SysSemWait(ctx->Rdi, ctx); };
    g_syscall_table[413] = [](CONTEXT* ctx) -> s64 { return SysSemPost(ctx->Rdi, ctx); };
    g_syscall_table[414] = [](CONTEXT* ctx) -> s64 { return SysSemTrywait(ctx->Rdi, ctx); };
    g_syscall_table[415] = [](CONTEXT* ctx) -> s64 { return SysSemGetvalue(ctx->Rdi, ctx->Rsi, ctx); };

    // PS5 Specific / Extensions
    g_syscall_table[540] = [](CONTEXT* ctx) -> s64 { return SysKernControl(ctx); };
    g_syscall_table[541] = [](CONTEXT* ctx) -> s64 { return SysKernGetpid(ctx); };
    g_syscall_table[542] = [](CONTEXT* ctx) -> s64 { return SysKernGettid(ctx); };

    // Syscall audit (Phase 2): a block of stub-with-log entries for
    // commonly-hit Orbis numbers (wait4/getuid/madvise/sigaction/kqueue/
    // kevent/…) was implemented here but REVERTED: its mere presence made
    // syscall_validation_tests die at process exit with 0xC0000409
    // (fastfail) when stdout is a pipe, although none of the stubs ever ran.
    // Root cause is a layout-sensitive latent exit-time bug elsewhere —
    // re-add the stubs once that is root-caused (see task report).
}

void RegisterSyscallHandler(u32 syscall_number, SyscallHandler handler) {
    if (syscall_number < 512) {
        g_syscall_table[syscall_number] = handler;
    }
}

s64 HandleSyscall(u32 syscall_number, CONTEXT* context) {
    if (syscall_number >= 512) {
        LOG_WARN(Kernel, "Unknown syscall number: %u", syscall_number);
        return -ENOSYS;
    }
    
    SyscallHandler handler = g_syscall_table[syscall_number];
    if (!handler) {
        LOG_WARN(Kernel, "Unimplemented syscall: %u", syscall_number);
        return -ENOSYS;
    }

    static bool init_breaks = false;
    static int break_sys_num = -1;
    if (!init_breaks) {
        init_breaks = true;
        const char* env = std::getenv("PCSX5_BREAK_SYSCALL");
        if (env) {
            break_sys_num = std::atoi(env);
            LOG_INFO(Kernel, "Configured breakpoint on syscall %d", break_sys_num);
        }
    }
    if (break_sys_num == static_cast<int>(syscall_number)) {
        LOG_INFO(Kernel, "Debugger breakpoint hit on syscall %u!", syscall_number);
#ifdef _WIN32
        DebugBreak();
#endif
    }
    
    LOG_DEBUG(Kernel, "Handling syscall %u (args: Rdi=0x%llx, Rsi=0x%llx, Rdx=0x%llx, R10=0x%llx, R8=0x%llx, R9=0x%llx)",
              syscall_number, context->Rdi, context->Rsi, context->Rdx, context->R10, context->R8, context->R9);
              
    s64 result = handler(context);
    
    LOG_DEBUG(Kernel, "Syscall %u returned: %lld (0x%llx)", syscall_number, result, result);
    return result;
}

s64 SysExit(s32 status, CONTEXT*) {
    LOG_INFO(Kernel, "sys_exit(status=%d)", status);
    if (status != 0x999999) {
        ExitProcess(static_cast<UINT>(status));
    }
    return 0;
}

s64 SysRead(s64 fd, guest_addr_t buf, u64 count, CONTEXT*) {
    LOG_DEBUG(Kernel, "sys_read(fd=%lld, buf=0x%llx, count=%llu)", fd, buf, count);
    if (!IsValidFd(static_cast<int>(fd))) {
        return -EBADF;
    }
    FdEntry* entry = GetFd(static_cast<int>(fd));
    if (!(entry->flags & O_RDWR) && (entry->flags & O_WRONLY)) {
        return -EBADF;
    }
    if (fd == 0) {
        return 0;
    }
    if (entry->type == FD_TYPE_FILE && entry->handle != nullptr) {
        std::vector<char> buffer(count);
        DWORD bytesRead = 0;
        LARGE_INTEGER li;
        li.QuadPart = entry->offset;
        if (!SetFilePointerEx(static_cast<HANDLE>(entry->handle), li, nullptr, FILE_BEGIN)) {
            return -EIO;
        }
        if (!ReadFile(static_cast<HANDLE>(entry->handle), buffer.data(), static_cast<DWORD>(count), &bytesRead, nullptr)) {
            return -EIO;
        }
        if (bytesRead > 0) {
            if (!SafeWriteBuffer(buf, buffer.data(), bytesRead)) {
                return -EFAULT;
            }
            entry->offset += bytesRead;
        }
        return static_cast<s64>(bytesRead);
    }
    LOG_WARN(Kernel, "sys_read: Reading from fd %lld not fully implemented", fd);
    return 0;
}

s64 SysWrite(s64 fd, guest_addr_t buf, u64 count, CONTEXT*) {
    LOG_DEBUG(Kernel, "sys_write(fd=%lld, buf=0x%llx, count=%llu)", fd, buf, count);
    if (fd == 1 || fd == 2) {
        std::vector<char> buffer(count);
        if (!SafeReadBuffer(buf, buffer.data(), count)) {
            return -EFAULT;
        }
        std::string output(buffer.data(), count);
        if (fd == 1) {
            std::cout << output;
            std::flush(std::cout);
        } else {
            std::cerr << output;
            std::flush(std::cerr);
        }
        return static_cast<s64>(count);
    }
    if (!IsValidFd(static_cast<int>(fd))) {
        return -EBADF;
    }
    FdEntry* entry = GetFd(static_cast<int>(fd));
    if (!(entry->flags & O_RDWR) && !(entry->flags & O_WRONLY)) {
        return -EBADF;
    }
    if (entry->type == FD_TYPE_FILE && entry->handle != nullptr) {
        std::vector<char> buffer(count);
        if (!SafeReadBuffer(buf, buffer.data(), count)) {
            return -EFAULT;
        }
        DWORD bytesWritten = 0;
        LARGE_INTEGER li;
        li.QuadPart = entry->offset;
        if (!SetFilePointerEx(static_cast<HANDLE>(entry->handle), li, nullptr, FILE_BEGIN)) {
            return -EIO;
        }
        if (!WriteFile(static_cast<HANDLE>(entry->handle), buffer.data(), static_cast<DWORD>(count), &bytesWritten, nullptr)) {
            return -EIO;
        }
        entry->offset += bytesWritten;
        return static_cast<s64>(bytesWritten);
    }
    LOG_WARN(Kernel, "sys_write: Writing to fd %lld not fully implemented", fd);
    return static_cast<s64>(count);
}

s64 SysOpen(guest_addr_t pathname, u32 flags, u32 mode, CONTEXT*) {
    char path[4096];
    if (!ReadGuestString(pathname, path, sizeof(path))) {
        return -EFAULT;
    }
    const std::string host_path = TranslateGuestPath(path);
    LOG_INFO(Kernel, "sys_open: Opening '%s' with flags=0x%x, mode=0%o", path, flags, mode);
    
    DWORD desiredAccess = 0;
    if ((flags & 3) == O_RDONLY) desiredAccess = GENERIC_READ;
    else if ((flags & 3) == O_WRONLY) desiredAccess = GENERIC_WRITE;
    else if ((flags & 3) == O_RDWR) desiredAccess = GENERIC_READ | GENERIC_WRITE;
    
    DWORD creationDisposition = OPEN_EXISTING;
    if (flags & O_CREAT) {
        if (flags & O_EXCL) {
            creationDisposition = CREATE_NEW;
        } else if (flags & O_TRUNC) {
            creationDisposition = CREATE_ALWAYS;
        } else {
            creationDisposition = OPEN_ALWAYS;
        }
    } else if (flags & O_TRUNC) {
        creationDisposition = TRUNCATE_EXISTING;
    }
    
    HANDLE hFile = CreateFileA(
        host_path.c_str(),
        desiredAccess,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        creationDisposition,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    
    if (hFile == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) return -ENOENT;
        if (err == ERROR_ACCESS_DENIED) return -EACCES;
        if (err == ERROR_ALREADY_EXISTS || err == ERROR_FILE_EXISTS) return -EEXIST;
        return -EIO;
    }
    
    int fd = AllocateFd(FD_TYPE_FILE, hFile, flags, mode, path);
    if (fd == -1) {
        CloseHandle(hFile);
        return -EMFILE;
    }
    
    LOG_INFO(Kernel, "sys_open: Opened '%s' as fd=%d", path, fd);
    return fd;
}

s64 SysClose(s64 fd, CONTEXT*) {
    LOG_DEBUG(Kernel, "sys_close(fd=%lld)", fd);
    if (fd <= 2) {
        return -EBADF;
    }
    if (!CloseFd(static_cast<int>(fd))) {
        return -EBADF;
    }
    LOG_INFO(Kernel, "sys_close: Closed fd=%lld", fd);
    return 0;
}

s64 SysGetpid(CONTEXT*) {
    LOG_DEBUG(Kernel, "sys_getpid()");
    return static_cast<s64>(GetCurrentProcessId());
}

s64 SysBrk(guest_addr_t addr, CONTEXT*) {
    LOG_DEBUG(Kernel, "sys_brk(addr=0x%llx)", addr);
    if (addr == 0) {
        return static_cast<s64>(GetBreak());
    }
    guest_addr_t new_brk = SetBreak(addr);
    return static_cast<s64>(new_brk);
}

s64 SysIoctl(s64 fd, u32 request, guest_addr_t argp, CONTEXT*) {
    LOG_DEBUG(Kernel, "sys_ioctl(fd=%lld, request=0x%x, argp=0x%llx)", fd, request, argp);
    if (!IsValidFd(static_cast<int>(fd))) {
        return -EBADF;
    }
    LOG_WARN(Kernel, "sys_ioctl: request 0x%x not implemented, returning 0", request);
    return 0;
}

s64 SysFcntl(s64 fd, u32 cmd, guest_addr_t arg, CONTEXT*) {
    LOG_DEBUG(Kernel, "sys_fcntl(fd=%lld, cmd=%u, arg=0x%llx)", fd, cmd, arg);
    if (!IsValidFd(static_cast<int>(fd))) {
        return -EBADF;
    }
    if (cmd == 3) {
        return GetFdFlags(static_cast<int>(fd));
    } else if (cmd == 4) {
        SetFdFlags(static_cast<int>(fd), static_cast<int>(arg));
        return 0;
    }
    LOG_WARN(Kernel, "sys_fcntl: cmd %u not implemented", cmd);
    return 0;
}

s64 SysGettimeofday(guest_addr_t tv, guest_addr_t tz, CONTEXT*) {
    LOG_DEBUG(Kernel, "sys_gettimeofday(tv=0x%llx, tz=0x%llx)", tv, tz);
    if (tv != 0) {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        ULARGE_INTEGER ul;
        ul.LowPart = ft.dwLowDateTime;
        ul.HighPart = ft.dwHighDateTime;
        
        u64 unix_time_100ns = ul.QuadPart - 116444736000000000ULL;
        s64 tv_sec = static_cast<s64>(unix_time_100ns / 10000000ULL);
        s64 tv_usec = static_cast<s64>((unix_time_100ns % 10000000ULL) / 10ULL);
        
        struct {
            s64 tv_sec;
            s64 tv_usec;
        } tv_struct = { tv_sec, tv_usec };
        
        if (!SafeWriteBuffer(tv, &tv_struct, sizeof(tv_struct))) {
            return -EFAULT;
        }
    }
    return 0;
}

s64 SysStat(guest_addr_t pathname, guest_addr_t statbuf, CONTEXT*) {
    LOG_DEBUG(Kernel, "sys_stat(pathname=0x%llx, statbuf=0x%llx)", pathname, statbuf);
    char path[4096];
    if (!ReadGuestString(pathname, path, sizeof(path))) {
        return -EFAULT;
    }
    LOG_INFO(Kernel, "sys_stat: Stat'ing '%s'", path);
    const std::string host_path = TranslateGuestPath(path);
    
    struct {
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
    } st = {};
    
    DWORD attribs = GetFileAttributesA(host_path.c_str());
    if (attribs == INVALID_FILE_ATTRIBUTES) {
        return -ENOENT;
    }
    
    st.st_dev = 1;
    st.st_ino = 1;
    st.st_mode = ((attribs & FILE_ATTRIBUTE_DIRECTORY) ? 0040000 : 0100000) | 0644;
    st.st_nlink = 1;
    st.st_uid = 0;
    st.st_gid = 0;
    st.st_rdev = 0;
    
    HANDLE hFile = CreateFileA(host_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size;
        if (GetFileSizeEx(hFile, &size)) {
            st.st_size = size.QuadPart;
        }
        CloseHandle(hFile);
    }
    
    st.st_blksize = 4096;
    st.st_blocks = (st.st_size + 511) / 512;
    
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER ul;
    ul.LowPart = ft.dwLowDateTime;
    ul.HighPart = ft.dwHighDateTime;
    u64 unix_time_100ns = ul.QuadPart - 116444736000000000ULL;
    st.st_atime = static_cast<s64>(unix_time_100ns / 10000000ULL);
    st.st_mtime = st.st_atime;
    st.st_ctime = st.st_atime;
    
    if (!SafeWriteBuffer(statbuf, &st, sizeof(st))) {
        return -EFAULT;
    }
    return 0;
}

s64 SysFstat(s64 fd, guest_addr_t statbuf, CONTEXT*) {
    LOG_DEBUG(Kernel, "sys_fstat(fd=%lld, statbuf=0x%llx)", fd, statbuf);
    if (!IsValidFd(static_cast<int>(fd))) {
        return -EBADF;
    }
    FdEntry* entry = GetFd(static_cast<int>(fd));
    
    struct {
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
    } st = {};
    
    st.st_dev = 1;
    st.st_ino = 1;
    st.st_mode = 0100000 | 0644;
    st.st_nlink = 1;
    st.st_uid = 0;
    st.st_gid = 0;
    st.st_rdev = 0;
    st.st_blksize = 4096;
    
    if (entry->type == FD_TYPE_FILE && entry->handle != nullptr) {
        LARGE_INTEGER size;
        if (GetFileSizeEx(static_cast<HANDLE>(entry->handle), &size)) {
            st.st_size = size.QuadPart;
        }
    }
    st.st_blocks = (st.st_size + 511) / 512;
    
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER ul;
    ul.LowPart = ft.dwLowDateTime;
    ul.HighPart = ft.dwHighDateTime;
    u64 unix_time_100ns = ul.QuadPart - 116444736000000000ULL;
    st.st_atime = static_cast<s64>(unix_time_100ns / 10000000ULL);
    st.st_mtime = st.st_atime;
    st.st_ctime = st.st_atime;
    
    if (!SafeWriteBuffer(statbuf, &st, sizeof(st))) {
        return -EFAULT;
    }
    return 0;
}

s64 SysLseek(s64 fd, s64 offset, u32 whence, CONTEXT*) {
    LOG_DEBUG(Kernel, "sys_lseek(fd=%lld, offset=%lld, whence=%u)", fd, offset, whence);
    if (!IsValidFd(static_cast<int>(fd))) {
        return -EBADF;
    }
    FdEntry* entry = GetFd(static_cast<int>(fd));
    s64 new_offset = 0;
    
    if (whence == 0) {
        new_offset = offset;
    } else if (whence == 1) {
        new_offset = entry->offset + offset;
    } else if (whence == 2) {
        if (entry->type == FD_TYPE_FILE && entry->handle != nullptr) {
            LARGE_INTEGER size;
            if (GetFileSizeEx(static_cast<HANDLE>(entry->handle), &size)) {
                new_offset = size.QuadPart + offset;
            } else {
                new_offset = offset;
            }
        } else {
            new_offset = offset;
        }
    } else {
        return -EINVAL;
    }
    
    if (new_offset < 0) {
        return -EINVAL;
    }
    
    entry->offset = new_offset;
    return new_offset;
}

s64 SysNanosleep(guest_addr_t req, guest_addr_t rem, CONTEXT*) {
    LOG_DEBUG(Kernel, "sys_nanosleep(req=0x%llx, rem=0x%llx)", req, rem);
    if (req == 0) {
        return -EINVAL;
    }
    struct {
        s64 tv_sec;
        s64 tv_nsec;
    } req_ts;
    if (!SafeReadBuffer(req, &req_ts, sizeof(req_ts))) {
        return -EFAULT;
    }
    if (req_ts.tv_sec < 0 || req_ts.tv_nsec < 0 || req_ts.tv_nsec >= 1000000000) {
        return -EINVAL;
    }
    DWORD sleep_ms = static_cast<DWORD>(req_ts.tv_sec * 1000 + req_ts.tv_nsec / 1000000);
    if (sleep_ms == 0 && (req_ts.tv_sec > 0 || req_ts.tv_nsec > 0)) {
        sleep_ms = 1;
    }
    Sleep(sleep_ms);
    if (rem != 0) {
        struct {
            s64 tv_sec;
            s64 tv_nsec;
        } rem_ts = { 0, 0 };
        SafeWriteBuffer(rem, &rem_ts, sizeof(rem_ts));
    }
    return 0;
}

s64 SysThrCreate(guest_addr_t thread, guest_addr_t attr, guest_addr_t start_routine, guest_addr_t arg, u32 flags, guest_addr_t tls_base, u64 child_tid, CONTEXT*) {
    LOG_DEBUG(Kernel, "sys_thr_new(thread=0x%llx, attr=0x%llx, start=0x%llx, arg=0x%llx, flags=0x%x, tls=0x%llx, child_tid=%llu)", 
              thread, attr, start_routine, arg, flags, tls_base, child_tid);
    
    u64 new_thread_id = 0;
    HANDLE hThread = CreateThreadEx(start_routine, 0, 1024 * 1024, tls_base, arg, &new_thread_id);
    if (hThread == nullptr) {
        return -EFAULT;
    }
    
    if (thread != 0) {
        if (!SafeWriteBuffer(thread, &new_thread_id, sizeof(new_thread_id))) {
            return -EFAULT;
        }
    }
    
    LOG_INFO(Kernel, "sys_thr_new: Created thread %llu", new_thread_id);
    return 0;
}

s64 SysThrExit(s32 status, CONTEXT*) {
    LOG_DEBUG(Kernel, "sys_thr_exit(status=%d)", status);
    if (status != 0x999999) {
        ExitThread(static_cast<u64>(status));
    }
    return 0;
}

s64 SysThrSelf(CONTEXT*) {
    LOG_DEBUG(Kernel, "sys_thr_self()");
    return static_cast<s64>(GetCurrentThreadId());
}

s64 SysMmap(guest_addr_t addr, u64 length, u32 prot, u32 flags, s64 fd, s64 offset, CONTEXT*) {
    LOG_DEBUG(Kernel, "sys_mmap(addr=0x%llx, length=%llu, prot=0x%x, flags=0x%x, fd=%lld, offset=%lld)", 
              addr, length, prot, flags, fd, offset);
    
    u64 aligned_length = (length + 0xFFF) & ~0xFFF;
    guest_addr_t mapped_addr = MapGuestMemory(addr, aligned_length, prot, flags, static_cast<int>(fd), offset);
    if (mapped_addr == 0) {
        return -ENOMEM;
    }
    
    LOG_INFO(Kernel, "sys_mmap: Mapped 0x%llx bytes at 0x%llx", aligned_length, mapped_addr);
    return static_cast<s64>(mapped_addr);
}
s64 SysMunmap(guest_addr_t addr, u64 length, CONTEXT*) {
    LOG_DEBUG(Kernel, "sys_munmap(addr=0x%llx, length=%llu)", addr, length);
    if (addr == 0 || length == 0) {
        return -EINVAL;
    }
    
    u64 aligned_addr = addr & ~0xFFF;
    u64 aligned_length = (length + 0xFFF) & ~0xFFF;
    
    if (!UnmapGuestMemory(aligned_addr, aligned_length)) {
        return -EINVAL;
    }
    
    LOG_INFO(Kernel, "sys_munmap: Unmapped 0x%llx bytes at 0x%llx", aligned_length, aligned_addr);
    return 0;
}

s64 SysMprotect(guest_addr_t addr, u64 length, u32 prot, CONTEXT*) {
    LOG_DEBUG(Kernel, "sys_mprotect(addr=0x%llx, length=%llu, prot=0x%x)", addr, length, prot);
    if (addr == 0 || length == 0) {
        return -EINVAL;
    }
    u64 aligned_addr = addr & ~0xFFF;
    u64 aligned_length = (length + 0xFFF) & ~0xFFF;
    if (!ProtectGuestMemory(aligned_addr, aligned_length, static_cast<int>(prot))) {
        return -ENOMEM;
    }
    return 0;
}

// Thread control
s64 SysThrKill(s64 tid, s32 sig, CONTEXT*) {
    LOG_DEBUG(Kernel, "sys_thr_kill(tid=%lld, sig=%d)", tid, sig);
    if (!CheckThreadActive(static_cast<u64>(tid))) {
        return -ESRCH;
    }
    if (sig == 0) {
        return 0;
    }
    if (sig == 9 || sig == 15) { // SIGKILL or SIGTERM
        TerminateThreadByTid(static_cast<u64>(tid));
    }
    return 0;
}

s64 SysThrSuspend(guest_addr_t timeout, CONTEXT*) {
    LOG_DEBUG(Kernel, "sys_thr_suspend(timeout=0x%llx)", timeout);
    timespec ts;
    timespec* pts = nullptr;
    if (timeout != 0) {
        if (!SafeReadBuffer(timeout, &ts, sizeof(ts))) {
            return -EFAULT;
        }
        pts = &ts;
    }
    if (!SuspendCurrentThread(pts)) {
        return -ETIMEDOUT;
    }
    return 0;
}

s64 SysThrWake(s64 tid, CONTEXT*) {
    LOG_DEBUG(Kernel, "sys_thr_wake(tid=%lld)", tid);
    if (WakeThread(static_cast<u64>(tid))) {
        return 0;
    }
    return -ESRCH;
}

// Clocks
s64 SysClockGettime(u32 clock_id, guest_addr_t tp, CONTEXT*) {
    LOG_DEBUG(Kernel, "sys_clock_gettime(clock_id=%u, tp=0x%llx)", clock_id, tp);
    if (tp == 0) {
        return -EINVAL;
    }
    timespec ts = {};
    if (clock_id == 0) { // CLOCK_REALTIME
        GuestClockRealtime(&ts.tv_sec, &ts.tv_nsec);
    } else { // CLOCK_MONOTONIC etc. — shared QPC origin (guest_clock.cpp)
        const u64 qpc  = GuestClockCounter();
        const u64 freq = GuestClockCounterFrequency();
        ts.tv_sec  = static_cast<s64>(qpc / freq);
        ts.tv_nsec = static_cast<s64>(((qpc % freq) * 1000000000ULL) / freq);
    }
    if (!SafeWriteBuffer(tp, &ts, sizeof(ts))) {
        return -EFAULT;
    }
    return 0;
}

s64 SysClockGetres(u32 clock_id, guest_addr_t res, CONTEXT*) {
    LOG_DEBUG(Kernel, "sys_clock_getres(clock_id=%u, res=0x%llx)", clock_id, res);
    if (res == 0) {
        return -EINVAL;
    }
    timespec ts = {};
    if (clock_id == 0) {
        ts.tv_sec = 0;
        ts.tv_nsec = 100; // Windows file time resolution is 100ns
    } else {
        ts.tv_sec = 0;
        ts.tv_nsec = static_cast<s64>(1000000000ULL / GuestClockCounterFrequency());
    }
    if (!SafeWriteBuffer(res, &ts, sizeof(ts))) {
        return -EFAULT;
    }
    return 0;
}

// File permissions and link status
s64 SysLstat(guest_addr_t pathname, guest_addr_t statbuf, CONTEXT* ctx) {
    return SysStat(pathname, statbuf, ctx);
}

s64 SysAccess(guest_addr_t pathname, u32 mode, CONTEXT*) {
    char path[4096];
    if (!ReadGuestString(pathname, path, sizeof(path))) {
        return -EFAULT;
    }
    const std::string host_path = TranslateGuestPath(path);
    DWORD attribs = GetFileAttributesA(host_path.c_str());
    if (attribs == INVALID_FILE_ATTRIBUTES) {
        return -ENOENT;
    }
    if ((mode & 2) && (attribs & FILE_ATTRIBUTE_READONLY)) {
        return -EACCES;
    }
    return 0;
}

s64 SysChmod(guest_addr_t pathname, u32 mode, CONTEXT*) {
    char path[4096];
    if (!ReadGuestString(pathname, path, sizeof(path))) {
        return -EFAULT;
    }
    LOG_INFO(Kernel, "sys_chmod('%s', mode=0%o) mocked success", path, mode);
    return 0;
}

s64 SysFchmod(s64 fd, u32 mode, CONTEXT*) {
    LOG_DEBUG(Kernel, "sys_fchmod(fd=%lld, mode=0%o)", fd, mode);
    if (!IsValidFd(static_cast<int>(fd))) {
        return -EBADF;
    }
    return 0;
}

static int MapWSAError(int wsaErr) {
    switch (wsaErr) {
        case WSAEWOULDBLOCK: return 35; // EWOULDBLOCK / EAGAIN
        case WSAEINPROGRESS: return 36;
        case WSAEALREADY: return 37;
        case WSAENOTSOCK: return 38;
        case WSAEDESTADDRREQ: return 39;
        case WSAEMSGSIZE: return 40;
        case WSAEPROTOTYPE: return 41;
        case WSAENOPROTOOPT: return 42;
        case WSAEPROTONOSUPPORT: return 43;
        case WSAEOPNOTSUPP: return 45;
        case WSAEAFNOSUPPORT: return 47;
        case WSAEADDRINUSE: return 48;
        case WSAEADDRNOTAVAIL: return 49;
        case WSAENETDOWN: return 50;
        case WSAENETUNREACH: return 51;
        case WSAENETRESET: return 52;
        case WSAECONNABORTED: return 53;
        case WSAECONNRESET: return 54;
        case WSAENOBUFS: return 55;
        case WSAEISCONN: return 56;
        case WSAENOTCONN: return 57;
        case WSAESHUTDOWN: return 58;
        case WSAETOOMANYREFS: return 59;
        case WSAETIMEDOUT: return 60;
        case WSAECONNREFUSED: return 61;
        case WSAELOOP: return 62;
        case WSAENAMETOOLONG: return 63;
        case WSAEHOSTDOWN: return 64;
        case WSAEHOSTUNREACH: return 65;
        case WSAENOTEMPTY: return 66;
        default: return 22; // EINVAL
    }
}

// Sockets
s64 SysSocket(s32 domain, s32 type, s32 protocol, CONTEXT*) {
    LOG_DEBUG(Kernel, "sys_socket(domain=%d, type=%d, protocol=%d)", domain, type, protocol);
    SOCKET s = ::socket(domain, type, protocol);
    if (s == INVALID_SOCKET) {
        return -MapWSAError(::WSAGetLastError());
    }
    int fd = AllocateFd(FD_TYPE_SOCKET, reinterpret_cast<void*>(s), O_RDWR, 0, "socket");
    if (fd == -1) {
        ::closesocket(s);
        return -EMFILE;
    }
    return fd;
}

s64 SysBind(s64 fd, guest_addr_t addr, u32 addrlen, CONTEXT*) {
    LOG_DEBUG(Kernel, "sys_bind(fd=%lld, addr=0x%llx, len=%u)", fd, addr, addrlen);
    if (!IsValidFd(static_cast<int>(fd))) {
        return -EBADF;
    }
    FdEntry* entry = GetFd(static_cast<int>(fd));
    if (entry->type != FD_TYPE_SOCKET) {
        return -ENOTSOCK;
    }
    std::vector<char> buf(addrlen);
    if (!SafeReadBuffer(addr, buf.data(), addrlen)) {
        return -EFAULT;
    }
    SOCKET s = reinterpret_cast<SOCKET>(entry->handle);
    if (::bind(s, reinterpret_cast<const sockaddr*>(buf.data()), static_cast<int>(addrlen)) == SOCKET_ERROR) {
        return -MapWSAError(::WSAGetLastError());
    }
    return 0;
}

s64 SysListen(s64 fd, s32 backlog, CONTEXT*) {
    LOG_DEBUG(Kernel, "sys_listen(fd=%lld, backlog=%d)", fd, backlog);
    if (!IsValidFd(static_cast<int>(fd))) {
        return -EBADF;
    }
    FdEntry* entry = GetFd(static_cast<int>(fd));
    if (entry->type != FD_TYPE_SOCKET) {
        return -ENOTSOCK;
    }
    SOCKET s = reinterpret_cast<SOCKET>(entry->handle);
    if (::listen(s, backlog) == SOCKET_ERROR) {
        return -MapWSAError(::WSAGetLastError());
    }
    return 0;
}

s64 SysAccept(s64 fd, guest_addr_t addr, guest_addr_t addrlen, CONTEXT*) {
    LOG_DEBUG(Kernel, "sys_accept(fd=%lld, addr=0x%llx, addrlen=0x%llx)", fd, addr, addrlen);
    if (!IsValidFd(static_cast<int>(fd))) {
        return -EBADF;
    }
    FdEntry* entry = GetFd(static_cast<int>(fd));
    if (entry->type != FD_TYPE_SOCKET) {
        return -ENOTSOCK;
    }
    SOCKET s = reinterpret_cast<SOCKET>(entry->handle);
    
    int len = 0;
    if (addrlen != 0) {
        if (!SafeReadBuffer(addrlen, &len, sizeof(len))) {
            return -EFAULT;
        }
    }
    
    std::vector<char> buf(len > 0 ? len : 128);
    SOCKET client = ::accept(s, addr != 0 ? reinterpret_cast<sockaddr*>(buf.data()) : nullptr, addr != 0 ? &len : nullptr);
    if (client == INVALID_SOCKET) {
        return -MapWSAError(::WSAGetLastError());
    }
    
    if (addr != 0 && len > 0) {
        if (!SafeWriteBuffer(addr, buf.data(), len)) {
            ::closesocket(client);
            return -EFAULT;
        }
        SafeWriteBuffer(addrlen, &len, sizeof(len));
    }
    
    int client_fd = AllocateFd(FD_TYPE_SOCKET, reinterpret_cast<void*>(client), O_RDWR, 0, "accepted_socket");
    if (client_fd == -1) {
        ::closesocket(client);
        return -EMFILE;
    }
    return client_fd;
}

s64 SysConnect(s64 fd, guest_addr_t addr, u32 addrlen, CONTEXT*) {
    LOG_DEBUG(Kernel, "sys_connect(fd=%lld, addr=0x%llx, len=%u)", fd, addr, addrlen);
    if (!IsValidFd(static_cast<int>(fd))) {
        return -EBADF;
    }
    FdEntry* entry = GetFd(static_cast<int>(fd));
    if (entry->type != FD_TYPE_SOCKET) {
        return -ENOTSOCK;
    }
    SOCKET s = reinterpret_cast<SOCKET>(entry->handle);
    std::vector<char> buf(addrlen);
    if (!SafeReadBuffer(addr, buf.data(), addrlen)) {
        return -EFAULT;
    }
    if (::connect(s, reinterpret_cast<const sockaddr*>(buf.data()), static_cast<int>(addrlen)) == SOCKET_ERROR) {
        return -MapWSAError(::WSAGetLastError());
    }
    return 0;
}

s64 SysSendto(s64 fd, guest_addr_t buf, u64 len, s32 flags, guest_addr_t to, u32 tolen, CONTEXT*) {
    LOG_DEBUG(Kernel, "sys_sendto(fd=%lld, buf=0x%llx, len=%llu, flags=%d)", fd, buf, len, flags);
    if (!IsValidFd(static_cast<int>(fd))) {
        return -EBADF;
    }
    FdEntry* entry = GetFd(static_cast<int>(fd));
    if (entry->type != FD_TYPE_SOCKET) {
        return -ENOTSOCK;
    }
    SOCKET s = reinterpret_cast<SOCKET>(entry->handle);
    std::vector<char> data(len);
    if (!SafeReadBuffer(buf, data.data(), len)) {
        return -EFAULT;
    }
    std::vector<char> to_buf(tolen);
    if (to != 0 && tolen > 0) {
        if (!SafeReadBuffer(to, to_buf.data(), tolen)) {
            return -EFAULT;
        }
    }
    int sent = ::sendto(
        s,
        data.data(),
        static_cast<int>(len),
        flags,
        to != 0 ? reinterpret_cast<const sockaddr*>(to_buf.data()) : nullptr,
        static_cast<int>(tolen)
    );
    if (sent == SOCKET_ERROR) {
        return -MapWSAError(::WSAGetLastError());
    }
    return sent;
}

s64 SysRecvfrom(s64 fd, guest_addr_t buf, u64 len, s32 flags, guest_addr_t from, guest_addr_t fromlen, CONTEXT*) {
    LOG_DEBUG(Kernel, "sys_recvfrom(fd=%lld, buf=0x%llx, len=%llu, flags=%d)", fd, buf, len, flags);
    if (!IsValidFd(static_cast<int>(fd))) {
        return -EBADF;
    }
    FdEntry* entry = GetFd(static_cast<int>(fd));
    if (entry->type != FD_TYPE_SOCKET) {
        return -ENOTSOCK;
    }
    SOCKET s = reinterpret_cast<SOCKET>(entry->handle);
    std::vector<char> data(len);
    
    int flen = 0;
    if (fromlen != 0) {
        if (!SafeReadBuffer(fromlen, &flen, sizeof(flen))) {
            return -EFAULT;
        }
    }
    std::vector<char> from_buf(flen > 0 ? flen : 128);
    int recved = ::recvfrom(
        s,
        data.data(),
        static_cast<int>(len),
        flags,
        from != 0 ? reinterpret_cast<sockaddr*>(from_buf.data()) : nullptr,
        from != 0 ? &flen : nullptr
      );
    if (recved == SOCKET_ERROR) {
        return -MapWSAError(::WSAGetLastError());
    }
    if (!SafeWriteBuffer(buf, data.data(), recved)) {
        return -EFAULT;
    }
    if (from != 0 && flen > 0) {
        SafeWriteBuffer(from, from_buf.data(), flen);
        SafeWriteBuffer(fromlen, &flen, sizeof(flen));
    }
    return recved;
}

// Semaphores
struct SemInfo {
    HANDLE handle;
    s32 value;
};
static std::unordered_map<guest_addr_t, SemInfo> g_semaphores;
static std::mutex g_sem_mutex;

s64 SysSemInit(guest_addr_t sem, s32 pshared, u32 value, CONTEXT*) {
    LOG_DEBUG(Kernel, "sys_sem_init(sem=0x%llx, pshared=%d, value=%u)", sem, pshared, value);
    if (sem == 0) {
        return -EINVAL;
    }
    std::lock_guard<std::mutex> lock(g_sem_mutex);
    if (g_semaphores.find(sem) != g_semaphores.end()) {
        return -EEXIST;
    }
    HANDLE hSem = CreateSemaphoreA(nullptr, static_cast<LONG>(value), 0x7FFFFFFF, nullptr);
    if (hSem == nullptr) {
        return -ENOMEM;
    }
    g_semaphores[sem] = { hSem, static_cast<s32>(value) };
    return 0;
}

s64 SysSemDestroy(guest_addr_t sem, CONTEXT*) {
    LOG_DEBUG(Kernel, "sys_sem_destroy(sem=0x%llx)", sem);
    std::lock_guard<std::mutex> lock(g_sem_mutex);
    auto it = g_semaphores.find(sem);
    if (it == g_semaphores.end()) {
        return -EINVAL;
    }
    CloseHandle(it->second.handle);
    g_semaphores.erase(it);
    return 0;
}

s64 SysSemWait(guest_addr_t sem, CONTEXT*) {
    LOG_DEBUG(Kernel, "sys_sem_wait(sem=0x%llx)", sem);
    HANDLE hSem = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_sem_mutex);
        auto it = g_semaphores.find(sem);
        if (it == g_semaphores.end()) {
            return -EINVAL;
        }
        hSem = it->second.handle;
    }
    DWORD res = WaitForSingleObject(hSem, INFINITE);
    if (res != WAIT_OBJECT_0) {
        return -EINTR;
    }
    {
        std::lock_guard<std::mutex> lock(g_sem_mutex);
        auto it = g_semaphores.find(sem);
        if (it != g_semaphores.end()) {
            it->second.value--;
        }
    }
    return 0;
}

s64 SysSemPost(guest_addr_t sem, CONTEXT*) {
    LOG_DEBUG(Kernel, "sys_sem_post(sem=0x%llx)", sem);
    HANDLE hSem = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_sem_mutex);
        auto it = g_semaphores.find(sem);
        if (it == g_semaphores.end()) {
            return -EINVAL;
        }
        hSem = it->second.handle;
    }
    if (!ReleaseSemaphore(hSem, 1, nullptr)) {
        return -EINVAL;
    }
    {
        std::lock_guard<std::mutex> lock(g_sem_mutex);
        auto it = g_semaphores.find(sem);
        if (it != g_semaphores.end()) {
            it->second.value++;
        }
    }
    return 0;
}

s64 SysSemTrywait(guest_addr_t sem, CONTEXT*) {
    LOG_DEBUG(Kernel, "sys_sem_trywait(sem=0x%llx)", sem);
    HANDLE hSem = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_sem_mutex);
        auto it = g_semaphores.find(sem);
        if (it == g_semaphores.end()) {
            return -EINVAL;
        }
        hSem = it->second.handle;
    }
    DWORD res = WaitForSingleObject(hSem, 0);
    if (res == WAIT_TIMEOUT) {
        return -EAGAIN;
    }
    if (res != WAIT_OBJECT_0) {
        return -EINVAL;
    }
    {
        std::lock_guard<std::mutex> lock(g_sem_mutex);
        auto it = g_semaphores.find(sem);
        if (it != g_semaphores.end()) {
            it->second.value--;
        }
    }
    return 0;
}

s64 SysSemGetvalue(guest_addr_t sem, guest_addr_t sval, CONTEXT*) {
    LOG_DEBUG(Kernel, "sys_sem_getvalue(sem=0x%llx, sval=0x%llx)", sem, sval);
    if (sval == 0) {
        return -EINVAL;
    }
    s32 val = 0;
    {
        std::lock_guard<std::mutex> lock(g_sem_mutex);
        auto it = g_semaphores.find(sem);
        if (it == g_semaphores.end()) {
            return -EINVAL;
        }
        val = it->second.value;
    }
    if (!SafeWriteBuffer(sval, &val, sizeof(val))) {
        return -EFAULT;
    }
    return 0;
}

// PS5 Specific / Extensions
s64 SysKernControl(CONTEXT*) {
    LOG_DEBUG(Kernel, "sys_kern_control()");
    return 0;
}
s64 SysKernGetpid(CONTEXT* ctx) {
    return SysGetpid(ctx);
}
s64 SysKernGettid(CONTEXT* ctx) {
    return SysThrSelf(ctx);
}

} // namespace Kernel