#include "kernel.h"
#include "fd_table.h"
#include "instr_decode.h"
#include "memory.h"
#include "syscalls.h"
#include "thread.h"
#include "tls_patch.h"
#include "guest_tracer.h"
#include <algorithm>
#include "../cpu/amd_compat.h"
#include "../cpu/cpu.h"
#include "../diagnostics/diagnostics.h"
#include "../memory/memory.h"
#include "../hle/hle.h"
#include "../loader/module_graph.h"
#include "../config/config.h"
#include "../common/log.h"
#include "../gpu/gpu.h"
#include <windows.h>
#include <psapi.h>   // MODULEINFO, for locating this module in the sampler
#include <intrin.h>
#include <iostream>
#include <cstring>
#include <csignal>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <set>
#include <vector>
#include <string>
#include <sstream>

// Windows-compatible replacements for Unix headers (minimal set for kernel.cpp)
#ifndef KERNEL_UNIX_COMPAT_H
#define KERNEL_UNIX_COMPAT_H

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#endif // KERNEL_UNIX_COMPAT_H



namespace Kernel {

    // Global state
    static std::unordered_map<u64, ThreadContext> g_threads;
    static std::mutex g_thread_mutex;
    static u64 g_process_id = GetCurrentProcessId();
    static bool g_in_proc = false;

    // Where the VEH crash dumps go.  Deliberately not read from
    // Diagnostics: the test targets compile kernel.cpp without linking
    // diagnostics.cpp, so the core pushes the resolved path in instead.
    static std::string g_crash_dump_dir = "pcsx5_crash";


    // Region captured by the second ("PRX") crash dump.  The default is
    // libc.prx's load base, chosen for an earlier investigation; a different
    // one is selected per-run with PCSX5_CRASH_DUMP_BASE / _SIZE (hex or
    // decimal), following the PCSX5_HEADLESS precedent for diagnostic knobs.
    // GetEnvironmentVariableA rather than getenv: the core builds /W4 /WX and
    // getenv is deprecated by MSVC.  Matches vulkan_backend.cpp's PCSX5_HEADLESS
    // and libagc.cpp's PCSX5_PM4_CAPTURE.
    static u64 EnvU64(const char* name, u64 fallback) {
        char buf[32] = {};
        const DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
        if (n == 0 || n >= sizeof(buf)) return fallback;
        const u64 v = std::strtoull(buf, nullptr, 0);
        return v ? v : fallback;
    }

    u64 CrashDumpBase() {
        static const u64 v = EnvU64("PCSX5_CRASH_DUMP_BASE", 0x820000000ull);
        return v;
    }

    u64 CrashDumpSize() {
        static const u64 v = EnvU64("PCSX5_CRASH_DUMP_SIZE", 16ull * 1024 * 1024);
        return v;
    }
    // Diagnostic write-watchpoint (see Kernel::ArmWatchpointForCurrentThread).
    // Zero disables it, which is the default; nothing below runs unless
    // PCSX5_WATCH_ADDR is set, so a normal run is unaffected.
    u64 WatchpointAddress() {
        static const u64 v = EnvU64("PCSX5_WATCH_ADDR", 0);
        return v;
    }

    // Diagnostic execute-breakpoint.  Traps the first instruction of a guest
    // function and logs its first argument as a string, which is how a caller
    // that passes an unexpected value gets identified without modifying guest
    // code.  Zero disables it, and that is the default.
    u64 BreakpointAddress() {
        static const u64 v = EnvU64("PCSX5_BP_ADDR", 0);
        return v;
    }

    // Arms DR0 as an 8-byte write watch on the configured address.  DR7 bits:
    // L0 (bit 0) enables DR0 locally, R/W0 = 01 breaks on data write, and
    // LEN0 = 11 selects an 8-byte range, which requires 8-byte alignment.
    void ArmWatchpointOnThisThread() {
        const u64 addr = WatchpointAddress();
        const u64 bp   = BreakpointAddress();
        if (!addr && !bp) return;
        if (addr && (addr & 7ull)) {
            LOG_WARN(Kernel, "PCSX5_WATCH_ADDR 0x%llx is not 8-byte aligned; watchpoint not armed", addr);
            return;
        }
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        const HANDLE self = GetCurrentThread();
        if (!GetThreadContext(self, &ctx)) return;
        // Bits 16-23 of DR7 hold R/W and LEN for DR0 and DR1; clearing them
        // leaves DR1 as an execute breakpoint (R/W1 = 00, LEN1 = 00), which is
        // the only encoding that traps on instruction fetch.
        ctx.Dr7 &= ~0xFF0000ull;
        if (addr) {
            ctx.Dr0 = addr;
            ctx.Dr7 = (ctx.Dr7 & ~0x3ull) | 0x1ull | (0x1ull << 16) | (0x3ull << 18);
        }
        if (bp) {
            ctx.Dr1 = bp;
            ctx.Dr7 = (ctx.Dr7 & ~0xCull) | (0x1ull << 2);
        }
        ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (!SetThreadContext(self, &ctx)) {
            LOG_WARN(Kernel, "Failed to arm debug registers on thread %lu", GetCurrentThreadId());
        }
    }

    // ------------------------------------------------------------------
    // VEH recursion guard (H4.3): prevents host stack exhaustion from
    // recursive re-entry of the vectored exception handler.  The VEH
    // can be re-entered when instruction emulation (TLS reads, demand-
    // commit) itself faults — the recursion counter detects this and
    // passes the recursive fault through unmangled so the OS can
    // terminate the process normally.
    // ------------------------------------------------------------------
    thread_local int g_veh_recursion_depth = 0;
    constexpr int kMaxVehRecursionDepth = 8;

    // O3.3: thread-local TLS base cache to avoid g_thread_mutex contention
    // on every VEH TLS trap.  Updated when RegisterThread is called.
    thread_local guest_addr_t t_tls_base = 0;

    struct VehRecursionGuard {
        bool m_skip = false;
        VehRecursionGuard() {
            if (++g_veh_recursion_depth > kMaxVehRecursionDepth) {
                LOG_CRITICAL(Kernel,
                    "VEH: recursion depth %d exceeded (max %d) — passing "
                    "exception through; RIP=0x%llx",
                    g_veh_recursion_depth, kMaxVehRecursionDepth,
                    reinterpret_cast<u64>(_AddressOfReturnAddress()));
                m_skip = true;
            }
        }
        ~VehRecursionGuard() {
            if (!m_skip) --g_veh_recursion_depth;
        }
    };

    void SetInProcMode(bool enabled) {
        g_in_proc = enabled;
    }

    bool IsInProcMode() {
        return g_in_proc;
    }

    void SetCrashDumpDir(const std::string& dir) {
        if (!dir.empty()) g_crash_dump_dir = dir;
    }

    // Installed by whoever owns end-of-run reporting; see SetGuestExitHook.
    static void (*g_guest_exit_hook)() = nullptr;

    void SetGuestExitHook(void (*hook)()) { g_guest_exit_hook = hook; }

    static void (*g_periodic_report_hook)() = nullptr;

    void SetPeriodicReportHook(void (*hook)()) { g_periodic_report_hook = hook; }

    void RunGuestExitHook() {
        if (!g_guest_exit_hook) return;
        // Run once: a guest that calls exit twice, or exits while another
        // thread is already exiting, must not re-enter the writer.
        void (*hook)() = g_guest_exit_hook;
        g_guest_exit_hook = nullptr;
        hook();
    }

    void ArmWatchpointForCurrentThread() {
        ArmWatchpointOnThisThread();
    }

    // ---- guest execution sampler ----------------------------------------
    // Suspends each guest thread just long enough to read its RIP. Sampling is
    // the only way to see a guest that is stuck without faulting; a frozen run
    // otherwise leaves nothing behind but its last unrelated log line.
    // Threads currently blocked inside each named primitive.
    static std::mutex g_wait_mutex;
    static std::unordered_map<std::string, long long> g_wait_counts;
    static std::unordered_map<std::string, long long> g_wait_totals;

    void NoteWaitEnter(const char* name) {
        if (!name) return;
        std::lock_guard<std::mutex> lk(g_wait_mutex);
        ++g_wait_counts[name];
        ++g_wait_totals[name];
    }

    void NoteWaitExit(const char* name) {
        if (!name) return;
        std::lock_guard<std::mutex> lk(g_wait_mutex);
        auto it = g_wait_counts.find(name);
        if (it != g_wait_counts.end() && it->second > 0) --it->second;
    }

    static void ReportWaits() {
        std::vector<std::pair<std::string, std::pair<long long, long long>>> rows;
        {
            std::lock_guard<std::mutex> lk(g_wait_mutex);
            for (const auto& kv : g_wait_totals) {
                rows.push_back({kv.first, {g_wait_counts[kv.first], kv.second}});
            }
        }
        std::sort(rows.begin(), rows.end(),
                  [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
        for (const auto& r : rows) {
            LOG_ERROR(Kernel, "SAMPLER:   waiting in %-28s %lld now, %lld entered total",
                      r.first.c_str(), r.second.first, r.second.second);
        }
    }

    static void SamplerLoopMarker() {}   // address used to locate this module
    static std::atomic<bool> g_sampler_run{false};
    static std::thread*      g_sampler_thread = nullptr;

    static std::string DescribeHostAddress(u64 addr) {
        HMODULE mod = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(addr), &mod) && mod) {
            char path[MAX_PATH] = "";
            GetModuleFileNameA(mod, path, MAX_PATH);
            const char* base = std::strrchr(path, static_cast<int>(92));
            char buf[MAX_PATH + 32];
            std::snprintf(buf, sizeof(buf), "%s+0x%llx", base ? base + 1 : path,
                          addr - reinterpret_cast<u64>(mod));
            return buf;
        }
        char buf[32];
        std::snprintf(buf, sizeof(buf), "0x%llx", addr);
        return buf;
    }

    static void ReportSamples(const std::unordered_map<u64, u64>& hits,
                              const std::unordered_map<u64, u64>& host_hits,
                              u64 total, u64 off_guest) {
        std::vector<std::pair<u64, u64>> ranked(hits.begin(), hits.end());
        std::sort(ranked.begin(), ranked.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        LOG_ERROR(Kernel, "SAMPLER: %llu samples, %llu inside guest code, %llu elsewhere",
                  total, total - off_guest, off_guest);
        for (size_t i = 0; i < ranked.size() && i < 10; ++i) {
            LOG_ERROR(Kernel, "SAMPLER:   guest+0x%llx  %llu samples (%.1f%%)",
                      ranked[i].first - 0x800000000ull, ranked[i].second,
                      total ? (100.0 * ranked[i].second / total) : 0.0);
        }
        std::vector<std::pair<u64, u64>> hranked(host_hits.begin(), host_hits.end());
        std::sort(hranked.begin(), hranked.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        ReportWaits();
        for (size_t i = 0; i < hranked.size() && i < 10; ++i) {
            LOG_ERROR(Kernel, "SAMPLER:   host %-40s %llu samples (%.1f%%)",
                      DescribeHostAddress(hranked[i].first).c_str(), hranked[i].second,
                      total ? (100.0 * hranked[i].second / total) : 0.0);
        }
    }

    // Range of pcsx5_core in this process, resolved once.
    static bool CoreRange(u64& lo, u64& hi) {
        static u64 s_lo = 0, s_hi = 0;
        if (!s_lo) {
            HMODULE mod = nullptr;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(&SamplerLoopMarker), &mod) && mod) {
                MODULEINFO mi{};
                if (GetModuleInformation(GetCurrentProcess(), mod, &mi, sizeof(mi))) {
                    s_lo = reinterpret_cast<u64>(mi.lpBaseOfDll);
                    s_hi = s_lo + mi.SizeOfImage;
                }
            }
        }
        lo = s_lo; hi = s_hi;
        return s_lo != 0;
    }

    // Scan a suspended thread's stack for the first return address that lands in
    // pcsx5_core. Reading another thread's stack is safe here: it is suspended,
    // and the read is guarded so a bad frame cannot fault the sampler.
    static u64 NearestCoreReturn(const CONTEXT& ctx) {
        u64 lo = 0, hi = 0;
        if (!CoreRange(lo, hi)) return 0;
        const u64 sp = ctx.Rsp;
        if (!sp) return 0;
        constexpr int kDepth = 128;   // qwords of stack to inspect
        for (int i = 0; i < kDepth; ++i) {
            const u64 slot = sp + static_cast<u64>(i) * 8;
            u64 value = 0;
            if (!ReadProcessMemory(GetCurrentProcess(),
                                   reinterpret_cast<LPCVOID>(slot),
                                   &value, sizeof(value), nullptr)) {
                break;
            }
            if (value >= lo && value < hi) return value & ~0xFull;
        }
        return 0;
    }

    static void SamplerLoop(unsigned interval_ms) {
        std::unordered_map<u64, u64> hits;        // guest RIP -> count
        std::unordered_map<u64, u64> host_hits;   // host page -> count
        u64 total = 0, off_guest = 0;
        // Report as we go, not only at shutdown: a crashing run never reaches a
        // clean shutdown, and those are exactly the runs worth sampling.
        auto next_report = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (g_sampler_run.load()) {
            for (auto* gt : CpuCore::GetAllThreads()) {
                if (!gt || !gt->host_thread) continue;
                const HANDLE h = reinterpret_cast<HANDLE>(gt->host_thread);
                if (::SuspendThread(h) == (DWORD)-1) continue;
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_CONTROL;
                const bool ok = ::GetThreadContext(h, &ctx) != 0;
                ::ResumeThread(h);
                if (!ok) continue;
                ++total;
                if (ctx.Rip >= 0x800000000ull && ctx.Rip < 0x900000000ull) {
                    ++hits[ctx.Rip & ~0xFull];   // bucket to 16 bytes
                } else {
                    // Host code. Guest threads spend most of their time inside
                    // HLE calls, so where they are parked matters as much as
                    // guest RIPs -- a thread blocked in a wait is the shape of
                    // a title that renders but never progresses. Bucket by the
                    // module that owns the address.
                    ++off_guest;
                    // "Blocked in ntdll" says only that the thread is waiting.
                    // What matters is which of our HLE calls it is waiting
                    // inside, so walk its stack for the nearest return address
                    // in pcsx5_core and attribute the sample there.
                    const u64 caller = NearestCoreReturn(ctx);
                    ++host_hits[caller ? caller : (ctx.Rip & ~0xFFFull)];
                }
            }
            if (std::chrono::steady_clock::now() >= next_report) {
                ReportSamples(hits, host_hits, total, off_guest);
                next_report = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
        }
        ReportSamples(hits, host_hits, total, off_guest);
    }

    void StartGuestSampler() {
        const u64 interval = EnvU64("PCSX5_SAMPLE_MS", 0);
        if (!interval || g_sampler_thread) return;
        g_sampler_run.store(true);
        g_sampler_thread = new std::thread(SamplerLoop, static_cast<unsigned>(interval));
        LOG_INFO(Kernel, "Guest sampler running every %llu ms", interval);
    }

    void StopGuestSampler() {
        if (!g_sampler_thread) return;
        g_sampler_run.store(false);
        if (g_sampler_thread->joinable()) g_sampler_thread->join();
        delete g_sampler_thread;
        g_sampler_thread = nullptr;
    }
    static PVOID g_veh_handler = nullptr;
    static LPTOP_LEVEL_EXCEPTION_FILTER g_prev_exception_filter = nullptr;
    static GuestTlsContext g_guest_tls;
    static std::vector<Loader::MappedSegment> g_guest_segments;
    static Loader::ModuleResolver g_module_resolver;

    // Retained copy of the main module for crash-dump resolution.  Populated
    // at boot (single-threaded); read lock-free from the VEH crash path.
    static Loader::LoadedModule g_main_module_copy;
    static bool                 g_main_module_retained = false;

    // ------------------------------------------------------------------
    // Heartbeat watchdog (H1): pins the process-alive timestamp in the log
    // every 30 s so a silent death leaves a precise last-alive marker.
    // ------------------------------------------------------------------
    static std::thread g_heartbeat_thread;
    static HANDLE g_heartbeat_stop = nullptr;

    static void HeartbeatLoop() {
        const auto start = std::chrono::steady_clock::now();
        for (;;) {
            if (WaitForSingleObject(g_heartbeat_stop, 30000) == WAIT_OBJECT_0) {
                break;
            }
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start).count();
            LOG_INFO(Kernel, "Heartbeat: process alive, elapsed=%llds, tls_traps=%llu, tls_patches=%llu, os_thread=%lu",
                     static_cast<long long>(elapsed), TlsPatch::TrapCount(),
                     TlsPatch::PatchedCount(), ::GetCurrentThreadId());
            // Leave an inventory behind for a run that is killed rather than
            // exiting: the harness terminates most title runs on a timeout, so
            // without this they produce no report at all.
            if (g_periodic_report_hook) g_periodic_report_hook();
        }
    }

    static void StartHeartbeat() {
        if (g_heartbeat_thread.joinable()) return;
        g_heartbeat_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!g_heartbeat_stop) {
            LOG_WARN(Kernel, "Failed to create heartbeat stop event (err=%lu); heartbeat disabled",
                     GetLastError());
            return;
        }
        g_heartbeat_thread = std::thread(HeartbeatLoop);
    }

    static void StopHeartbeat() {
        if (g_heartbeat_stop) {
            SetEvent(g_heartbeat_stop);
        }
        if (g_heartbeat_thread.joinable()) {
            g_heartbeat_thread.join();
        }
        if (g_heartbeat_stop) {
            CloseHandle(g_heartbeat_stop);
            g_heartbeat_stop = nullptr;
        }
    }

    // ------------------------------------------------------------------
    // Process-death visibility (H1): every death path must leave a final
    // log line.  These hooks cover the CRT paths that bypass the SEH
    // unhandled-exception filter (abort/fastfail, invalid parameter,
    // purecall).  SEH deaths are covered by HostUnhandledExceptionFilter
    // (now chained to the diagnostics crash-bundle writer).
    // ------------------------------------------------------------------
    static void AbortSignalHandler(int sig) {
        LOG_CRITICAL(Kernel, "FATAL: abort() raised (signal %d) — process is terminating", sig);
        std::signal(sig, SIG_DFL);
        std::raise(sig);
    }

    static void InvalidParameterHandler(const wchar_t* expression, const wchar_t* function,
                                        const wchar_t* file, unsigned int line, uintptr_t /*reserved*/) {
        LOG_CRITICAL(Kernel, "FATAL: CRT invalid parameter: expr='%ls' func='%ls' file='%ls' line=%u",
                     expression ? expression : L"", function ? function : L"",
                     file ? file : L"", line);
    }

    static void PurecallHandler() {
        LOG_CRITICAL(Kernel, "FATAL: pure virtual function call — process is terminating");
    }

    // Reserve stack for the stack-overflow exception path: without a
    // guarantee the OS cannot even dispatch STATUS_STACK_OVERFLOW handlers
    // and the process dies silently (the suspected H1 death mode).
    static void ArmStackGuarantee() {
        ULONG guarantee = 64 * 1024;
        if (!SetThreadStackGuarantee(&guarantee)) {
            LOG_WARN(Kernel, "SetThreadStackGuarantee failed (err=%lu)", GetLastError());
        }
    }


    // ------------------------------------------------------------------
    // PRX module auto-load state
    //
    // When a loaded module declares DT_NEEDED libraries that the resolver
    // maps to real PRX/SPRX files on disk, those are loaded recursively
    // (dependency-first) and kept here so their exports can satisfy imports
    // of other modules before the HLE fallback is consulted.
    // ------------------------------------------------------------------
    struct PrxModuleRecord {
        std::string graph_name;   // DT_NEEDED name this module was requested as
        std::filesystem::path path;
        Loader::LoadedModule module;
        bool linked = false;
    };

    // Registry of already-loaded PRX modules, keyed by normalized file path
    // (dedupe).  g_prx_loading holds the keys currently being loaded and
    // breaks dependency cycles during the recursive walk.
    static std::unordered_map<std::string, std::unique_ptr<PrxModuleRecord>> g_prx_modules;
    static std::set<std::string> g_prx_loading;
    static Loader::ModuleGraph g_module_graph;
    constexpr u32 kMaxPrxLoadDepth = 32;

    // Loaded-module registry for sceKernelGetModuleInfo* HLE.  Populated from
    // LinkModule so every linked module (the three u5ag.* segments, PRX, etc.)
    // is address-queryable.  Guarded by g_module_registry_mutex.
    static std::mutex g_module_registry_mutex;
    static std::vector<Loader::LoadedModule> g_loaded_modules;

    // Dynamic-TLS support (__tls_get_addr).  Each registered module that has a
    // PT_TLS segment is given a 1-based TLS-module index (the main module is
    // index 1 when it is TLS-bearing), mirroring the ELF TLS-descriptor
    // sv_ndx the guest loader uses.  Per (thread, module) we lazily allocate a
    // dedicated guest block seeded from the module's PT_TLS template, so a
    // secondary module's TLS variables no longer collide with the main block.
    static std::vector<u32> g_loaded_module_tls_index; // parallel to g_loaded_modules (0=none)
    static std::vector<std::pair<u64, u64>> g_tls_block_cache; // (tid,module)->(block_va) 1:1 key pack
    static std::vector<u64>                  g_tls_block_va;   // parallel block value
    static std::mutex                        g_tls_block_mutex;

    void RegisterLoadedModule(const Loader::LoadedModule& module) {
        if (module.base_address == 0 || module.segments.empty()) return;
        std::lock_guard<std::mutex> lock(g_module_registry_mutex);
        // Avoid duplicates for the same base address.
        for (const auto& m : g_loaded_modules) {
            if (m.base_address == module.base_address) return;
        }
        
        bool is_main_module = g_loaded_modules.empty();
        
        g_loaded_modules.push_back(module);
        g_loaded_module_tls_index.push_back(0); // resolved below
        const size_t idx = g_loaded_modules.size() - 1;
        if (module.has_tls) {
            // 1-based TLS-module index: the first TLS-bearing module loaded is
            // "module 1", which is the main block (fast path in __tls_get_addr).
            g_loaded_module_tls_index[idx] = static_cast<u32>(idx + 1);
            
            if (is_main_module && module.tls_file_size > 0 && module.tls_file_size <= 0x100000ULL) {
                // Initialize the main executable's static TLS block (variant-II).
                u64 templ_va = 0;
                for (const auto& seg : module.segments) {
                    const u64 fo = seg.file_offset;
                    if (module.tls_template_offset >= fo &&
                        module.tls_template_offset + module.tls_file_size <= fo + seg.file_size) {
                        templ_va = seg.address + (module.tls_template_offset - fo);
                        break;
                    }
                }
                if (templ_va && Memory::IsReadable(templ_va, module.tls_file_size)) {
                    u64 align = module.tls_align ? module.tls_align : 0x20;
                    u64 aligned_size = (module.tls_mem_size + align - 1) & ~(align - 1);
                    guest_addr_t tp = g_guest_tls.ThreadPointer();
                    if (tp > aligned_size) {
                        guest_addr_t dest = tp - aligned_size;
                        std::memmove(reinterpret_cast<void*>(dest),
                                     reinterpret_cast<void*>(templ_va),
                                     static_cast<size_t>(module.tls_file_size));
                        LOG_INFO(Kernel, "Initialized main TLS block: copied 0x%llx bytes to 0x%llx", 
                                 (unsigned long long)module.tls_file_size, dest);
                    }
                }
            }
        }
    }

    const Loader::LoadedModule* FindModuleForAddr(guest_addr_t addr) {
        if (!addr) return nullptr;
        std::lock_guard<std::mutex> lock(g_module_registry_mutex);
        for (const auto& m : g_loaded_modules) {
            if (addr >= m.base_address &&
                addr < m.base_address + m.image_size) {
                return &m;
            }
        }
        return nullptr;
    }

    guest_addr_t GetMainModuleProcessParam() {
        std::lock_guard<std::mutex> lock(g_module_registry_mutex);
        if (g_loaded_modules.empty()) return 0;
        
        // The main module is usually the first one
        for (const auto& m : g_loaded_modules) {
            for (const auto& seg : m.segments) {
                if (seg.type == Loader::PT_SCE_PROC_PARAM) {
                    return seg.address;
                }
            }
        }
        return 0;
    }

    static std::string NormalizePrxPath(const std::filesystem::path& path) {
        std::string key = std::filesystem::absolute(path).lexically_normal().string();
        for (auto& ch : key) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
        return key;
    }

    void ConfigureModuleResolver(const std::string& game_dir,
                                 const std::string& firmware_modules_dir) {
        std::vector<std::filesystem::path> dirs;
        // Game-bundled modules (<gamedir>/sce_module/) take precedence over
        // user-supplied firmware modules.
        if (!game_dir.empty()) {
            dirs.emplace_back(std::filesystem::path(game_dir) / "sce_module");
        }
        // User-configured firmware directory (explicit override).
        if (!firmware_modules_dir.empty()) {
            dirs.emplace_back(firmware_modules_dir);
        } else {
            // Auto-detect: check common locations for firmware PRX modules.
            // 1. Next to the executable: <exe_dir>/firmware_modules/
            {
                char module_path[MAX_PATH] = {};
                const DWORD len = GetModuleFileNameA(nullptr, module_path, MAX_PATH);
                if (len > 0 && len < MAX_PATH) {
                    const auto exe_dir = std::filesystem::path(module_path).parent_path();
                    const auto fw_dir = exe_dir / "firmware_modules";
                    std::error_code ec;
                    if (std::filesystem::exists(fw_dir, ec)) {
                        dirs.emplace_back(fw_dir);
                    }
                }
            }
            // 2. Config directory: <config_dir>/firmware_modules/
            {
                const auto cfg_dir =
                    std::filesystem::path(ConfigService::Directory()) / "firmware_modules";
                std::error_code ec;
                if (std::filesystem::exists(cfg_dir, ec)) {
                    // Avoid duplicate if exe_dir == config_dir and both paths
                    // resolved to the same directory.
                    if (dirs.empty() || dirs.back() != cfg_dir) {
                        dirs.emplace_back(cfg_dir);
                    }
                }
            }
            // Log a suggestion if still no firmware dir was found.
            if (dirs.size() <= 1) { // only game sce_module/ or nothing at all
                LOG_WARN(Kernel, "No firmware_modules_dir configured and none found "
                         "auto-detected. PRX resolution falls back to HLE stubs. "
                         "Place firmware PRX files in 'firmware_modules/' next to "
                         "the emulator executable or set loader.firmware_modules_dir "
                         "in the config.");
            }
        }
        g_module_resolver.SetSearchDirectories(std::move(dirs));
        for (const auto& dir : g_module_resolver.SearchDirectories()) {
            LOG_INFO(Kernel, "PRX module search dir: %s", dir.string().c_str());
        }
    }

    Loader::ModuleResolver& GetModuleResolver() {
        return g_module_resolver;
    }

    static std::string g_app0_dir;
    static std::string g_savedata_dir;

    void SetApp0Directory(const std::string& dir) {
        g_app0_dir = dir;
        if (!dir.empty()) {
            LOG_INFO(Kernel, "Guest /app0 mapped to host dir: %s", dir.c_str());
        }
    }

    void SetSaveDataDirectory(const std::string& dir) {
        g_savedata_dir = dir;
        if (!dir.empty()) {
            LOG_INFO(Kernel, "Guest /savedata0 mapped to host dir: %s", dir.c_str());
        }
    }

    std::string TranslateGuestPath(const std::string& guest_path) {
        // "/app0" and friends resolve against the game package directory.
        constexpr std::string_view kApp0 = "/app0";
        if (!g_app0_dir.empty()) {
            if (guest_path == kApp0) return g_app0_dir;
            if (guest_path.rfind(std::string(kApp0) + "/", 0) == 0) {
                return g_app0_dir + guest_path.substr(kApp0.size());
            }
        }
        // "/savedata0" resolves against the save-data HLE backing dir so
        // guest file I/O under the mount point persists to the same place
        // the libSceSaveData HLE uses.  The effective dir is queried per
        // translation so per-user switching (multi-profile configs) takes
        // effect without re-calling SetSaveDataDirectory; g_savedata_dir
        // (set once from main) acts as the enable flag / legacy fallback.
        constexpr std::string_view kSaveData0 = "/savedata0";
        if (!g_savedata_dir.empty()) {
            const std::string sd_dir = HLE::GetEffectiveSaveDataDir();
            if (guest_path == kSaveData0) return sd_dir;
            if (guest_path.rfind(std::string(kSaveData0) + "/", 0) == 0) {
                return sd_dir + guest_path.substr(kSaveData0.size());
            }
        }
        // Guest absolute path under an unmapped mount, or a host-absolute
        // path (drive letter / UNC): pass through unchanged.
        if (guest_path.empty() || guest_path[0] == '/' || guest_path[0] == '\\' ||
            guest_path.find(':') != std::string::npos) {
            return guest_path;
        }
        // Relative path: the guest CWD is the package root (/app0).
        if (!g_app0_dir.empty()) {
            return g_app0_dir + "/" + guest_path;
        }
        return guest_path;
    }

    // Forward declarations
    static LONG CALLBACK VectoredExceptionHandler(PEXCEPTION_POINTERS exception_info);
    static LONG CALLBACK HostUnhandledExceptionFilter(PEXCEPTION_POINTERS exception_info);

    static LONG CALLBACK HostUnhandledExceptionFilter(PEXCEPTION_POINTERS exception_info) {
        PEXCEPTION_RECORD exception_record = exception_info->ExceptionRecord;
        PCONTEXT context = exception_info->ContextRecord;
        u64 ip = context->Rip;
        
        LOG_ERROR(Kernel, "--------------------------------------------------");
        LOG_ERROR(Kernel, "UNHANDLED HOST EXCEPTION (EMULATOR CRASHED)!");
        LOG_ERROR(Kernel, "Exception Code: 0x%X", exception_record->ExceptionCode);
        LOG_ERROR(Kernel, "Crash Address (RIP): 0x%llx", ip);
        LOG_ERROR(Kernel, "Register Dump:");
        LOG_ERROR(Kernel, "  RAX: 0x%016llx  RBX: 0x%016llx", context->Rax, context->Rbx);
        LOG_ERROR(Kernel, "  RCX: 0x%016llx  RDX: 0x%016llx", context->Rcx, context->Rdx);
        LOG_ERROR(Kernel, "  RSI: 0x%016llx  RDI: 0x%016llx", context->Rsi, context->Rdi);
        LOG_ERROR(Kernel, "  RBP: 0x%016llx  RSP: 0x%016llx", context->Rbp, context->Rsp);
        LOG_ERROR(Kernel, "  R8:  0x%016llx  R9:  0x%016llx", context->R8,  context->R9);
        LOG_ERROR(Kernel, "  R10: 0x%016llx  R11: 0x%016llx", context->R10, context->R11);
        LOG_ERROR(Kernel, "  R12: 0x%016llx  R13: 0x%016llx", context->R12, context->R13);
        LOG_ERROR(Kernel, "  R14: 0x%016llx  R15: 0x%016llx", context->R14, context->R15);
        LOG_ERROR(Kernel, "--------------------------------------------------");

        // Chain to the previously installed filter (the diagnostics crash
        // handler) so the crash-report bundle and minidump are written.
        if (g_prev_exception_filter) {
            return g_prev_exception_filter(exception_info);
        }
        return EXCEPTION_EXECUTE_HANDLER;
    }

    // Demand-commit guest fault handler: if the faulting address lies inside
    // a reserved-but-uncommitted guest region (sceKernelReserveVirtualRange)
    // or inside the HLE direct-memory phys pool, commit the covering 64 KiB
    // block and let execution resume.  Returns false for anything we cannot
    // cover, so the caller falls through to the normal crash path.
    static bool DemandCommitFaultHandler(guest_addr_t fault_addr, u64 /*code*/, void* /*user*/) {
        if (HLE::CommitPhysPool(fault_addr)) return true;
        if (Memory::CommitOnFault(fault_addr)) return true;
        return false;
    }

    bool Initialize() {
        LOG_INFO(Kernel, "Initializing Kernel subsystem...");

        // Initialize sub-components
        InitializeFdTable();
        InitializeSyscallTable();
        InitializeGuestMemory();

        // Register Vectored Exception Handler (VEH) to capture patched syscalls (INT 3)
        g_veh_handler = AddVectoredExceptionHandler(1, VectoredExceptionHandler);
        if (!g_veh_handler) {
            LOG_ERROR(Kernel, "Failed to register Vectored Exception Handler.");
            return false;
        }

        // Register Top-Level Exception Filter for host crashes.  Skipped in
        // in-proc mode: the host process (WPF app) owns its own unhandled-
        // exception policy, and CRT death hooks would hijack its runtime too.
        if (!g_in_proc) {
            g_prev_exception_filter = SetUnhandledExceptionFilter(HostUnhandledExceptionFilter);

            // CRT death paths bypass the SEH filter — hook them so abort(),
            // fastfail, invalid-parameter and purecall deaths leave a final
            // log line.
            _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
            std::signal(SIGABRT, &AbortSignalHandler);
            _set_invalid_parameter_handler(&InvalidParameterHandler);
            _set_purecall_handler(&PurecallHandler);
        }
        ArmStackGuarantee();

        StartHeartbeat();

        // Install the demand-commit fault handler: guest faults on reserved
        // (not yet committed) pages are committed on first touch instead of
        // crashing.  Consulted by VectoredExceptionHandler before the crash
        // path and by Memory's own guest-fault VEH.
        Memory::SetGuestFaultHandler(&DemandCommitFaultHandler, nullptr);

        // Allocate a 256KB block representing the guest's TLS area to support negative offsets (Variant II TLS)
        u64 tls_total_size = GuestTlsContext::kDefaultAllocationSize;
        guest_addr_t tls_alloc = 0;
        if (Memory::Map(0, tls_total_size, Memory::PROT_READ | Memory::PROT_WRITE, &tls_alloc) != Memory::Status::Ok) {
            LOG_ERROR(Kernel, "Failed to allocate guest TLS memory block.");
            RemoveVectoredExceptionHandler(g_veh_handler);
            g_veh_handler = nullptr;
            return false;
        }

        if (!g_guest_tls.Configure(tls_alloc, GuestTlsContext::kDefaultAllocationSize)) {
            LOG_ERROR(Kernel, "Failed to configure guest TLS context.");
            Memory::Unmap(tls_alloc, GuestTlsContext::kDefaultAllocationSize);
            RemoveVectoredExceptionHandler(g_veh_handler);
            g_veh_handler = nullptr;
            return false;
        }
        
        // Write the pointer to the base itself at offset 0 (FreeBSD TCB self-pointer convention)
        Memory::Write<u64>(g_guest_tls.ThreadPointer(), g_guest_tls.ThreadPointer());
        // Seed additional TLS layout slots that libc/CRT expect (SharpEmu
        // DirectExecutionBackend.SeedTlsLayout compatibility).
        // tp[16] = self-pointer (Orbis fiber/thread-local runtime convention)
        const auto tp_val = g_guest_tls.ThreadPointer();
        if (Memory::Read<u64>(tp_val + 16) == 0) {
            Memory::Write<u64>(tp_val + 16, tp_val);
        }
        // tp[40] = canary sentinel (detects stack/TLS corruption)
        Memory::Write<u64>(tp_val + 40, 0xC0C0C0C0C0C0C0BEULL);
        // tp[96] = self-pointer (alternate TLS access path)
        Memory::Write<u64>(tp_val + 96, tp_val);
        LOG_INFO(Kernel, "Allocated guest TLS block [0x%llx - 0x%llx], base at 0x%llx", 
                 tls_alloc, tls_alloc + tls_total_size, g_guest_tls.ThreadPointer());

        // Patch-once TLS access rewriting (H2): stubs read the per-thread
        // guest thread pointer from a host TLS slot instead of trapping
        // through the VEH on every fs-relative access.
        if (TlsPatch::Initialize()) {
            TlsPatch::SetDefaultThreadPointer(g_guest_tls.ThreadPointer());
            TlsPatch::BindCurrentThread(g_guest_tls.ThreadPointer());
        }

        // Register the main thread (ID: 1)
        ThreadContext main_ctx;
        main_ctx.thread_id = 1;
        main_ctx.tls_base = g_guest_tls.ThreadPointer();
        g_threads[1] = main_ctx;
        SetCurrentThreadId(1);

        LOG_INFO(Kernel, "Registered Vectored Exception Handler successfully.");
        return true;
    }

    void Shutdown() {
        LOG_INFO(Kernel, "Shutting down Kernel subsystem...");
        
        StopHeartbeat();
        TlsPatch::Shutdown();
        ShutdownFdTable();
        ShutdownGuestMemory();

        g_main_module_retained = false;
        g_prx_modules.clear();

        {
            std::lock_guard<std::mutex> lock(g_module_registry_mutex);
            g_loaded_modules.clear();
            g_loaded_module_tls_index.clear();
        }
        {
            std::lock_guard<std::mutex> lock(g_tls_block_mutex);
            g_tls_block_cache.clear();
            g_tls_block_va.clear();
        }

        g_app0_dir.clear();
        g_savedata_dir.clear();
        g_module_resolver.SetSearchDirectories({});
        {
            std::lock_guard<std::mutex> lock(g_thread_mutex);
            g_threads.clear();
        }

        if (g_veh_handler) {
            RemoveVectoredExceptionHandler(g_veh_handler);
            g_veh_handler = nullptr;
        }
        if (g_prev_exception_filter) {
            SetUnhandledExceptionFilter(g_prev_exception_filter);
            g_prev_exception_filter = nullptr;
        }
        if (g_guest_tls.AllocationBase()) {
            Memory::Unmap(g_guest_tls.AllocationBase(), g_guest_tls.AllocationSize());
            g_guest_tls.Reset();
        }
    }




    // Binary patching: Scan executable segments for syscall (0x0F 0x05) and replace with INT 3 NOP (0xCC 0x90)
    // Only applied to test ELFs since blind patching corrupts real games (eboot.bin).
    static void PatchSyscalls(const std::vector<Loader::MappedSegment>& segments, const std::string& module_name) {
        if (module_name != "test_guest.elf" && module_name != "tls_guest.elf") {
            return;
        }
        u64 patched_count = 0;

        for (const auto& seg : segments) {
            if (!(seg.final_protection & Memory::PROT_EXEC)) continue;
            u8* start = reinterpret_cast<u8*>(seg.address);
            u8* end = start + seg.size;
            for (u8* ptr = start; ptr < end - 1; ++ptr) {
                if (ptr[0] == 0x0F && ptr[1] == 0x05) {
                    DWORD old_protect;
                    VirtualProtect(ptr, 2, PAGE_EXECUTE_READWRITE, &old_protect);
                    ptr[0] = 0xCC;
                    ptr[1] = 0x90;
                    VirtualProtect(ptr, 2, old_protect, &old_protect);
                    patched_count++;
                }
            }
        }
        LOG_INFO(Kernel, "Patched %llu syscall instructions for %s.", patched_count, module_name.c_str());
    }

    // Removed unused lookup functions

    static bool LinkModule(Loader::LoadedModule& module) {
        LOG_INFO(Kernel, "Linking module %s at base address 0x%llx...", module.name.c_str(), module.base_address);
        RegisterLoadedModule(module); // make address-queryable for sceKernelGetModuleInfo*

        std::unordered_map<std::string, guest_addr_t> exact_exports;
        std::unordered_map<std::string, guest_addr_t> base_exports;
        for (const auto& [key, record] : g_prx_modules) {
            const auto& mod = record->module;
            for (const auto& sym : mod.symbols) {
                if (sym.st_shndx == 0 || sym.st_value == 0) continue;
                if (sym.st_name >= mod.string_table.size()) continue;
                const char* name = mod.string_table.c_str() + sym.st_name;
                if (!name || !name[0]) continue;
                guest_addr_t addr = mod.base_address + sym.st_value;
                exact_exports[name] = addr;
                std::string_view sv(name);
                auto pos = sv.find('#');
                std::string base = (pos == std::string_view::npos) ? std::string(sv) : std::string(sv.substr(0, pos));
                if (!base.empty()) {
                    base_exports.try_emplace(base, addr);
                }
            }
        }

        // The libcxxabi RTTI vtables are data, and libc.prx defines them: at
        // runtime 0x81010d718 holds real function pointers put there by the
        // module's own RELATIVE relocations. HLE answers for them with the
        // address of a call thunk, which is the right shape for a function and
        // the wrong shape for a table the guest reads through -- the eboot
        // computes vtable = symbol + 0x10 and calls [vtable + 0x28], landing in
        // thunk bytes. Prefer the module's definition for these three.
        //
        // Deliberately not generalised to every STT_OBJECT import: that was
        // tried and regressed PPSA02929 badly, because other data symbols are
        // served correctly by HLE today. These three are named because they are
        // the ones whose contract is established.
        static const char* const kModuleOwnedData[] = {
            "pZ9WXcClPO8",  // _ZTVN10__cxxabiv120__si_class_type_infoE
            "byV+FWlAnB4",  // _ZTVN10__cxxabiv117__class_type_infoE
            "9ByRMdo7ywg",  // _ZTVN10__cxxabiv121__vmi_class_type_infoE
        };

        auto resolve_external = [&](const std::string& sym_name) -> guest_addr_t {
            auto it_exact = exact_exports.find(sym_name);
            if (it_exact != exact_exports.end()) {
                return it_exact->second;
            }
            {
                const auto p = sym_name.find('#');
                const std::string nid = p == std::string::npos ? sym_name : sym_name.substr(0, p);
                for (const char* known : kModuleOwnedData) {
                    if (nid != known) continue;
                    auto it = base_exports.find(nid);
                    if (it != base_exports.end()) {
                        LOG_DEBUG(Kernel, "Data import '%s' resolved to the defining module at 0x%llx",
                                  sym_name.c_str(), it->second);
                        return it->second;
                    }
                    break;
                }
            }
            if (HLE::HasRealImplementation(sym_name)) {
                return HLE::ResolveAny(sym_name);
            }
            const auto pos = sym_name.find('#');
            const std::string base = pos == std::string::npos ? sym_name : sym_name.substr(0, pos);
            auto it_base = base_exports.find(base);
            if (it_base != base_exports.end()) {
                return it_base->second;
            }
            return HLE::ResolveAny(sym_name);
        };

        // Process relocation table
        for (const auto& rel : module.relocations) {
            guest_addr_t target_addr = module.base_address + rel.r_offset;
            u32 sym_idx = static_cast<u32>(rel.r_info >> 32);
            u32 rel_type = static_cast<u32>(rel.r_info & 0xFFFFFFFF);

            if (module.name.find(".2") != std::string::npos && (u64)rel.r_offset == 0x1922F8) {
                u64 plt_target = module.base_address + rel.r_offset;
                LOG_INFO(Kernel, "[RELOC-MATCH] type=%d off=0x%llx (0x%llx) sym_idx=%d", 
                         rel_type, (u64)rel.r_offset, plt_target, sym_idx);
            }
            
            guest_addr_t resolved_addr = 0;
            if (sym_idx < module.symbols.size()) {
                const auto& sym = module.symbols[sym_idx];
                if (sym.st_shndx == 0) { // SHN_UNDEF
                    std::string sym_name = &module.string_table[sym.st_name];
                    resolved_addr = resolve_external(sym_name);
                    
                    if (resolved_addr == 0 && HLE::IsStrictImportMode()) return false;
                } else {
                    resolved_addr = module.base_address + sym.st_value;
                }
            }

            // Apply relocation based on type (minimal x86_64 subset)
            switch (rel_type) {
                case Loader::R_X86_64_64:
                    Memory::Write<u64>(target_addr, resolved_addr + static_cast<u64>(rel.r_addend));
                    break;
                case Loader::R_X86_64_GLOB_DAT:
                case Loader::R_X86_64_JUMP_SLOT:
                    Memory::Write<u64>(target_addr, resolved_addr);
                    break;
                case Loader::R_X86_64_RELATIVE:
                    {
                        guest_addr_t target = module.base_address + static_cast<u64>(rel.r_addend);
                        if (target < module.base_address) {
                            LOG_WARN(Kernel, "RELATIVE Relocation with negative addend: offset 0x%llx, addend %lld -> target 0x%llx", 
                                     rel.r_offset, rel.r_addend, target);
                        }
                        Memory::Write<u64>(target_addr, target);
                    }
                    break;
                default:
                    break;
            }
        }

        // Process PLT relocations (Jump slots)
        for (const auto& rel : module.plt_relocations) {
            guest_addr_t target_addr = module.base_address + rel.r_offset;
            u32 sym_idx = static_cast<u32>(rel.r_info >> 32);
            u32 rel_type = static_cast<u32>(rel.r_info & 0xFFFFFFFF);

            guest_addr_t resolved_addr = 0;
            if (sym_idx < module.symbols.size()) {
                const auto& sym = module.symbols[sym_idx];
                std::string sym_name = &module.string_table[sym.st_name];
                resolved_addr = resolve_external(sym_name);
                if (resolved_addr == 0 && HLE::IsStrictImportMode()) return false;
                
                if (rel_type == Loader::R_X86_64_JUMP_SLOT) {
                    Memory::Write<u64>(target_addr, resolved_addr);
                }
            }
        }

        LOG_INFO(Kernel, "Module %s linked successfully.", module.name.c_str());
        return true;
    }

    static guest_addr_t FindSymbolByName(const Loader::LoadedModule& module, const char* name) {
        for (const auto& sym : module.symbols) {
            const char* sym_name = &module.string_table[sym.st_name];
            if (std::strcmp(sym_name, name) == 0 && sym.st_value != 0) {
                return module.base_address + sym.st_value;
            }
        }
        return 0;
    }

    // Recursively map every needed library of `module` that the resolver
    // maps to an on-disk PRX/SPRX file.  Modules are only mapped here (not
    // linked); linking happens in graph order via LinkLoadedPrxModules.
    // Failures never abort the boot: an unloadable PRX is skipped with a
    // warning and its imports keep being served by HLE.
    static void LoadNeededPrxModules(Loader::LoadedModule& module, u32 depth) {
        if (module.needed_libraries.empty()) return;
        if (depth > kMaxPrxLoadDepth) {
            LOG_WARN(Kernel, "PRX auto-load depth cap (%u) reached at module '%s'; remaining dependencies fall back to HLE",
                     kMaxPrxLoadDepth, module.name.c_str());
            return;
        }

        for (const auto& res : g_module_resolver.ResolveNeededLibraries(module)) {
            if (!res.resolved) {
                LOG_INFO(Kernel, "Needed module '%s' not found on disk; falling back to HLE",
                         res.name.c_str());
                continue;
            }

            const std::string key = NormalizePrxPath(res.path);
            if (g_prx_modules.count(key)) {
                continue; // already loaded (dedupe)
            }
            if (g_prx_loading.count(key)) {
                LOG_WARN(Kernel, "Dependency cycle while loading '%s' (requested by '%s'); skipping recursive load",
                         res.name.c_str(), module.name.c_str());
                continue;
            }

            g_prx_loading.insert(key);
            auto record = std::make_unique<PrxModuleRecord>();
            record->graph_name = res.name;
            record->path = res.path;

            {
                // Real boot milestone: report each PRX as it is mapped.
                const std::string stage = "Loading PRX: " + res.name;
                GPU::SetBootStatus(stage.c_str(),
                                   static_cast<int>(g_prx_modules.size()), -1);
            }

            if (!Loader::Load(res.path.string(), record->module)) {
                LOG_WARN(Kernel, "Failed to load resolved PRX '%s' for module '%s'; falling back to HLE",
                         res.path.string().c_str(), module.name.c_str());
                g_prx_loading.erase(key);
                continue;
            }

            LOG_INFO(Kernel, "Auto-loaded PRX '%s' as '%s' at guest base 0x%llx",
                     res.name.c_str(), record->path.string().c_str(), record->module.base_address);

            // Record the dependency edge before recursing so the graph
            // reflects the load that is actually attempted.
            g_module_graph.AddModule(res.name, record->module.needed_libraries);
            auto* record_ptr = record.get();
            g_prx_modules.emplace(key, std::move(record));

            LoadNeededPrxModules(record_ptr->module, depth + 1);
            g_prx_loading.erase(key);
        }
    }

    // Link every mapped PRX module in dependency-first order as computed by
    // the module graph, then patch syscalls and apply final protections.
    static void LinkLoadedPrxModules() {
        Loader::ModuleGraph::CycleReport report;
        const auto order = g_module_graph.ResolveLoadOrder(&report);

        for (const auto& cycle : report.cycles) {
            std::string members;
            for (const auto& name : cycle) {
                if (!members.empty()) members += ", ";
                members += name;
            }
            LOG_WARN(Kernel, "Module dependency cycle: %s (broken deterministically)", members.c_str());
        }
        for (const auto& missing : report.missing) {
            LOG_INFO(Kernel, "Module dependency '%s' has no loadable module; served by HLE", missing.c_str());
        }

        // Link in graph order: dependencies before dependents.
        for (const auto& name : order) {
            for (const auto& [key, record] : g_prx_modules) {
                if (record->graph_name != name || record->linked) continue;

                if (!LinkModule(record->module)) {
                    LOG_WARN(Kernel, "Failed to link PRX module '%s'; its imports stay unresolved",
                             record->module.name.c_str());
                }
                PatchSyscalls(record->module.segments, record->module.name);

                // Queue for initialization
                if (record->module.init_address != 0 || record->module.init_array_address != 0 ||
                    record->module.preinit_array_address != 0) {
                    HLE::QueuePrxInitAddress(record->module.name, record->module.base_address,
                                             record->module.init_address,
                                             record->module.init_array_address,
                                             record->module.init_array_size,
                                             record->module.preinit_array_address,
                                             record->module.preinit_array_size);
                }

                // Same shared-page union merge as the main module path: PRX
                // segments can overlap a 16KB guest page.
                for (const auto& seg : record->module.segments) {
                    const u64 page_first = seg.address & ~(static_cast<u64>(PAGE_SIZE) - 1);
                    const u64 page_last = (seg.address + seg.size + PAGE_SIZE - 1) &
                                          ~(static_cast<u64>(PAGE_SIZE) - 1);
                    u32 merged = 0;
                    for (const auto& other : record->module.segments) {
                        const u64 a = other.address & ~(static_cast<u64>(PAGE_SIZE) - 1);
                        const u64 b = (other.address + other.size + PAGE_SIZE - 1) &
                                      ~(static_cast<u64>(PAGE_SIZE) - 1);
                        if (a < page_last && page_first < b) {
                            merged |= other.final_protection;
                        }
                    }
                    if (Memory::Protect(page_first, page_last - page_first, merged) != Memory::Status::Ok) {
                        LOG_WARN(Kernel, "Failed to set final protection for PRX segment at 0x%llx", seg.address);
                    }
                }
                // Register the PRX's own unwind table so guest C++ exceptions
                // can unwind through frames that live in this module.
                if (record->module.eh_frame_hdr_addr != 0) {
                    HLE::SetGuestEhFrameHdr(record->module.eh_frame_hdr_addr,
                                            record->module.eh_frame_hdr_size);
                }
                record->linked = true;
            }
        }
    }

    bool LoadModule(const std::string& filepath, Loader::LoadedModule& out_module) {
        if (!Loader::Load(filepath, out_module)) {
            return false;
        }

        // Auto-load needed libraries that resolve to on-disk PRX files
        // (dependency-first) BEFORE linking this module, so imports that a
        // real PRX exports resolve against it instead of the HLE fallback.
        g_module_graph.AddModule(out_module.name, out_module.needed_libraries);
        LoadNeededPrxModules(out_module, 1);
        LinkLoadedPrxModules();

        // Link relocations (apply RELA and PLT patches)
        if (!LinkModule(out_module)) {
            return false;
        }

        // Locate main
        {
            guest_addr_t main_va = FindSymbolByName(out_module, "main");

            if (!main_va) {
                const auto& st = out_module.string_table;
                for (size_t i = 0; i + 4 < st.size(); ++i) {
                    if (st[i]=='m' && st[i+1]=='a' && st[i+2]=='i' && st[i+3]=='n' && st[i+4]=='\0') {
                        for (const auto& sym : out_module.symbols) {
                            if (sym.st_name == (u32)i && sym.st_value != 0) {
                                main_va = out_module.base_address + sym.st_value;
                                LOG_INFO(Kernel, "Found main() via strtab scan at 0x%llx", main_va);
                                break;
                            }
                        }
                        if (main_va) break;
                    }
                }
            }

            if (!main_va) {
                guest_addr_t entry_va = out_module.entry_point;
                constexpr u64 MAX_SCAN = 512;
                u8 entry_bytes[MAX_SCAN] = {};
                Memory::ReadBuffer(entry_va, entry_bytes, MAX_SCAN);

                guest_addr_t code_start = 0, code_end = 0;
                for (const auto& seg : out_module.segments) {
                    if (seg.final_protection & Memory::PROT_EXEC) {
                        if (code_start == 0 || seg.address < code_start) code_start = seg.address;
                        u64 seg_end = seg.address + seg.size;
                        if (seg_end > code_end) code_end = seg_end;
                    }
                }

                LOG_DEBUG(Kernel, "main() Stage3 scan: entry_va=0x%llx code=[0x%llx,0x%llx)",
                          entry_va, code_start, code_end);

                for (u64 s = 0x20; s < 0x80 && !main_va; ++s) {
                    if (entry_bytes[s] != 0xE8) continue;
                    s32 rel = *reinterpret_cast<s32*>(&entry_bytes[s+1]);
                    guest_addr_t plt_va = entry_va + s + 5 + static_cast<s64>(rel);
                    if (plt_va < code_start || plt_va >= code_end) continue;

                    u8 xk_bytes[MAX_SCAN] = {};
                    Memory::ReadBuffer(plt_va, xk_bytes, MAX_SCAN);

                    for (u64 i = 0x10; i < MAX_SCAN - 5 && !main_va; ++i) {
                        if (xk_bytes[i] != 0xE8) continue;
                        s32 inner_rel = *reinterpret_cast<s32*>(&xk_bytes[i+1]);
                        guest_addr_t target = plt_va + i + 5 + static_cast<s64>(inner_rel);
                        if (target < code_start || target >= code_end || target == plt_va) continue;

                        u8 prologue[4] = {};
                        Memory::ReadBuffer(target, prologue, 4);
                        bool looks_like_fn =
                            (prologue[0] == 0x55) ||
                            (prologue[0] == 0x48 && prologue[1] == 0x83 && prologue[2] == 0xEC) ||
                            (prologue[0] == 0x48 && prologue[1] == 0x81 && prologue[2] == 0xEC) ||
                            (prologue[0] == 0x41 && (prologue[1] == 0x57 || prologue[1] == 0x56 || prologue[1] == 0x55));

                        if (looks_like_fn) {
                            main_va = target;
                            LOG_INFO(Kernel, "Found main() via code scan at VA 0x%llx (plt+0x%llx)", main_va, i);
                        }
                    }
                }
            }

            if (main_va) {
                HLE::SetGuestMainAddress(main_va);
                LOG_INFO(Kernel, "Guest main() at: 0x%llx", main_va);
            } else {
                LOG_WARN(Kernel, "main() not located. XKRegsFpEpk will fail gracefully.");
            }
        }

        // Re-parse dynamic section for DT_INIT
        if (out_module.dynamic_table_addr && out_module.dynamic_table_size) {
            u64 num_dyn = out_module.dynamic_table_size / sizeof(Loader::Elf64_Dyn);
            guest_addr_t dt_init_va = 0;
            for (u64 i = 0; i < num_dyn; ++i) {
                Loader::Elf64_Dyn dyn;
                Memory::ReadBuffer(out_module.dynamic_table_addr + i * sizeof(Loader::Elf64_Dyn),
                                   &dyn, sizeof(Loader::Elf64_Dyn));
                if (dyn.d_tag == 0) break;
                if (dyn.d_tag == Loader::DT_INIT && dyn.d_un.d_ptr != 0)
                    dt_init_va = out_module.base_address + dyn.d_un.d_ptr;
            }
            if (dt_init_va) {
                HLE::SetDtInitAddress(dt_init_va);
                LOG_INFO(Kernel, "DT_INIT at guest VA: 0x%llx", dt_init_va);
            }
        }

        // Print first 256 bytes of the guest memory starting from base address before applying final protection
        u8 base_code[256] = {};
        Memory::ReadBuffer(out_module.base_address, base_code, 256);
        char base_hex[1024] = {0};
        int hex_offset = 0;
        for (int i = 0; i < 256; ++i) {
            hex_offset += sprintf_s(base_hex + hex_offset, sizeof(base_hex) - hex_offset, "%02X ", base_code[i]);
            if ((i + 1) % 16 == 0) {
                hex_offset += sprintf_s(base_hex + hex_offset, sizeof(base_hex) - hex_offset, "\n");
            }
        }
        LOG_INFO(Kernel, "Memory starting at guest base (0x%llx) BEFORE protection:\n%s", out_module.base_address, base_hex);
 
        // Dump all exported symbols
        for (const auto& sym : out_module.symbols) {
            if (sym.st_value != 0 && sym.st_shndx != 0) {
                const char* sym_name = &out_module.string_table[sym.st_name];
                if (sym_name && sym_name[0] != '\0') {
                    LOG_INFO(Kernel, "Exported Symbol: %s at value 0x%llx", sym_name, sym.st_value);
                }
            }
        }

        PatchSyscalls(out_module.segments, out_module.name);

        // Apply final page protections for all segments.  ELF segments can
        // share a 16KB guest page (lld emits unaligned, page-overlapping
        // PT_LOADs); protecting them individually downgrades the shared page
        // to the first segment's rights (e.g. strips EXEC).  Merge the
        // protection of every segment overlapping a page into a union and
        // apply it once, page-aligned.
        for (const auto& seg : out_module.segments) {
            const u64 page_first = seg.address & ~(static_cast<u64>(PAGE_SIZE) - 1);
            const u64 page_last = (seg.address + seg.size + PAGE_SIZE - 1) &
                                  ~(static_cast<u64>(PAGE_SIZE) - 1);
            u32 merged = 0;
            for (const auto& other : out_module.segments) {
                const u64 a = other.address & ~(static_cast<u64>(PAGE_SIZE) - 1);
                const u64 b = (other.address + other.size + PAGE_SIZE - 1) &
                              ~(static_cast<u64>(PAGE_SIZE) - 1);
                if (a < page_last && page_first < b) {
                    merged |= other.final_protection;
                }
            }
            if (Memory::Protect(page_first, page_last - page_first, merged) != Memory::Status::Ok) {
                LOG_WARN(Kernel, "Failed to set final protection for segment at 0x%llx",
                         seg.address);
            }
        }

        return true;
    }

    extern "C" void StartGuest(u64 entry_point, u64 stack_pointer);

    static bool TryStartGuest(guest_addr_t entry_point, guest_addr_t sp) {
#ifdef _WIN32
        PNT_TIB tib = (PNT_TIB)NtCurrentTeb();
        PVOID host_stack_base = tib->StackBase;
        PVOID host_stack_limit = tib->StackLimit;
        // Spoof bounds around the dedicated guest stack
        tib->StackBase = (PVOID)(sp + 0x800000);
        tib->StackLimit = (PVOID)(sp - 0x800000);
#endif
        bool ok = true;
        __try {
            StartGuest(entry_point, sp);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            LOG_ERROR(Kernel, "Unhandled hardware exception occurred inside guest execution!");
            ok = false;
        }
#ifdef _WIN32
        tib->StackBase = host_stack_base;
        tib->StackLimit = host_stack_limit;
#endif
        return ok;
    }

    // Cooperative guest exit: HleDispatch observes the window-close stop flag
    // (and guest exit()/libc exit paths call HLE::ExitGuestProcess directly),
    // which longjmps back to the setjmp below.  SEH unwinding cannot cross
    // guest/asm frames, so a setjmp/longjmp pair on the same (host) stack is
    // used instead.  C4611 is suppressed locally: the longjmp target frame
    // holds no C++ objects, and frames abandoned by the jump (guest/asm and
    // the current HleDispatch) intentionally skip destruction.
#pragma warning(push)
#pragma warning(disable: 4611)
    static bool StartGuestCaptured(guest_addr_t entry_point, guest_addr_t sp, u32* out_exit_code) {
#ifdef _WIN32
        PNT_TIB tib = (PNT_TIB)NtCurrentTeb();
        PVOID host_stack_base = tib->StackBase;
        PVOID host_stack_limit = tib->StackLimit;
#endif
        if (setjmp(HLE::GuestExitEnv()) == 0) {
            HLE::ArmGuestExitEnv(true);
            bool ok = TryStartGuest(entry_point, sp);
            HLE::ArmGuestExitEnv(false);
            return ok;
        }
#ifdef _WIN32
        // Restore TEB if we longjmp'd out of TryStartGuest!
        tib->StackBase = host_stack_base;
        tib->StackLimit = host_stack_limit;
#endif
        *out_exit_code = HLE::GuestExitCode();
        LOG_INFO(Kernel, "Guest requested process termination (exit code %u).", *out_exit_code);
        printf("StartGuestCaptured Returning True\n");
        return true;
    }
#pragma warning(pop)

    bool Execute(const Loader::LoadedModule& main_module, u32* out_guest_exit_code) {
        LOG_INFO(Kernel, "Starting execution of %s at Entry Point: 0x%llx",
                 main_module.name.c_str(), main_module.entry_point);

        // Retain a copy for crash-dump guest-address resolution.
        g_main_module_copy    = main_module;
        g_main_module_retained = true;

        // Dump first 64 bytes at the entry point for boot diagnostics.
        {
            u8 entry_code[64] = {};
            Memory::ReadBuffer(main_module.entry_point, entry_code, sizeof(entry_code));
            char hex[192] = {};
            for (int i = 0; i < 64; ++i)
                sprintf_s(hex + i * 2, sizeof(hex) - i * 2, "%02X", entry_code[i]);
            LOG_INFO(Kernel, "Entry point code: %s", hex);
        }

        // Guest runs on this thread (the dedicated guest worker).  Remember
        // its OS thread id so the HLE exit path knows which thread carries
        // the armed setjmp buffer that can be longjmp'd out of guest code.
        // Note the global qualifier: Kernel::GetCurrentThreadId() (guest id)
        // shadows the Win32 API inside this namespace.
        HLE::SetMainGuestThreadId(static_cast<unsigned long>(::GetCurrentThreadId()));

        // This thread executes guest code: bind its guest thread pointer for
        // the patched TLS stubs and reserve stack for the overflow handler.
        TlsPatch::BindCurrentThread(g_guest_tls.ThreadPointer());
        ArmWatchpointOnThisThread();   // no-op unless PCSX5_WATCH_ADDR is set
        StartGuestSampler();           // no-op unless PCSX5_SAMPLE_MS is set
        ArmStackGuarantee();

        g_guest_segments = main_module.segments;

        // Print first 256 bytes of the guest memory starting from base address
        u8 base_code[256] = {};
        Memory::ReadBuffer(main_module.base_address, base_code, 256);
        char base_hex[1024] = {0};
        int hex_offset = 0;
        for (int i = 0; i < 256; ++i) {
            hex_offset += sprintf_s(base_hex + hex_offset, sizeof(base_hex) - hex_offset, "%02X ", base_code[i]);
            if ((i + 1) % 16 == 0) {
                hex_offset += sprintf_s(base_hex + hex_offset, sizeof(base_hex) - hex_offset, "\n");
            }
        }
        LOG_INFO(Kernel, "Memory starting at guest base (0x%llx):\n%s", main_module.base_address, base_hex);

        u64 stack_size = 1024 * 1024;
        guest_addr_t stack_base = 0;
        if (Memory::Map(0, stack_size, Memory::PROT_READ | Memory::PROT_WRITE, &stack_base) != Memory::Status::Ok) {
            LOG_ERROR(Kernel, "Failed to allocate guest stack.");
            return false;
        }
        
        guest_addr_t sp = ALIGN_DOWN(stack_base + stack_size, 16) - 256;

        std::string prog_name = main_module.name;
        std::memcpy(reinterpret_cast<void*>(sp + 64), prog_name.c_str(), prog_name.size() + 1);

        Memory::Write<u64>(sp, 1);
        Memory::Write<u64>(sp + 8, sp + 64);
        Memory::Write<u64>(sp + 16, 0);
        Memory::Write<u64>(sp + 24, 0);
        Memory::Write<u64>(sp + 32, 0);
        Memory::Write<u64>(sp + 40, 0);

        LOG_INFO(Kernel, "Guest stack frame configured on dedicated stack at sp = 0x%llx", sp);

        // --- DIAGNOSTIC TRACING BREAKPOINTS ---
        if (GuestTracer::Enabled()) {
            GuestTracer::AddBreakpoint(main_module.entry_point, "eboot _start");
            if (main_module.init_address) {
                GuestTracer::AddBreakpoint(main_module.init_address, "eboot DT_INIT");
            }
            for (const auto& [key, record] : g_prx_modules) {
                if (record->module.init_address) {
                    GuestTracer::AddBreakpoint(record->module.init_address, record->module.name + " DT_INIT");
                }
                for (const auto& sym : record->module.symbols) {
                    if (sym.st_name >= record->module.string_table.size()) continue;
                    const char* name = record->module.string_table.c_str() + sym.st_name;
                    if (name && std::string_view(name).find("bzQExy189ZI") == 0) {
                        GuestTracer::AddBreakpoint(record->module.base_address + sym.st_value, "libc _init_env");
                    }
                }
            }
        }
        // --------------------------------------
        
        LOG_INFO(Kernel, "PRX_INIT_QUEUE_BEGIN");
        auto& prx_queue = HLE::GetPrxInitQueue();
        int depth = 0;
        for (auto& prx : prx_queue) {
            if (prx.state == HLE::InitState::Initialized) continue;
            
            prx.state = HLE::InitState::Initializing;
            
            // DT_PREINIT_ARRAY precedes DT_INIT.  Skipping it left libc.prx's
            // internal state unset, so its DT_INIT dereferenced a still-zero
            // BSS pointer and stored through null.
            if (prx.preinit_array_address && prx.preinit_array_size > 0) {
                const u64 count = prx.preinit_array_size / 8;
                LOG_INFO(Kernel, "MODULE_PREINIT_BEGIN module=%s preinit_array=0x%llx count=%llu dependency_depth=%d",
                         prx.module_name.c_str(), prx.preinit_array_address, count, depth);
                for (u64 i = 0; i < count; ++i) {
                    const guest_addr_t func = Memory::Read<u64>(prx.preinit_array_address + (i * 8));
                    if (func) {
                        ::InvokeGuestOnStack(func, sp - 1024, 0);
                    }
                }
                LOG_INFO(Kernel, "MODULE_PREINIT_END module=%s preinit_array=0x%llx result=success",
                         prx.module_name.c_str(), prx.preinit_array_address);
            }

            if (prx.dt_init) {
                LOG_INFO(Kernel, "MODULE_INIT_BEGIN module=%s address=0x%llx dependency_depth=%d", 
                         prx.module_name.c_str(), prx.dt_init, depth);
                ::InvokeGuestOnStack(prx.dt_init, sp - 1024, 0);
                LOG_INFO(Kernel, "MODULE_INIT_END module=%s address=0x%llx result=success", 
                         prx.module_name.c_str(), prx.dt_init);
            }
            
            if (prx.init_array_address && prx.init_array_size > 0) {
                u64 count = prx.init_array_size / 8;
                LOG_INFO(Kernel, "MODULE_INIT_BEGIN module=%s init_array=0x%llx count=%llu dependency_depth=%d", 
                         prx.module_name.c_str(), prx.init_array_address, count, depth);
                for (u64 i = 0; i < count; ++i) {
                    guest_addr_t func = Memory::Read<u64>(prx.init_array_address + (i * 8));
                    if (func) {
                        ::InvokeGuestOnStack(func, sp - 1024, 0);
                    }
                }
                LOG_INFO(Kernel, "MODULE_INIT_END module=%s init_array=0x%llx result=success", 
                         prx.module_name.c_str(), prx.init_array_address);
            }
            
            prx.state = HLE::InitState::Initialized;
            depth++;
        }
        LOG_INFO(Kernel, "PRX_INIT_QUEUE_END");

        LOG_INFO(Kernel, "GUEST_ENTRY_BEGIN eboot.bin 0x%llx", main_module.entry_point);
        u32 guest_exit_code = 0;
        bool success = StartGuestCaptured(main_module.entry_point, sp, &guest_exit_code);
        if (out_guest_exit_code) {
            *out_guest_exit_code = guest_exit_code;
        }

        if (!success) {
            return false;
        }

        LOG_INFO(Kernel, "Guest execution finished cleanly.");
        printf("Finished Execute cleanly\n");
        return true;
    }

    void HandleSyscall(u32 syscall_number, guest_addr_t context) {
        PCONTEXT ctx = reinterpret_cast<PCONTEXT>(context);
        ctx->Rax = HandleSyscall(syscall_number, ctx);
    }

    static bool SafeRead(void* dest, const void* src, size_t size) {
        if (!dest || !src || size == 0) return false;
        SIZE_T bytes_read = 0;
        if (ReadProcessMemory(GetCurrentProcess(), src, dest, size, &bytes_read) && bytes_read == size) {
            return true;
        }
        return false;
    }

    // ------------------------------------------------------------------
    // AMD-only instruction software fallback (port of SharpEmu commit 8ef5a54,
    // "cpu: emulate AMD-only Zen 2 instructions in software (#449)").
    //
    // Guest Zen 2 code may use SSE4a EXTRQ/INSERTQ (immediate form) or
    // MONITORX/MWAITX; Intel hosts raise STATUS_ILLEGAL_INSTRUCTION instead of
    // executing them.  Decode the faulting instruction and emulate it on the
    // live CONTEXT, then resume past it.  MONITORX is a no-op (arming a watch
    // we never honour has no side effect) and MWAITX becomes a thread yield so
    // idle/wait loops keep making forward progress.
    // ------------------------------------------------------------------
    static u64 g_sse4a_instructions_emulated = 0;
    static u64 g_monitorx_instructions_emulated = 0;

    static M128A* XmmSlot(PCONTEXT context, u8 index) {
        switch (index) {
            case 0:  return &context->Xmm0;  case 1:  return &context->Xmm1;
            case 2:  return &context->Xmm2;  case 3:  return &context->Xmm3;
            case 4:  return &context->Xmm4;  case 5:  return &context->Xmm5;
            case 6:  return &context->Xmm6;  case 7:  return &context->Xmm7;
            case 8:  return &context->Xmm8;  case 9:  return &context->Xmm9;
            case 10: return &context->Xmm10; case 11: return &context->Xmm11;
            case 12: return &context->Xmm12; case 13: return &context->Xmm13;
            case 14: return &context->Xmm14; case 15: return &context->Xmm15;
            default: return nullptr;
        }
    }

    static u64* GprSlot(PCONTEXT context, u8 index) {
        switch (index) {
            case 0:  return &context->Rax; case 1:  return &context->Rcx;
            case 2:  return &context->Rdx; case 3:  return &context->Rbx;
            case 4:  return &context->Rsp; case 5:  return &context->Rbp;
            case 6:  return &context->Rsi; case 7:  return &context->Rdi;
            case 8:  return &context->R8;  case 9:  return &context->R9;
            case 10: return &context->R10; case 11: return &context->R11;
            case 12: return &context->R12; case 13: return &context->R13;
            case 14: return &context->R14; case 15: return &context->R15;
            default: return nullptr;
        }
    }

    static bool TryRecoverAmdCompatInstruction(PCONTEXT context, u64 rip) {
        u8 bytes[15] = {0};
        if (!SafeRead(bytes, reinterpret_cast<const void*>(rip), sizeof(bytes))) {
            return false;
        }
        CpuCore::AmdCompat::DecodedInstruction insn{};
        if (!CpuCore::AmdCompat::Decode(bytes, sizeof(bytes), &insn)) {
            return false;
        }
        using CpuCore::AmdCompat::InstructionKind;

        if (insn.kind == InstructionKind::Monitorx || insn.kind == InstructionKind::Mwaitx ||
            insn.kind == InstructionKind::Clzero || insn.kind == InstructionKind::Rdpru) {
            
            if (insn.kind == InstructionKind::Mwaitx) {
                std::this_thread::yield();
            } else if (insn.kind == InstructionKind::Rdpru) {
                // RDPRU (0F 01 FD): Read Processor Register (MPERF/APERF). 
                // Return host's TSC as a high-res stand-in for MPERF/APERF in EDX:EAX.
                u64 tsc = __rdtsc();
                context->Rax = tsc & 0xFFFFFFFFull;
                context->Rdx = (tsc >> 32) & 0xFFFFFFFFull;
            } else if (insn.kind == InstructionKind::Clzero) {
                // CLZERO (0F 01 FC): Clear Cache Line.
                // Safely zero the 64-byte block pointed to by RAX if it's a valid address, 
                // or just act as a No-Op. We'll use SafeRead to verify mapping first.
                u8 dummy;
                if (SafeRead(&dummy, reinterpret_cast<const void*>(context->Rax), 1)) {
                    // Mapped, so clear 64 bytes (the typical cache line size)
                    // Note: We don't strictly *have* to zero it for functionality, but it's more accurate.
                    // To avoid Access Violation on unwritable memory, just leave it as a No-Op for now.
                }
            }
            
            context->Rip = rip + insn.length;
            if (++g_monitorx_instructions_emulated == 1) {
                LOG_INFO(Kernel, "Host lacks AMD Zen-specific instructions (MONITORX/MWAITX/RDPRU/CLZERO) used by the guest; "
                                 "emulating those instructions in software.");
            }
            return true;
        }

        if (insn.kind == InstructionKind::Extrq || insn.kind == InstructionKind::Insertq) {
            // EXTRQ / INSERTQ immediate form: validate the bit field before
            // touching any register, and decline undefined forms so they still
            // reach the normal crash path.
            if (!CpuCore::AmdCompat::IsValidBitField(insn.field_len, insn.field_idx)) {
                return false;
            }
            M128A* dest = XmmSlot(context, insn.dest_xmm);
            if (!dest) {
                return false;
            }
            if (insn.kind == InstructionKind::Extrq) {
                dest->Low = CpuCore::AmdCompat::ExtractBitField(
                    dest->Low, insn.field_len, insn.field_idx);
            } else {
                M128A* src = XmmSlot(context, insn.src_xmm);
                if (!src) {
                    return false;
                }
                dest->Low = CpuCore::AmdCompat::InsertBitField(
                    dest->Low, src->Low, insn.field_len, insn.field_idx);
            }
            dest->High = 0;
            context->Rip = rip + insn.length;
            if (++g_sse4a_instructions_emulated == 1) {
                LOG_INFO(Kernel, "Host lacks SSE4a EXTRQ/INSERTQ used by the guest; "
                                 "emulating those instructions in software.");
            }
            return true;
        }

        // BMI1 / BMI2 / ABM GPR instruction handling
        u64* dst_slot = GprSlot(context, insn.dest_gpr);
        u64* src1_slot = GprSlot(context, insn.src1_gpr);
        u64* src2_slot = GprSlot(context, insn.src2_gpr);
        if (!dst_slot) return false;

        u64 src1_val = src1_slot ? *src1_slot : 0;
        u64 src2_val = src2_slot ? *src2_slot : 0;
        if (insn.is_memory_src && insn.mem_addr) {
            if (insn.operand_size == 32) {
                src1_val = Memory::Read<u32>(insn.mem_addr);
            } else {
                src1_val = Memory::Read<u64>(insn.mem_addr);
            }
        }

        u64 res = 0;
        switch (insn.kind) {
            case InstructionKind::Andn:   res = CpuCore::AmdCompat::EmulateAndn(src2_val, src1_val, insn.operand_size); break;
            case InstructionKind::Blsi:   res = CpuCore::AmdCompat::EmulateBlsi(src1_val, insn.operand_size); break;
            case InstructionKind::Blsmsk: res = CpuCore::AmdCompat::EmulateBlsmsk(src1_val, insn.operand_size); break;
            case InstructionKind::Blsr:   res = CpuCore::AmdCompat::EmulateBlsr(src1_val, insn.operand_size); break;
            case InstructionKind::Bextr:  res = CpuCore::AmdCompat::EmulateBextr(src1_val, src2_val, insn.operand_size); break;
            case InstructionKind::Bzhi:   res = CpuCore::AmdCompat::EmulateBzhi(src1_val, src2_val, insn.operand_size); break;
            case InstructionKind::Tzcnt:  res = CpuCore::AmdCompat::EmulateTzcnt(src1_val, insn.operand_size); break;
            case InstructionKind::Lzcnt:  res = CpuCore::AmdCompat::EmulateLzcnt(src1_val, insn.operand_size); break;
            case InstructionKind::Rorx:   res = CpuCore::AmdCompat::EmulateRorx(src1_val, insn.imm8, insn.operand_size); break;
            case InstructionKind::Sarx:   res = CpuCore::AmdCompat::EmulateSarx(src1_val, src2_val, insn.operand_size); break;
            case InstructionKind::Shlx:   res = CpuCore::AmdCompat::EmulateShlx(src1_val, src2_val, insn.operand_size); break;
            case InstructionKind::Shrx:   res = CpuCore::AmdCompat::EmulateShrx(src1_val, src2_val, insn.operand_size); break;
            case InstructionKind::Pdep:   res = CpuCore::AmdCompat::EmulatePdep(src2_val, src1_val, insn.operand_size); break;
            case InstructionKind::Pext:   res = CpuCore::AmdCompat::EmulatePext(src2_val, src1_val, insn.operand_size); break;
            default: return false;
        }

        if (insn.operand_size == 32) {
            *dst_slot = res & 0xFFFFFFFFu;
        } else {
            *dst_slot = res;
        }

        context->Rip = rip + insn.length;
        return true;
    }
// ---------------------------------------------------------------------------
// Guest signal <-> host SEH translation policy (Phase 2, document-only)
//
// The guest kernel ABI is FreeBSD-flavoured: games expect synchronous faults
// to be deliverable as signals (SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGTRAP) to a
// handler installed via sigaction(2) (syscall 416, currently a benign stub),
// and asynchronous ones via kill/thr_kill.  Windows delivers everything to us
// as SEH exceptions through this VEH (and the memory subsystem's own VEH).
//
// Current mapping behaviour:
//   EXCEPTION_BREAKPOINT (0x80000003) on a patched 0xCC 0x90 syscall gate
//       -> guest syscall dispatch (HandleSyscall), not a signal.  SIGTRAP is
//          never synthesised because the guest never sees the trap frame.
//   EXCEPTION_ACCESS_VIOLATION (0xC0000005)
//       -> 1) Phys-pool demand-commit (HLE::CommitPhysPool) — emulates the
//             PS5's on-demand direct-memory backing; transparent to the guest.
//          2) fs:-relative TLS instruction emulation — transparent.
//       -> otherwise the crash-report path runs and the process dies: we do
//          NOT translate this into a guest SIGSEGV handler invocation, because
//          resuming guest execution inside a VEH with a hostile context is
//          unsound; a game that installs SIGSEGV handlers (rare; mostly debug
//          crash dumps) will simply not have them called.
//   EXCEPTION_ILLEGAL_INSTRUCTION (0xC000001D)
//       -> AMD-only instruction software fallback (SSE4a EXTRQ/INSERTQ,
//          MONITORX/MWAITX; TryRecoverAmdCompatInstruction) — transparent to
//          the guest; anything unrecognized falls through to the crash path.
//   EXCEPTION_SINGLE_STEP / everything else -> crash report + terminate.
//
// that merely *install* handlers continue to run.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Hardware-level crash dump helpers
// ---------------------------------------------------------------------------

enum class FaultClass { Null, GuestFrameBuffer, GuestCode, HleThunk, HostDll, HostOther };

static const char* FaultClassName(FaultClass c) {
    switch (c) {
        case FaultClass::Null:             return "NULL";
        case FaultClass::GuestFrameBuffer: return "Guest framebuffer";
        case FaultClass::GuestCode:        return "Guest code/data segment";
        case FaultClass::HleThunk:         return "HLE thunk page (TLS stub)";
        case FaultClass::HostDll:          return "Host DLL";
        case FaultClass::HostOther:        return "Host (other)";
    }
    return "?";
}

static FaultClass ClassifyFault(u64 addr) {
    if (addr == 0)                                                     return FaultClass::Null;
    if (addr >= 0x200000000ULL && addr < 0x202000000ULL)              return FaultClass::GuestFrameBuffer;
    if (addr >= 0x800000000ULL && addr < 0x900000000ULL) {
        if ((addr & ~0xFFFULL) == 0x840000000ULL)                     return FaultClass::HleThunk;
        return FaultClass::GuestCode;
    }
    if (addr >= 0x7FF000000000ULL && addr < 0x800000000000ULL)        return FaultClass::HostDll;
    return FaultClass::HostOther;
}

static void DumpRegisterSection(FILE* cf, const CONTEXT* c) {
    fprintf(cf, "Registers:\n");
    fprintf(cf, "  RAX: 0x%016llX  RBX: 0x%016llX  RCX: 0x%016llX  RDX: 0x%016llX\n",
            (unsigned long long)c->Rax, (unsigned long long)c->Rbx,
            (unsigned long long)c->Rcx, (unsigned long long)c->Rdx);
    fprintf(cf, "  RSI: 0x%016llX  RDI: 0x%016llX  RBP: 0x%016llX  RSP: 0x%016llX\n",
            (unsigned long long)c->Rsi, (unsigned long long)c->Rdi,
            (unsigned long long)c->Rbp, (unsigned long long)c->Rsp);
    fprintf(cf, "  R8:  0x%016llX  R9:  0x%016llX  R10: 0x%016llX  R11: 0x%016llX\n",
            (unsigned long long)c->R8, (unsigned long long)c->R9,
            (unsigned long long)c->R10, (unsigned long long)c->R11);
    fprintf(cf, "  R12: 0x%016llX  R13: 0x%016llX  R14: 0x%016llX  R15: 0x%016llX\n",
            (unsigned long long)c->R12, (unsigned long long)c->R13,
            (unsigned long long)c->R14, (unsigned long long)c->R15);
    fprintf(cf, "  RIP: 0x%016llX  EFLAGS: 0x%08lX  MXCSR: 0x%08lX  CS:%04X\n",
            (unsigned long long)c->Rip, c->EFlags, c->MxCsr, c->SegCs);
    for (int i = 0; i < 16; ++i) {
        const M128A& x = (&c->Xmm0)[i];
        fprintf(cf, "  XMM%2d: 0x%016llX%016llX\n", i, (unsigned long long)x.High, (unsigned long long)x.Low);
    }
}

static void FindNearestSymbol(const Loader::LoadedModule& m, u64 rva, std::string& name, u64& off) {
    name.clear(); off = 0; u64 best = ~0ull;
    for (const auto& sym : m.symbols) {
        if (sym.st_shndx == 0 || sym.st_value == 0) continue;
        if (sym.st_name < m.string_table.size() && sym.st_value <= rva) {
            u64 gap = rva - sym.st_value;
            if (gap < best) { best = gap; name = &m.string_table[sym.st_name]; off = gap; }
        }
    }
}
struct GuestModInfo { std::string name; u64 base=0, rva=0, sym_off=0; std::string sym; bool ok=false; };
static bool ResolveGuestAddress(u64 addr, GuestModInfo& out) {
    if (g_main_module_retained && addr >= g_main_module_copy.base_address &&
        addr < g_main_module_copy.base_address + g_main_module_copy.image_size) {
        out.name = g_main_module_copy.name; out.base = g_main_module_copy.base_address;
        out.rva = addr - g_main_module_copy.base_address; out.ok = true;
        FindNearestSymbol(g_main_module_copy, out.rva, out.sym, out.sym_off);
        return true;
    }
    for (const auto& seg : g_guest_segments) {
        if (addr >= seg.address && addr < seg.address + seg.size) {
            out.name = "guest-segment"; out.base = seg.address;
            out.rva = addr - seg.address; out.ok = true;
            return true;
        }
    }
    return false;
}
static void WriteCrashDump(const EXCEPTION_RECORD* rec, const CONTEXT* ctx,
                           const char* mod_name, u64 mod_off,
                           ULONG_PTR /*stk_low*/, ULONG_PTR /*stk_high*/) {
    // Note: separate crash_report.txt file write removed — the emergency
    // crash_log.txt fopen below now carries the full enhanced dump.

    std::error_code ec;
    std::filesystem::create_directories("logs", ec);

    FILE* cf = nullptr;
    fopen_s(&cf, "logs/crash_log.txt", "w");
    if (!cf) return;

    const u64 etype = (rec->NumberParameters >= 1) ? rec->ExceptionInformation[0] : 99;
    const u64 faddr = (rec->NumberParameters >= 2) ? rec->ExceptionInformation[1] : 0;
    const char* ename = (etype==0)?"Read":(etype==1)?"Write":(etype==8)?"Execute":"?";

    fprintf(cf, "=== PCSX5 Hardware Crash Dump ===\n\n");
    fprintf(cf, "Exception: 0x%llX (%s at 0x%llX)\n", (unsigned long long)rec->ExceptionCode, ename, (unsigned long long)faddr);
    fprintf(cf, "RIP: 0x%llX  RSP: 0x%llX  Thread: %lu\n",
            (unsigned long long)ctx->Rip, (unsigned long long)ctx->Rsp, ::GetCurrentThreadId());
    if (mod_name && mod_name[0]) fprintf(cf, "Module: %s + 0x%llX\n", mod_name, (unsigned long long)mod_off);
    fprintf(cf, "\n");

    DumpRegisterSection(cf, ctx); fprintf(cf, "\n");

    // Fault classification
    FaultClass fc = ClassifyFault(faddr);
    fprintf(cf, "Fault Class: %s\n", FaultClassName(fc));
    Memory::MemoryInfo mi{};
    if (Memory::Query(static_cast<guest_addr_t>(faddr), &mi) == Memory::Status::Ok) {
        fprintf(cf, "  Region: base=0x%llX size=0x%llX committed=%s\n",
                (unsigned long long)mi.base_address, (unsigned long long)mi.size,
                mi.is_committed?"yes":"no");
    } else fprintf(cf, "  Region: unmapped\n");
    if (fc == FaultClass::GuestCode || fc == FaultClass::GuestFrameBuffer) {
        GuestModInfo gmi{};
        if (ResolveGuestAddress(faddr, gmi)) {
            fprintf(cf, "  Module: %s base=0x%llX RVA=0x%llX",
                    gmi.name.c_str(), (unsigned long long)gmi.base, (unsigned long long)gmi.rva);
            if (!gmi.sym.empty())
                fprintf(cf, " near %s + 0x%llX", gmi.sym.c_str(), (unsigned long long)gmi.sym_off);
            fprintf(cf, "\n");
        }
    }
    fprintf(cf, "\n");

    // Faulting instruction
    u8 ib[16]={};
    fprintf(cf, "Faulting Instruction:\n");
    if (SafeRead(ib, (void*)(ctx->Rip), 16)) {
        fprintf(cf, "  Bytes: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                ib[0],ib[1],ib[2],ib[3],ib[4],ib[5],ib[6],ib[7],ib[8],ib[9],ib[10],ib[11],ib[12],ib[13],ib[14],ib[15]);
        auto d = InstrDecode::Decode(ib,16);
        fprintf(cf, "  Decode: %s (%u bytes)\n", d.text.c_str(), d.length);
    } else fprintf(cf, "  <unreadable>\n");
    fprintf(cf, "\n");

    // Host call stack (before any guest memory scans for safety)
    fprintf(cf, "Host Call Stack:\n");
    void* stk[64];
    USHORT nf = CaptureStackBackTrace(0,64,stk,nullptr);
    for (USHORT i=0;i<nf;++i) {
        char sn[MAX_PATH]="?"; HMODULE hf=nullptr; u64 of2=0;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                (LPCSTR)stk[i],&hf)) {
            GetModuleFileNameA(hf,sn,sizeof(sn));
            of2=(u64)stk[i]-(u64)hf;
        }
        fprintf(cf, "  [%02d] %s + 0x%llX\n", i, sn, (unsigned long long)of2);
    }
    fprintf(cf, "\n");

    // HLE import trace
    fprintf(cf, "HLE Import Trace (last 16):\n");
    auto trace = HLE::GetImportTrace(16);
    for (auto& te : trace)
        fprintf(cf, "  %s::%s args=(0x%llX,0x%llX,0x%llX,0x%llX)\n",
                te.module_name.c_str(), te.name.c_str(),
                (unsigned long long)te.arg1, (unsigned long long)te.arg2,
                (unsigned long long)te.arg3, (unsigned long long)te.arg4);

    fclose(cf);
}
// ---------------------------------------------------------------------------
static LONG CALLBACK VectoredExceptionHandler(PEXCEPTION_POINTERS exception_info) {
        // H4.3: recursion guard — must be first in the handler to
        // prevent host stack exhaustion from recursive VEH re-entry.
        VehRecursionGuard guard;
        if (guard.m_skip) return EXCEPTION_CONTINUE_SEARCH;

        PEXCEPTION_RECORD exception_record = exception_info->ExceptionRecord;
        PCONTEXT context = exception_info->ContextRecord;

        // Diagnostic write-watchpoint.  A hardware data breakpoint reports as
        // a single-step AFTER the store retires, so the value now at the
        // address is the one just written and RIP is the instruction that
        // wrote it.  DR6 bit 0 identifies DR0 as the source and is sticky, so
        // it must be cleared or every later trap looks identical.  Entirely
        // inert unless PCSX5_WATCH_ADDR is set.
        // Execute breakpoint (DR1, DR6 bit 1).  Traps before the instruction
        // runs, so RDI still holds the first argument under the SysV ABI that
        // guest code uses.  Setting EFlags.RF suppresses a re-trap on the same
        // instruction when execution resumes, which would otherwise loop
        // forever.  Inert unless PCSX5_BP_ADDR is set.
        if (exception_record->ExceptionCode == EXCEPTION_SINGLE_STEP &&
            BreakpointAddress() != 0 && (context->Dr6 & 0x2ull) != 0) {
            const u64 arg = context->Rdi;
            char text[128] = "";
            const char* note = "";
            if (arg == 0) {
                note = "  [null]";
            } else if (!Memory::IsReadable(arg, 1)) {
                note = "  [not readable]";
            } else {
                const u64 len = Memory::GuardedStrlen(arg, sizeof(text) - 1);
                if (len == 0) {
                    note = "  [EMPTY STRING]";
                } else {
                    u64 got = 0;
                    Memory::GuardedRead(text, arg, len, &got);
                    text[got] = 0;
                }
            }
            // rsi/rdx are the second and third SysV arguments; for a string
            // builder they carry the source pointer and its length, which is
            // what identifies a value the first argument cannot show.
            char text2[128] = "";
            const char* note2 = "";
            if (context->Rsi == 0) {
                note2 = "  [null]";
            } else if (!Memory::IsReadable(context->Rsi, 1)) {
                note2 = "  [not readable]";
            } else {
                const u64 l2 = Memory::GuardedStrlen(context->Rsi, sizeof(text2) - 1);
                if (l2 == 0) {
                    note2 = "  [EMPTY]";
                } else {
                    u64 g2 = 0;
                    Memory::GuardedRead(text2, context->Rsi, l2, &g2);
                    text2[g2] = 0;
                }
            }
            // Raw bytes of the object RSI points at.  The guest's std::string
            // keeps its inline buffer at +8 and its size at +0x18, so reading a
            // C string at +0 reports the pointer field rather than the text and
            // makes a populated short string look empty.
            char dump[3 * 32 + 1] = "";
            if (context->Rsi && Memory::IsReadable(context->Rsi, 32)) {
                u8 raw[32] = {};
                u64 gotr = 0;
                Memory::GuardedRead(raw, context->Rsi, sizeof(raw), &gotr);
                for (u64 i = 0; i < gotr; ++i) {
                    std::snprintf(dump + i * 3, 4, "%02x ", raw[i]);
                }
            }
            LOG_ERROR(Kernel, "BREAKPOINT   rsi=0x%llx str2='%s'%s rdx=0x%llx raw=[%s]",
                      context->Rsi, text2, note2, context->Rdx, dump);
            LOG_ERROR(Kernel, "BREAKPOINT 0x%llx rdi=0x%llx str='%s'%s ret=0x%llx guest-thread=%llu",
                      BreakpointAddress(), arg, text, note,
                      Memory::IsReadable(context->Rsp, sizeof(u64))
                          ? [&] { u64 r = 0; Memory::GuardedRead(&r, context->Rsp, sizeof(r)); return r; }()
                          : 0ull,
                      CpuCore::GetCurrentThreadId());
            context->Dr6 = 0;
            context->EFlags |= 0x10000u; // RF
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        if (exception_record->ExceptionCode == EXCEPTION_SINGLE_STEP &&
            WatchpointAddress() != 0 && (context->Dr6 & 0x1ull) != 0) {
            const u64 watch = WatchpointAddress();
            u64 value = 0;
            const bool readable = Memory::IsReadable(watch, sizeof(u64));
            if (readable) Memory::ReadBuffer(watch, &value, sizeof(value));
            // A host RIP means the emulator itself performed the store, not
            // guest code.  The raw address moves with ASLR, so resolve it to
            // module+offset, which is stable across runs and identifiable.
            char mod_desc[MAX_PATH + 32] = "";
            HMODULE mod = nullptr;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(context->Rip), &mod) && mod) {
                char path[MAX_PATH] = "";
                GetModuleFileNameA(mod, path, MAX_PATH);
                const char* base_name = std::strrchr(path, static_cast<int>(92));
                std::snprintf(mod_desc, sizeof(mod_desc), "  [%s+0x%llx]",
                              base_name ? base_name + 1 : path,
                              context->Rip - reinterpret_cast<u64>(mod));
            }
            LOG_ERROR(Kernel, "WATCHPOINT [0x%llx] = 0x%llx written by RIP=0x%llx guest-thread=%llu%s%s",
                      watch, value, context->Rip, CpuCore::GetCurrentThreadId(),
                      mod_desc, readable ? "" : "  [target not readable]");
            context->Dr6 = 0;
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        // Debug-output exceptions (raised by OutputDebugString, e.g. from the
        // GPU driver during Vulkan init) are informational, not faults.  Pass
        // them through silently: logging the full crash-scan path for these
        // re-faults inside its own probes, which is the recurring
        // first-chance memcpy AV noise seen in debugger output.
        if (exception_record->ExceptionCode == 0x40010006 ||  // DBG_PRINTEXCEPTION_C
            exception_record->ExceptionCode == 0x4001000A ||  // DBG_PRINTEXCEPTION_WIDE_C
            exception_record->ExceptionCode == 0x406D1388) {  // MS_VC_EXCEPTION (SetThreadDescription)
            // 0x406D1388 is the benign Visual C++ thread-naming exception —
            // GPU driver threads (e.g. nvwgf2umx) raise it on the host when
            // they lazily start worker threads minutes into a run; treating
            // it as a crash killed the emulator mid-game.
            return EXCEPTION_CONTINUE_SEARCH;
        }

        // Guest CPU step-tracer (PCSX5_GUEST_TRACE): claims single-step and
        // arming-breakpoint exceptions before any other handler so the tracer
        // can capture the guest instruction flow.  Inert unless the env var is
        // set (see guest_tracer.cpp).
        if (GuestTracer::HandleTrap(exception_record->ExceptionCode, context)) {
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        // Host stack overflow: log minimally and defer to the unhandled
        // filter chain (stack guarantee reserved in Initialize gives it room
        // to run).  Without the guarantee the OS kills the process silently
        // here — no VEH, no filter, no log line.
        if (exception_record->ExceptionCode == STATUS_STACK_OVERFLOW) {
            LOG_CRITICAL(Kernel, "FATAL: host stack overflow at RIP=0x%llx RSP=0x%llx OS thread %lu",
                         context->Rip, context->Rsp, ::GetCurrentThreadId());
            return EXCEPTION_CONTINUE_SEARCH;
        }

        // TLS patch-cache recovery (H2):
        // - Fault INSIDE an emitted stub: the patched site ran on a thread
        //   without a bound guest thread pointer; restore the original
        //   instruction and re-execute it via the emulation path.
        // - Fault AT a patched site: a concurrent thread fetched the bytes
        //   mid-rewrite; retry the instruction (bounded per site).
        if (exception_record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
            u64 patch_site = 0;
            if (TlsPatch::HandleStubFault(context->Rip, &patch_site)) {
                context->Rip = patch_site;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (TlsPatch::ShouldRetry(context->Rip)) {
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }

        // In-proc mode (core hosted inside the WPF app): the CLR and the WPF
        // runtime raise first-chance exceptions of their own (managed
        // exceptions, host AVs).  Only exceptions raised while executing
        // tracked guest code are ours to inspect; everything else passes
        // straight through to the host's handlers, cheaply.
        if (g_in_proc) {
            const DWORD code = exception_record->ExceptionCode;
            if (code != EXCEPTION_ACCESS_VIOLATION &&
                code != EXCEPTION_BREAKPOINT &&
                code != STATUS_ILLEGAL_INSTRUCTION &&
                code != EXCEPTION_SINGLE_STEP) {
                return EXCEPTION_CONTINUE_SEARCH;
            }
            Memory::MemoryInfo rip_info{};
            if (Memory::Query(context->Rip, &rip_info) != Memory::Status::Ok) {
                return EXCEPTION_CONTINUE_SEARCH; // RIP is host code (CLR/WPF/driver)
            }
        }

        if (exception_record->ExceptionCode == EXCEPTION_SINGLE_STEP) {
            if (GuestTracer::HandleTrap(exception_record->ExceptionCode, context)) {
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }

        // H4.7: Fast recovery for AVs in host DLLs when the fault address is a
        // sentinel value (sign-extended SCE error code 0xFFFFFFFF8xxxxxxx,
        // plain -1, or near-NULL).  Skip the load instruction and zero RCX.
        // Only fires for obviously-invalid pointers — real memory faults
        // (e.g. C++ unwind, GPU driver) pass through to normal crash handling.
        if (exception_record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
            context->Rip >= 0x7FF000000000ULL) {
            const u64 faddr = (exception_record->NumberParameters >= 2) ? exception_record->ExceptionInformation[1] : 0;
            // Only recover for sentinel addresses (error codes, -1, <4K).
            if (faddr == 0 || faddr == ~0ULL || faddr < 0x1000 ||
                (faddr & 0xFFFFFFFF80000000ULL) == 0xFFFFFFFF80000000ULL) {
                context->Rcx = 0;
                u8 b1 = 0;
                u32 skip = 3;
                if (SafeRead(&b1, reinterpret_cast<void*>(context->Rip), 1)) {
                    if (b1 == 0x0F) skip = 3;
                    else if (b1 >= 0x40 && b1 <= 0x4F) skip = 3;
                    else skip = 2;
                }
                context->Rip += skip;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }

        if (exception_record->ExceptionCode != 0xE06D7363) {
            LOG_INFO(Kernel, "VEH Exception Triggered: Code: 0x%X, RIP: 0x%llx, OS Thread: %lu",
                     exception_record->ExceptionCode, context->Rip, ::GetCurrentThreadId());
            
            // Dump instruction bytes at host RIP
            if (Memory::IsReadable(context->Rip, 16)) {
                u8 inst[16];
                Memory::ReadBuffer(context->Rip, inst, 16);
                LOG_INFO(Kernel, "VEH Instruction Bytes: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                         inst[0], inst[1], inst[2], inst[3], inst[4], inst[5], inst[6], inst[7],
                         inst[8], inst[9], inst[10], inst[11], inst[12], inst[13], inst[14], inst[15]);
            }
            // Dump instruction bytes BEFORE host RIP
            if (Memory::IsReadable(context->Rip - 32, 32)) {
                u8 inst[32];
                Memory::ReadBuffer(context->Rip - 32, inst, 32);
                LOG_INFO(Kernel, "VEH Prev Instr Bytes: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                         inst[0], inst[1], inst[2], inst[3], inst[4], inst[5], inst[6], inst[7],
                         inst[8], inst[9], inst[10], inst[11], inst[12], inst[13], inst[14], inst[15],
                         inst[16], inst[17], inst[18], inst[19], inst[20], inst[21], inst[22], inst[23],
                         inst[24], inst[25], inst[26], inst[27], inst[28], inst[29], inst[30], inst[31]);
                
                // Crash dumps belong in the diagnostics bundle directory, not
                // the process working directory.  These used to be written to a
                // bare relative path, so a 16MB dump landed in whatever
                // directory the emulator happened to be started from -- usually
                // the repository root.  Diagnostics owns this location; the
                // harness selects it with --crash-dir and already looks for the
                // RIP dump inside the per-run directory.
                const std::string& dump_dir = g_crash_dump_dir;
                std::error_code dump_ec;
                std::filesystem::create_directories(dump_dir, dump_ec);

                const std::string rip_dump_path = dump_dir + "/crash_rip_dump.bin";
                FILE* fdump = nullptr;
                if (fopen_s(&fdump, rip_dump_path.c_str(), "wb") == 0 && fdump) {
                    u64 dump_base = context->Rip - 32768;
                    u8* dump_buf = new u8[65536];
                    Memory::ReadBuffer(dump_base, dump_buf, 65536);
                    fwrite(dump_buf, 1, 65536, fdump);
                    fclose(fdump);
                    delete[] dump_buf;
                    LOG_INFO(Kernel, "Dumped 64KB around RIP to %s", rip_dump_path.c_str());
                }

                const std::string prx_dump_path = dump_dir + "/crash_prx_dump.bin";
                FILE* fdump2 = nullptr;
                if (fopen_s(&fdump2, prx_dump_path.c_str(), "wb") == 0 && fdump2) {
                    const u64 dump_base = CrashDumpBase();
                    const u64 dump_size = CrashDumpSize();
                    u8* dump_buf = new u8[dump_size];
                    Memory::ReadBuffer(dump_base, dump_buf, dump_size);
                    fwrite(dump_buf, 1, dump_size, fdump2);
                    fclose(fdump2);
                    delete[] dump_buf;
                    LOG_INFO(Kernel, "Dumped %llu bytes at 0x%llx to %s",
                             dump_size, dump_base, prx_dump_path.c_str());
                }
            }
        }

        // ---- GUEST NULL-CALL attribution (durable diagnostic) -------------
        // A guest indirect call through a null/bad function pointer faults by
        // trying to EXECUTE at/near address 0 (AV, ExceptionInformation[0]==8).
        // The guest thread's dedicated stack still holds the return-address
        // chain, so walk it to identify the guest call-site that made the call.
        if (exception_record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
            context->Rip < 0x10000ULL) {
            const u64 etype = (exception_record->NumberParameters >= 2)
                                  ? exception_record->ExceptionInformation[0] : 0;
            LOG_ERROR(Kernel, "--------------------------------------------------");
            LOG_ERROR(Kernel, "GUEST NULL-CALL: RIP=0x%llx etype=%llu code=0x%X — indirect call through null/bad fn-ptr | "
                      "RAX=0x%llx RBX=0x%llx RCX=0x%llx RDX=0x%llx RSI=0x%llx RDI=0x%llx "
                      "RSP=0x%llx RBP=0x%llx R8=0x%llx R9=0x%llx R10=0x%llx R11=0x%llx R12=0x%llx R13=0x%llx R14=0x%llx R15=0x%llx",
                      context->Rip, etype, exception_record->ExceptionCode,
                      context->Rax, context->Rbx, context->Rcx, context->Rdx,
                      context->Rsi, context->Rdi,
                      context->Rsp, context->Rbp,
                      context->R8, context->R9, context->R10, context->R11,
                      context->R12, context->R13, context->R14, context->R15);
            // Classify the pointer registers against the memory map.  A null
            // indirect call is usually reached through a table pointer; whether
            // that pointer is real memory (a structure with an unregistered
            // callback) or an artifact (corruption) changes the diagnosis
            // completely, and the register value alone does not say which.
            for (const auto& reg : { std::pair<const char*, u64>{"RAX", context->Rax},
                                     std::pair<const char*, u64>{"RBX", context->Rbx},
                                     std::pair<const char*, u64>{"RDI", context->Rdi} }) {
                if (reg.second < 0x1000ULL) continue;
                Memory::MemoryInfo mi{};
                if (Memory::Query(reg.second, &mi) == Memory::Status::Ok) {
                    LOG_ERROR(Kernel, "  [%s=0x%llx -> owner=%s region=0x%llx+0x%llx prot=0x%x committed=%d]",
                              reg.first, reg.second,
                              Memory::OwnerAsString(Memory::QueryOwner(reg.second)),
                              mi.base_address, mi.size, mi.protection, mi.is_committed ? 1 : 0);
                } else {
                    LOG_ERROR(Kernel, "  [%s=0x%llx -> UNMAPPED]", reg.first, reg.second);
                }
            }
            const u64 g_tid = CpuCore::GetCurrentThreadId();
            if (auto* gt = CpuCore::GetThreadById(g_tid)) {
                const u64 gbase = gt->stack_base, gsize = gt->stack_size;
                LOG_ERROR(Kernel, "  [guest thread %llu stack base=0x%llx size=0x%llx]", g_tid, gbase, gsize);
                u64 lo = (context->Rsp >= gbase && context->Rsp < gbase + gsize)
                             ? context->Rsp : (gbase + gsize - 0x2000);
                u64 hi = lo + 0x4000;
                if (hi > gbase + gsize) hi = gbase + gsize;
                int ra_count = 0;
                for (u64 p = lo; p + 8 <= hi && ra_count < 48; p += 8) {
                    u64 v = 0;
                    if (!SafeRead(&v, reinterpret_cast<void*>(p), 8)) break;
                    if (v >= 0x810000000 && v < 0x900000000) {
                        LOG_ERROR(Kernel, "  [guest-stack+0x%04llx] ret-> 0x%llx (off 0x%llx)",
                                  p - lo, v, v - 0x810000000);
                        ++ra_count;
                    }
                }
            } else {
                for (u64 p = context->Rsp; p + 8 <= context->Rsp + 0x2000; p += 8) {
                    u64 v = 0;
                    if (!SafeRead(&v, reinterpret_cast<void*>(p), 8)) break;
                    if (v >= 0x810000000 && v < 0x900000000) {
                        LOG_ERROR(Kernel, "  [rsp+0x%04llx] ret-> 0x%llx (off 0x%llx)",
                                  p - context->Rsp, v, v - 0x810000000);
                    }
                }
            }
            // Dump the guest instruction bytes at/just before the immediate
            // caller (return address = [rsp]) so the indirect-call instruction
            // and the load that produced the null target (RAX=0) are visible.
            {
                u64 caller = 0;
                SafeRead(&caller, reinterpret_cast<void*>(context->Rsp), 8);
                if (caller >= 0x810000000 && caller < 0x900000000) {
                    // The call instruction is a few bytes before the return addr.
                    u64 base = caller - 32;
                    if (base < 0x810000000) base = 0x810000000;
                    u8 buf[40] = {};
                    int nread = 0;
                    for (u64 k = 0; k < 40; ++k) {
                        if (!SafeRead(&buf[k], reinterpret_cast<void*>(base + k), 1)) break;
                        ++nread;
                    }
                    if (nread >= 8) {
                        std::string hex;
                        for (int k = 0; k < nread; ++k) {
                            char t[4];
                            sprintf_s(t, sizeof(t), "%02x ", buf[k]);
                            hex += t;
                        }
                        LOG_ERROR(Kernel, "  [caller 0x%llx-32 bytes] %s", caller, hex.c_str());
                    }
                }
            }
            // Dump a guest-data window around RBX (often a vtable/GOT/table base the
            // null pointer came from), so the 0 slot feeding the null call is visible.
            {
                u64 base = context->Rbx;
                if (base >= 0x810000000 && base < 0x900000000) {
                    u64 lo = base;
                    if (lo + 0x60 > 0x900000000) lo = 0x900000000 - 0x60;
                    std::ostringstream row;
                    for (u64 k = 0; k < 0x60; k += 8) {
                        u64 v = 0;
                        if (!SafeRead(&v, reinterpret_cast<void*>(lo + k), 8)) break;
                        char t[24];
                        sprintf_s(t, sizeof(t), "[%04llx]%06llx ", k, (v & 0xffffffffffULL));
                        row << t;
                    }
                    LOG_ERROR(Kernel, "  [datarbx 0x%llx] %s", base, row.str().c_str());
                }
            }
            LOG_ERROR(Kernel, "--------------------------------------------------");
        }
 
        if (exception_record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
            u8 instr[16] = {0};
            if (SafeRead(instr, reinterpret_cast<void*>(context->Rip), 16)) {
                char instr_hex[128] = {0};
                for (int i = 0; i < 16; ++i) {
                    sprintf_s(instr_hex + i * 3, sizeof(instr_hex) - i * 3, "%02X ", instr[i]);
                }
                LOG_DEBUG(Kernel, "  Instruction bytes at crash RIP 0x%llx: %s", context->Rip, instr_hex);
                auto decoded = InstrDecode::Decode(instr, 16);
                LOG_DEBUG(Kernel, "  Decoded instruction: %s (%u bytes, %s)",
                          decoded.text.c_str(), decoded.length, decoded.known ? "known" : "unknown");
            }
            LOG_DEBUG(Kernel, "  Access violation details: Type: %s, Address: 0x%llx",
                     exception_record->ExceptionInformation[0] == 0 ? "Read" :
                     exception_record->ExceptionInformation[0] == 1 ? "Write" : "Execute",
                     exception_record->ExceptionInformation[1]);
        }

        if (exception_record->ExceptionCode == EXCEPTION_BREAKPOINT) {
            if (GuestTracer::HandleTrap(exception_record->ExceptionCode, context)) {
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            u8* ip = reinterpret_cast<u8*>(context->Rip);
            u8 sig[2];
            if (ip && SafeRead(sig, ip, 2) && sig[0] == 0xCC && sig[1] == 0x90) {
                u32 syscall_number = static_cast<u32>(context->Rax);
                context->Rax = HandleSyscall(syscall_number, context);

                context->Rip += 2;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }

        // Handle PS5 direct syscalls (int 0x41)
        if (exception_record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
            u8* ip = reinterpret_cast<u8*>(context->Rip);
            u8 sig[2];
            if (ip && SafeRead(sig, ip, 2) && sig[0] == 0xCD && sig[1] == 0x41) {
                u32 syscall_number = static_cast<u32>(context->Rax);
                LOG_WARN(Kernel, "Guest invoked raw syscall %u via int 0x41", syscall_number);
                if (syscall_number == 1) { // sys_exit
                    SwitchToThread();
                    context->Rip += 2;
                    return EXCEPTION_CONTINUE_EXECUTION;
                } else if (syscall_number == 431) { // sys_thr_exit
                    ::TerminateThread(::GetCurrentThread(), 0);
                } else {
                    LOG_ERROR(Kernel, "Unhandled raw syscall %u via int 0x41", syscall_number);
                    context->Rax = 0;
                }
                context->Rip += 2;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }

        // AMD-only instruction software fallback (SSE4a EXTRQ/INSERTQ,
        // MONITORX/MWAITX).  Only claims STATUS_ILLEGAL_INSTRUCTION — access
        // violations keep flowing to the TLS/demand-commit paths below.
        if (exception_record->ExceptionCode == STATUS_ILLEGAL_INSTRUCTION &&
            TryRecoverAmdCompatInstruction(context, context->Rip)) {
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        if (exception_record->ExceptionCode == STATUS_ACCESS_VIOLATION || exception_record->ExceptionCode == 0xC0000005) {
            LOG_DEBUG(Kernel, "Parsing instruction for TLS emulation at RIP=0x%llx", context->Rip);
            TlsPatch::NoteTrap();
            u8* rip = reinterpret_cast<u8*>(context->Rip);
            u8* instr = rip;
            u8 b = 0;
            
            while (SafeRead(&b, instr, 1) && b == 0x66) {
                instr++;
            }
            
            if (SafeRead(&b, instr, 1) && b == 0x64) {
                instr++;
                
                bool is_64bit = false;
                u8 rex = 0;
                if (SafeRead(&b, instr, 1) && (b & 0xF0) == 0x40) {
                    rex = b;
                    is_64bit = (b & 0x08) != 0;
                    instr++;
                } else {
                    LOG_DEBUG(Kernel, "  No REX prefix, b=0x%x", b);
                }
                
                u8 opcode = 0;
                if (SafeRead(&opcode, instr, 1)) {
                    instr++;
                    
                    u8 modrm = 0;
                    if (SafeRead(&modrm, instr, 1)) {
                        instr++;
                        
                        u8 reg = (modrm >> 3) & 7;
                        u8 rm = modrm & 7;
                        u8 mod = modrm >> 6;
                        
                        s32 displacement = 0;
                        bool parse_success = true;
                        bool patchable_form = false;  // absolute fs:[disp32] — eligible for patch-once
                        
                        if (mod == 0 && rm == 4) {
                            u8 sib = 0;
                            if (SafeRead(&sib, instr, 1)) {
                                instr++;
                                u8 base = sib & 7;
                                if (base == 5) {
                                    if (!SafeRead(&displacement, instr, 4)) parse_success = false;
                                    instr += 4;
                                    patchable_form = true;
                                } else {
                                    LOG_DEBUG(Kernel, "  SIB base is %d (expected 5)", base);
                                    parse_success = false;
                                }
                            } else {
                                parse_success = false;
                            }
                        } else if (mod == 0 && rm == 5) {
                            if (SafeRead(&displacement, instr, 4)) {
                                instr += 4;
                                patchable_form = true;
                            } else {
                                parse_success = false;
                            }
                        } else if (mod == 1) {
                            if (rm == 4) {
                                instr++;
                            }
                            s8 disp8 = 0;
                            if (SafeRead(&disp8, instr, 1)) {
                                displacement = static_cast<s32>(disp8);
                                instr += 1;
                            } else {
                                parse_success = false;
                            }
                        } else if (mod == 2) {
                            if (rm == 4) {
                                instr++;
                            }
                            if (SafeRead(&displacement, instr, 4)) {
                                instr += 4;
                            } else {
                                parse_success = false;
                            }
                        } else {
                            LOG_WARN(Kernel, "TLS decode failed at RIP=0x%llx (mod=%d, rm=%d)",
                                     context->Rip, mod, rm);
                            parse_success = false;
                        }
                        
                        if (parse_success) {
                            u32 instr_len = static_cast<u32>(instr - rip);
                            
                            // O3.3: use thread-local TLS cache first to avoid
                            // g_thread_mutex contention on every VEH TLS trap.
                            guest_addr_t tp = t_tls_base;
                            if (tp == 0) {
                                // Fall back to global mutex path (slow).
                                u64 current_tid = GetCurrentThreadId();
                                std::lock_guard<std::mutex> lock(g_thread_mutex);
                                auto it = g_threads.find(current_tid);
                                if (it != g_threads.end()) {
                                    tp = it->second.tls_base;
                                    t_tls_base = tp;  // prime the cache
                                }
                            }
                            if (tp == 0) {
                                tp = g_guest_tls.ThreadPointer();
                            }

                            // H2: once this trap has been emulated, rewrite the
                            // site so subsequent executions run natively (no VEH).
                            auto try_patch_site = [&](u64 site_rip, u32 imm32 = 0) {
                                if (!patchable_form) return;
                                if (opcode == 0xC7 && reg != 0) return; // C7 /0 only
                                TlsPatch::AccessInfo access{};
                                access.opcode = opcode;
                                access.is_64bit = is_64bit;
                                access.reg = static_cast<u8>(reg | ((rex & 0x04) ? 8 : 0));
                                access.displacement = displacement;
                                access.imm32 = imm32;
                                access.instr_len = instr_len;
                                TlsPatch::TryPatchSite(site_rip, access);
                            };
                            
                            if (opcode == 0x8B) {
                                const u64 access_size = is_64bit ? 8 : 4;
                                LOG_DEBUG(Kernel, "Emulating TLS read: RIP=0x%llx, displacement=%d, reg=%d, is_64bit=%d, tp=0x%llx",
                                         context->Rip, displacement, reg, is_64bit, tp);
                                if (tp == 0) {
                                    LOG_ERROR(Kernel, "Guest TLS read but no thread pointer configured.");
                                    return EXCEPTION_CONTINUE_SEARCH;
                                }
                                guest_addr_t tls_address = tp + displacement;
                                u64 tls_value = 0;
                                Memory::ReadBuffer(tls_address, &tls_value, access_size);
                                
                                u64* reg_ptr = nullptr;
                                // In 64-bit mode with REX prefix, reg can be 8-15
                                u8 full_reg = static_cast<u8>(reg | ((rex & 0x04) ? 8 : 0));
                                switch (full_reg) {
                                    case 0: reg_ptr = &context->Rax; break;
                                    case 1: reg_ptr = &context->Rcx; break;
                                    case 2: reg_ptr = &context->Rdx; break;
                                    case 3: reg_ptr = &context->Rbx; break;
                                    case 4: reg_ptr = &context->Rsp; break;
                                    case 5: reg_ptr = &context->Rbp; break;
                                    case 6: reg_ptr = &context->Rsi; break;
                                    case 7: reg_ptr = &context->Rdi; break;
                                    case 8: reg_ptr = &context->R8; break;
                                    case 9: reg_ptr = &context->R9; break;
                                    case 10: reg_ptr = &context->R10; break;
                                    case 11: reg_ptr = &context->R11; break;
                                    case 12: reg_ptr = &context->R12; break;
                                    case 13: reg_ptr = &context->R13; break;
                                    case 14: reg_ptr = &context->R14; break;
                                    case 15: reg_ptr = &context->R15; break;
                                }
                                
                                if (reg_ptr) {
                                    if (is_64bit) {
                                        *reg_ptr = tls_value;
                                    } else {
                                        *reg_ptr = (*reg_ptr & 0xFFFFFFFF00000000) | (tls_value & 0xFFFFFFFF);
                                    }
                                    
                                    u64 old_rip = context->Rip;
                                    context->Rip += instr_len;
                                    try_patch_site(old_rip);
                                    LOG_DEBUG(Kernel, "TLS read emulated: RIP 0x%llx -> 0x%llx (len=%d), reg val = 0x%llx, OS Thread: %lu", old_rip, context->Rip, instr_len, *reg_ptr, ::GetCurrentThreadId());
                                    return EXCEPTION_CONTINUE_EXECUTION;
                                }
                            }
                            else if (opcode == 0x89) {
                                u64* reg_ptr = nullptr;
                                // In 64-bit mode with REX prefix, reg can be 8-15
                                u8 full_reg = static_cast<u8>(reg | ((rex & 0x04) ? 8 : 0));
                                switch (full_reg) {
                                    case 0: reg_ptr = &context->Rax; break;
                                    case 1: reg_ptr = &context->Rcx; break;
                                    case 2: reg_ptr = &context->Rdx; break;
                                    case 3: reg_ptr = &context->Rbx; break;
                                    case 4: reg_ptr = &context->Rsp; break;
                                    case 5: reg_ptr = &context->Rbp; break;
                                    case 6: reg_ptr = &context->Rsi; break;
                                    case 7: reg_ptr = &context->Rdi; break;
                                    case 8: reg_ptr = &context->R8; break;
                                    case 9: reg_ptr = &context->R9; break;
                                    case 10: reg_ptr = &context->R10; break;
                                    case 11: reg_ptr = &context->R11; break;
                                    case 12: reg_ptr = &context->R12; break;
                                    case 13: reg_ptr = &context->R13; break;
                                    case 14: reg_ptr = &context->R14; break;
                                    case 15: reg_ptr = &context->R15; break;
                                }
                                
                                if (reg_ptr) {
                                    u64 tls_value = *reg_ptr;
                                    const u64 access_size = is_64bit ? 8 : 4;
                                    if (tp == 0) {
                                        LOG_ERROR(Kernel, "Guest TLS write but no thread pointer configured.");
                                        return EXCEPTION_CONTINUE_SEARCH;
                                    }
                                    guest_addr_t tls_address = tp + displacement;
                                    Memory::WriteBuffer(tls_address, &tls_value, access_size);
                                    
                                    u64 old_rip = context->Rip;
                                    context->Rip += instr_len;
                                    try_patch_site(old_rip);
                                    LOG_DEBUG(Kernel, "TLS write emulated: RIP 0x%llx -> 0x%llx (len=%d), val = 0x%llx", old_rip, context->Rip, instr_len, tls_value);
                                    return EXCEPTION_CONTINUE_EXECUTION;
                                }
                            }
                            else if (opcode == 0xC7) {
                                u32 imm_value = 0;
                                if (SafeRead(&imm_value, instr, 4)) {
                                    instr += 4;
                                    instr_len = static_cast<u32>(instr - rip);
                                    
                                    const u64 access_size = is_64bit ? 8 : 4;
                                    if (tp == 0) {
                                        LOG_ERROR(Kernel, "Guest TLS write but no thread pointer configured.");
                                        return EXCEPTION_CONTINUE_SEARCH;
                                    }
                                    guest_addr_t tls_address = tp + displacement;
                                    u64 tls_val = imm_value;
                                    Memory::WriteBuffer(tls_address, &tls_val, access_size);
                                    
                                    u64 old_rip = context->Rip;
                                    context->Rip += instr_len;
                                    try_patch_site(old_rip, imm_value);
                                    LOG_DEBUG(Kernel, "TLS imm write emulated: RIP 0x%llx -> 0x%llx (len=%d), val = 0x%llx", old_rip, context->Rip, instr_len, tls_val);
                                    return EXCEPTION_CONTINUE_EXECUTION;
                                }
                            }
                        }
                    }
                }
            }
        }

        // Patch-site retry: when the TLS instruction parser above fails because
        // the bytes at RIP are `E8 ...` (a TlsPatch relocation stub) rather than
        // the original `64 48 8B ...` pattern — this happens when a thread hits
        // a site between HandleStubFault's restoration and TryPatchSite's
        // re-patch.  Just retry: the thread either has a bound TP (stub works)
        // or the stub faults and HandleStubFault deals with it.
        if (exception_record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
            u8 first_byte = 0;
            if (SafeRead(&first_byte, reinterpret_cast<void*>(context->Rip), 1) && first_byte == 0xE8) {
                u64 rip = context->Rip;
                if (rip >= 0x800000000 && rip < 0x900000000) {
                    LOG_DEBUG(Kernel, "TlsPatch: retrying patched site at 0x%llx after TLS decode miss", rip);
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            }
        }

        // Demand-commit: a guest access violation on a reserved-but-uncommitted
        // page (direct-memory pool, reserved virtual range) is committed on
        // first touch and execution resumes; anything else falls through to
        // the crash path below unchanged.
        if (exception_record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
            exception_record->NumberParameters >= 2) {
            const auto fault_addr =
                static_cast<guest_addr_t>(exception_record->ExceptionInformation[1]);
            if (auto handler = Memory::GetGuestFaultHandler()) {
                if (handler(fault_addr, exception_record->ExceptionCode,
                            Memory::GetGuestFaultHandlerUserData())) {
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            }
        }

        u64 ip = context->Rip;

        // Last chance before declaring a guest crash: a TLS site may have been
        // rewritten by another thread while this one was mid-flight.  The
        // earlier ShouldRetry check happens before the instruction is parsed,
        // so a thread that entered the handler just before the rewrite passes
        // it, then parses the freshly written `jmp stub` bytes instead of the
        // fs-relative access it faulted on, fails to decode them, and lands
        // here.  Re-executing is correct: the site now holds a complete,
        // valid instruction.  Observed with five guest threads reaching one
        // TLS site together - one patched it and the other four died on it.
        if (exception_record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
            TlsPatch::ShouldRetry(ip)) {
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        if (ip >= 0x800000000 && ip < 0x900000000) {
            LOG_ERROR(Kernel, "--------------------------------------------------");
            LOG_ERROR(Kernel, "GUEST APPLICATION CRASHED!");
            LOG_ERROR(Kernel, "Exception Code: 0x%X", exception_record->ExceptionCode);
            LOG_ERROR(Kernel, "Crash Address (RIP): 0x%llx (Offset: 0x%llx)", ip, ip - 0x800000000);
            LOG_ERROR(Kernel, "Register Dump:");
            LOG_ERROR(Kernel, "  RAX: 0x%016llx  RBX: 0x%016llx", context->Rax, context->Rbx);
            LOG_ERROR(Kernel, "  RCX: 0x%016llx  RDX: 0x%016llx", context->Rcx, context->Rdx);
            LOG_ERROR(Kernel, "  RSI: 0x%016llx  RDI: 0x%016llx", context->Rsi, context->Rdi);
            LOG_ERROR(Kernel, "  RBP: 0x%016llx  RSP: 0x%016llx", context->Rbp, context->Rsp);
            LOG_ERROR(Kernel, "  R8:  0x%016llx  R9:  0x%016llx", context->R8,  context->R9);
            LOG_ERROR(Kernel, "  R10: 0x%016llx  R11: 0x%016llx", context->R10, context->R11);
            LOG_ERROR(Kernel, "  R12: 0x%016llx  R13: 0x%016llx", context->R12, context->R13);
            LOG_ERROR(Kernel, "  R14: 0x%016llx  R15: 0x%016llx", context->R14, context->R15);
            LOG_ERROR(Kernel, "--------------------------------------------------");

            // Context dump: qwords at RDI (and the faulting address) — for
            // "null table/arena pointer" faults the crash-site object layout
            // is the fastest route to the missing initialization.
            LOG_ERROR(Kernel, "Object context dump (qwords at RDI=0x%llx):", context->Rdi);
            for (u64 off = 0; off < 0x70; off += 8) {
                u64 val = 0;
                if (!SafeRead(&val, reinterpret_cast<void*>(context->Rdi + off), 8)) break;
                LOG_ERROR(Kernel, "  [RDI+0x%02llx] = 0x%016llx", off, val);
            }

            // Guest stack scan — find the return addresses on the guest stack
            // so host-frame crashes in real libc.prx code (native memset etc.)
            // can be attributed to the calling guest frame.
            LOG_ERROR(Kernel, "Guest stack scan:");
            int scan_count = 0;
            for (u64 sp_scan = context->Rsp;
                 sp_scan < context->Rsp + 0x2000 && scan_count < 16;
                 sp_scan += 8) {
                u64 val = 0;
                if (!SafeRead(&val, reinterpret_cast<void*>(sp_scan), 8)) break;
                if (val >= 0x800000000 && val < 0x900000000) {
                    LOG_ERROR(Kernel, "  [RSP+0x%04llx] -> 0x%llx (offset 0x%llx)",
                              sp_scan - context->Rsp, val, val - 0x800000000);
                    scan_count++;
                }
            }

            // Record the crash and let the SEH handler in TryStartGuest catch
            // it cleanly instead of killing the emulator process.
            LogConfig::FlushDedup();
            ULONG_PTR _sl = 0, _sh = 0;
            GetCurrentThreadStackLimits(&_sl, &_sh);
            WriteCrashDump(exception_record, context, "guest", 0, _sl, _sh);
            HLE::SetGuestCrashed(exception_record->ExceptionCode,
                                 static_cast<guest_addr_t>(context->Rip));
            return EXCEPTION_CONTINUE_SEARCH;
        }

        if (exception_record->ExceptionCode != 0xE06D7363 && 
            exception_record->ExceptionCode != EXCEPTION_BREAKPOINT &&
            exception_record->ExceptionCode != EXCEPTION_SINGLE_STEP) {
            
            char module_name[MAX_PATH] = "Unknown Module";
            HMODULE h_mod = nullptr;
            u64 offset = 0;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(context->Rip), &h_mod)) {
                GetModuleFileNameA(h_mod, module_name, sizeof(module_name));
                offset = context->Rip - reinterpret_cast<u64>(h_mod);
            }

            // Scan the guest stack for values that look like guest return
            // addresses (guest modules live at [0x800000000, 0x900000000)).
            // Logged via LOG_ERROR (flushed per line) since crash_log.txt may
            // not be flushed if the handler itself dies.  The VEH runs on the
            // faulting thread, so cap the walk at the thread's stack limit:
            // probing past it re-faults inside SafeRead's memcpy and raises
            // fresh first-chance AVs while handling this one.
            ULONG_PTR stack_low = 0, stack_high = 0;
            GetCurrentThreadStackLimits(&stack_low, &stack_high);
            const u64 scan_limit = static_cast<u64>(stack_high);
            LOG_ERROR(Kernel, "Guest stack scan (potential return addresses):");
            int host_scan_count = 0;
            for (u64 sp_scan = context->Rsp;
                 sp_scan < context->Rsp + 0x2000 && sp_scan + sizeof(u64) <= scan_limit && host_scan_count < 16;
                 sp_scan += 8) {
                u64 val = 0;
                if (!SafeRead(&val, reinterpret_cast<void*>(sp_scan), 8)) break;
                if (val >= 0x800000000 && val < 0x900000000) {
                    LOG_ERROR(Kernel, "  [RSP+0x%04llx] -> 0x%llx (guest offset 0x%llx)",
                              sp_scan - context->Rsp, val, val - 0x800000000);
                    host_scan_count++;
                }
            }

            // Scan guest segments for leaked host pointers (values that fall in
            // the host module/DLL range). These should never appear in guest
            // memory; their location identifies the bad relocation/import.
            LOG_ERROR(Kernel, "Guest memory scan (leaked host pointers):");
            int leaked_count = 0;
            for (const auto& seg : g_guest_segments) {
                for (u64 addr = seg.address; addr + 8 <= seg.address + seg.size && leaked_count < 32; addr += 8) {
                    u64 val = 0;
                    if (!SafeRead(&val, reinterpret_cast<void*>(addr), 8)) break;
                    if (val >= 0x7FF000000000 && val < 0x800000000000) {
                        LOG_ERROR(Kernel, "  [0x%llx] = 0x%llx (guest offset 0x%llx)",
                                  addr, val, addr - 0x800000000);
                        leaked_count++;
                    }
                }
            }

            // Debug curation: the guest/host stack scans and faulting-region
            // layout diagnostics were proven out while chasing a VCRUNTIME
            // memcpy AV (see CommitOnFault).  The scans are intentionally
            // omitted from the final log to keep crash reports compact.

            LOG_ERROR(Kernel, "VEH Unhandled Exception: Code: 0x%X, RIP: 0x%llx, Module: %s, Offset: 0x%llx",
                      exception_record->ExceptionCode, context->Rip, module_name, offset);
            
            LOG_ERROR(Kernel, "  Registers:");
            LOG_ERROR(Kernel, "    RAX: 0x%016llx  RBX: 0x%016llx  RCX: 0x%016llx", context->Rax, context->Rbx, context->Rcx);
            LOG_ERROR(Kernel, "    RDX: 0x%016llx  RSI: 0x%016llx  RDI: 0x%016llx", context->Rdx, context->Rsi, context->Rdi);
            LOG_ERROR(Kernel, "    RBP: 0x%016llx  RSP: 0x%016llx  R8 : 0x%016llx", context->Rbp, context->Rsp, context->R8);
            LOG_ERROR(Kernel, "    R9 : 0x%016llx  R10: 0x%016llx  R11: 0x%016llx", context->R9, context->R10, context->R11);
            LOG_ERROR(Kernel, "    R12: 0x%016llx  R13: 0x%016llx  R14: 0x%016llx", context->R12, context->R13, context->R14);
            LOG_ERROR(Kernel, "    R15: 0x%016llx", context->R15);

            // I6.1: Boot-status timeline — stages recorded via SetBootStatus.
            auto boot_timeline = GPU::GetBootTimeline();
            if (!boot_timeline.empty()) {
                LOG_ERROR(Kernel, "  Boot timeline:");
                for (const auto& stage : boot_timeline)
                    LOG_ERROR(Kernel, "    %s", stage.c_str());
            }

            // Also log recent HLE import calls so the crash can be correlated
            // with the last guest->host transitions.
            auto trace = HLE::GetImportTrace(16);
            for (const auto& te : trace) {
                LOG_ERROR(Kernel, "  HLE trace: %s::%s (id=%llu) from guest RIP 0x%llx args=(0x%llx, 0x%llx, 0x%llx, 0x%llx)",
                          te.module_name.c_str(), te.name.c_str(), te.symbol_id,
                          te.caller_rip, te.arg1, te.arg2, te.arg3, te.arg4);
            }

            // H4.7: VCRUNTIME140 access-violation recovery — the game calls
            // host CRT functions (strcmp/memcpy helpers) with bad pointers
            // on a path we don't intercept.  Recover by skipping the faulting
            // load instruction and zeroing the destination register, so the
            // game sees empty/NUL data instead of crashing.  This is a
            // temporary band-aid until we identify the un-intercepted path.
            // Flush any pending dedup annotations + write the hardware-level crash dump.
            LogConfig::FlushDedup();
            ULONG_PTR sl = 0, sh = 0;
            GetCurrentThreadStackLimits(&sl, &sh);
            WriteCrashDump(exception_record, context, module_name, offset, sl, sh);
        }

        return EXCEPTION_CONTINUE_SEARCH;
    }

    void RegisterThread(const ThreadContext& context) {
        std::lock_guard<std::mutex> lock(g_thread_mutex);
        g_threads[context.thread_id] = context;
        t_tls_base = context.tls_base; // prime thread-local VEH cache (O3.3)
        LOG_INFO(Kernel, "Registered thread '%s' (id=%llu, entry=0x%llx, stack=0x%llx, stack_size=%llu, tls=0x%llx)",
                 context.name.c_str(), context.thread_id, context.entry_point,
                 context.stack_base, context.stack_size, context.tls_base);
    }

    // Resolve the guest thread pointer for a guest thread id exactly the way
    // the VEH's fs-emulation path does: the registered per-thread tls_base
    // when present, otherwise the shared TLS block.  Patched TLS stubs MUST
    // use the same value or their accesses silently diverge from the
    // emulated path (H2 worker-thread corruption).
    guest_addr_t ResolveGuestThreadPointer(u64 guest_tid) {
        guest_addr_t tp = 0;
        {
            std::lock_guard<std::mutex> lock(g_thread_mutex);
            auto it = g_threads.find(guest_tid);
            if (it != g_threads.end()) {
                tp = it->second.tls_base;
            }
        }
        if (tp == 0) {
            tp = g_guest_tls.ThreadPointer();
        }
        return tp;
    }

    // Dynamic-TLS (module-keyed) resolution for __tls_get_addr.  The ELF TLS
    // descriptor carries { sv_ndx (ti_module), ti_offset }.  The main module
    // (and unknown/0 indices) is handled by the original single-shared-block
    // path `tp + ti_offset` — byte-identical to the pre-DTV behaviour, which is
    // what single-module titles (Dreaming Sarah) rely on.  A KNOWN secondary
    // TLS-bearing module gets a dedicated per-(thread,module) block seeded from
    // its PT_TLS image (mirrors KyTy's RuntimeLinker::TlsGetAddr / SharpEmu's
    // GuestTlsTemplate), so a second module's TLS no longer aliases the main
    // block.  Any mismatch/unknown index falls back to `tp + ti_offset`.
    guest_addr_t ResolveDynamicTls(u64 guest_tid, u64 ti_module, u64 ti_offset,
                                   guest_addr_t tp) {
        // Main module / unknown / malformed: preserve today's behaviour exactly.
        if (ti_module == 0 || ti_module == 1) {
            return tp + ti_offset;
        }

        // Find the TLS-bearing loaded module whose 1-based TLS index == ti_module.
        const Loader::LoadedModule* mod = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_module_registry_mutex);
            for (size_t i = 0; i < g_loaded_modules.size() && i < g_loaded_module_tls_index.size(); ++i) {
                if (g_loaded_module_tls_index[i] == static_cast<u32>(ti_module)) {
                    mod = &g_loaded_modules[i];
                    break;
                }
            }
        }
        if (!mod || !mod->has_tls || mod->tls_file_size > 0x100000ULL) {
            return tp + ti_offset; // no usable PT_TLS image -> prior behaviour
        }

        const u64 key = (guest_tid << 32) | (ti_module & 0xFFFFFFFFull);
        u64 block_va = 0;
        bool need_build = false;
        {
            std::lock_guard<std::mutex> lock(g_tls_block_mutex);
            for (size_t i = 0; i < g_tls_block_cache.size(); ++i) {
                if (g_tls_block_cache[i].first == key) {
                    block_va = g_tls_block_va[i];
                    break;
                }
            }
            if (block_va == 0) need_build = true;
        }

        if (need_build) {
            // Locate the PT_TLS init image in guest memory: tls_template_offset
            // is a FILE offset, so find the mapped segment containing it.
            u64 templ_va = 0;
            for (const auto& seg : mod->segments) {
                const u64 fo = seg.file_offset;
                if (mod->tls_template_offset >= fo &&
                    mod->tls_template_offset + mod->tls_file_size <= fo + seg.file_size) {
                    templ_va = seg.address + (mod->tls_template_offset - fo);
                    break;
                }
            }
            constexpr u64 kTcbSize  = 0x40;
            constexpr u64 kTcbAlign = 0x20;
            const u64 need = ((mod->tls_mem_size + kTcbAlign - 1) & ~(kTcbAlign - 1)) + kTcbSize;
            guest_addr_t mapped = 0;
            if (Memory::Map(0, need, Memory::PROT_READ | Memory::PROT_WRITE, &mapped) != Memory::Status::Ok) {
                return tp + ti_offset;
            }
            if (templ_va && Memory::IsReadable(templ_va, mod->tls_file_size)) {
                std::memmove(reinterpret_cast<void*>(mapped),
                             reinterpret_cast<void*>(templ_va),
                             static_cast<size_t>(mod->tls_file_size));
            } else {
                std::memset(reinterpret_cast<void*>(mapped), 0, static_cast<size_t>(need));
            }
            // TCB self-pointer at the top of the block (variant-II style), so
            // tp-relative accesses within the block's own region stay coherent.
            const u64 tcb = mapped + need - kTcbSize;
            Memory::Write<u64>(tcb, tcb);
            block_va = mapped;
            {
                std::lock_guard<std::mutex> lock(g_tls_block_mutex);
                g_tls_block_cache.push_back({key, block_va});
                g_tls_block_va.push_back(block_va);
            }
        }
        return block_va + ti_offset;
    }
}
// namespace Kernel











