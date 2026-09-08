// TEST ONLY: link isolation for CPUState::FromContext/ToContext in the otherwise
// monolithic legacy cpu.cpp. Neither conversion calls these dependencies.
// These are fail-fast tripwires, not fake implementations or runtime evidence.
// Never link this file into the emulator or any lifecycle/syscall/TLS test.
#ifndef PCSX5_CONTEXT_CHARACTERIZATION_ONLY
#error "CPU dependency tripwires require the context-only characterization target"
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "common/platform/platform.h"
#include "config/config.h"
#include "hle/hle.h"
#include "kernel/kernel.h"
#include "kernel/syscalls.h"
#include "kernel/tls_patch.h"
#include <cstdio>
#include <cstdlib>

namespace {
[[noreturn]] void UnexpectedDependency(const char* name) {
    std::fprintf(stderr, "FAIL context-only characterization reached forbidden dependency: %s\n", name);
    std::abort();
}
}

namespace Platform {
bool SetThreadName(const char*) { UnexpectedDependency("Platform::SetThreadName"); }
}
namespace ConfigService {
Config EffectiveFor(const std::string&) { UnexpectedDependency("ConfigService::EffectiveFor"); }
}
extern "C" u64 InvokeGuestOnStack(u64, u64, u64) {
    UnexpectedDependency("InvokeGuestOnStack");
}
extern "C" void SetHostStackPointer(uintptr_t) {
    UnexpectedDependency("SetHostStackPointer");
}
namespace Kernel {
void ArmWatchpointForCurrentThread() { UnexpectedDependency("Kernel::ArmWatchpointForCurrentThread"); }
void RegisterThread(const ThreadContext&) { UnexpectedDependency("Kernel::RegisterThread"); }
guest_addr_t ResolveGuestThreadPointer(u64) { UnexpectedDependency("Kernel::ResolveGuestThreadPointer"); }
void InitializeSyscallTable() { UnexpectedDependency("Kernel::InitializeSyscallTable"); }
s64 HandleSyscall(u32, CONTEXT*) { UnexpectedDependency("Kernel::HandleSyscall"); }
void RegisterSyscallHandler(u32, SyscallHandler) { UnexpectedDependency("Kernel::RegisterSyscallHandler"); }
namespace TlsPatch {
void BindCurrentThread(guest_addr_t) { UnexpectedDependency("Kernel::TlsPatch::BindCurrentThread"); }
}
}
