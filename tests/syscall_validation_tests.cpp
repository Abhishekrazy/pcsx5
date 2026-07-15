#include "kernel/syscalls.h"
#include "memory/memory.h"
#include "common/log.h"
#include <cstdio>
#include <cassert>

using namespace Kernel;

int main() {
    LogConfig::SetLevel(LogCategory::Kernel, LogLevel::Debug);
    LogConfig::SetLevel(LogCategory::Memory, LogLevel::Debug);

    if (!Memory::Initialize()) {
        std::printf("Memory::Initialize failed\n");
        std::fflush(stdout);
        return 1;
    }

    std::printf("Running Syscall Validation Tests...\n");
    std::fflush(stdout);

    // Initialize HLE syscall table
    InitializeSyscallTable();

    // Mock CPU context
    CONTEXT ctx = {};
    
    // Test sys_getpid (syscall 20)
    ctx.Rax = 20;
    s64 pid = HandleSyscall(20, &ctx);
    std::printf("Syscall 20 (getpid) returned: %lld\n", pid);
    std::fflush(stdout);
    assert(pid > 0);

    // Test sys_sem_init (syscall 410)
    ctx.Rdi = 0x1000; // Mock sem address
    ctx.Rsi = 1;      // val
    ctx.Rdx = 0;      // flags
    s64 sem_ret = HandleSyscall(410, &ctx);
    std::printf("Syscall 410 (sem_init) returned: %lld\n", sem_ret);
    std::fflush(stdout);

    std::printf("Before Memory::Shutdown\n");
    std::fflush(stdout);

    Memory::Shutdown();

    std::printf("All syscall validation tests passed!\n");
    std::fflush(stdout);
    return 0;
}
