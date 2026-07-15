#define _CRT_SECURE_NO_WARNINGS
#include "hle.h"
#include "../kernel/kernel.h"
#include "../memory/memory.h"
#include "../common/log.h"
#include "../gpu/gpu.h"
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <windows.h>
#include <cstring>
#include <chrono>
#include <thread>
#include <iostream>
#include <unordered_set>
#include <mutex>
#include <io.h>
#include <fcntl.h>
#include <cstdio>
#include <string>

namespace HLE {

    struct SysVAmd64VaList {
        u32 gp_offset;
        u32 fp_offset;
        u64 overflow_arg_area;
        u64 reg_save_area;
    };

    // -----------------------------------------------------------------------
    // Thread tracking for scePthreadCreate / scePthreadJoin / scePthreadDetach
    // -----------------------------------------------------------------------
    struct GuestThreadInfo {
        HANDLE handle;          // Windows thread handle
        u64 guest_tid;          // PS5 thread ID
        u64 entry_point;        // guest function address
        u64 arg;                // argument to pass
        u64 stack_base;         // guest stack base
        u64 stack_size;         // guest stack size
        u64 tls_base;           // TLS base for this thread
        std::string name;
        bool detached = false;  // true if thread was detached (auto-cleanup)
    };

    static std::mutex g_thread_mutex;
    static std::unordered_map<u64, GuestThreadInfo> g_threads;  // keyed by guest_tid
    static std::atomic<u64> g_next_tid{3};  // 1=main, 2=reserved, start at 3

    // Windows thread proc wrapper: translates to SysV ABI and calls the guest entry point.
    static DWORD WINAPI GuestThreadProc(LPVOID param) {
        GuestThreadInfo* info = static_cast<GuestThreadInfo*>(param);

        LOG_INFO(HLE, "Guest thread '%s' (tid=%llu) starting at entry=0x%llx, arg=0x%llx",
                 info->name.c_str(), info->guest_tid, info->entry_point, info->arg);

        // Call the guest entry point via the ABI translation trampoline.
        // InvokeGuestFunction(guest_func_va, rdi_arg, rsi_arg, rdx_arg)
        u64 result = InvokeGuestFunction(info->entry_point, info->arg, 0, 0);

        LOG_INFO(HLE, "Guest thread '%s' (tid=%llu) returned 0x%llx",
                 info->name.c_str(), info->guest_tid, result);

        // If the thread was detached, clean up our tracking info.
        if (info->detached) {
            std::lock_guard<std::mutex> lock(g_thread_mutex);
            g_threads.erase(info->guest_tid);
            // Free the guest stack
            if (info->stack_base) {
                VirtualFree(reinterpret_cast<void*>(static_cast<uintptr_t>(info->stack_base)), 0, MEM_RELEASE);
            }
            delete info;
        }

        return static_cast<DWORD>(result);
    }

    static u64 GetNextVaListArg(SysVAmd64VaList& valist) {
        u64 val = 0;
        if (valist.gp_offset < 48) {
            val = Memory::Read<u64>(valist.reg_save_area + valist.gp_offset);
            valist.gp_offset += 8;
        } else {
            val = Memory::Read<u64>(valist.overflow_arg_area);
            valist.overflow_arg_area += 8;
        }
        return val;
    }

    static std::string FormatGuestString(const std::string& fmt, SysVAmd64VaList& valist) {
        std::string result;
        size_t i = 0;
        while (i < fmt.size()) {
            if (fmt[i] == '%' && i + 1 < fmt.size()) {
                i++;
                if (fmt[i] == '%') {
                    result += '%';
                    i++;
                    continue;
                }
                
                bool is_long = false;
                bool is_long_long = false;
                while (i < fmt.size() && (fmt[i] == '-' || fmt[i] == '+' || fmt[i] == ' ' || fmt[i] == '#' || fmt[i] == '0' || (fmt[i] >= '0' && fmt[i] <= '9') || fmt[i] == '.' || fmt[i] == 'l' || fmt[i] == 'h' || fmt[i] == 'z')) {
                    if (fmt[i] == 'l') {
                        if (i + 1 < fmt.size() && fmt[i+1] == 'l') {
                            is_long_long = true;
                            i++;
                        } else {
                            is_long = true;
                        }
                    }
                    i++;
                }
                
                if (i >= fmt.size()) break;
                
                char type = fmt[i];
                i++;
                
                u64 arg = GetNextVaListArg(valist);
                
                char buf[128];
                if (type == 's') {
                    guest_addr_t str_ptr = static_cast<guest_addr_t>(arg);
                    std::string str_val;
                    if (str_ptr) {
                        u64 offset = 0;
                        while (true) {
                            u8 ch = Memory::Read<u8>(str_ptr + offset++);
                            if (ch == 0) break;
                            str_val += static_cast<char>(ch);
                        }
                    } else {
                        str_val = "(null)";
                    }
                    result += str_val;
                } else if (type == 'd' || type == 'i') {
                    if (is_long_long) {
                        snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(arg));
                    } else if (is_long) {
                        snprintf(buf, sizeof(buf), "%ld", static_cast<long>(arg));
                    } else {
                        snprintf(buf, sizeof(buf), "%d", static_cast<int>(arg));
                    }
                    result += buf;
                } else if (type == 'u') {
                    if (is_long_long) {
                        snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(arg));
                    } else if (is_long) {
                        snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(arg));
                    } else {
                        snprintf(buf, sizeof(buf), "%u", static_cast<unsigned int>(arg));
                    }
                    result += buf;
                } else if (type == 'x' || type == 'X') {
                    std::string fmt_str = is_long_long ? "%ll" : (is_long ? "%l" : "%");
                    fmt_str += type;
                    snprintf(buf, sizeof(buf), fmt_str.c_str(), arg);
                    result += buf;
                } else if (type == 'p') {
                    snprintf(buf, sizeof(buf), "%p", reinterpret_cast<void*>(arg));
                    result += buf;
                } else if (type == 'c') {
                    result += static_cast<char>(arg);
                } else {
                    result += "%" + std::string(1, type);
                }
            } else {
                result += fmt[i];
                i++;
            }
        }
        return result;
    }

    // Helper to register standard stubs
    void RegisterLibKernel() {
        LOG_INFO(HLE, "Registering libkernel HLE symbols...");

        // =====================================================================
        // XKRegsFpEpk#T#T  ===  PS5 __libc_start_main / sceLibcInitialize
        // Called by _start after TLS setup and DT_INIT:
        //   XKRegsFpEpk(argc, argv, envp)  -> should NEVER return to _start
        // We find the game's main() via HLE::GetGuestMainAddress() (set at load time
        // by scanning the symbol table for "main"), then call it via InvokeGuestFunction.
        // =====================================================================
        RegisterSymbol("libkernel", "XKRegsFpEpk#T#T", [](const GuestArgs& args) -> u64 {
            u64 argc_val = args.arg1;   // rdi (argc)
            u64 argv_val = args.arg2;   // rsi (argv pointer)
            // args.arg3 = envp (rdx) = 0 typically

            LOG_INFO(HLE, "XKRegsFpEpk (__libc_start_main): argc=%llu argv=0x%llx", argc_val, argv_val);

            guest_addr_t main_va = HLE::GetGuestMainAddress();
            if (main_va == 0) {
                LOG_WARN(HLE, "XKRegsFpEpk: main() address not found — game may exit immediately.");
                // Return 0; the guest will call exit(0) from _start's fallthrough.
                return 0;
            }

            LOG_INFO(HLE, "XKRegsFpEpk: Calling game main() at 0x%llx", main_va);

            // Call the game's main(argc, argv) using the SYSV-to-Windows ABI trampoline.
            // InvokeGuestFunction updates g_host_stack_pointer so nested HLE calls
            // from within main() correctly route back through our host stack.
            u64 ret = InvokeGuestFunction(main_va, argc_val, argv_val, 0);

            LOG_INFO(HLE, "XKRegsFpEpk: main() returned %llu — calling exit()", ret);

            // After main() returns, show the last frame and exit cleanly.
            GPU::RunIdleLoop();
            std::exit(static_cast<int>(ret));
        });

        // =====================================================================
        // XwLA5cTHjt4#T#T  ===  sceKernelGetProcessType
        // Returns the process type (1 = SceKernelMainProc for the main process).
        // Called at startup to determine execution context.
        // =====================================================================
        RegisterSymbol("libkernel", "XwLA5cTHjt4#T#T", [](const GuestArgs& /*args*/) -> u64 {
            LOG_DEBUG(HLE, "sceKernelGetProcessType() -> 1 (SceKernelMainProc)");
            return 1; // SCE_KERNEL_MAIN_PROC
        });

        // =====================================================================
        // scePthreadAttrInit  /  scePthreadAttrDestroy
        // Minimal attr init: write the struct size as a sentinel, return 0.
        // =====================================================================
        RegisterSymbol("libkernel", "scePthreadAttrInit", [](const GuestArgs& args) -> u64 {
            guest_addr_t attr_ptr = args.arg1;
            if (attr_ptr) Memory::Write<u64>(attr_ptr, 0x38); // attr struct sentinel size
            LOG_DEBUG(HLE, "scePthreadAttrInit(0x%llx) -> OK", attr_ptr);
            return 0;
        });
        RegisterSymbol("libkernel", "scePthreadAttrDestroy", [](const GuestArgs& args) -> u64 {
            LOG_DEBUG(HLE, "scePthreadAttrDestroy(0x%llx) -> OK", args.arg1);
            return 0;
        });
        RegisterSymbol("libkernel", "scePthreadAttrSetstacksize", [](const GuestArgs& args) -> u64 {
            LOG_DEBUG(HLE, "scePthreadAttrSetstacksize(attr=0x%llx, size=0x%llx) -> OK", args.arg1, args.arg2);
            return 0;
        });
        RegisterSymbol("libkernel", "scePthreadAttrSetdetachstate", [](const GuestArgs& args) -> u64 {
            LOG_DEBUG(HLE, "scePthreadAttrSetdetachstate(attr=0x%llx, state=%llu) -> OK", args.arg1, args.arg2);
            return 0;
        });
        RegisterSymbol("libkernel", "scePthreadAttrSetschedparam", [](const GuestArgs& args) -> u64 {
            LOG_DEBUG(HLE, "scePthreadAttrSetschedparam(attr=0x%llx) -> OK", args.arg1);
            return 0;
        });
        RegisterSymbol("libkernel", "scePthreadAttrSetinheritsched", [](const GuestArgs& args) -> u64 {
            LOG_DEBUG(HLE, "scePthreadAttrSetinheritsched(attr=0x%llx, inherit=%llu) -> OK", args.arg1, args.arg2);
            return 0;
        });
        RegisterSymbol("libkernel", "scePthreadAttrSetschedpolicy", [](const GuestArgs& args) -> u64 {
            LOG_DEBUG(HLE, "scePthreadAttrSetschedpolicy(attr=0x%llx, policy=%llu) -> OK", args.arg1, args.arg2);
            return 0;
        });

        // =====================================================================
        // scePthreadCreate  ===  Create a PS5 thread.
        // Spawns a real Windows thread that calls InvokeGuestFunction.
        // =====================================================================
        RegisterSymbol("libkernel", "scePthreadCreate", [](const GuestArgs& args) -> u64 {
            guest_addr_t thread_ptr   = args.arg1;  // out: pthread_t*
            guest_addr_t attr_ptr     = args.arg2;  // optional attr
            guest_addr_t start_fn     = args.arg3;  // entry point
            guest_addr_t start_arg    = args.arg4;  // argument
            guest_addr_t name_ptr     = args.arg5;  // optional name string
            (void)attr_ptr;

            // Read thread name from guest memory
            std::string tname = "<unnamed>";
            if (name_ptr) {
                for (u64 i = 0; i < 128; ++i) {
                    u8 c = Memory::Read<u8>(name_ptr + i);
                    if (!c) break;
                    tname += static_cast<char>(c);
                }
            }

            // Allocate a guest stack (1 MB, same as main thread)
            constexpr u64 kGuestStackSize = 1024 * 1024;
            void* guest_stack = VirtualAlloc(nullptr, kGuestStackSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!guest_stack) {
                LOG_ERROR(HLE, "scePthreadCreate: VirtualAlloc failed for guest stack");
                return 11; // EAGAIN
            }
            u64 stack_base = reinterpret_cast<u64>(guest_stack);

            // Allocate TLS for this thread (16 KB, same as main thread)
            constexpr u64 kTlsSize = 0x4000;
            void* tls_block = VirtualAlloc(nullptr, kTlsSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            u64 tls_base = reinterpret_cast<u64>(tls_block);

            // Assign a new guest thread ID
            u64 tid = g_next_tid.fetch_add(1);

            // Build tracking info
            auto* info = new GuestThreadInfo{};
            info->guest_tid   = tid;
            info->entry_point = start_fn;
            info->arg         = start_arg;
            info->stack_base  = stack_base;
            info->stack_size  = kGuestStackSize;
            info->tls_base    = tls_base;
            info->name        = tname;

            // Register with the kernel
            Kernel::ThreadContext ctx;
            ctx.thread_id   = tid;
            ctx.name        = tname;
            ctx.entry_point = start_fn;
            ctx.stack_base  = stack_base;
            ctx.stack_size  = kGuestStackSize;
            ctx.tls_base    = tls_base;
            Kernel::RegisterThread(ctx);

            // Spawn the Windows thread
            info->handle = CreateThread(nullptr, 0, GuestThreadProc, info, 0, nullptr);
            if (!info->handle) {
                LOG_ERROR(HLE, "scePthreadCreate: CreateThread failed (err=%lu)", GetLastError());
                Kernel::RegisterThread(ctx); // already registered, but fine
                VirtualFree(guest_stack, 0, MEM_RELEASE);
                VirtualFree(tls_block, 0, MEM_RELEASE);
                delete info;
                return 11; // EAGAIN
            }

            {
                std::lock_guard<std::mutex> lock(g_thread_mutex);
                g_threads[tid] = *info;
            }

            // Write the guest thread handle (tid) to the output pointer
            if (thread_ptr) Memory::Write<u64>(thread_ptr, tid);

            LOG_INFO(HLE, "scePthreadCreate(entry=0x%llx, arg=0x%llx, name='%s') -> tid=%llu",
                     start_fn, start_arg, tname.c_str(), tid);
            return 0;
        });

        // =====================================================================
        // scePthreadJoin  ===  Wait for a thread to finish and get its exit value.
        // =====================================================================
        RegisterSymbol("libkernel", "scePthreadJoin", [](const GuestArgs& args) -> u64 {
            u64 tid = args.arg1;
            guest_addr_t value_ptr = args.arg2;  // out: void**

            LOG_INFO(HLE, "scePthreadJoin(tid=%llu)", tid);

            HANDLE handle = nullptr;
            GuestThreadInfo info_copy;
            {
                std::lock_guard<std::mutex> lock(g_thread_mutex);
                auto it = g_threads.find(tid);
                if (it == g_threads.end()) {
                    LOG_ERROR(HLE, "scePthreadJoin: thread %llu not found", tid);
                    return 3; // ESRCH
                }
                info_copy = it->second;
                handle = it->second.handle;
            }

            // Wait for the thread to finish
            DWORD wait_result = WaitForSingleObject(handle, INFINITE);
            if (wait_result != WAIT_OBJECT_0) {
                LOG_ERROR(HLE, "scePthreadJoin: WaitForSingleObject failed (err=%lu)", GetLastError());
                return 5; // EDEADLK or other error
            }

            // Get the exit code (return value from GuestThreadProc)
            DWORD exit_code = 0;
            GetExitCodeThread(handle, &exit_code);

            // Write exit value to guest memory
            if (value_ptr) Memory::Write<u64>(value_ptr, static_cast<u64>(exit_code));

            // Clean up
            CloseHandle(handle);
            if (info_copy.stack_base) {
                VirtualFree(reinterpret_cast<void*>(static_cast<uintptr_t>(info_copy.stack_base)), 0, MEM_RELEASE);
            }
            if (info_copy.tls_base) {
                VirtualFree(reinterpret_cast<void*>(static_cast<uintptr_t>(info_copy.tls_base)), 0, MEM_RELEASE);
            }
            {
                std::lock_guard<std::mutex> lock(g_thread_mutex);
                g_threads.erase(tid);
            }

            LOG_INFO(HLE, "scePthreadJoin(tid=%llu) -> exit_code=%lu", tid, exit_code);
            return 0;
        });

        // =====================================================================
        // scePthreadDetach  ===  Detach a thread (auto-cleanup on exit).
        // =====================================================================
        RegisterSymbol("libkernel", "scePthreadDetach", [](const GuestArgs& args) -> u64 {
            u64 tid = args.arg1;
            LOG_INFO(HLE, "scePthreadDetach(tid=%llu)", tid);

            std::lock_guard<std::mutex> lock(g_thread_mutex);
            auto it = g_threads.find(tid);
            if (it == g_threads.end()) {
                LOG_ERROR(HLE, "scePthreadDetach: thread %llu not found", tid);
                return 3; // ESRCH
            }
            it->second.detached = true;
            // Close the handle — the thread will clean itself up in GuestThreadProc
            CloseHandle(it->second.handle);
            it->second.handle = nullptr;
            return 0;
        });

        // =====================================================================
        // scePthreadExit  ===  Exit the current thread.
        // =====================================================================
        RegisterSymbol("libkernel", "scePthreadExit", [](const GuestArgs& args) -> u64 {
            u64 exit_value = args.arg1;
            LOG_INFO(HLE, "scePthreadExit(value=0x%llx)", exit_value);
            ExitThread(static_cast<DWORD>(exit_value));
        });

        // =====================================================================
        // scePthreadSelf  ===  Return the calling thread's ID.
        // =====================================================================
        RegisterSymbol("libkernel", "scePthreadSelf", [](const GuestArgs& /*args*/) -> u64 {
            // For the main thread, return 1. For guest threads, we'd need TLS-based lookup.
            // For now, return 1 (main thread) — guest threads will need a proper TLS slot.
            LOG_DEBUG(HLE, "scePthreadSelf() -> 0x1");
            return 0x1;
        });
        RegisterSymbol("libkernel", "scePthreadGetprio", [](const GuestArgs& args) -> u64 {
            LOG_DEBUG(HLE, "scePthreadGetprio(thread=0x%llx) -> 700", args.arg1);
            if (args.arg2) Memory::Write<s32>(args.arg2, 700); // Default PS5 priority
            return 0;
        });
        RegisterSymbol("libkernel", "scePthreadSetprio", [](const GuestArgs& args) -> u64 {
            LOG_DEBUG(HLE, "scePthreadSetprio(thread=0x%llx, prio=%lld)", args.arg1, (s64)args.arg2);
            return 0;
        });

        // =====================================================================
        // Pthread mutex variants (extending the existing sceKernelMutex stubs)
        // =====================================================================
        RegisterSymbol("libkernel", "scePthreadMutexInit", [](const GuestArgs& args) -> u64 {
            guest_addr_t mutex_ptr = args.arg1;
            if (mutex_ptr) Memory::Write<u64>(mutex_ptr, 1); // initialized sentinel
            LOG_DEBUG(HLE, "scePthreadMutexInit(0x%llx) -> OK", mutex_ptr);
            return 0;
        });
        RegisterSymbol("libkernel", "scePthreadMutexDestroy", [](const GuestArgs& args) -> u64 {
            LOG_DEBUG(HLE, "scePthreadMutexDestroy(0x%llx) -> OK", args.arg1);
            return 0;
        });
        RegisterSymbol("libkernel", "scePthreadMutexLock", [](const GuestArgs& args) -> u64 {
            LOG_DEBUG(HLE, "scePthreadMutexLock(0x%llx) -> OK (stub)", args.arg1);
            return 0;
        });
        RegisterSymbol("libkernel", "scePthreadMutexUnlock", [](const GuestArgs& args) -> u64 {
            LOG_DEBUG(HLE, "scePthreadMutexUnlock(0x%llx) -> OK (stub)", args.arg1);
            return 0;
        });
        RegisterSymbol("libkernel", "scePthreadMutexTrylock", [](const GuestArgs& args) -> u64 {
            LOG_DEBUG(HLE, "scePthreadMutexTrylock(0x%llx) -> OK (stub)", args.arg1);
            return 0;
        });
        RegisterSymbol("libkernel", "scePthreadMutexattrInit", [](const GuestArgs& args) -> u64 {
            if (args.arg1) Memory::Write<u64>(args.arg1, 0);
            return 0;
        });
        RegisterSymbol("libkernel", "scePthreadMutexattrDestroy", [](const GuestArgs& /*args*/) -> u64 { return 0; });
        RegisterSymbol("libkernel", "scePthreadMutexattrSettype", [](const GuestArgs& /*args*/) -> u64 { return 0; });
        RegisterSymbol("libkernel", "scePthreadMutexattrSetprotocol", [](const GuestArgs& /*args*/) -> u64 { return 0; });

        // =====================================================================
        // Pthread key (thread-local storage keys)
        // =====================================================================
        static std::unordered_map<u64, u64> s_tls_keys;  // key -> destructor
        static std::unordered_map<u64, u64> s_tls_values; // key -> value
        static std::mutex s_tls_mutex;
        static u64 s_next_key = 1;

        RegisterSymbol("libkernel", "scePthreadKeyCreate", [](const GuestArgs& args) -> u64 {
            guest_addr_t key_ptr  = args.arg1;
            // u64 destructor = args.arg2;
            std::lock_guard<std::mutex> lock(s_tls_mutex);
            u64 key = s_next_key++;
            if (key_ptr) Memory::Write<u64>(key_ptr, key);
            LOG_DEBUG(HLE, "scePthreadKeyCreate() -> key=%llu", key);
            return 0;
        });
        RegisterSymbol("libkernel", "scePthreadKeyDelete", [](const GuestArgs& args) -> u64 {
            std::lock_guard<std::mutex> lock(s_tls_mutex);
            s_tls_values.erase(args.arg1);
            LOG_DEBUG(HLE, "scePthreadKeyDelete(key=%llu) -> OK", args.arg1);
            return 0;
        });
        RegisterSymbol("libkernel", "scePthreadGetspecific", [](const GuestArgs& args) -> u64 {
            std::lock_guard<std::mutex> lock(s_tls_mutex);
            auto it = s_tls_values.find(args.arg1);
            u64 val = (it != s_tls_values.end()) ? it->second : 0;
            LOG_DEBUG(HLE, "scePthreadGetspecific(key=%llu) -> 0x%llx", args.arg1, val);
            return val;
        });
        RegisterSymbol("libkernel", "scePthreadSetspecific", [](const GuestArgs& args) -> u64 {
            std::lock_guard<std::mutex> lock(s_tls_mutex);
            s_tls_values[args.arg1] = args.arg2;
            LOG_DEBUG(HLE, "scePthreadSetspecific(key=%llu, val=0x%llx) -> OK", args.arg1, args.arg2);
            return 0;
        });

        // =====================================================================
        // Condition variable stubs
        // =====================================================================
        RegisterSymbol("libkernel", "scePthreadCondInit", [](const GuestArgs& args) -> u64 {
            if (args.arg1) Memory::Write<u64>(args.arg1, 1);
            return 0;
        });
        RegisterSymbol("libkernel", "scePthreadCondDestroy", [](const GuestArgs& /*args*/) -> u64 { return 0; });
        RegisterSymbol("libkernel", "scePthreadCondWait", [](const GuestArgs& /*args*/) -> u64 { return 0; });
        RegisterSymbol("libkernel", "scePthreadCondSignal", [](const GuestArgs& /*args*/) -> u64 { return 0; });
        RegisterSymbol("libkernel", "scePthreadCondBroadcast", [](const GuestArgs& /*args*/) -> u64 { return 0; });
        RegisterSymbol("libkernel", "scePthreadCondattrInit", [](const GuestArgs& args) -> u64 {
            if (args.arg1) Memory::Write<u64>(args.arg1, 0);
            return 0;
        });
        RegisterSymbol("libkernel", "scePthreadCondattrDestroy", [](const GuestArgs& /*args*/) -> u64 { return 0; });

        // =====================================================================
        // rwlock stubs
        // =====================================================================
        RegisterSymbol("libkernel", "scePthreadRwlockInit", [](const GuestArgs& args) -> u64 {
            if (args.arg1) Memory::Write<u64>(args.arg1, 0);
            return 0;
        });
        RegisterSymbol("libkernel", "scePthreadRwlockDestroy", [](const GuestArgs& /*args*/) -> u64 { return 0; });
        RegisterSymbol("libkernel", "scePthreadRwlockRdlock", [](const GuestArgs& /*args*/) -> u64 { return 0; });
        RegisterSymbol("libkernel", "scePthreadRwlockWrlock", [](const GuestArgs& /*args*/) -> u64 { return 0; });
        RegisterSymbol("libkernel", "scePthreadRwlockUnlock", [](const GuestArgs& /*args*/) -> u64 { return 0; });

        // =====================================================================
        // sceKernelGetProcessType (also registered via NID alias)
        // =====================================================================
        RegisterSymbol("libkernel", "sceKernelGetProcessType", [](const GuestArgs& /*args*/) -> u64 {
            return 1; // SCE_KERNEL_MAIN_PROC
        });

        // =====================================================================
        // Usleep
        RegisterSymbol("libkernel", "sceKernelUsleep", [](const GuestArgs& args) -> u64 {
            u32 microseconds = static_cast<u32>(args.arg1);

            LOG_DEBUG(HLE, "sceKernelUsleep(%u us)", microseconds);
            std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
            return 0;
        });

        // Gettimeofday
        RegisterSymbol("libkernel", "sceKernelGettimeofday", [](const GuestArgs& args) -> u64 {
            guest_addr_t tv_ptr = args.arg1;
            // timeval structure: 8-byte tv_sec, 8-byte tv_usec
            if (tv_ptr) {
                auto now = std::chrono::system_clock::now();
                auto duration = now.time_since_epoch();
                auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
                auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(duration).count() % 1000000;

                Memory::Write<u64>(tv_ptr, seconds);
                Memory::Write<u64>(tv_ptr + 8, microseconds);
            }
            return 0;
        });

        // Mutex operations stubs (mocking mutexes with simple status codes or host handles)
        RegisterSymbol("libkernel", "sceKernelCreateMutex", [](const GuestArgs& args) -> u64 {
            guest_addr_t name_ptr = args.arg1;
            u32 attribute = static_cast<u32>(args.arg2);
            guest_addr_t opt = args.arg3;
            (void)opt;
            
            const char* name = name_ptr ? reinterpret_cast<const char*>(name_ptr) : "unnamed";
            LOG_INFO(HLE, "sceKernelCreateMutex(name: '%s', attr: 0x%X)", name, attribute);
            
            // Return a mock mutex handle (e.g. 0x1000 + incrementing index)
            static u32 mock_mutex_id = 0x1000;
            return mock_mutex_id++;
        });

        RegisterSymbol("libkernel", "sceKernelLockMutex", [](const GuestArgs& args) -> u64 {
            u32 handle = static_cast<u32>(args.arg1);
            s32 count = static_cast<s32>(args.arg2);
            guest_addr_t timeout = args.arg3;
            (void)timeout;
            LOG_DEBUG(HLE, "sceKernelLockMutex(handle: 0x%X, count: %d)", handle, count);
            return 0; // Success
        });

        RegisterSymbol("libkernel", "sceKernelUnlockMutex", [](const GuestArgs& args) -> u64 {
            u32 handle = static_cast<u32>(args.arg1);
            s32 count = static_cast<s32>(args.arg2);
            LOG_DEBUG(HLE, "sceKernelUnlockMutex(handle: 0x%X, count: %d)", handle, count);
            return 0; // Success
        });

        // Direct memory allocations (mocking virtual heap allocations)
        RegisterSymbol("libkernel", "sceKernelAllocateMainDirectMemory", [](const GuestArgs& args) -> u64 {
            u64 size = args.arg1;
            u64 alignment = args.arg2;
            u32 type = static_cast<u32>(args.arg3);
            guest_addr_t phys_addr_out = args.arg4;

            LOG_INFO(HLE, "sceKernelAllocateMainDirectMemory(size: %llu, align: %llu, type: %u)", size, alignment, type);
            
            // Map virtual memory block representing physical direct memory
            guest_addr_t virt = 0;
            if (Memory::Map(0, size, Memory::PROT_READ | Memory::PROT_WRITE, &virt) != Memory::Status::Ok) {
                return static_cast<u64>(-1);
            }
            if (phys_addr_out) {
                Memory::Write<u64>(phys_addr_out, virt); // We simplify: phys addr = virt addr
            }
            return 0; // Success
        });

        RegisterSymbol("libkernel", "sceKernelMapDirectMemory", [](const GuestArgs& args) -> u64 {
            guest_addr_t start = args.arg1;
            u64 size = args.arg2;
            u32 prot = static_cast<u32>(args.arg3);
            u32 flags = static_cast<u32>(args.arg4);
            u64 phys_addr = args.arg5;
            u64 alignment = args.arg6;
            (void)flags;
            (void)alignment;

            LOG_INFO(HLE, "sceKernelMapDirectMemory(start: 0x%llx, size: %llu, phys: 0x%llx)", start, size, phys_addr);
            // Protect direct memory range
            if (Memory::Protect(start, size, prot) != Memory::Status::Ok) {
                LOG_WARN(HLE, "sceKernelMapDirectMemory: Protect failed");
            }
            return 0; // Success
        });

        // _init_env (bzQExy189ZI#T#T) — libc environment init, called at startup
        RegisterSymbol("libkernel", "bzQExy189ZI#T#T", [](const GuestArgs& /*args*/) -> u64 {
            LOG_DEBUG(HLE, "libkernel::_init_env() -> 0 (success)");
            return 0;
        });

        // atexit (8G2LB+A3rzg#T#T) — register process-exit callback
        RegisterSymbol("libkernel", "8G2LB+A3rzg#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t func = args.arg1;
            LOG_DEBUG(HLE, "libkernel::atexit(func: 0x%llx) -> 0", func);
            return 0; // success
        });

        RegisterSymbol("libkernel", "sceKernelLoadStartModule", [](const GuestArgs& args) -> u64 {
            guest_addr_t path_ptr = args.arg1;
            u32 argc = static_cast<u32>(args.arg2);
            guest_addr_t argv = args.arg3;
            u32 flags = static_cast<u32>(args.arg4);
            guest_addr_t opts = args.arg5;
            guest_addr_t res = args.arg6;
            (void)argc;
            (void)argv;
            (void)opts;
            (void)res;

            const char* path = path_ptr ? reinterpret_cast<const char*>(path_ptr) : "unknown";
            LOG_INFO(HLE, "sceKernelLoadStartModule(path: '%s', flags: 0x%X)", path, flags);

            // Attempt to load the module using our kernel loader
            Loader::LoadedModule loaded_lib;
            std::string filepath = path;
            
            if (Kernel::LoadModule(filepath, loaded_lib)) {
                static u32 mock_module_id = 0x2000;
                u32 mod_id = mock_module_id++;
                LOG_INFO(HLE, "Successfully loaded PRX module '%s' (assigned ID: 0x%X)", filepath.c_str(), mod_id);
                return mod_id;
            }

            LOG_ERROR(HLE, "Failed to load PRX module: %s", filepath.c_str());
            return 0x80020001; // Standard Sony error code for module not found
        });

        // memset (8zTFvBIAIN8#T#T)
        RegisterSymbol("libkernel", "8zTFvBIAIN8#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t dest = args.arg1;
            u32 ch = static_cast<u32>(args.arg2);
            u64 count = args.arg3;
            
            // Guard against corrupted count values (e.g. count > 256MB is likely garbage)
            constexpr u64 MAX_MEMSET = 256ULL * 1024 * 1024;
            if (count > MAX_MEMSET) {
                LOG_WARN(HLE, "libkernel::memset: count 0x%llx exceeds 256MB limit, clamping to 0", count);
                count = 0;
            }
            
            LOG_DEBUG(HLE, "libkernel::memset(dest: 0x%llx, ch: %u, count: %llu)", dest, ch, count);
            if (dest && count > 0) {
                std::memset(reinterpret_cast<void*>(dest), static_cast<int>(ch & 0xFF), count);
            }
            return dest;
        });

        // strlen (j4ViWNHEgww#T#T)
        RegisterSymbol("libkernel", "j4ViWNHEgww#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t str = args.arg1;
            if (!str) return 0;
            
            u64 len = 0;
            while (Memory::Read<u8>(str + len) != 0) {
                len++;
            }
            
            std::string content;
            for (u64 i = 0; i < (len < 256 ? len : 256); ++i) {
                content += static_cast<char>(Memory::Read<u8>(str + i));
            }
            LOG_DEBUG(HLE, "libkernel::strlen(str: 0x%llx) -> %llu (Value: '%s')", str, len, content.c_str());
            return len;
        });

        // __cxa_atexit (tsvEmnenz48#T#T)
        RegisterSymbol("libkernel", "tsvEmnenz48#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t func = args.arg1;
            guest_addr_t arg = args.arg2;
            guest_addr_t dso = args.arg3;
            LOG_DEBUG(HLE, "libkernel::__cxa_atexit(func: 0x%llx, arg: 0x%llx, dso: 0x%llx)", func, arg, dso);
            return 0; // Success
        });

        // vsnprintf (Q2V+iqvjgC0#T#T)
        RegisterSymbol("libkernel", "Q2V+iqvjgC0#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t dest = args.arg1;
            u64 size = args.arg2;
            guest_addr_t fmt_ptr = args.arg3;
            guest_addr_t valist_ptr = args.arg4;
            
            if (!fmt_ptr) return 0;
            
            std::string fmt;
            u64 offset = 0;
            while (true) {
                u8 ch = Memory::Read<u8>(fmt_ptr + offset++);
                if (ch == 0) break;
                fmt += static_cast<char>(ch);
            }
            
            SysVAmd64VaList valist;
            Memory::ReadBuffer(valist_ptr, &valist, sizeof(SysVAmd64VaList));
            
            std::string formatted = FormatGuestString(fmt, valist);
            LOG_DEBUG(HLE, "libkernel::vsnprintf(dest: 0x%llx, size: %llu, fmt: '%s') -> Result: '%s'", 
                      dest, size, fmt.c_str(), formatted.c_str());
            
            if (dest && size > 0) {
                u64 copy_len = (formatted.size() < size - 1) ? formatted.size() : size - 1;
                Memory::WriteBuffer(dest, formatted.c_str(), copy_len);
                Memory::Write<u8>(dest + copy_len, 0);
            }
            
            return formatted.size();
        });

        // fputs (QrZZdJ8XsX0#T#T)
        RegisterSymbol("libkernel", "QrZZdJ8XsX0#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t str_ptr = args.arg1;
            std::string msg;
            if (str_ptr) {
                u64 offset = 0;
                while (true) {
                    u8 ch = Memory::Read<u8>(str_ptr + offset++);
                    if (ch == 0) break;
                    msg += static_cast<char>(ch);
                }
            }
            std::cerr << "[GUEST][FPUTS]: " << msg;
            return 0; // Success
        });

        // exit (uMei1W9uyNo#T#T)
        RegisterSymbol("libkernel", "uMei1W9uyNo#T#T", [](const GuestArgs& args) -> u64 {
            u32 code = static_cast<u32>(args.arg1);
            LOG_ERROR(Kernel, "Guest requested exit with code: %u", code);
            // Keep the window (and last rendered frame) alive until the user closes it
            GPU::RunIdleLoop();
            std::exit(static_cast<int>(code));
        });

        // sceKernelGetDirectMemorySize (pO96TwzOm5E#S#N)
        // Returns the total size of the physical "direct" memory pool (16GB on PS5)
        RegisterSymbol("libkernel", "pO96TwzOm5E#S#N", [](const GuestArgs& /*args*/) -> u64 {
            constexpr u64 DIRECT_MEM_SIZE = 16384ULL * 1024 * 1024; // 16 GB
            LOG_DEBUG(HLE, "sceKernelGetDirectMemorySize() -> 0x%llx", DIRECT_MEM_SIZE);
            return DIRECT_MEM_SIZE;
        });

        // =====================================================================
        // Physical memory pool (emulates PS5 direct memory / GPU-visible memory)
        //
        // On the real PS5:
        //   AllocateDirectMemory  -> returns a sequential PHYSICAL OFFSET (0-based)
        //   MapDirectMemory       -> maps [physOffset, physOffset+len) into VA space
        //
        // We emulate this with a single large VirtualAlloc reservation.
        // Physical offsets are just byte offsets into that reservation.
        // =====================================================================
        static constexpr u64 PHYS_POOL_SIZE = 2ULL * 1024 * 1024 * 1024; // 2 GB pool
        static guest_addr_t  g_phys_pool_base = 0;
        static u64           g_phys_pool_offset = 0x10000; // start past offset 0
        static std::mutex    g_phys_mutex;

        // Lazily initialise the physical pool on first AllocateDirectMemory
        auto EnsurePhysPool = [&]() -> bool {
            if (g_phys_pool_base) return true;
            // Reserve (but don't commit) 2 GB.  We'll commit individual pages on Map.
            void* p = VirtualAlloc(nullptr, PHYS_POOL_SIZE, MEM_RESERVE, PAGE_NOACCESS);
            if (!p) {
                LOG_ERROR(HLE, "PhysPool: VirtualAlloc reserve failed!");
                return false;
            }
            g_phys_pool_base = reinterpret_cast<guest_addr_t>(p);
            LOG_INFO(HLE, "PhysPool: reserved 2 GB at base 0x%llx", g_phys_pool_base);
            return true;
        };

        // sceKernelAllocateDirectMemory (rTXw65xmLIA#S#N)
        RegisterSymbol("libkernel", "rTXw65xmLIA#S#N", [EnsurePhysPool](const GuestArgs& args) -> u64 {
            u64 search_start = args.arg1;
            u64 search_end   = args.arg2;
            u64 length       = args.arg3;
            u64 alignment    = args.arg4;
            u32 mem_type     = static_cast<u32>(args.arg5);
            guest_addr_t out_ptr = args.arg6;
            (void)search_start; (void)search_end; (void)mem_type;

            LOG_INFO(HLE, "sceKernelAllocateDirectMemory(len: 0x%llx, align: 0x%llx, type: %u, out: 0x%llx)",
                     length, alignment, mem_type, out_ptr);

            if (!out_ptr) return 0x800D0004; // EINVAL

            u64 alloc_size = (length < 0x1000) ? 0x1000 : length;
            if (alignment < 0x1000) alignment = 0x1000;
            // Round up to alignment
            u64 aligned_size = (alloc_size + alignment - 1) & ~(alignment - 1);

            std::lock_guard<std::mutex> lk(g_phys_mutex);
            if (!EnsurePhysPool()) return 0x800D0006;

            // Align the current offset
            u64 phys_offset = (g_phys_pool_offset + alignment - 1) & ~(alignment - 1);
            if (phys_offset + aligned_size > PHYS_POOL_SIZE) {
                LOG_ERROR(HLE, "sceKernelAllocateDirectMemory: out of physical pool space!");
                return 0x800D0006;
            }
            g_phys_pool_offset = phys_offset + aligned_size;

            // Write the physical OFFSET (not a host address!) back to the game
            Memory::Write<u64>(out_ptr, phys_offset);
            LOG_INFO(HLE, "sceKernelAllocateDirectMemory -> physOffset: 0x%llx (size: 0x%llx)", phys_offset, aligned_size);
            return 0;
        });

        auto MapDirectMemoryImpl = [EnsurePhysPool](const GuestArgs& args) -> u64 {
            guest_addr_t addr_ptr  = args.arg1; // in/out: pointer to VA hint
            u64 length             = args.arg2;
            u32 prot               = static_cast<u32>(args.arg3);
            u32 flags              = static_cast<u32>(args.arg4);
            u64 phys_offset        = args.arg5; // offset into physical pool
            u64 alignment          = args.arg6;
            (void)flags;

            LOG_INFO(HLE, "sceKernelMapDirectMemory(addr_ptr: 0x%llx, len: 0x%llx, prot: 0x%X, physOff: 0x%llx, align: 0x%llx)",
                     addr_ptr, length, prot, phys_offset, alignment);

            if (!addr_ptr || !length) {
                LOG_WARN(HLE, "sceKernelMapDirectMemory: null addr_ptr or zero length");
                return 0x800D0004;
            }
            if (alignment < 0x1000) alignment = 0x1000;

            std::lock_guard<std::mutex> lk(g_phys_mutex);
            if (!EnsurePhysPool()) return 0x800D0006;

            // Sanitize prot
            if (prot > 0xF) {
                LOG_WARN(HLE, "sceKernelMapDirectMemory: bad prot=0x%X -> defaulting to RW", prot);
                prot = 3;
            }

            // Determine Windows protection
            DWORD win_prot = PAGE_READWRITE;
            bool r = (prot & 1), w = (prot & 2), x = (prot & 4);
            if (x)      win_prot = w ? PAGE_EXECUTE_READWRITE : (r ? PAGE_EXECUTE_READ : PAGE_EXECUTE_READ); // Always Exec+Read for safety
            else if (w) win_prot = PAGE_READWRITE;
            else if (r) win_prot = PAGE_READONLY;
            else        win_prot = PAGE_NOACCESS;

            u64 rounded = (length + 0xFFF) & ~0xFFFULL;
            guest_addr_t hint = Memory::Read<u64>(addr_ptr);
            void* target = nullptr;
            bool alloc_ok = false;
            if (hint != 0) {
                target = reinterpret_cast<void*>(hint);
                // Reserve and commit at the hint address
                if (VirtualAlloc(target, rounded, MEM_RESERVE | MEM_COMMIT, win_prot)) {
                    alloc_ok = true;
                } else {
                    // Try to commit only in case it's already reserved
                    if (VirtualAlloc(target, rounded, MEM_COMMIT, win_prot)) {
                        alloc_ok = true;
                    } else {
                        DWORD err = GetLastError();
                        LOG_ERROR(HLE, "MapDirectMemoryImpl: VirtualAlloc failed at 0x%llx size=0x%llx (err=%lu)",
                                  hint, rounded, err);
                    }
                }
            } else {
                target = reinterpret_cast<void*>(g_phys_pool_base + phys_offset);
                if (VirtualAlloc(target, rounded, MEM_COMMIT, win_prot)) {
                    alloc_ok = true;
                } else {
                    DWORD old;
                    if (VirtualProtect(target, rounded, win_prot, &old)) {
                        alloc_ok = true;
                    } else {
                        DWORD err = GetLastError();
                        LOG_ERROR(HLE, "MapDirectMemoryImpl: Phys pool commit failed at 0x%llx size=0x%llx (err=%lu)",
                                  (u64)target, rounded, err);
                    }
                }
            }

            if (!alloc_ok || !target) {
                LOG_ERROR(HLE, "MapDirectMemoryImpl: Allocation failed!");
                return 0x800D0006;
            }

            guest_addr_t mapped_va = reinterpret_cast<guest_addr_t>(target);
            Memory::Write<u64>(addr_ptr, mapped_va);
            LOG_INFO(HLE, "sceKernelMapDirectMemory -> va: 0x%llx", mapped_va);
            return 0;
        };

        RegisterSymbol("libkernel", "L-Q3LEjIbgA#S#N", MapDirectMemoryImpl);
        RegisterSymbol("libkernel", "7oxv3PPCumo#y#J", MapDirectMemoryImpl);

        // __cxa_guard_acquire (3GPpjQdAMTw#T#T)
        // C++ one-time static init guard. Returns 1 if caller must initialize, 0 if already done.
        // Guard layout (64-bit): bits [7:0] = initialized flag (1 = done), bits [15:8] = pending flag.
        RegisterSymbol("libkernel", "3GPpjQdAMTw#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t guard_ptr = args.arg1;
            if (!guard_ptr) return 0;

            // Read current guard state byte
            u8 state = Memory::Read<u8>(guard_ptr);
            if (state & 0x01) {
                // Already initialized
                return 0;
            }

            // Mark as pending (bit 8 = 0x01 in second byte), return 1 to signal caller to initialize
            Memory::Write<u8>(guard_ptr + 1, 0x01);
            LOG_DEBUG(HLE, "__cxa_guard_acquire(guard: 0x%llx) -> 1 (needs init)", guard_ptr);
            return 1;
        });

        // __cxa_guard_release (9rAeANT2tyE#T#T)
        // Marks the guard as initialized after the caller finishes initialization.
        RegisterSymbol("libkernel", "9rAeANT2tyE#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t guard_ptr = args.arg1;
            if (!guard_ptr) return 0;

            // Set initialized flag (byte 0 = 1) and clear pending flag (byte 1 = 0)
            Memory::Write<u8>(guard_ptr, 0x01);
            Memory::Write<u8>(guard_ptr + 1, 0x00);
            LOG_DEBUG(HLE, "__cxa_guard_release(guard: 0x%llx) -> done", guard_ptr);
            return 0;
        });

        // __cxa_guard_abort (nKCFAMmBEgQ#T#T)
        // Called if initialization fails — clears the pending flag.
        RegisterSymbol("libkernel", "nKCFAMmBEgQ#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t guard_ptr = args.arg1;
            if (guard_ptr) Memory::Write<u8>(guard_ptr + 1, 0x00);
            LOG_WARN(HLE, "__cxa_guard_abort(guard: 0x%llx)", guard_ptr);
            return 0;
        });
        // memmove (+P6FRGH4LfA#T#T)
        RegisterSymbol("libkernel", "+P6FRGH4LfA#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t dest = args.arg1;
            guest_addr_t src  = args.arg2;
            u64 count         = args.arg3;

            constexpr u64 MAX_MOVE = 256ULL * 1024 * 1024;
            if (count > MAX_MOVE) {
                LOG_WARN(HLE, "libkernel::memmove: count 0x%llx too large, clamping to 0", count);
                count = 0;
            }
            LOG_DEBUG(HLE, "libkernel::memmove(dest: 0x%llx, src: 0x%llx, count: %llu)", dest, src, count);
            if (dest && src && count > 0) {
                std::memmove(reinterpret_cast<void*>(dest), reinterpret_cast<const void*>(src), count);
            }
            return dest;
        });

        // realloc (0E5HFqWCBSA#T#T)
        RegisterSymbol("libkernel", "0E5HFqWCBSA#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t old_ptr = args.arg1;
            u64 new_size         = args.arg2;
            if (new_size == 0) return 0;
            guest_addr_t mem = 0;
            if (Memory::Map(0, (new_size + 0xFFF) & ~0xFFFULL,
                            Memory::PROT_READ | Memory::PROT_WRITE, &mem) != Memory::Status::Ok) {
                return 0;
            }
            LOG_DEBUG(HLE, "libkernel::realloc(ptr: 0x%llx, size: %llu) -> 0x%llx", old_ptr, new_size, mem);
            return mem;
        });

        RegisterSymbol("libkernel", "Q3VBxCXhUHs#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t dest = args.arg1;
            guest_addr_t src  = args.arg2;
            u64 count         = args.arg3;
            
            constexpr u64 MAX_COPY = 256ULL * 1024 * 1024;
            if (count > MAX_COPY) {
                LOG_WARN(HLE, "libkernel::memcpy: count 0x%llx too large, clamping", count);
                count = 0;
            }
            
            LOG_DEBUG(HLE, "libkernel::memcpy(dest: 0x%llx, src: 0x%llx, count: %llu)", dest, src, count);
            if (dest && src && count > 0) {
                std::memmove(reinterpret_cast<void*>(dest), reinterpret_cast<const void*>(src), count);
            }
            return dest;
        });

        // malloc (gQX+4GDQjpM#T#T)
        RegisterSymbol("libkernel", "gQX+4GDQjpM#T#T", [](const GuestArgs& args) -> u64 {
            u64 size = args.arg1;
            if (size == 0) size = 1;

            // Use Windows heap for guest allocations, then map them into guest space
            guest_addr_t mem = 0;
            if (Memory::Map(0, (size + 0xFFF) & ~0xFFFULL,
                            Memory::PROT_READ | Memory::PROT_WRITE, &mem) != Memory::Status::Ok) {
                return 0;
            }
            LOG_DEBUG(HLE, "libkernel::malloc(size: %llu) -> 0x%llx", size, mem);
            return mem;
        });

        // free (tIhsqj0qsFE#T#T)
        RegisterSymbol("libkernel", "tIhsqj0qsFE#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t ptr = args.arg1;
            if (ptr) {
                LOG_DEBUG(HLE, "libkernel::free(ptr: 0x%llx) - stub (no-op)", ptr);
                // In a full impl we'd track allocation sizes and VirtualFree here.
                // For now, leak: VirtualFree needs the exact base & size.
            }
            return 0;
        });

        // calloc (2X5agFjKxMc#T#T) — malloc + zero-fill
        RegisterSymbol("libkernel", "2X5agFjKxMc#T#T", [](const GuestArgs& args) -> u64 {
            u64 nmemb = args.arg1;
            u64 size  = args.arg2;
            u64 total = nmemb * size;
            if (total == 0) total = 1;

            guest_addr_t mem = 0;
            if (Memory::Map(0, (total + 0xFFF) & ~0xFFFULL,
                            Memory::PROT_READ | Memory::PROT_WRITE, &mem) != Memory::Status::Ok) {
                return 0;
            }
            // Memory::Map already commits zeroed pages on Windows
            LOG_DEBUG(HLE, "libkernel::calloc(nmemb: %llu, size: %llu) -> 0x%llx", nmemb, size, mem);
            return mem;
        });

        // =====================================================================
        // POSIX-like file I/O (C stdio)
        // =====================================================================
        // fopen (xeYO4u7uyJ0#T#T)
        RegisterSymbol("libkernel", "xeYO4u7uyJ0#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t path_ptr = args.arg1;
            guest_addr_t mode_ptr = args.arg2;
            std::string path, mode;
            for (u64 i = 0; ; ++i) { u8 c = Memory::Read<u8>(path_ptr + i); if (!c) break; path += (char)c; }
            for (u64 i = 0; ; ++i) { u8 c = Memory::Read<u8>(mode_ptr + i); if (!c) break; mode += (char)c; }
            FILE* f = fopen(path.c_str(), mode.c_str());
            LOG_INFO(HLE, "libkernel::fopen('%s', '%s') -> %p", path.c_str(), mode.c_str(), f);
            return reinterpret_cast<u64>(f);
        });

        // fclose (uodLYyUip20#T#T)
        RegisterSymbol("libkernel", "uodLYyUip20#T#T", [](const GuestArgs& args) -> u64 {
            FILE* f = reinterpret_cast<FILE*>(args.arg1);
            int r = f ? fclose(f) : -1;
            LOG_DEBUG(HLE, "libkernel::fclose(%p) -> %d", f, r);
            return (u64)(s64)r;
        });

        // fread (lbB+UlZqVG0#T#T)
        RegisterSymbol("libkernel", "lbB+UlZqVG0#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t buf = args.arg1;
            u64 size  = args.arg2;
            u64 count = args.arg3;
            FILE* f   = reinterpret_cast<FILE*>(args.arg4);
            if (!f || !buf) return 0;
            u64 n = fread(reinterpret_cast<void*>(buf), size, count, f);
            LOG_DEBUG(HLE, "libkernel::fread(buf: 0x%llx, size: %llu, count: %llu) -> %llu", buf, size, count, n);
            return n;
        });

        // fseek (rQFVBXp-Cxg#T#T)
        RegisterSymbol("libkernel", "rQFVBXp-Cxg#T#T", [](const GuestArgs& args) -> u64 {
            FILE* f    = reinterpret_cast<FILE*>(args.arg1);
            s64 offset = static_cast<s64>(args.arg2);
            int whence = static_cast<int>(args.arg3);
            int r = f ? fseek(f, (long)offset, whence) : -1;
            LOG_DEBUG(HLE, "libkernel::fseek(%p, %lld, %d) -> %d", f, offset, whence, r);
            return (u64)(s64)r;
        });

        // ftell (Qazy8LmXTvw#T#T)
        RegisterSymbol("libkernel", "Qazy8LmXTvw#T#T", [](const GuestArgs& args) -> u64 {
            FILE* f = reinterpret_cast<FILE*>(args.arg1);
            long r = f ? ftell(f) : -1;
            LOG_DEBUG(HLE, "libkernel::ftell(%p) -> %ld", f, r);
            return (u64)(s64)r;
        });

        // fwrite (unresolved, common enough to pre-register)
        RegisterSymbol("libkernel", "fwrite#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t buf = args.arg1;
            u64 size  = args.arg2;
            u64 count = args.arg3;
            FILE* f   = reinterpret_cast<FILE*>(args.arg4);
            if (!f || !buf) return 0;
            return fwrite(reinterpret_cast<const void*>(buf), size, count, f);
        });

        // printf (hcuQgD53UxM#T#T) — just log to stderr
        RegisterSymbol("libkernel", "hcuQgD53UxM#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t fmt_ptr = args.arg1;
            if (!fmt_ptr) return 0;
            std::string fmt;
            for (u64 i = 0; i < 512; ++i) { u8 c = Memory::Read<u8>(fmt_ptr + i); if (!c) break; fmt += (char)c; }
            std::cerr << "[GUEST][PRINTF]: " << fmt;
            return (u64)fmt.size();
        });

        // puts (SfQIZcqvvms stub — unknown, may be puts)
        RegisterSymbol("libkernel", "SfQIZcqvvms#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t str_ptr = args.arg1;
            if (str_ptr) {
                std::string s;
                for (u64 i = 0; i < 1024; ++i) { u8 c = Memory::Read<u8>(str_ptr + i); if (!c) break; s += (char)c; }
                std::cerr << "[GUEST][PUTS]: " << s << "\n";
            }
            return 0;
        });

        // =====================================================================
        // Kernel file operations (sceKernel* wrappers over POSIX fd)
        // =====================================================================
        // sceKernelOpen (1G3lF1Gg1k8#S#N)
        RegisterSymbol("libkernel", "1G3lF1Gg1k8#S#N", [](const GuestArgs& args) -> u64 {
            guest_addr_t path_ptr = args.arg1;
            int flags  = static_cast<int>(args.arg2);
            int mode   = static_cast<int>(args.arg3);
            std::string path;
            for (u64 i = 0; i < 4096; ++i) { u8 c = Memory::Read<u8>(path_ptr + i); if (!c) break; path += (char)c; }
            // Map O_RDONLY=0, O_WRONLY=1, O_RDWR=2 — same on Windows with _open
            int fd = _open(path.c_str(), flags | _O_BINARY, mode);
            LOG_INFO(HLE, "sceKernelOpen('%s', flags=0x%X, mode=0x%X) -> %d", path.c_str(), flags, mode, fd);
            return (u64)(s64)fd;
        });

        // sceKernelRead (Cg4srZ6TKbU#S#N)
        RegisterSymbol("libkernel", "Cg4srZ6TKbU#S#N", [](const GuestArgs& args) -> u64 {
            int fd            = static_cast<int>(args.arg1);
            guest_addr_t buf  = args.arg2;
            u64 count         = args.arg3;
            if (!buf || count == 0) return 0;
            int n = _read(fd, reinterpret_cast<void*>(buf), (unsigned)count);
            LOG_DEBUG(HLE, "sceKernelRead(fd=%d, buf=0x%llx, count=%llu) -> %d", fd, buf, count, n);
            return (u64)(s64)n;
        });

        // sceKernelClose (UK2Tl2DWUns#S#N)
        RegisterSymbol("libkernel", "UK2Tl2DWUns#S#N", [](const GuestArgs& args) -> u64 {
            int fd = static_cast<int>(args.arg1);
            int r  = _close(fd);
            LOG_DEBUG(HLE, "sceKernelClose(fd=%d) -> %d", fd, r);
            return (u64)(s64)r;
        });

        // sceKernelLseek (oib76F-12fk#S#N)
        RegisterSymbol("libkernel", "oib76F-12fk#S#N", [](const GuestArgs& args) -> u64 {
            int fd     = static_cast<int>(args.arg1);
            s64 offset = static_cast<s64>(args.arg2);
            int whence = static_cast<int>(args.arg3);
            s64 r = _lseeki64(fd, offset, whence);
            LOG_DEBUG(HLE, "sceKernelLseek(fd=%d, off=%lld, whence=%d) -> %lld", fd, offset, whence, r);
            return (u64)r;
        });

        // sceKernelStat (eV9wAD2riIA#S#N) — stub: return ENOENT
        RegisterSymbol("libkernel", "eV9wAD2riIA#S#N", [](const GuestArgs& args) -> u64 {
            guest_addr_t path_ptr = args.arg1;
            std::string path;
            if (path_ptr) for (u64 i = 0; i < 512; ++i) { u8 c = Memory::Read<u8>(path_ptr + i); if (!c) break; path += (char)c; }
            LOG_WARN(HLE, "sceKernelStat('%s') -> stub ENOENT", path.c_str());
            return 0x800D0002; // ENOENT
        });

        // sceKernelMunmap (cQke9UuBQOk#S#N)
        RegisterSymbol("libkernel", "cQke9UuBQOk#S#N", [](const GuestArgs& args) -> u64 {
            guest_addr_t addr = args.arg1;
            u64 size = args.arg2;
            LOG_DEBUG(HLE, "sceKernelMunmap(addr: 0x%llx, size: 0x%llx) -> stub", addr, size);
            // No-op: we don't track allocation sizes, so we can't VirtualFree safely
            return 0;
        });

        // sceKernelVirtualQuery (rVjRvHJ0X6c#S#N)
        RegisterSymbol("libkernel", "rVjRvHJ0X6c#S#N", [](const GuestArgs& args) -> u64 {
            guest_addr_t addr = args.arg1;
            guest_addr_t info_ptr = args.arg2;
            u64 info_size = args.arg3;
            LOG_INFO(HLE, "sceKernelVirtualQuery(addr: 0x%llx, info: 0x%llx, size: %llu)", addr, info_ptr, info_size);

            if (!info_ptr || info_size < 16) {
                return 0x80020016; // EINVAL
            }

            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) == 0) {
                LOG_ERROR(HLE, "sceKernelVirtualQuery: VirtualQuery failed (err=%lu)", GetLastError());
                return 0x80020005; // EFAULT
            }

            // Write start address
            Memory::Write<u64>(info_ptr + 0, reinterpret_cast<u64>(mbi.BaseAddress));
            // Write end address
            Memory::Write<u64>(info_ptr + 8, reinterpret_cast<u64>(mbi.BaseAddress) + mbi.RegionSize);

            // Convert Windows protection to Unix prot
            u32 prot = 0;
            if (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) {
                prot |= 1; // PROT_READ
            }
            if (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) {
                prot |= 2; // PROT_WRITE
            }
            if (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) {
                prot |= 4; // PROT_EXEC
            }

            if (info_size >= 20) {
                Memory::Write<u32>(info_ptr + 16, prot);
            }
            if (info_size >= 24) {
                Memory::Write<u32>(info_ptr + 20, mbi.State);
            }

            return 0; // Success
        });

        // =====================================================================
        // pthread stubs (single-threaded model — all calls succeed trivially)
        // =====================================================================
        // scePthreadSelf (aI+OeCz8xrQ#T#T) — returns a fake thread handle (1)
        RegisterSymbol("libkernel", "aI+OeCz8xrQ#T#T", [](const GuestArgs& /*args*/) -> u64 {
            return 1; // fake main thread handle
        });

        // scePthreadYield (T72hz6ffq08#T#T) — no-op in single-threaded mode
        RegisterSymbol("libkernel", "T72hz6ffq08#T#T", [](const GuestArgs& /*args*/) -> u64 {
            return 0;
        });

        // scePthreadCreate (6UgtwV+0zb4#T#T) — real implementation: spawns a Windows thread
        RegisterSymbol("libkernel", "6UgtwV+0zb4#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t tid_out   = args.arg1;
            guest_addr_t attr_ptr  = args.arg2;
            guest_addr_t entry_ptr = args.arg3;
            guest_addr_t start_arg = args.arg4;
            guest_addr_t name_ptr  = args.arg5;
            (void)attr_ptr;

            std::string name = "<unnamed>";
            if (name_ptr) {
                for (u64 i = 0; i < 128; ++i) {
                    u8 c = Memory::Read<u8>(name_ptr + i);
                    if (!c) break;
                    name += static_cast<char>(c);
                }
            }

            constexpr u64 kGuestStackSize = 1024 * 1024;
            void* guest_stack = VirtualAlloc(nullptr, kGuestStackSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!guest_stack) {
                LOG_ERROR(HLE, "scePthreadCreate(NID): VirtualAlloc failed for guest stack");
                return 11; // EAGAIN
            }
            u64 stack_base = reinterpret_cast<u64>(guest_stack);

            constexpr u64 kTlsSize = 0x4000;
            void* tls_block = VirtualAlloc(nullptr, kTlsSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            u64 tls_base = reinterpret_cast<u64>(tls_block);

            u64 tid = g_next_tid.fetch_add(1);

            auto* info = new GuestThreadInfo{};
            info->guest_tid   = tid;
            info->entry_point = entry_ptr;
            info->arg         = start_arg;
            info->stack_base  = stack_base;
            info->stack_size  = kGuestStackSize;
            info->tls_base    = tls_base;
            info->name        = name;

            Kernel::ThreadContext ctx;
            ctx.thread_id   = tid;
            ctx.name        = name;
            ctx.entry_point = entry_ptr;
            ctx.stack_base  = stack_base;
            ctx.stack_size  = kGuestStackSize;
            ctx.tls_base    = tls_base;
            Kernel::RegisterThread(ctx);

            info->handle = CreateThread(nullptr, 0, GuestThreadProc, info, 0, nullptr);
            if (!info->handle) {
                LOG_ERROR(HLE, "scePthreadCreate(NID): CreateThread failed (err=%lu)", GetLastError());
                VirtualFree(guest_stack, 0, MEM_RELEASE);
                VirtualFree(tls_block, 0, MEM_RELEASE);
                delete info;
                return 11;
            }

            {
                std::lock_guard<std::mutex> lock(g_thread_mutex);
                g_threads[tid] = *info;
            }

            if (tid_out) Memory::Write<u64>(tid_out, tid);
            LOG_INFO(HLE, "scePthreadCreate(NID)(entry=0x%llx, arg=0x%llx, name='%s') -> tid=%llu",
                     entry_ptr, start_arg, name.c_str(), tid);
            return 0;
        });

        // scePthreadJoin (NID) — real implementation
        RegisterSymbol("libkernel", "scePthreadJoin", [](const GuestArgs& args) -> u64 {
            u64 tid = args.arg1;
            guest_addr_t value_ptr = args.arg2;

            HANDLE handle = nullptr;
            GuestThreadInfo info_copy;
            {
                std::lock_guard<std::mutex> lock(g_thread_mutex);
                auto it = g_threads.find(tid);
                if (it == g_threads.end()) {
                    LOG_ERROR(HLE, "scePthreadJoin(NID): thread %llu not found", tid);
                    return 3; // ESRCH
                }
                info_copy = it->second;
                handle = it->second.handle;
            }

            DWORD wait_result = WaitForSingleObject(handle, INFINITE);
            if (wait_result != WAIT_OBJECT_0) {
                LOG_ERROR(HLE, "scePthreadJoin(NID): WaitForSingleObject failed (err=%lu)", GetLastError());
                return 5;
            }

            DWORD exit_code = 0;
            GetExitCodeThread(handle, &exit_code);
            if (value_ptr) Memory::Write<u64>(value_ptr, static_cast<u64>(exit_code));

            CloseHandle(handle);
            if (info_copy.stack_base) {
                VirtualFree(reinterpret_cast<void*>(static_cast<uintptr_t>(info_copy.stack_base)), 0, MEM_RELEASE);
            }
            if (info_copy.tls_base) {
                VirtualFree(reinterpret_cast<void*>(static_cast<uintptr_t>(info_copy.tls_base)), 0, MEM_RELEASE);
            }
            {
                std::lock_guard<std::mutex> lock(g_thread_mutex);
                g_threads.erase(tid);
            }

            LOG_INFO(HLE, "scePthreadJoin(NID)(tid=%llu) -> exit_code=%lu", tid, exit_code);
            return 0;
        });

        // scePthreadMutexInit / Lock / Unlock / Destroy (stubs)
        RegisterSymbol("libkernel", "scePthreadMutexInit", [](const GuestArgs& /*a*/) -> u64 { return 0; });
        RegisterSymbol("libkernel", "scePthreadMutexLock", [](const GuestArgs& /*a*/) -> u64 { return 0; });
        RegisterSymbol("libkernel", "scePthreadMutexUnlock", [](const GuestArgs& /*a*/) -> u64 { return 0; });
        RegisterSymbol("libkernel", "scePthreadMutexDestroy", [](const GuestArgs& /*a*/) -> u64 { return 0; });

        // scePthreadCondInit / Wait / Signal / Broadcast / Destroy (stubs)
        RegisterSymbol("libkernel", "scePthreadCondInit", [](const GuestArgs& /*a*/) -> u64 { return 0; });
        RegisterSymbol("libkernel", "scePthreadCondWait", [](const GuestArgs& /*a*/) -> u64 { return 0; });
        RegisterSymbol("libkernel", "scePthreadCondSignal", [](const GuestArgs& /*a*/) -> u64 { return 0; });
        RegisterSymbol("libkernel", "scePthreadCondBroadcast", [](const GuestArgs& /*a*/) -> u64 { return 0; });
        RegisterSymbol("libkernel", "scePthreadCondDestroy", [](const GuestArgs& /*a*/) -> u64 { return 0; });

        // Missing mutex/init NIDs hit by PPSA01668
        RegisterSymbol("libkernel", "F8bUHwAG284#k#N", [](const GuestArgs& /*a*/) -> u64 { return 0; });
        RegisterSymbol("libkernel", "iMp8QpE+XO4#k#N", [](const GuestArgs& /*a*/) -> u64 { return 0; });
        RegisterSymbol("libkernel", "1FGvU0i9saQ#k#N", [](const GuestArgs& /*a*/) -> u64 { return 0; });
        RegisterSymbol("libkernel", "cmo1RIYva9o#k#N", [](const GuestArgs& /*a*/) -> u64 { return 0; });
        RegisterSymbol("libkernel", "smWEktiyyG0#k#N", [](const GuestArgs& /*a*/) -> u64 { return 0; });
        RegisterSymbol("libkernel", "188x57JYp0g#k#N", [](const GuestArgs& /*a*/) -> u64 { return 0; });


        // =====================================================================
        // String utilities
        // =====================================================================
        // strncpy (6sJWiWSRuqk#T#T)
        RegisterSymbol("libkernel", "6sJWiWSRuqk#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t dst = args.arg1, src = args.arg2;
            u64 n = args.arg3;
            if (dst && src && n > 0 && n < 0x10000000ULL)
                strncpy(reinterpret_cast<char*>(dst), reinterpret_cast<const char*>(src), n);
            return dst;
        });

        // strcpy
        RegisterSymbol("libkernel", "strcpy#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t dst = args.arg1, src = args.arg2;
            if (dst && src) strcpy(reinterpret_cast<char*>(dst), reinterpret_cast<const char*>(src));
            return dst;
        });

        // strcmp
        RegisterSymbol("libkernel", "strcmp#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t a = args.arg1, b = args.arg2;
            if (!a || !b) return (u64)(s64)-1;
            int cmp = strcmp(reinterpret_cast<const char*>(a), reinterpret_cast<const char*>(b));
            return (u64)(s64)cmp;
        });

        // AV6ipCNa4Rw = strcasecmp
        RegisterSymbol("libkernel", "AV6ipCNa4Rw#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t a = args.arg1, b = args.arg2;
            if (!a || !b) return (u64)(s64)-1;
            int cmp = _stricmp(reinterpret_cast<const char*>(a), reinterpret_cast<const char*>(b));
            return (u64)(s64)cmp;
        });

        // sprintf (g7zzzLDYGw0#T#T) — simple passthrough, format string already in guest memory
        RegisterSymbol("libkernel", "g7zzzLDYGw0#T#T", [](const GuestArgs& args) -> u64 {
            guest_addr_t dst = args.arg1;
            guest_addr_t fmt_ptr = args.arg2;
            if (!dst || !fmt_ptr) return 0;
            // For safety we'll just copy the format string as-is (no va_args support here)
            std::string fmt;
            for (u64 i = 0; i < 512; ++i) { u8 c = Memory::Read<u8>(fmt_ptr + i); if (!c) break; fmt += (char)c; }
            std::memcpy(reinterpret_cast<void*>(dst), fmt.c_str(), fmt.size() + 1);
            return fmt.size();
        });

        // =====================================================================
        // Semaphore stubs (sceKernelCreate/Wait/Signal/PollSema)
        // =====================================================================
        RegisterSymbol("libkernel", "Zxa0VhQVTsk#S#N", [](const GuestArgs& /*a*/) -> u64 {
            LOG_DEBUG(HLE, "sceKernelWaitSema() -> stub 0"); return 0;
        });
        RegisterSymbol("libkernel", "12wOHk8ywb0#S#N", [](const GuestArgs& /*a*/) -> u64 {
            LOG_DEBUG(HLE, "sceKernelPollSema() -> stub 0"); return 0;
        });
        RegisterSymbol("libkernel", "4czppHBiriw#S#N", [](const GuestArgs& /*a*/) -> u64 {
            LOG_DEBUG(HLE, "sceKernelSignalSema() -> stub 0"); return 0;
        });

        // =====================================================================
        // Misc stubs — return success/0 for unresolved PS5-specific functions
        // These will be updated as we understand them better
        // =====================================================================
        // Unknown PS5-specific NIDs (return 0 to allow game to continue)
        for (const char* nid : {
            "Q4rRL34CEeE#T#T", "pztV4AF18iI#T#T", "8zsu04XNsZ4#T#T",
            "YQ0navp+YIc#T#T", "Ls4tzzhimqQ#T#T", "weDug8QD-lE#T#T",
            "RQXLbdT2lc4#T#T", "-P6FNMzk2Kc#T#T", "s9e3+YpRnzw#H#I",
            "4tPhsP6FpDI#H#I", "m5-2bsNfv7s#S#N", "2Tb92quprl0#S#N",
            "waPcxYiR3WA#S#N", "g+PZd2hiacg#S#N", "4wSze92BhLI#S#N",
            "-Wreprtu0Qs#S#N", "DzES9hQF4f4#S#N", "x1X76arYMxU#S#N",
            "-quPa4SEJUw#S#N", "ie7qhZ4X0Cc#G#H", "AUXVxWeJU-A#S#N",
            "MBuItvba6z8#S#N", "hT0IAEvN+M0#E#F", "5zBnau1uIEo#E#F",
            "tpFJ8LIKvPw#E#F", "AUIHb7jUX3I#E#F", "sUXGfNMalIo#F#G",
            "Bagshr7OQ6Q#F#G", "p+GcLqwpL9M#E#F", "YE4dbtbz6OE#E#F",
            "CzkKf7ahIyU#E#F", "wG+84pnNIuo#E#F", "MfDb+4Nln64#E#F",
            "Wxbg5x3pTXA#E#F", "4llLk7YJRTE#E#F", "3kg7rT0NQIs#S#N",
            "TywrFKCoLGY#G#H", "gjRZNnw0JPE#G#H", "dyIhnXq-0SM#G#H",
            "ZP4e7rlzOUk#G#H", "sDCBrmc61XU#G#H", "85zul--eGXs#G#H",
            "c88Yy54Mx0w#G#H", "fPhymKNvK-A#U#U", "xk0AcarP3V4#L#M",
            "clVvL4ZDntw#L#M", "gjP9-KQzoUk#L#M", "YndgXqQVV7c#L#M",
            "rPo6tV8D9bM#Q#R", "656LMQSrg6U#Q#R", "jEIXUAr9XE8#R#S",
            "rPl0INNc-M8#R#S", "hZIg1EWGsHM#P#Q", "Vo5V8KAwCmk#Q#R",
            "j3YMu1MVNNo#U#U", "hv1luiJrqQM#L#M", "uoUpLGNkygk#O#P",
            "ERKzksauAJA#H#I", "fMP5NHUOaMk#D#E", "yKDy8S5yLA0#G#H",
            "6ncge5+l5Qs#L#M", "bwFjS+bX9mA#U#U", "eR2bZFAAU0Q#D#E",
            "d-kSG2fLrvI#P#Q", "ekNvsT22rsY#N#O", "b+uAV89IlxE#N#O",
            "QOQtbeDqsT4#N#O", "s1--uE9mBFw#N#O"
        }) {
            RegisterSymbol("libkernel", nid, [nid](const GuestArgs& /*a*/) -> u64 {
                LOG_DEBUG(HLE, "PS5-specific stub: %s -> 0", nid);
                return 0;
            });
        }
    }
}
// namespace HLE
