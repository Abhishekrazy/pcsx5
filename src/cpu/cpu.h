#pragma once
//
// CPU core subsystem for PCSX5.
//
// PS5 uses an AMD Zen-based x86_64 CPU. Since the guest and host architectures
// are identical, we use **direct execution** — guest code runs natively without
// translation. The CPU core's role is to:
//
// 1. Manage per-thread CPU state (register context)
// 2. Create and manage guest threads (mapped to host OS threads)
// 3. Provide the syscall handler table for guest kernel calls
// 4. Synchronize guest threads
//
// Key design decisions:
// - CPUState captures the full x86_64 register context for a thread
// - Guest threads are mapped 1:1 to host OS threads via CreateThread
// - The VEH handler in kernel.cpp handles INT 3 (syscall interception) and
//   FS segment (TLS) access — the CPU core provides the syscall dispatch table
// - No JIT compiler is needed; guest code executes natively
//

#include "../common/types.h"
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <mutex>

// ---------------------------------------------------------------------------
// CPUState — full x86_64 register context for one guest thread
// ---------------------------------------------------------------------------
struct CPUState {
    // General-purpose registers (64-bit)
    u64 rax = 0;
    u64 rbx = 0;
    u64 rcx = 0;
    u64 rdx = 0;
    u64 rsi = 0;
    u64 rdi = 0;
    u64 rsp = 0;
    u64 rbp = 0;
    u64 r8  = 0;
    u64 r9  = 0;
    u64 r10 = 0;
    u64 r11 = 0;
    u64 r12 = 0;
    u64 r13 = 0;
    u64 r14 = 0;
    u64 r15 = 0;

    // Instruction pointer and flags
    u64 rip = 0;
    u64 rflags = 0;

    // XMM registers (128-bit each, stored as two 64-bit halves)
    struct XmmReg {
        u64 lo = 0;
        u64 hi = 0;
    } xmm[16] = {};

    // MXCSR control/status register
    u32 mxcsr = 0x1F80; // default: all exceptions masked, round-to-nearest

    // Convert to/from PCONTEXT for VEH integration
    void FromContext(PCONTEXT ctx);
    void ToContext(PCONTEXT ctx) const;
};

// ---------------------------------------------------------------------------
// GuestThread — represents one guest thread mapped to a host OS thread
// ---------------------------------------------------------------------------
struct GuestThread {
    u64 id = 0;                    // guest thread ID (monotonically increasing)
    std::string name;               // thread name (for logging)
    guest_addr_t entry_point = 0;  // entry point VA
    guest_addr_t stack_base = 0;   // guest stack base
    u64 stack_size = 0;            // guest stack size
    guest_addr_t tls_base = 0;     // guest TLS base for this thread

    // Host OS thread handle and native thread ID
    void* host_thread = nullptr;
    unsigned long host_thread_id = 0;

    // CPU state snapshot (captured on context switch or crash)
    CPUState cpu_state;

    // Running state
    bool is_running = false;
    bool is_joined = false;

    // Synchronization
    std::mutex mtx;
    std::condition_variable cv;

    // Callback invoked when the thread exits
    std::function<void(u64 exit_code)> on_exit;

    // Thread entry function (runs on the host thread)
    static unsigned long __stdcall ThreadEntrypoint(void* arg);
};

// ---------------------------------------------------------------------------
// Syscall handler type
// ---------------------------------------------------------------------------
using SyscallHandler = u64 (*)(u64 arg1, u64 arg2, u64 arg3, u64 arg4, u64 arg5, u64 arg6);

// ---------------------------------------------------------------------------
// CpuCore — top-level CPU subsystem API
// ---------------------------------------------------------------------------
namespace CpuCore {

    // Initialize the CPU core subsystem
    bool Initialize();

    // Shutdown the CPU core subsystem
    void Shutdown();

    // Create a new guest thread. Returns the guest thread ID, or 0 on failure.
    u64 CreateThread(guest_addr_t entry_point,
                     guest_addr_t stack_base,
                     u64 stack_size,
                     const char* name = nullptr);

    // Get the current thread's CPU state (captures registers from VEH context)
    CPUState& CurrentCpuState();

    // Get a guest thread by ID (thread-safe)
    GuestThread* GetThreadById(u64 thread_id);

    // Get all active guest threads
    std::vector<GuestThread*> GetAllThreads();

    // Signal a thread to wake up (for condvar / wait synchronization)
    void WakeThread(u64 thread_id);

    // Register a syscall handler for a specific syscall number
    void RegisterSyscall(u32 syscall_number, SyscallHandler handler);

    // Unregister a syscall handler
    void UnregisterSyscall(u32 syscall_number);

    // Invoke a syscall handler and return the result (called from VEH handler)
    u64 InvokeSyscall(u32 syscall_number,
                      u64 arg1, u64 arg2, u64 arg3,
                      u64 arg4, u64 arg5, u64 arg6);

    // Get the default syscall handler (returns ENOSYS / -38)
    static u64 DefaultSyscallHandler(u64, u64, u64, u64, u64, u64);

    // Register all default syscalls (called during Initialize)
    void RegisterDefaultSyscalls();

    // Get the next available guest thread ID
    static u64 NextThreadId();

} // namespace CpuCore
