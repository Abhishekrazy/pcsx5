#include "kernel.h"
#include "fd_table.h"
#include "memory.h"
#include "syscalls.h"
#include "thread.h"
#include "../memory/memory.h"
#include "../hle/hle.h"
#include "../loader/module_graph.h"
#include "../common/log.h"
#include "../gpu/gpu.h"
#include <windows.h>
#include <iostream>
#include <cstring>
#include <filesystem>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <set>
#include <vector>
#include <string>

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
    static PVOID g_veh_handler = nullptr;
    static LPTOP_LEVEL_EXCEPTION_FILTER g_prev_exception_filter = nullptr;
    static GuestTlsContext g_guest_tls;
    static std::vector<Loader::MappedSegment> g_guest_segments;
    static Loader::ModuleResolver g_module_resolver;

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
        if (!firmware_modules_dir.empty()) {
            dirs.emplace_back(firmware_modules_dir);
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
        // the libSceSaveData HLE uses.
        constexpr std::string_view kSaveData0 = "/savedata0";
        if (!g_savedata_dir.empty()) {
            if (guest_path == kSaveData0) return g_savedata_dir;
            if (guest_path.rfind(std::string(kSaveData0) + "/", 0) == 0) {
                return g_savedata_dir + guest_path.substr(kSaveData0.size());
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

        // Register Top-Level Exception Filter for host crashes
        g_prev_exception_filter = SetUnhandledExceptionFilter(HostUnhandledExceptionFilter);

        // Install the demand-commit fault handler: guest faults on reserved
        // (not yet committed) pages are committed on first touch instead of
        // crashing.  Consulted by VectoredExceptionHandler before the crash
        // path and by Memory's own guest-fault VEH.
        Memory::SetGuestFaultHandler(&DemandCommitFaultHandler, nullptr);

        // Allocate a 128KB block representing the guest's TLS area to support negative offsets (Variant II TLS)
        u64 tls_total_size = 128 * 1024;
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
        LOG_INFO(Kernel, "Allocated guest TLS block [0x%llx - 0x%llx], base at 0x%llx", 
                 tls_alloc, tls_alloc + tls_total_size, g_guest_tls.ThreadPointer());

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
        
        ShutdownFdTable();
        ShutdownGuestMemory();

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

    // Binary patching: Scan executable segments for "syscall" (0x0F 0x05) and replace with "INT 3; NOP" (0xCC 0x90)
    static void PatchSyscalls(const std::vector<Loader::MappedSegment>& segments) {
        u64 patched_count = 0;

        for (const auto& seg : segments) {
            // Only scan executable segments
            if (!(seg.final_protection & Memory::PROT_EXEC)) continue;

            u8* start = reinterpret_cast<u8*>(seg.address);
            u8* end = start + seg.size;

            for (u8* ptr = start; ptr < end - 1; ++ptr) {
                // Find "syscall" instruction: 0x0F 0x05
                if (ptr[0] == 0x0F && ptr[1] == 0x05) {
                    DWORD old_protect;
                    // Make segment temporarily writable if it isn't
                    VirtualProtect(ptr, 2, PAGE_EXECUTE_READWRITE, &old_protect);
                    
                    // Replace with INT 3 (0xCC) and NOP (0x90)
                    ptr[0] = 0xCC;
                    ptr[1] = 0x90;
                    
                    // Restore protection
                    VirtualProtect(ptr, 2, old_protect, &old_protect);
                    patched_count++;
                }
            }
        }

        LOG_INFO(Kernel, "Patched %llu syscall instructions in executable segments.", patched_count);
    }

    // Look up an import name in the export tables of already-loaded PRX
    // modules.  Returns 0 when no loaded PRX exports the symbol.
    static guest_addr_t FindLoadedPrxExport(const std::string& sym_name) {
        for (const auto& [key, record] : g_prx_modules) {
            const auto& module = record->module;
            for (const auto& sym : module.symbols) {
                if (sym.st_shndx == 0 || sym.st_value == 0) continue; // not an export
                const char* name = &module.string_table[sym.st_name];
                if (name && sym_name == name) {
                    return module.base_address + sym.st_value;
                }
            }
        }
        return 0;
    }

    static bool LinkModule(Loader::LoadedModule& module) {
        LOG_INFO(Kernel, "Linking module %s at base address 0x%llx...", module.name.c_str(), module.base_address);

        auto resolve_external = [&](const std::string& sym_name) -> guest_addr_t {
            // Prefer real exports from loaded PRX modules; fall back to HLE.
            guest_addr_t addr = FindLoadedPrxExport(sym_name);
            if (addr != 0) {
                return addr;
            }
            addr = HLE::ResolveAny(sym_name);
            if (addr == 0) {
                LOG_WARN(Kernel, "Unresolved external symbol: %s", sym_name.c_str());
                if (HLE::IsStrictImportMode()) {
                    LOG_ERROR(Kernel, "Strict import mode active. Aborting load.");
                    return 0;
                }
            }
            return addr;
        };

        // Process relocation table
        for (const auto& rel : module.relocations) {
            guest_addr_t target_addr = module.base_address + rel.r_offset;
            u32 sym_idx = static_cast<u32>(rel.r_info >> 32);
            u32 rel_type = static_cast<u32>(rel.r_info & 0xFFFFFFFF);

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
                PatchSyscalls(record->module.segments);
                for (const auto& seg : record->module.segments) {
                    if (Memory::Protect(seg.address, seg.size, seg.final_protection) != Memory::Status::Ok) {
                        LOG_WARN(Kernel, "Failed to set final protection for PRX segment at 0x%llx", seg.address);
                    }
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

        // Scan and patch "syscall" instructions to trap them via VEH
        PatchSyscalls(out_module.segments);

        // Apply final page protections for all segments
        for (const auto& seg : out_module.segments) {
            if (Memory::Protect(seg.address, seg.size, seg.final_protection) != Memory::Status::Ok) {
                LOG_WARN(Kernel, "Failed to set final protection for segment at 0x%llx",
                         seg.address);
            }
        }

        return true;
    }

    extern "C" void StartGuest(u64 entry_point, u64 stack_pointer);

    static bool TryStartGuest(guest_addr_t entry_point, guest_addr_t sp) {
        __try {
            StartGuest(entry_point, sp);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            LOG_ERROR(Kernel, "Unhandled hardware exception occurred inside guest execution!");
            return false;
        }
        return true;
    }

    bool Execute(const Loader::LoadedModule& main_module) {
        LOG_INFO(Kernel, "Starting execution of %s at Entry Point: 0x%llx", 
                 main_module.name.c_str(), main_module.entry_point);

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

        bool success = TryStartGuest(main_module.entry_point, sp);

        if (!success) {
            return false;
        }

        LOG_INFO(Kernel, "Guest execution finished cleanly.");
        return true;
    }

    void HandleSyscall(u32 syscall_number, guest_addr_t context) {
        PCONTEXT ctx = reinterpret_cast<PCONTEXT>(context);
        ctx->Rax = HandleSyscall(syscall_number, ctx);
    }

    static bool SafeRead(void* dest, const void* src, size_t size) {
        __try {
            std::memcpy(dest, src, size);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
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
//   EXCEPTION_SINGLE_STEP / everything else -> crash report + terminate.
//
// Policy for future work: if a game is found that depends on guest signal
// delivery, the translation point is here — snapshot the faulting CONTEXT,
// enqueue a guest sigframe on the faulting thread's guest stack, redirect RIP
// to the registered handler, and translate codes: AV->SIGSEGV (read=SEGV_MAPERR/
// write=SEGV_ACCERR), INT 3 (non-syscall-gate)->SIGTRAP, illegal opcode
// (0xC000001D)->SIGILL, FP exceptions (0xC000008E/8F/90..)->SIGFPE.  Until
// then, sys_sigaction/sys_sigprocmask (416/340) succeed silently so games
// that merely *install* handlers continue to run.
// ---------------------------------------------------------------------------
static LONG CALLBACK VectoredExceptionHandler(PEXCEPTION_POINTERS exception_info) {
        PEXCEPTION_RECORD exception_record = exception_info->ExceptionRecord;
        PCONTEXT context = exception_info->ContextRecord;

        // Debug-output exceptions (raised by OutputDebugString, e.g. from the
        // GPU driver during Vulkan init) are informational, not faults.  Pass
        // them through silently: logging the full crash-scan path for these
        // re-faults inside its own probes, which is the recurring
        // first-chance memcpy AV noise seen in debugger output.
        if (exception_record->ExceptionCode == 0x40010006 ||  // DBG_PRINTEXCEPTION_C
            exception_record->ExceptionCode == 0x4001000A) {  // DBG_PRINTEXCEPTION_WIDE_C
            return EXCEPTION_CONTINUE_SEARCH;
        }

        LOG_INFO(Kernel, "VEH Exception Triggered: Code: 0x%X, RIP: 0x%llx, OS Thread: %lu", 
                 exception_record->ExceptionCode, context->Rip, ::GetCurrentThreadId());
 
        if (exception_record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
            u8 instr[16] = {0};
            if (SafeRead(instr, reinterpret_cast<void*>(context->Rip), 16)) {
                char instr_hex[128] = {0};
                for (int i = 0; i < 16; ++i) {
                    sprintf_s(instr_hex + i * 3, sizeof(instr_hex) - i * 3, "%02X ", instr[i]);
                }
                LOG_INFO(Kernel, "  Instruction bytes at crash RIP 0x%llx: %s", context->Rip, instr_hex);
            }
            LOG_INFO(Kernel, "  Access violation details: Type: %s, Address: 0x%llx",
                     exception_record->ExceptionInformation[0] == 0 ? "Read" :
                     exception_record->ExceptionInformation[0] == 1 ? "Write" : "Execute",
                     exception_record->ExceptionInformation[1]);
        }

        if (exception_record->ExceptionCode == EXCEPTION_BREAKPOINT) {
            u8* ip = reinterpret_cast<u8*>(context->Rip);
            u8 sig[2];
            if (ip && SafeRead(sig, ip, 2) && sig[0] == 0xCC && sig[1] == 0x90) {
                u32 syscall_number = static_cast<u32>(context->Rax);
                context->Rax = HandleSyscall(syscall_number, context);

                context->Rip += 2;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }

        if (exception_record->ExceptionCode == STATUS_ACCESS_VIOLATION || exception_record->ExceptionCode == 0xC0000005) {
            LOG_INFO(Kernel, "Parsing instruction for TLS emulation at RIP=0x%llx", context->Rip);
            u8* rip = reinterpret_cast<u8*>(context->Rip);
            u8* instr = rip;
            u8 b = 0;
            
            while (SafeRead(&b, instr, 1) && b == 0x66) {
                instr++;
            }
            
            if (SafeRead(&b, instr, 1) && b == 0x64) {
                instr++;
                
                bool is_64bit = false;
                if (SafeRead(&b, instr, 1) && (b & 0xF0) == 0x40) {
                    is_64bit = (b & 0x08) != 0;
                    instr++;
                } else {
                    LOG_INFO(Kernel, "  No REX prefix, b=0x%x", b);
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
                        
                        if (mod == 0 && rm == 4) {
                            u8 sib = 0;
                            if (SafeRead(&sib, instr, 1)) {
                                instr++;
                                u8 base = sib & 7;
                                if (base == 5) {
                                    if (!SafeRead(&displacement, instr, 4)) parse_success = false;
                                    instr += 4;
                                } else {
                                    LOG_INFO(Kernel, "  SIB base is %d (expected 5)", base);
                                    parse_success = false;
                                }
                            } else {
                                parse_success = false;
                            }
                        } else if (mod == 0 && rm == 5) {
                            if (SafeRead(&displacement, instr, 4)) {
                                instr += 4;
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
                            LOG_INFO(Kernel, "  Unsupported mod=%d, rm=%d", mod, rm);
                            parse_success = false;
                        }
                        
                        if (parse_success) {
                            u32 instr_len = static_cast<u32>(instr - rip);
                            
                            u64 current_tid = GetCurrentThreadId();
                            guest_addr_t tp = 0;
                            {
                                std::lock_guard<std::mutex> lock(g_thread_mutex);
                                auto it = g_threads.find(current_tid);
                                if (it != g_threads.end()) {
                                    tp = it->second.tls_base;
                                }
                            }
                            if (tp == 0) {
                                tp = g_guest_tls.ThreadPointer();
                            }
                            
                            if (opcode == 0x8B) {
                                const u64 access_size = is_64bit ? 8 : 4;
                                LOG_INFO(Kernel, "Emulating TLS read: RIP=0x%llx, displacement=%d, reg=%d, is_64bit=%d, tp=0x%llx",
                                         context->Rip, displacement, reg, is_64bit, tp);
                                if (tp == 0) {
                                    LOG_ERROR(Kernel, "Guest TLS read but no thread pointer configured.");
                                    return EXCEPTION_CONTINUE_SEARCH;
                                }
                                guest_addr_t tls_address = tp + displacement;
                                u64 tls_value = 0;
                                Memory::ReadBuffer(tls_address, &tls_value, access_size);
                                
                                u64* reg_ptr = nullptr;
                                switch (reg) {
                                    case 0: reg_ptr = &context->Rax; break;
                                    case 1: reg_ptr = &context->Rcx; break;
                                    case 2: reg_ptr = &context->Rdx; break;
                                    case 3: reg_ptr = &context->Rbx; break;
                                    case 4: reg_ptr = &context->Rsp; break;
                                    case 5: reg_ptr = &context->Rbp; break;
                                    case 6: reg_ptr = &context->Rsi; break;
                                    case 7: reg_ptr = &context->Rdi; break;
                                }
                                
                                if (reg_ptr) {
                                    if (is_64bit) {
                                        *reg_ptr = tls_value;
                                    } else {
                                        *reg_ptr = (*reg_ptr & 0xFFFFFFFF00000000) | (tls_value & 0xFFFFFFFF);
                                    }
                                    
                                    u64 old_rip = context->Rip;
                                    context->Rip += instr_len;
                                    LOG_INFO(Kernel, "TLS read emulated: RIP 0x%llx -> 0x%llx (len=%d), reg val = 0x%llx, OS Thread: %lu", old_rip, context->Rip, instr_len, *reg_ptr, ::GetCurrentThreadId());
                                    return EXCEPTION_CONTINUE_EXECUTION;
                                }
                            }
                            else if (opcode == 0x89) {
                                u64* reg_ptr = nullptr;
                                switch (reg) {
                                    case 0: reg_ptr = &context->Rax; break;
                                    case 1: reg_ptr = &context->Rcx; break;
                                    case 2: reg_ptr = &context->Rdx; break;
                                    case 3: reg_ptr = &context->Rbx; break;
                                    case 4: reg_ptr = &context->Rsp; break;
                                    case 5: reg_ptr = &context->Rbp; break;
                                    case 6: reg_ptr = &context->Rsi; break;
                                    case 7: reg_ptr = &context->Rdi; break;
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
                                    LOG_INFO(Kernel, "TLS write emulated: RIP 0x%llx -> 0x%llx (len=%d), val = 0x%llx", old_rip, context->Rip, instr_len, tls_value);
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
                                    LOG_INFO(Kernel, "TLS imm write emulated: RIP 0x%llx -> 0x%llx (len=%d), val = 0x%llx", old_rip, context->Rip, instr_len, tls_val);
                                    return EXCEPTION_CONTINUE_EXECUTION;
                                }
                            }
                        }
                    }
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
        if (ip >= 0x800000000 && ip < 0x900000000) {
            FILE* f = nullptr;
            fopen_s(&f, "crash_log.txt", "w");
            if (f) {
                fprintf(f, "GUEST APPLICATION CRASHED!\n");
                fprintf(f, "Exception Code: 0x%X\n", exception_record->ExceptionCode);
                fprintf(f, "Crash Address (RIP): 0x%llx (Offset: 0x%llx)\n", ip, ip - 0x800000000);
                fprintf(f, "Register Dump:\n");
                fprintf(f, "  RAX: 0x%016llx  RBX: 0x%016llx\n", context->Rax, context->Rbx);
                fprintf(f, "  RCX: 0x%016llx  RDX: 0x%016llx\n", context->Rcx, context->Rdx);
                fprintf(f, "  RSI: 0x%016llx  RDI: 0x%016llx\n", context->Rsi, context->Rdi);
                fprintf(f, "  RBP: 0x%016llx  RSP: 0x%016llx\n", context->Rbp, context->Rsp);
                fprintf(f, "  R8:  0x%016llx  R9:  0x%016llx\n", context->R8,  context->R9);
                fprintf(f, "  R10: 0x%016llx  R11: 0x%016llx\n", context->R10, context->R11);
                fprintf(f, "  R12: 0x%016llx  R13: 0x%016llx\n", context->R12, context->R13);
                fprintf(f, "  R14: 0x%016llx  R15: 0x%016llx\n", context->R14, context->R15);
                fclose(f);
            }

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

            GPU::RunIdleLoop();
            ExitProcess(1);
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
            for (u64 sp_scan = context->Rsp;
                 sp_scan < context->Rsp + 0x2000 && sp_scan + sizeof(u64) <= scan_limit;
                 sp_scan += 8) {
                u64 val = 0;
                if (!SafeRead(&val, reinterpret_cast<void*>(sp_scan), 8)) break;
                if (val >= 0x800000000 && val < 0x900000000) {
                    LOG_ERROR(Kernel, "  [RSP+0x%04llx] -> 0x%llx (guest offset 0x%llx)",
                              sp_scan - context->Rsp, val, val - 0x800000000);
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

            FILE* f = nullptr;
            fopen_s(&f, "crash_log.txt", "w");
            if (f) {
                fprintf(f, "VEH Unhandled Exception: Code: 0x%X, RIP: 0x%llx, RSP: 0x%llx\n", 
                        exception_record->ExceptionCode, context->Rip, context->Rsp);
                fprintf(f, "Module: %s (Offset: 0x%llx)\n", module_name, offset);
                fprintf(f, "Register Dump:\n");
                fprintf(f, "  RAX: 0x%016llx  RBX: 0x%016llx\n", context->Rax, context->Rbx);
                fprintf(f, "  RCX: 0x%016llx  RDX: 0x%016llx\n", context->Rcx, context->Rdx);
                fprintf(f, "  RSI: 0x%016llx  RDI: 0x%016llx\n", context->Rsi, context->Rdi);
                fprintf(f, "  RBP: 0x%016llx  RSP: 0x%016llx\n", context->Rbp, context->Rsp);
                fprintf(f, "  R8:  0x%016llx  R9:  0x%016llx\n", context->R8,  context->R9);
                fprintf(f, "  R10: 0x%016llx  R11: 0x%016llx\n", context->R10, context->R11);
                fprintf(f, "  R12: 0x%016llx  R13: 0x%016llx\n", context->R12, context->R13);
                fprintf(f, "  R14: 0x%016llx  R15: 0x%016llx\n", context->R14, context->R15);
                
                void* stack[64];
                USHORT frames = CaptureStackBackTrace(0, 64, stack, nullptr);
                fprintf(f, "Call Stack:\n");
                for (USHORT i = 0; i < frames; ++i) {
                    char symbol_name[MAX_PATH] = "Unknown";
                    HMODULE h_mod_frame = nullptr;
                    u64 offset_frame = 0;
                    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                           reinterpret_cast<LPCSTR>(stack[i]), &h_mod_frame)) {
                        GetModuleFileNameA(h_mod_frame, symbol_name, sizeof(symbol_name));
                        offset_frame = reinterpret_cast<u64>(stack[i]) - reinterpret_cast<u64>(h_mod_frame);
                    }
                    fprintf(f, "  [%02d] %s + 0x%llx (Address: 0x%llx)\n", i, symbol_name, offset_frame, reinterpret_cast<u64>(stack[i]));
                }
                
                // Scan the guest stack for values that look like guest return
                // addresses (guest modules live at [0x800000000, 0x900000000)).
                // This reveals the guest call chain when execution ended up in
                // host code with a guest RSP.  Same stack-limit cap as above:
                // probing past the top re-faults inside SafeRead's memcpy.
                fprintf(f, "Guest stack scan (potential return addresses):\n");
                for (u64 sp_scan = context->Rsp;
                     sp_scan < context->Rsp + 0x2000 && sp_scan + sizeof(u64) <= scan_limit;
                     sp_scan += 8) {
                    u64 val = 0;
                    if (!SafeRead(&val, reinterpret_cast<void*>(sp_scan), 8)) break;
                    if (val >= 0x800000000 && val < 0x900000000) {
                        fprintf(f, "  [RSP+0x%04llx] -> 0x%llx (guest offset 0x%llx)\n",
                                sp_scan - context->Rsp, val, val - 0x800000000);
                    }
                }

                fclose(f);
            }
            LOG_ERROR(Kernel, "VEH Unhandled Exception: Code: 0x%X, RIP: 0x%llx, Module: %s, Offset: 0x%llx", 
                      exception_record->ExceptionCode, context->Rip, module_name, offset);

            // Also log recent HLE import calls so the crash can be correlated
            // with the last guest->host transitions.
            auto trace = HLE::GetImportTrace(16);
            for (const auto& te : trace) {
                LOG_ERROR(Kernel, "  HLE trace: %s::%s (id=%llu) from guest RIP 0x%llx args=(0x%llx, 0x%llx, 0x%llx, 0x%llx)",
                          te.module_name.c_str(), te.name.c_str(), te.symbol_id,
                          te.caller_rip, te.arg1, te.arg2, te.arg3, te.arg4);
            }
        }

        return EXCEPTION_CONTINUE_SEARCH;
    }

    void RegisterThread(const ThreadContext& context) {
        std::lock_guard<std::mutex> lock(g_thread_mutex);
        g_threads[context.thread_id] = context;
        LOG_INFO(Kernel, "Registered thread '%s' (id=%llu, entry=0x%llx, stack=0x%llx, stack_size=%llu, tls=0x%llx)",
                 context.name.c_str(), context.thread_id, context.entry_point,
                 context.stack_base, context.stack_size, context.tls_base);
    }
}
// namespace Kernel
