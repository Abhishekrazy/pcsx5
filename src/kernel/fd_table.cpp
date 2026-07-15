#include "fd_table.h"
#include "kernel.h"
#include "memory/memory.h"
#include "hle/hle.h"
#include <windows.h>
#include <vector>
#include <mutex>
#include <algorithm>

namespace Kernel {

static const int MAX_FDS = 4096;
static std::vector<FdEntry> g_fd_table;
static std::mutex g_fd_mutex;
static int g_next_fd = 0;

// Initialize FD table
void InitializeFdTable() {
    std::lock_guard<std::mutex> lock(g_fd_mutex);
    g_fd_table.clear();
    g_fd_table.resize(MAX_FDS);
    g_next_fd = 0;
    
    // Reserve stdin, stdout, stderr (0, 1, 2)
    for (int i = 0; i < 3; i++) {
        g_fd_table[i].fd = i;
        g_fd_table[i].in_use = true;
        g_fd_table[i].type = FD_TYPE_FILE;
        g_fd_table[i].flags = (i == 0) ? O_RDONLY : O_WRONLY;
    }
    g_next_fd = 3;
}

// Shutdown FD table
void ShutdownFdTable() {
    std::lock_guard<std::mutex> lock(g_fd_mutex);
    for (auto& entry : g_fd_table) {
        if (entry.in_use && entry.fd >= 3) {
            CloseFd(entry.fd);
        }
    }
    g_fd_table.clear();
}

// Allocate a new file descriptor
int AllocateFd(int type, void* handle, int flags, int mode, const std::string& path) {
    std::lock_guard<std::mutex> lock(g_fd_mutex);
    
    // Find first available FD
    int fd = -1;
    for (int i = g_next_fd; i < MAX_FDS; i++) {
        if (!g_fd_table[i].in_use) {
            fd = i;
            break;
        }
    }
    
    // If not found, search from beginning
    if (fd == -1) {
        for (int i = 3; i < g_next_fd; i++) {
            if (!g_fd_table[i].in_use) {
                fd = i;
                break;
            }
        }
    }
    
    if (fd == -1) {
        return -1;  // EMFILE - too many open files
    }
    
    FdEntry& entry = g_fd_table[fd];
    entry.fd = fd;
    entry.type = type;
    entry.handle = handle;
    entry.offset = 0;
    entry.flags = flags;
    entry.mode = mode;
    entry.path = path;
    entry.in_use = true;
    
    g_next_fd = fd + 1;
    
    return fd;
}

// Get FD entry
FdEntry* GetFd(int fd) {
    std::lock_guard<std::mutex> lock(g_fd_mutex);
    
    if (fd < 0 || fd >= MAX_FDS) {
        return nullptr;
    }
    
    FdEntry& entry = g_fd_table[fd];
    if (!entry.in_use) {
        return nullptr;
    }
    
    return &entry;
}

// Release FD
bool ReleaseFd(int fd) {
    std::lock_guard<std::mutex> lock(g_fd_mutex);
    
    if (fd < 0 || fd >= MAX_FDS) {
        return false;
    }
    
    // Don't allow closing stdin/stdout/stderr
    if (fd < 3) {
        return false;
    }
    
    FdEntry& entry = g_fd_table[fd];
    if (!entry.in_use) {
        return false;
    }
    
    entry.in_use = false;
    entry.handle = nullptr;
    entry.path.clear();
    
    return true;
}

// Close FD (with actual handle cleanup)
bool CloseFd(int fd) {
    FdEntry* entry = GetFd(fd);
    if (!entry) {
        return false;
    }
    
    // Close the underlying handle based on type
    switch (entry->type) {
        case FD_TYPE_FILE:
            if (entry->handle) {
                CloseHandle(static_cast<HANDLE>(entry->handle));
            }
            break;
        case FD_TYPE_SOCKET:
            // Socket cleanup would go here
            break;
        case FD_TYPE_PIPE:
            if (entry->handle) {
                CloseHandle(static_cast<HANDLE>(entry->handle));
            }
            break;
        default:
            break;
    }
    
    return ReleaseFd(fd);
}

// Set file offset
bool SetFdOffset(int fd, u64 offset) {
    FdEntry* entry = GetFd(fd);
    if (!entry) {
        return false;
    }
    
    entry->offset = offset;
    return true;
}

// Get file offset
u64 GetFdOffset(int fd) {
    FdEntry* entry = GetFd(fd);
    if (!entry) {
        return 0;
    }
    
    return entry->offset;
}

// Get FD flags
int GetFdFlags(int fd) {
    FdEntry* entry = GetFd(fd);
    if (!entry) {
        return -1;
    }
    
    return entry->flags;
}

// Set FD flags
bool SetFdFlags(int fd, int flags) {
    FdEntry* entry = GetFd(fd);
    if (!entry) {
        return false;
    }
    
    entry->flags = flags;
    return true;
}

// Get FD type
int GetFdType(int fd) {
    FdEntry* entry = GetFd(fd);
    if (!entry) {
        return 0;
    }
    
    return entry->type;
}

// Check if FD is valid
bool IsValidFd(int fd) {
    return GetFd(fd) != nullptr;
}

// Get FD path (for debugging)
std::string GetFdPath(int fd) {
    FdEntry* entry = GetFd(fd);
    if (!entry) {
        return "";
    }
    
    return entry->path;
}

// Duplicate FD (for dup/dup2 syscalls)
int DuplicateFd(int oldfd, int newfd) {
    std::lock_guard<std::mutex> lock(g_fd_mutex);
    
    if (oldfd < 0 || oldfd >= MAX_FDS || !g_fd_table[oldfd].in_use) {
        return -1;  // EBADF
    }
    
    if (newfd >= 0) {
        // dup2 - close newfd first if it's open
        if (newfd < MAX_FDS && g_fd_table[newfd].in_use) {
            CloseFd(newfd);
        }
        
        if (newfd >= MAX_FDS) {
            return -1;  // EBADF
        }
        
        // Copy the entry
        g_fd_table[newfd] = g_fd_table[oldfd];
        g_fd_table[newfd].fd = newfd;
        g_fd_table[newfd].in_use = true;
        
        // Increment handle reference count if needed
        // (For Windows handles, we'd use DuplicateHandle)
        
        return newfd;
    } else {
        // dup - find first available
        return AllocateFd(
            g_fd_table[oldfd].type,
            g_fd_table[oldfd].handle,
            g_fd_table[oldfd].flags,
            g_fd_table[oldfd].mode,
            g_fd_table[oldfd].path
        );
    }
}

// Get number of open FDs
int GetOpenFdCount() {
    std::lock_guard<std::mutex> lock(g_fd_mutex);
    
    int count = 0;
    for (const auto& entry : g_fd_table) {
        if (entry.in_use) {
            count++;
        }
    }
    
    return count;
}

} // namespace Kernel