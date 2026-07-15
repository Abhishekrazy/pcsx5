#include "kernel.h"
#include "thread.h"
#include "syscalls.h"
#include "../memory/memory.h"
#include "../hle/hle.h"
#include "../common/types.h"
#include <windows.h>
#include <vector>
#include <mutex>

namespace Kernel {

// Thread management globals
static std::mutex g_thread_mutex;
static u64 g_next_thread_id = 1;
static thread_local u64 g_current_thread_id = 0;

// Thread context storage
struct ThreadInfo {
    u64 tid;
    HANDLE handle;
    guest_addr_t entry;
    guest_addr_t stack;
    u64 stack_size;
    guest_addr_t tls_base;
    HANDLE wake_event;
    bool active;
};

static std::vector<ThreadInfo> g_threads;
static const u64 MAX_THREADS = 1024;

u64 AllocateThreadId() {
    std::lock_guard<std::mutex> lock(g_thread_mutex);
    u64 tid = g_next_thread_id++;
    if (tid == 0) {
        tid = g_next_thread_id++;
    }
    return tid;
}

void SetCurrentThreadId(u64 tid) {
    g_current_thread_id = tid;
}

u64 GetCurrentThreadId() {
    return g_current_thread_id;
}

bool RegisterThread(u64 tid, HANDLE handle, guest_addr_t entry, guest_addr_t stack, u64 stack_size, guest_addr_t tls_base) {
    std::lock_guard<std::mutex> lock(g_thread_mutex);
    
    if (g_threads.size() >= MAX_THREADS) {
        return false;
    }
    
    ThreadInfo info;
    info.tid = tid;
    info.handle = handle;
    info.entry = entry;
    info.stack = stack;
    info.stack_size = stack_size;
    info.tls_base = tls_base;
    info.wake_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    info.active = true;
    
    g_threads.push_back(info);
    return true;
}

bool UnregisterThread(u64 tid) {
    std::lock_guard<std::mutex> lock(g_thread_mutex);
    for (auto& thread : g_threads) {
        if (thread.tid == tid && thread.active) {
            thread.active = false;
            if (thread.wake_event) {
                CloseHandle(thread.wake_event);
                thread.wake_event = nullptr;
            }
            return true;
        }
    }
    return false;
}

DWORD WINAPI ThreadEntryPoint(LPVOID lpParameter) {
    ThreadContext* ctx = static_cast<ThreadContext*>(lpParameter);
    
    // Set current thread ID
    SetCurrentThreadId(ctx->thread_id);
    
    // Execute the thread entry point
    // Note: The entry point is in guest memory, so in a full implementation we would execute it.
    // For now we delete ctx and return.
    delete ctx;
    return 0;
}

HANDLE CreateThread(guest_addr_t entry, guest_addr_t stack, u64 stack_size, guest_addr_t tls_base, u64* out_tid) {
    u64 tid = AllocateThreadId();
    
    ThreadContext* ctx = new ThreadContext();
    ctx->thread_id = tid;
    ctx->entry_point = entry;
    ctx->stack_base = stack;
    ctx->stack_size = stack_size;
    ctx->tls_base = tls_base;
    
    HANDLE handle = ::CreateThread(
        nullptr,                    // Default security attributes
        static_cast<SIZE_T>(stack_size), // Stack size
        ThreadEntryPoint,           // Thread function
        ctx,                        // Parameter
        0,                          // Default creation flags
        nullptr                     // Thread ID
    );
    
    if (handle == nullptr) {
        delete ctx;
        return nullptr;
    }
    
    if (!RegisterThread(tid, handle, entry, stack, stack_size, tls_base)) {
        CloseHandle(handle);
        delete ctx;
        return nullptr;
    }
    
    if (out_tid) {
        *out_tid = tid;
    }
    
    return handle;
}

void ExitThread(u64 status) {
    u64 tid = GetCurrentThreadId();
    UnregisterThread(tid);
    
    // Clean up thread resources
    ::ExitThread(static_cast<DWORD>(status));
}

struct timespec {
    s64 tv_sec;
    s64 tv_nsec;
};

bool SuspendCurrentThread(const struct timespec* timeout) {
    u64 tid = GetCurrentThreadId();
    HANDLE wake_evt = nullptr;
    
    {
        std::lock_guard<std::mutex> lock(g_thread_mutex);
        for (const auto& thread : g_threads) {
            if (thread.tid == tid && thread.active) {
                wake_evt = thread.wake_event;
                break;
            }
        }
    }
    
    if (!wake_evt) {
        return false;
    }
    
    DWORD ms = INFINITE;
    if (timeout) {
        ms = static_cast<DWORD>(timeout->tv_sec * 1000 + timeout->tv_nsec / 1000000);
    }
    
    DWORD res = WaitForSingleObject(wake_evt, ms);
    return (res == WAIT_OBJECT_0);
}

bool WakeThread(u64 tid) {
    HANDLE wake_evt = nullptr;
    
    {
        std::lock_guard<std::mutex> lock(g_thread_mutex);
        for (const auto& thread : g_threads) {
            if (thread.tid == tid && thread.active) {
                wake_evt = thread.wake_event;
                break;
            }
        }
    }
    
    if (wake_evt) {
        SetEvent(wake_evt);
        return true;
    }
    return false;
}

bool CheckThreadActive(u64 tid) {
    std::lock_guard<std::mutex> lock(g_thread_mutex);
    for (const auto& thread : g_threads) {
        if (thread.tid == tid && thread.active) {
            return true;
        }
    }
    return false;
}

bool TerminateThreadByTid(u64 tid) {
    HANDLE hThread = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_thread_mutex);
        for (auto& thread : g_threads) {
            if (thread.tid == tid && thread.active) {
                hThread = thread.handle;
                thread.active = false;
                if (thread.wake_event) {
                    CloseHandle(thread.wake_event);
                    thread.wake_event = nullptr;
                }
                break;
            }
        }
    }
    
    if (hThread) {
        TerminateThread(hThread, 0);
        return true;
    }
    return false;
}

} // namespace Kernel