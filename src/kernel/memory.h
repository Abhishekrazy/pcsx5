#pragma once

#include "kernel.h"
#include <windows.h>

namespace Kernel {

// Memory protection flags (matching POSIX)
constexpr int PROT_READ  = 0x1;
constexpr int PROT_WRITE = 0x2;
constexpr int PROT_EXEC  = 0x4;
constexpr int PROT_NONE  = 0x0;

// mmap flags
constexpr int MAP_SHARED     = 0x01;
constexpr int MAP_PRIVATE    = 0x02;
constexpr int MAP_FIXED      = 0x10;
constexpr int MAP_ANONYMOUS  = 0x20;
constexpr int MAP_ANON       = MAP_ANONYMOUS;
constexpr int MAP_FIXED_NOREPLACE = 0x80000;

// Initialize/shutdown guest memory allocator
void InitializeGuestMemory();
void ShutdownGuestMemory();

// Guest memory allocation
guest_addr_t AllocGuestMemory(u64 size, u64 alignment, int prot, int flags, int fd, s64 offset);
bool FreeGuestMemory(guest_addr_t addr, u64 size);
bool ProtectGuestMemory(guest_addr_t addr, u64 size, int prot);
bool GetGuestMemoryInfo(guest_addr_t addr, MEMORY_BASIC_INFORMATION* info);
bool IsValidGuestAddress(guest_addr_t addr, u64 size);

// mmap/munmap support
guest_addr_t MapGuestMemory(guest_addr_t addr, u64 length, int prot, int flags, int fd, s64 offset);
bool UnmapGuestMemory(guest_addr_t addr, u64 length);

// brk/sbrk support
guest_addr_t GetBreak();
guest_addr_t SetBreak(guest_addr_t new_break);

} // namespace Kernel