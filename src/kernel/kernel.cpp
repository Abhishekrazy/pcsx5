#include "kernel.h"
#include "fd_table.h"
#include "memory.h"
#include "syscalls.h"
#include "../memory/memory.h"
#include "../hle/hle.h"
#include "../common/log.h"
#include "../gpu/gpu.h"
#include <windows.h>
#include <iostream>
#include <cstring>
#include <unordered_map>
#include <mutex>
#include <vector>
#include <string>

// Windows-compatible replacements for Unix headers (minimal set for kernel.cpp)
#ifndef KERNEL_UNIX_COMPAT_H
#define KERNEL_UNIX_COMPAT_H

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#endif // KERNEL_UNIX_COMPAT_H

extern "C" u64 g_host_stack_pointer = 0;

namespace Kernel {

    // Global state
    static std::unordered_map<u64, ThreadContext> g_threads;
    static std::mutex g_thread_mutex;
    static u64 g_process_id = GetCurrentProcessId();
    static PVOID g_veh_handler = nullptr;
    static LPTOP_LEVEL_EXCEPTION_FILTER g_prev_exception_filter = nullptr;
    static GuestTlsContext g_guest_tls;

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

    static bool LinkModule(Loader::LoadedModule& module) {
        LOG_INFO(Kernel, "Linking module %s at base address 0x%llx...", module.name.c_str(), module.base_address);

        auto resolve_external = [&](const std::string& sym_name) -> guest_addr_t {
            guest_addr_t addr = HLE::ResolveAny(sym_name);
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
                    Memory::Write<u64>(target_addr, module.base_address + static_cast<u64>(rel.r_addend));
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

    bool LoadModule(const std::string& filepath, Loader::LoadedModule& out_module) {
        if (!Loader::Load(filepath, out_module)) {
            return false;
        }

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

    static LONG CALLBACK VectoredExceptionHandler(PEXCEPTION_POINTERS exception_info) {
        PEXCEPTION_RECORD exception_record = exception_info->ExceptionRecord;
        PCONTEXT context = exception_info->ContextRecord;

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
                        }
                        
                        if (parse_success) {
                            u32 instr_len = static_cast<u32>(instr - rip);
                            
                            if (opcode == 0x8B) {
                                const u64 access_size = is_64bit ? 8 : 4;
                                guest_addr_t tls_address = 0;
                                if (!g_guest_tls.Translate(displacement, access_size, tls_address)) {
                                    LOG_ERROR(Kernel, "Guest TLS read is outside the current thread context (offset: %d, size: %llu).", displacement, access_size);
                                    return EXCEPTION_CONTINUE_SEARCH;
                                }
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
                                    
                                    context->Rip += instr_len;
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
                                    guest_addr_t tls_address = 0;
                                    if (!g_guest_tls.Translate(displacement, access_size, tls_address)) {
                                        LOG_ERROR(Kernel, "Guest TLS write is outside the current thread context (offset: %d, size: %llu).", displacement, access_size);
                                        return EXCEPTION_CONTINUE_SEARCH;
                                    }
                                    Memory::WriteBuffer(tls_address, &tls_value, access_size);
                                    
                                    context->Rip += instr_len;
                                    return EXCEPTION_CONTINUE_EXECUTION;
                                }
                            }
                            else if (opcode == 0xC7) {
                                u32 imm_value = 0;
                                if (SafeRead(&imm_value, instr, 4)) {
                                    instr += 4;
                                    instr_len = static_cast<u32>(instr - rip);
                                    
                                    const u64 access_size = is_64bit ? 8 : 4;
                                    guest_addr_t tls_address = 0;
                                    if (!g_guest_tls.Translate(displacement, access_size, tls_address)) {
                                        LOG_ERROR(Kernel, "Guest TLS write is outside the current thread context (offset: %d, size: %llu).", displacement, access_size);
                                        return EXCEPTION_CONTINUE_SEARCH;
                                    }
                                    u64 tls_val = imm_value;
                                    Memory::WriteBuffer(tls_address, &tls_val, access_size);
                                    
                                    context->Rip += instr_len;
                                    return EXCEPTION_CONTINUE_EXECUTION;
                                }
                            }
                        }
                    }
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
                
                fclose(f);
            }
            LOG_ERROR(Kernel, "VEH Unhandled Exception: Code: 0x%X, RIP: 0x%llx, Module: %s, Offset: 0x%llx", 
                      exception_record->ExceptionCode, context->Rip, module_name, offset);
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
