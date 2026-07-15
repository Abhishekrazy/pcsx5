#pragma once

#include "kernel.h"
#include <windows.h>

namespace Kernel {

// Thread management functions
u64 AllocateThreadId();
void SetCurrentThreadId(u64 tid);
u64 GetCurrentThreadId();

bool RegisterThread(u64 tid, HANDLE handle, guest_addr_t entry, guest_addr_t stack, u64 stack_size, guest_addr_t tls_base);
bool UnregisterThread(u64 tid);

HANDLE CreateThread(guest_addr_t entry, guest_addr_t stack, u64 stack_size, guest_addr_t tls_base, u64* out_tid);
void ExitThread(u64 status);

// New thread control APIs for thr_suspend/thr_wake/thr_kill
struct timespec;
bool SuspendCurrentThread(const struct timespec* timeout);
bool WakeThread(u64 tid);
bool CheckThreadActive(u64 tid);
bool TerminateThreadByTid(u64 tid);

} // namespace Kernel