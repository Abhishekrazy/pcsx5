//
// CPU core subsystem implementation for PCSX5.
//
// This is the single owner of the guest thread registry: every guest thread
// (created via sys_thr_new, scePthreadCreate, or registered externally) is a
// GuestThread mapped 1:1 to a host OS thread.  The syscall entry points here
// are thin adapters over the kernel syscall table in src/kernel/syscalls.cpp
// — no parallel dispatch table is maintained.
//

#include "cpu.h"
#include "../common/log.h"
#include "../hle/hle.h"
#include "../kernel/syscalls.h"
#include "../kernel/kernel.h"
#include "../kernel/tls_patch.h"
#include <unordered_map>

namespace {

// ---------------------------------------------------------------------------
// Guest thread registry
// ---------------------------------------------------------------------------
std::mutex g_registry_mutex;
std::unordered_map<u64, std::unique_ptr<GuestThread>> g_threads;
u64 g_next_thread_id = 1;
constexpr size_t kMaxThreads = 1024;

thread_local u64 g_current_thread_id = 0;

// Custom syscall handlers registered through CpuCore::RegisterSyscall,
// dispatched via a thunk installed in the kernel syscall table.
std::mutex g_syscall_mutex;
std::unordered_map<u32, SyscallHandler> g_custom_syscalls;

bool g_initialized = false;

// Release guest stack/TLS allocations owned by a thread (if any).
void FreeThreadGuestMemory(GuestThread& thread) {
    if (thread.stack_base) {
        VirtualFree(reinterpret_cast<void*>(static_cast<uintptr_t>(thread.stack_base)), 0, MEM_RELEASE);
        thread.stack_base = 0;
    }
    if (thread.tls_base) {
        VirtualFree(reinterpret_cast<void*>(static_cast<uintptr_t>(thread.tls_base)), 0, MEM_RELEASE);
        thread.tls_base = 0;
    }
}

// Common exit path for GuestThread::ThreadEntrypoint.
void HandleThreadExit(u64 thread_id, u64 exit_code) {
    std::function<void(u64)> on_exit;
    {
        std::lock_guard<std::mutex> lock(g_registry_mutex);
        auto it = g_threads.find(thread_id);
        if (it == g_threads.end()) {
            return;
        }
        GuestThread& thread = *it->second;
        thread.is_running = false;
        if (thread.wake_event) {
            CloseHandle(static_cast<HANDLE>(thread.wake_event));
            thread.wake_event = nullptr;
        }
        on_exit = thread.on_exit;

        if (thread.detached) {
            // Detached threads clean themselves up; the host handle was
            // already closed by CpuCore::DetachThread.
            FreeThreadGuestMemory(thread);
            g_threads.erase(it);
        }
    }
    if (on_exit) {
        on_exit(exit_code);
    }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// CPUState <-> PCONTEXT conversion (used by the VEH in kernel.cpp)
// ---------------------------------------------------------------------------
void CPUState::FromContext(PCONTEXT ctx) {
    rax = ctx->Rax; rbx = ctx->Rbx; rcx = ctx->Rcx; rdx = ctx->Rdx;
    rsi = ctx->Rsi; rdi = ctx->Rdi; rsp = ctx->Rsp; rbp = ctx->Rbp;
    r8  = ctx->R8;  r9  = ctx->R9;  r10 = ctx->R10; r11 = ctx->R11;
    r12 = ctx->R12; r13 = ctx->R13; r14 = ctx->R14; r15 = ctx->R15;
    rip = ctx->Rip;
    rflags = ctx->EFlags;
    const M128A* xmm_regs = &ctx->Xmm0;
    for (int i = 0; i < 16; ++i) {
        xmm[i].lo = xmm_regs[i].Low;
        xmm[i].hi = static_cast<u64>(xmm_regs[i].High);
    }
    mxcsr = ctx->FltSave.MxCsr;
}

void CPUState::ToContext(PCONTEXT ctx) const {
    ctx->Rax = rax; ctx->Rbx = rbx; ctx->Rcx = rcx; ctx->Rdx = rdx;
    ctx->Rsi = rsi; ctx->Rdi = rdi; ctx->Rsp = rsp; ctx->Rbp = rbp;
    ctx->R8  = r8;  ctx->R9  = r9;  ctx->R10 = r10; ctx->R11 = r11;
    ctx->R12 = r12; ctx->R13 = r13; ctx->R14 = r14; ctx->R15 = r15;
    ctx->Rip = rip;
    ctx->EFlags = static_cast<DWORD>(rflags);
    M128A* xmm_regs = &ctx->Xmm0;
    for (int i = 0; i < 16; ++i) {
        xmm_regs[i].Low = xmm[i].lo;
        xmm_regs[i].High = static_cast<s64>(xmm[i].hi);
    }
    ctx->FltSave.MxCsr = mxcsr;
}

// ---------------------------------------------------------------------------
// GuestThread host entry point
// ---------------------------------------------------------------------------
unsigned long __stdcall GuestThread::ThreadEntrypoint(void* arg) {
    GuestThread* self = static_cast<GuestThread*>(arg);
    const u64 id        = self->id;
    const u64 entry     = self->entry_point;
    const u64 argument  = self->argument;

    CpuCore::SetCurrentThreadId(id);

    // This host thread executes guest code: bind its guest thread pointer so
    // patched TLS stubs (Kernel::TlsPatch) resolve fs-relative accesses for
    // this thread.  The value MUST match what the VEH emulation path would
    // use (Kernel::ResolveGuestThreadPointer) — binding the CpuCore tls_base
    // directly diverges when the kernel map has no entry for this thread
    // (shared-TLS fallback) and corrupts worker threads.
    Kernel::TlsPatch::BindCurrentThread(Kernel::ResolveGuestThreadPointer(id));
    {
        ULONG guarantee = 64 * 1024;
        SetThreadStackGuarantee(&guarantee);
    }

    // Initialize this thread's per-thread host stack pointer.
    // SetHostStackPointer stores the current RSP so that if the very first HLE call
    // happens before InvokeGuestFunction has a chance to update it, it still has a valid
    // host stack to return to.  InvokeGuestFunction will overwrite it with a more precise
    // value immediately on entry.
    SetHostStackPointer(reinterpret_cast<uintptr_t>(&entry));

    LOG_INFO(Cpu, "Guest thread %llu starting at entry=0x%llx, arg=0x%llx", id, entry, argument);

    // Execute the guest thread entry point.
    // The guest function receives its argument in rdi (SysV ABI first arg).
    u64 ret = InvokeGuestFunction(entry, argument, 0, 0);

    LOG_INFO(Cpu, "Guest thread %llu exited with 0x%llx", id, ret);
    HandleThreadExit(id, ret);
    return static_cast<unsigned long>(ret);
}

// ---------------------------------------------------------------------------
// CpuCore
// ---------------------------------------------------------------------------
namespace CpuCore {

bool Initialize() {
    if (g_initialized) {
        return true;
    }
    g_initialized = true;
    LOG_INFO(Cpu, "CPU core initialized (direct execution, x86-64 host)");
    return true;
}

void Shutdown() {
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    for (auto& [id, thread] : g_threads) {
        if (thread->is_running && thread->host_thread) {
            ::TerminateThread(static_cast<HANDLE>(thread->host_thread), 0);
        }
        if (thread->host_thread) {
            CloseHandle(static_cast<HANDLE>(thread->host_thread));
        }
        if (thread->wake_event) {
            CloseHandle(static_cast<HANDLE>(thread->wake_event));
        }
    }
    g_threads.clear();
    g_initialized = false;
    LOG_INFO(Cpu, "CPU core shut down");
}

u64 NextThreadId() {
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    u64 tid = g_next_thread_id++;
    if (tid == 0) {
        tid = g_next_thread_id++;
    }
    return tid;
}

void SetCurrentThreadId(u64 thread_id) {
    g_current_thread_id = thread_id;
}

u64 GetCurrentThreadId() {
    return g_current_thread_id;
}

u64 CreateThread(guest_addr_t entry_point, guest_addr_t stack_base, u64 stack_size,
                 guest_addr_t tls_base, u64 argument, const char* name) {
    auto thread = std::make_unique<GuestThread>();
    thread->entry_point = entry_point;
    thread->stack_base  = stack_base;
    thread->stack_size  = stack_size;
    thread->tls_base    = tls_base;
    thread->argument    = argument;
    thread->name        = name ? name : "<unnamed>";
    thread->wake_event  = CreateEventA(nullptr, FALSE, FALSE, nullptr);

    GuestThread* raw = thread.get();
    {
        std::lock_guard<std::mutex> lock(g_registry_mutex);
        if (g_threads.size() >= kMaxThreads) {
            CloseHandle(static_cast<HANDLE>(thread->wake_event));
            return 0;
        }
        u64 id = g_next_thread_id++;
        if (id == 0) {
            id = g_next_thread_id++;
        }
        raw->id = id;
        g_threads.emplace(id, std::move(thread));
    }

    // Use 1MB as Windows host thread stack (guest stack is separate).
    constexpr SIZE_T kHostStackSize = 1 * 1024 * 1024;
    DWORD host_tid = 0;
    HANDLE handle = ::CreateThread(
        nullptr,         // Default security attributes
        kHostStackSize,  // Host stack size (not the guest stack)
        &GuestThread::ThreadEntrypoint,
        raw,
        0,               // Run immediately
        &host_tid
    );

    if (handle == nullptr) {
        LOG_ERROR(Cpu, "CreateThread: host CreateThread failed (err=%lu)", GetLastError());
        std::lock_guard<std::mutex> lock(g_registry_mutex);
        CloseHandle(static_cast<HANDLE>(raw->wake_event));
        g_threads.erase(raw->id);
        return 0;
    }

    raw->host_thread = handle;
    raw->host_thread_id = host_tid;
    raw->is_running = true;

    LOG_INFO(Cpu, "Created guest thread '%s' (id=%llu, entry=0x%llx, stack=0x%llx, tls=0x%llx)",
             raw->name.c_str(), raw->id, entry_point, stack_base, tls_base);
    return raw->id;
}

bool RegisterExistingThread(u64 thread_id, void* host_handle,
                            guest_addr_t entry_point, guest_addr_t stack_base,
                            u64 stack_size, guest_addr_t tls_base) {
    auto thread = std::make_unique<GuestThread>();
    thread->id          = thread_id;
    thread->entry_point = entry_point;
    thread->stack_base  = stack_base;
    thread->stack_size  = stack_size;
    thread->tls_base    = tls_base;
    thread->host_thread = host_handle;
    thread->is_running  = true;
    thread->wake_event  = CreateEventA(nullptr, FALSE, FALSE, nullptr);

    std::lock_guard<std::mutex> lock(g_registry_mutex);
    if (g_threads.size() >= kMaxThreads) {
        CloseHandle(static_cast<HANDLE>(thread->wake_event));
        return false;
    }
    g_threads.emplace(thread_id, std::move(thread));
    return true;
}

bool UnregisterThread(u64 thread_id) {
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    auto it = g_threads.find(thread_id);
    if (it == g_threads.end() || !it->second->is_running) {
        return false;
    }
    GuestThread& thread = *it->second;
    thread.is_running = false;
    if (thread.wake_event) {
        CloseHandle(static_cast<HANDLE>(thread.wake_event));
        thread.wake_event = nullptr;
    }
    return true;
}

CPUState& CurrentCpuState() {
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    auto it = g_threads.find(g_current_thread_id);
    if (it != g_threads.end()) {
        return it->second->cpu_state;
    }
    // Threads not in the registry (e.g. the main thread) get a dummy slot.
    static thread_local CPUState s_dummy;
    return s_dummy;
}

GuestThread* GetThreadById(u64 thread_id) {
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    auto it = g_threads.find(thread_id);
    return (it != g_threads.end()) ? it->second.get() : nullptr;
}

std::vector<GuestThread*> GetAllThreads() {
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    std::vector<GuestThread*> out;
    out.reserve(g_threads.size());
    for (auto& [id, thread] : g_threads) {
        out.push_back(thread.get());
    }
    return out;
}

bool JoinThread(u64 thread_id, u64* out_exit_code) {
    HANDLE handle = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_registry_mutex);
        auto it = g_threads.find(thread_id);
        if (it == g_threads.end() || it->second->detached || it->second->is_joined) {
            return false;
        }
        handle = static_cast<HANDLE>(it->second->host_thread);
        it->second->is_joined = true;
    }

    if (handle == nullptr) {
        return false;
    }

    if (WaitForSingleObject(handle, INFINITE) != WAIT_OBJECT_0) {
        LOG_ERROR(Cpu, "JoinThread: WaitForSingleObject failed (err=%lu)", GetLastError());
        return false;
    }

    DWORD exit_code = 0;
    GetExitCodeThread(handle, &exit_code);
    CloseHandle(handle);

    {
        std::lock_guard<std::mutex> lock(g_registry_mutex);
        auto it = g_threads.find(thread_id);
        if (it != g_threads.end()) {
            FreeThreadGuestMemory(*it->second);
            g_threads.erase(it);
        }
    }

    if (out_exit_code) {
        *out_exit_code = static_cast<u64>(exit_code);
    }
    return true;
}

bool DetachThread(u64 thread_id) {
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    auto it = g_threads.find(thread_id);
    if (it == g_threads.end() || it->second->is_joined) {
        return false;
    }
    GuestThread& thread = *it->second;
    thread.detached = true;
    // The thread cleans itself up on exit (see HandleThreadExit).
    if (thread.host_thread) {
        CloseHandle(static_cast<HANDLE>(thread.host_thread));
        thread.host_thread = nullptr;
    }
    return true;
}

bool SuspendCurrentThread(s64 timeout_ms) {
    HANDLE wake_evt = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_registry_mutex);
        auto it = g_threads.find(g_current_thread_id);
        if (it != g_threads.end() && it->second->is_running) {
            wake_evt = static_cast<HANDLE>(it->second->wake_event);
        }
    }
    if (!wake_evt) {
        return false;
    }
    DWORD ms = (timeout_ms < 0) ? INFINITE : static_cast<DWORD>(timeout_ms);
    return WaitForSingleObject(wake_evt, ms) == WAIT_OBJECT_0;
}

bool WakeThread(u64 thread_id) {
    HANDLE wake_evt = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_registry_mutex);
        auto it = g_threads.find(thread_id);
        if (it != g_threads.end() && it->second->is_running) {
            wake_evt = static_cast<HANDLE>(it->second->wake_event);
        }
    }
    if (wake_evt) {
        SetEvent(wake_evt);
        return true;
    }
    return false;
}

bool CheckThreadActive(u64 thread_id) {
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    auto it = g_threads.find(thread_id);
    return it != g_threads.end() && it->second->is_running;
}

bool TerminateThread(u64 thread_id) {
    HANDLE handle = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_registry_mutex);
        auto it = g_threads.find(thread_id);
        if (it == g_threads.end() || !it->second->is_running) {
            return false;
        }
        GuestThread& thread = *it->second;
        thread.is_running = false;
        handle = static_cast<HANDLE>(thread.host_thread);
        if (thread.wake_event) {
            CloseHandle(static_cast<HANDLE>(thread.wake_event));
            thread.wake_event = nullptr;
        }
    }
    if (handle) {
        ::TerminateThread(handle, 0);
        return true;
    }
    return false;
}

guest_addr_t GetThreadTlsBase(u64 thread_id) {
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    auto it = g_threads.find(thread_id);
    return (it != g_threads.end()) ? it->second->tls_base : 0;
}

// ---------------------------------------------------------------------------
// Syscall adapters — delegate to the kernel syscall table (syscalls.cpp)
// ---------------------------------------------------------------------------
void RegisterSyscall(u32 syscall_number, SyscallHandler handler) {
    {
        std::lock_guard<std::mutex> lock(g_syscall_mutex);
        g_custom_syscalls[syscall_number] = handler;
    }
    // Install a thunk in the kernel table that unpacks the VEH context and
    // calls the registered handler.  The syscall number is in RAX per the
    // FreeBSD/PS5 syscall convention.
    Kernel::RegisterSyscallHandler(syscall_number, [](CONTEXT* ctx) -> s64 {
        SyscallHandler fn = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_syscall_mutex);
            auto it = g_custom_syscalls.find(static_cast<u32>(ctx->Rax));
            if (it != g_custom_syscalls.end()) {
                fn = it->second;
            }
        }
        if (!fn) {
            return -38; // ENOSYS
        }
        return static_cast<s64>(fn(ctx->Rdi, ctx->Rsi, ctx->Rdx, ctx->R10, ctx->R8, ctx->R9));
    });
}

void UnregisterSyscall(u32 syscall_number) {
    {
        std::lock_guard<std::mutex> lock(g_syscall_mutex);
        g_custom_syscalls.erase(syscall_number);
    }
    Kernel::RegisterSyscallHandler(syscall_number, nullptr);
}

u64 InvokeSyscall(u32 syscall_number,
                  u64 arg1, u64 arg2, u64 arg3,
                  u64 arg4, u64 arg5, u64 arg6) {
    // Route through the same kernel dispatcher used by the VEH: synthesize a
    // context from the SysV argument registers and dispatch by number.
    CONTEXT ctx = {};
    ctx.Rax = syscall_number;
    ctx.Rdi = arg1;
    ctx.Rsi = arg2;
    ctx.Rdx = arg3;
    ctx.R10 = arg4;
    ctx.R8  = arg5;
    ctx.R9  = arg6;
    return static_cast<u64>(Kernel::HandleSyscall(syscall_number, &ctx));
}

u64 DefaultSyscallHandler(u64, u64, u64, u64, u64, u64) {
    return static_cast<u64>(-38); // ENOSYS
}

void RegisterDefaultSyscalls() {
    Kernel::InitializeSyscallTable();
}

} // namespace CpuCore
