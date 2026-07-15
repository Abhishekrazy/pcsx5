#pragma once

#include "kernel.h"
#include <string>
#include <windows.h>

namespace Kernel {

// File descriptor types
constexpr int FD_TYPE_FILE = 1;
constexpr int FD_TYPE_SOCKET = 2;
constexpr int FD_TYPE_PIPE = 3;
constexpr int FD_TYPE_EVENT = 4;
constexpr int FD_TYPE_TIMER = 5;

// Open flags (matching POSIX)
#ifndef O_RDONLY
constexpr int O_RDONLY    = 0x0000;
#endif
#ifndef O_WRONLY
constexpr int O_WRONLY    = 0x0001;
#endif
#ifndef O_RDWR
constexpr int O_RDWR      = 0x0002;
#endif
#ifndef O_CREAT
constexpr int O_CREAT     = 0x0100;
#endif
#ifndef O_EXCL
constexpr int O_EXCL      = 0x0200;
#endif
#ifndef O_TRUNC
constexpr int O_TRUNC     = 0x0400;
#endif
#ifndef O_APPEND
constexpr int O_APPEND    = 0x0800;
#endif
#ifndef O_NONBLOCK
constexpr int O_NONBLOCK  = 0x1000;
#endif
#ifndef O_SYNC
constexpr int O_SYNC      = 0x2000;
#endif
#ifndef O_DIRECTORY
constexpr int O_DIRECTORY = 0x10000;
#endif
#ifndef O_NOFOLLOW
constexpr int O_NOFOLLOW  = 0x20000;
#endif
#ifndef O_CLOEXEC
constexpr int O_CLOEXEC   = 0x40000;
#endif

// File descriptor table entry
struct FdEntry {
    int fd;
    int type;           // FD_TYPE_FILE, FD_TYPE_SOCKET, FD_TYPE_PIPE, etc.
    void* handle;       // Windows handle or other opaque pointer
    u64 offset;         // Current file offset
    int flags;          // Open flags (O_RDONLY, O_WRONLY, etc.)
    int mode;           // File mode
    std::string path;   // Path for debugging
    bool in_use;
    
    FdEntry() : fd(-1), type(0), handle(nullptr), offset(0), flags(0), mode(0), in_use(false) {}
};

// Initialize/shutdown FD table
void InitializeFdTable();
void ShutdownFdTable();

// FD allocation and management
int AllocateFd(int type, void* handle, int flags, int mode, const std::string& path);
bool ReleaseFd(int fd);
bool CloseFd(int fd);

// FD queries
FdEntry* GetFd(int fd);
bool IsValidFd(int fd);
int GetFdType(int fd);
std::string GetFdPath(int fd);

// FD offset management
bool SetFdOffset(int fd, u64 offset);
u64 GetFdOffset(int fd);

// FD flags management
int GetFdFlags(int fd);
bool SetFdFlags(int fd, int flags);

// FD duplication
int DuplicateFd(int oldfd, int newfd);

// Statistics
int GetOpenFdCount();

} // namespace Kernel