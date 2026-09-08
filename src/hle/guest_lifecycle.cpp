#include "guest_lifecycle.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// Per-thread host stack pointer: each guest thread has its own copy so that
// concurrent HLE dispatches on different threads never clobber each other's RSP.
// Accessed from assembly via GetHostStackPointer/SetHostStackPointer helpers.
static __declspec(thread) uintptr_t tls_host_stack_pointer = 0;

extern "C" uintptr_t GetHostStackPointer() {
    return tls_host_stack_pointer;
}

extern "C" void SetHostStackPointer(uintptr_t rsp) {
    tls_host_stack_pointer = rsp;
}

// Thread-local storage for the incoming XMM0 floating-point argument.
// Set by the dispatcher asm before each HLE dispatch so handlers for
// functions like sce::Json::Value::set(double) can read the double value
// from XMM0 (SysV ABI: float args in XMM0-XMM7). GuestArgs only captures
// GPRs (rdi, rsi, rdx, rcx, r8, r9).
static thread_local u64 tls_incoming_xmm0 = 0;
static thread_local u64 tls_incoming_xmm_block[8] = {};

extern "C" void SetIncomingXmm0(u64 val) {
    tls_incoming_xmm0 = val;
}

extern "C" void SetIncomingXmmBlock(const u64* ptr) {
    if (ptr) {
        std::memcpy(tls_incoming_xmm_block, ptr, sizeof(tls_incoming_xmm_block));
        tls_incoming_xmm0 = tls_incoming_xmm_block[0];
    }
}

namespace HLE {
    u64 GetIncomingXmm0() {
        return tls_incoming_xmm0;
    }

    const u64* GetIncomingXmmBlock() {
        return tls_incoming_xmm_block;
    }

    // Cooperative guest shutdown state (see hle.h).  Written by the main
    // thread's window loop (RequestStop) / Kernel::Execute (thread id), read
    // by the HLE dispatch path on guest threads.
    static std::atomic<bool>          g_stop_requested{false};
    static std::atomic<unsigned long> g_main_guest_thread_id{0};
    // Armed by Kernel::Execute while the guest runs on the main guest thread;
    // ExitGuestProcess longjmps here (SEH unwinding cannot cross guest frames).
    static jmp_buf                    g_guest_exit_env;
    static std::atomic<bool>          g_guest_exit_env_armed{false};
    static u32                        g_guest_exit_code = 0;

    // Guest crash tracking --------------------------------------------------
    // Set by the VEH when a guest crash is detected.  The emulator process
    // survives and the SEH handler in TryStartGuest catches the exception.
    static std::atomic<bool>          g_guest_crashed{false};
    static u32                        g_crash_exception_code = 0;
    static u64                        g_crash_rip = 0;
    static std::mutex                 g_crash_mutex;
    static char                       g_crash_message[256] = {};

    void SetGuestCrashed(u32 exception_code, guest_addr_t rip) {
        std::lock_guard<std::mutex> lk(g_crash_mutex);
        g_guest_crashed.store(true, std::memory_order_release);
        g_crash_exception_code = exception_code;
        g_crash_rip = rip;
        std::snprintf(g_crash_message, sizeof(g_crash_message),
                      "GUEST_CRASH: code=0x%08X at RIP=0x%llx",
                      exception_code, static_cast<unsigned long long>(rip));
    }

    bool IsGuestCrashed() {
        return g_guest_crashed.load(std::memory_order_acquire);
    }

    bool GetLastGuestCrashInfo(u32* out_exception_code, guest_addr_t* out_rip,
                                char* out_buf, int buf_size) {
        std::lock_guard<std::mutex> lk(g_crash_mutex);
        if (!g_guest_crashed.load(std::memory_order_acquire)) return false;
        if (out_exception_code) *out_exception_code = g_crash_exception_code;
        if (out_rip) *out_rip = g_crash_rip;
        if (out_buf && buf_size > 0) {
            strncpy_s(out_buf, static_cast<size_t>(buf_size), g_crash_message, _TRUNCATE);
        }
        return true;
    }

    void SetMainGuestThreadId(unsigned long thread_id) {
        g_main_guest_thread_id.store(thread_id, std::memory_order_release);
    }

    void RequestStop() {
        g_stop_requested.store(true, std::memory_order_release);
    }

    bool StopRequested() {
        return g_stop_requested.load(std::memory_order_acquire);
    }

    jmp_buf& GuestExitEnv() {
        return g_guest_exit_env;
    }

    void ArmGuestExitEnv(bool armed) {
        g_guest_exit_env_armed.store(armed, std::memory_order_release);
    }

    u32 GuestExitCode() {
        return g_guest_exit_code;
    }

    void ExitGuestProcess(u32 exit_code) {
        g_guest_exit_code = exit_code;
        RequestStop(); // Signal all threads to stop
        
        if (::GetCurrentThreadId() == g_main_guest_thread_id.load(std::memory_order_acquire) &&
            g_guest_exit_env_armed.load(std::memory_order_acquire)) {
            longjmp(g_guest_exit_env, 1);
        }
        // Off the main guest thread there is no armed setjmp buffer; fall
        // back to terminating the process immediately to avoid letting other
        // active threads crash during host CRT exit teardown.
#ifdef _WIN32
        ::TerminateProcess(::GetCurrentProcess(), exit_code);
        ::Sleep(INFINITE); // Wait for termination
#else
        std::exit(static_cast<int>(exit_code));
#endif
    }

    void ResetGuestCrashState() {
        std::lock_guard<std::mutex> lk(g_crash_mutex);
        g_guest_crashed.store(false, std::memory_order_release);
        g_crash_exception_code = 0;
        g_crash_rip = 0;
        g_crash_message[0] = '\0';
    }

    void ResetGuestLifecycleState() {
        g_stop_requested.store(false, std::memory_order_release);
        g_main_guest_thread_id.store(0, std::memory_order_release);
        g_guest_exit_env_armed.store(false, std::memory_order_release);
        g_guest_exit_code = 0;
        ResetGuestCrashState();
    }
}
