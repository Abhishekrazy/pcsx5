#pragma once
//
// Syscall handler table for PS5 (FreeBSD x86_64) syscalls.
//
// PS5 uses FreeBSD's syscall convention where the syscall number is passed
// in RAX and the handler is invoked via INT 3 (0xCC) interception.
// The SyscallTable provides a centralized dispatch mechanism.
//
// Reference: FreeBSD amd64 syscall numbers
// https://github.com/freebsd/freebsd-src/blob/main/sys/amd64/include/syscall.h
//

#include "../common/types.h"
#include <unordered_map>
#include <mutex>

namespace SyscallTable {

// Initialize the default syscall table with all known handlers
void Initialize();

// Shutdown and clear the syscall table
void Shutdown();

// Register a custom syscall handler
void Register(u32 number, void* handler);

// Unregister a syscall handler
void Unregister(u32 number);

// Look up a syscall handler by number
void* Lookup(u32 number);

// Invoke a syscall handler with the given arguments
u64 Invoke(u32 number, u64 arg1, u64 arg2, u64 arg3, u64 arg4, u64 arg5, u64 arg6);

// Check if a syscall is implemented
bool IsImplemented(u32 number);

// Get the count of registered syscalls
u32 GetCount();

} // namespace SyscallTable
