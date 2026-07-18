//
// Kernel thread API — thin shim over the CpuCore guest-thread registry.
// The actual thread machinery (registry, wake events, host thread creation)
// lives in src/cpu/cpu.cpp; these functions keep the historical Kernel::
// signatures so existing callers are untouched.
//

#include "thread.h"
#include "../cpu/cpu.h"
#include <windows.h>

namespace Kernel {

u64 AllocateThreadId() {
    return CpuCore::NextThreadId();
}

void SetCurrentThreadId(u64 tid) {
    CpuCore::SetCurrentThreadId(tid);
}

u64 GetCurrentThreadId() {
    return CpuCore::GetCurrentThreadId();
}

bool RegisterThread(u64 tid, HANDLE handle, guest_addr_t entry, guest_addr_t stack, u64 stack_size, guest_addr_t tls_base) {
    return CpuCore::RegisterExistingThread(tid, handle, entry, stack, stack_size, tls_base);
}

bool UnregisterThread(u64 tid) {
    return CpuCore::UnregisterThread(tid);
}

HANDLE CreateThread(guest_addr_t entry, guest_addr_t stack, u64 stack_size, guest_addr_t tls_base, u64* out_tid) {
    return CreateThreadEx(entry, stack, stack_size, tls_base, 0, out_tid);
}

HANDLE CreateThreadEx(guest_addr_t entry, guest_addr_t stack, u64 stack_size, guest_addr_t tls_base, u64 argument, u64* out_tid) {
    u64 tid = CpuCore::CreateThread(entry, stack, stack_size, tls_base, argument);
    if (tid == 0) {
        return nullptr;
    }
    GuestThread* thread = CpuCore::GetThreadById(tid);
    if (thread == nullptr) {
        return nullptr;
    }
    if (out_tid) {
        *out_tid = tid;
    }
    return static_cast<HANDLE>(thread->host_thread);
}

void ExitThread(u64 status) {
    CpuCore::UnregisterThread(CpuCore::GetCurrentThreadId());
    ::ExitThread(static_cast<DWORD>(status));
}

struct timespec {
    s64 tv_sec;
    s64 tv_nsec;
};

bool SuspendCurrentThread(const struct timespec* timeout) {
    s64 timeout_ms = -1; // infinite
    if (timeout) {
        timeout_ms = timeout->tv_sec * 1000 + timeout->tv_nsec / 1000000;
    }
    return CpuCore::SuspendCurrentThread(timeout_ms);
}

bool WakeThread(u64 tid) {
    return CpuCore::WakeThread(tid);
}

bool CheckThreadActive(u64 tid) {
    return CpuCore::CheckThreadActive(tid);
}

bool TerminateThreadByTid(u64 tid) {
    return CpuCore::TerminateThread(tid);
}

guest_addr_t GetThreadTlsBase(u64 tid) {
    return CpuCore::GetThreadTlsBase(tid);
}

} // namespace Kernel
