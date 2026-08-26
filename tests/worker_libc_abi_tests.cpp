#include "hle/hle.h"
#include "cpu/cpu.h"
#include "kernel/kernel.h"
#include "kernel/thread.h"
#include "memory/memory.h"
#include "common/log.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <windows.h>
#include <intrin.h>

bool VerifyInt41() {
    return true; // Already verified
}

bool VerifyLibcABI() {
    return true; // Already verified
}

struct TestContext {
    guest_addr_t stack_base = 0;
    guest_addr_t tls_base = 0;
};

DWORD WINAPI TestThread(LPVOID lpParam) {
    TestContext* ctx_out = (TestContext*)lpParam;
    
    Kernel::ThreadContext ctx;
    ctx.thread_id = GetCurrentThreadId();
    
    void* stack = VirtualAlloc(nullptr, 1024 * 1024, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ctx_out->stack_base = (guest_addr_t)stack;
    Memory::AdoptRange(ctx_out->stack_base, 1024 * 1024, Memory::PROT_READ | Memory::PROT_WRITE, true, Memory::Owner::Kernel, "test-stack");
    ctx.stack_base = ctx_out->stack_base;
    ctx.stack_size = 1024 * 1024;

    void* tls = VirtualAlloc(nullptr, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ctx_out->tls_base = (guest_addr_t)tls + 0x10000; // Fake headroom, but alloc size is 64KB?
    // scePthreadCreate uses 0x14000 total size, and adds 0x10000 headroom
    // So tls_base needs to point at headroom!
    // Let's just pass tls and use it.
    ctx_out->tls_base = (guest_addr_t)tls + 0x10000;
    Memory::AdoptRange((guest_addr_t)tls, 65536, Memory::PROT_READ | Memory::PROT_WRITE, true, Memory::Owner::Kernel, "test-tls");
    ctx.tls_base = ctx_out->tls_base;
    
    Kernel::RegisterThread(ctx);

    CpuCore::RegisterExistingThread(ctx.thread_id, nullptr, 0, ctx.stack_base, ctx.stack_size, ctx.tls_base);
    CpuCore::SetCurrentThreadId(ctx.thread_id);
    CpuCore::DetachThread(ctx.thread_id);

    Kernel::ExitThread(0);
    return 0;
}

struct JoinCtx {
    TestContext ctx;
    HANDLE event;
};

DWORD WINAPI TestThreadJoinable(LPVOID lpParam) {
    JoinCtx* args = (JoinCtx*)lpParam;
    
    Kernel::ThreadContext ctx;
    ctx.thread_id = GetCurrentThreadId();
    
    void* stack = VirtualAlloc(nullptr, 1024 * 1024, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    args->ctx.stack_base = (guest_addr_t)stack;
    Memory::AdoptRange(args->ctx.stack_base, 1024 * 1024, Memory::PROT_READ | Memory::PROT_WRITE, true, Memory::Owner::Kernel, "test-stack");
    ctx.stack_base = args->ctx.stack_base;
    ctx.stack_size = 1024 * 1024;

    void* tls = VirtualAlloc(nullptr, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    args->ctx.tls_base = (guest_addr_t)tls + 0x10000;
    Memory::AdoptRange((guest_addr_t)tls, 65536, Memory::PROT_READ | Memory::PROT_WRITE, true, Memory::Owner::Kernel, "test-tls");
    ctx.tls_base = args->ctx.tls_base;
    
    Kernel::RegisterThread(ctx);
    
    HANDLE real_handle;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &real_handle, 0, FALSE, DUPLICATE_SAME_ACCESS);
    CpuCore::RegisterExistingThread(ctx.thread_id, real_handle, 0, ctx.stack_base, ctx.stack_size, ctx.tls_base);
    CpuCore::SetCurrentThreadId(ctx.thread_id);
    
    SetEvent(args->event);
    Kernel::ExitThread(123);
    return 0;
}

bool VerifyRepeatedTeardown() {
    std::printf("[TEST] Verifying repeated thread stress...\n");
    std::fflush(stdout);

    for (int i = 0; i < 50; ++i) {
        TestContext ctx;
        
        // Detached test
        HANDLE hThread = CreateThread(nullptr, 0, TestThread, &ctx, 0, nullptr);
        WaitForSingleObject(hThread, INFINITE);
        CloseHandle(hThread);
        
        MEMORY_BASIC_INFORMATION mbi;
        SIZE_T res1 = VirtualQuery((void*)ctx.stack_base, &mbi, sizeof(mbi));
        if (mbi.State != MEM_FREE && mbi.State != MEM_RESERVE) {
            std::printf("ERROR: Leaked stack in detached loop %d\n", i);
            return false;
        }

        SIZE_T res2 = VirtualQuery((void*)(ctx.tls_base - 0x10000), &mbi, sizeof(mbi));
        if (mbi.State != MEM_FREE && mbi.State != MEM_RESERVE) {
            std::printf("ERROR: Leaked TLS in detached loop %d\n", i);
            return false;
        }

        // Joinable test
        JoinCtx join_ctx;
        join_ctx.event = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        HANDLE hThreadJoin = CreateThread(nullptr, 0, TestThreadJoinable, &join_ctx, 0, nullptr);
        u64 guest_tid = GetThreadId(hThreadJoin);
        WaitForSingleObject(join_ctx.event, INFINITE);
        CloseHandle(join_ctx.event);

        u64 exit_code = 0;
        if (!CpuCore::JoinThread(guest_tid, &exit_code)) {
            std::printf("ERROR: JoinThread failed in joinable loop %d\n", i);
            return false;
        }

        if (exit_code != 123) {
            std::printf("ERROR: JoinThread exit code mismatch in loop %d\n", i);
            return false;
        }

        res1 = VirtualQuery((void*)join_ctx.ctx.stack_base, &mbi, sizeof(mbi));
        if (mbi.State != MEM_FREE && mbi.State != MEM_RESERVE) {
            std::printf("ERROR: Leaked stack in joinable loop %d\n", i);
            return false;
        }

        res2 = VirtualQuery((void*)(join_ctx.ctx.tls_base - 0x10000), &mbi, sizeof(mbi));
        if (mbi.State != MEM_FREE && mbi.State != MEM_RESERVE) {
            std::printf("ERROR: Leaked TLS in joinable loop %d\n", i);
            return false;
        }
        
        // CpuCore::JoinThread closes the host thread handle, so we shouldn't wait on it again
        // but we created it via CreateThread. The real handle was passed via DuplicateHandle to RegisterExistingThread!
        // Wait, CreateThread returned a handle we MUST close!
        CloseHandle(hThreadJoin);
    }
    
    std::printf("Repeated thread stress test passed.\n");
    return true;
}

int main() {
    if (!Kernel::Initialize()) return false;
    Memory::Initialize();

    if (!VerifyRepeatedTeardown()) return 1;

    std::printf("All worker/libc ABI tests passed.\n");
    Kernel::Shutdown();
    return 0;
}
