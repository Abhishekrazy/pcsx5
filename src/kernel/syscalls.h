#pragma once

#include "kernel.h"
#include <windows.h>

namespace Kernel {

// Syscall handler type
using SyscallHandler = s64(*)(CONTEXT*);

// Initialize syscall dispatch table
void InitializeSyscallTable();

// Main syscall dispatcher
s64 HandleSyscall(u32 syscall_number, CONTEXT* context);

// Syscall Implementations matching original kernel.cpp signatures and behavior
s64 SysExit(s32 status, CONTEXT* context);
s64 SysRead(s64 fd, guest_addr_t buf, u64 count, CONTEXT* context);
s64 SysWrite(s64 fd, guest_addr_t buf, u64 count, CONTEXT* context);
s64 SysOpen(guest_addr_t pathname, u32 flags, u32 mode, CONTEXT* context);
s64 SysClose(s64 fd, CONTEXT* context);
s64 SysGetpid(CONTEXT* context);
s64 SysBrk(guest_addr_t addr, CONTEXT* context);
s64 SysIoctl(s64 fd, u32 request, guest_addr_t argp, CONTEXT* context);
s64 SysFcntl(s64 fd, u32 cmd, guest_addr_t arg, CONTEXT* context);
s64 SysGettimeofday(guest_addr_t tv, guest_addr_t tz, CONTEXT* context);
s64 SysStat(guest_addr_t pathname, guest_addr_t statbuf, CONTEXT* context);
s64 SysFstat(s64 fd, guest_addr_t statbuf, CONTEXT* context);
s64 SysLseek(s64 fd, s64 offset, u32 whence, CONTEXT* context);
s64 SysNanosleep(guest_addr_t req, guest_addr_t rem, CONTEXT* context);
s64 SysThrCreate(guest_addr_t thread, guest_addr_t attr, guest_addr_t start_routine, guest_addr_t arg, u32 flags, guest_addr_t tls_base, u64 child_tid, CONTEXT* context);
s64 SysThrExit(s32 status, CONTEXT* context);
s64 SysThrSelf(CONTEXT* context);
s64 SysMmap(guest_addr_t addr, u64 length, u32 prot, u32 flags, s64 fd, s64 offset, CONTEXT* context);
s64 SysMunmap(guest_addr_t addr, u64 length, CONTEXT* context);
s64 SysMprotect(guest_addr_t addr, u64 length, u32 prot, CONTEXT* context);

// Thread control
s64 SysThrKill(s64 tid, s32 sig, CONTEXT* context);
s64 SysThrSuspend(guest_addr_t timeout, CONTEXT* context);
s64 SysThrWake(s64 tid, CONTEXT* context);

// Clocks
s64 SysClockGettime(u32 clock_id, guest_addr_t tp, CONTEXT* context);
s64 SysClockGetres(u32 clock_id, guest_addr_t res, CONTEXT* context);

// File permissions and link status
s64 SysLstat(guest_addr_t pathname, guest_addr_t statbuf, CONTEXT* context);
s64 SysAccess(guest_addr_t pathname, u32 mode, CONTEXT* context);
s64 SysChmod(guest_addr_t pathname, u32 mode, CONTEXT* context);
s64 SysFchmod(s64 fd, u32 mode, CONTEXT* context);

// Sockets
s64 SysSocket(s32 domain, s32 type, s32 protocol, CONTEXT* context);
s64 SysBind(s64 fd, guest_addr_t addr, u32 addrlen, CONTEXT* context);
s64 SysListen(s64 fd, s32 backlog, CONTEXT* context);
s64 SysAccept(s64 fd, guest_addr_t addr, guest_addr_t addrlen, CONTEXT* context);
s64 SysConnect(s64 fd, guest_addr_t addr, u32 addrlen, CONTEXT* context);
s64 SysSendto(s64 fd, guest_addr_t buf, u64 len, s32 flags, guest_addr_t to, u32 tolen, CONTEXT* context);
s64 SysRecvfrom(s64 fd, guest_addr_t buf, u64 len, s32 flags, guest_addr_t from, guest_addr_t fromlen, CONTEXT* context);

// Semaphores
s64 SysSemInit(guest_addr_t sem, s32 pshared, u32 value, CONTEXT* context);
s64 SysSemDestroy(guest_addr_t sem, CONTEXT* context);
s64 SysSemWait(guest_addr_t sem, CONTEXT* context);
s64 SysSemPost(guest_addr_t sem, CONTEXT* context);
s64 SysSemTrywait(guest_addr_t sem, CONTEXT* context);
s64 SysSemGetvalue(guest_addr_t sem, guest_addr_t sval, CONTEXT* context);

// Kernel control
s64 SysKernControl(CONTEXT* context);
s64 SysKernGetpid(CONTEXT* context);
s64 SysKernGettid(CONTEXT* context);

} // namespace Kernel