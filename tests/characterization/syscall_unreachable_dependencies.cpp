// TEST ONLY: fail-fast link isolation for raw syscall table registration and
// dispatch in legacy syscalls.cpp. No default handler or syscall body is tested.
// Never link into the emulator or a syscall-body/lifecycle/OS-ABI acceptance test.
#ifndef PCSX5_SYSCALL_TABLE_CHARACTERIZATION_ONLY
#error "Syscall dependency tripwires require the table-only characterization target"
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "kernel/fd_table.h"
#include "kernel/guest_clock.h"
#include "kernel/memory.h"
#include "kernel/thread.h"
#include "hle/hle.h"
#include <cstdio>
#include <cstdlib>

namespace {
[[noreturn]] void UnexpectedDependency(const char* name) {
    std::fprintf(stderr, "FAIL table-only characterization reached forbidden dependency: %s\n", name);
    std::abort();
}
}

namespace Kernel {
bool IsInProcMode() { UnexpectedDependency("Kernel::IsInProcMode"); }
void RunGuestExitHook() { UnexpectedDependency("Kernel::RunGuestExitHook"); }
std::string TranslateGuestPath(const std::string&) { UnexpectedDependency("Kernel::TranslateGuestPath"); }
int AllocateFd(int, void*, int, int, const std::string&) { UnexpectedDependency("Kernel::AllocateFd"); }
bool CloseFd(int) { UnexpectedDependency("Kernel::CloseFd"); }
FdEntry* GetFd(int) { UnexpectedDependency("Kernel::GetFd"); }
bool IsValidFd(int) { UnexpectedDependency("Kernel::IsValidFd"); }
int GetFdFlags(int) { UnexpectedDependency("Kernel::GetFdFlags"); }
bool SetFdFlags(int, int) { UnexpectedDependency("Kernel::SetFdFlags"); }
u64 GuestClockCounter() { UnexpectedDependency("Kernel::GuestClockCounter"); }
u64 GuestClockCounterFrequency() { UnexpectedDependency("Kernel::GuestClockCounterFrequency"); }
void GuestClockRealtime(s64*, s64*) { UnexpectedDependency("Kernel::GuestClockRealtime"); }
bool ProtectGuestMemory(guest_addr_t, u64, int) { UnexpectedDependency("Kernel::ProtectGuestMemory"); }
guest_addr_t MapGuestMemory(guest_addr_t, u64, int, int, int, s64) {
    UnexpectedDependency("Kernel::MapGuestMemory");
}
bool UnmapGuestMemory(guest_addr_t, u64) { UnexpectedDependency("Kernel::UnmapGuestMemory"); }
guest_addr_t GetBreak() { UnexpectedDependency("Kernel::GetBreak"); }
guest_addr_t SetBreak(guest_addr_t) { UnexpectedDependency("Kernel::SetBreak"); }
u64 GetCurrentThreadId() { UnexpectedDependency("Kernel::GetCurrentThreadId"); }
HANDLE CreateThreadEx(guest_addr_t, guest_addr_t, u64, guest_addr_t, u64, u64*) {
    UnexpectedDependency("Kernel::CreateThreadEx");
}
void ExitThread(u64) { UnexpectedDependency("Kernel::ExitThread"); }
bool SuspendCurrentThread(const timespec*) { UnexpectedDependency("Kernel::SuspendCurrentThread"); }
bool WakeThread(u64) { UnexpectedDependency("Kernel::WakeThread"); }
bool CheckThreadActive(u64) { UnexpectedDependency("Kernel::CheckThreadActive"); }
bool TerminateThreadByTid(u64) { UnexpectedDependency("Kernel::TerminateThreadByTid"); }
}
namespace HLE {
[[noreturn]] void ExitGuestProcess(u32) { UnexpectedDependency("HLE::ExitGuestProcess"); }
}
