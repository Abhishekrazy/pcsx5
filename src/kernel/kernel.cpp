#include "kernel.h"
#include "../memory/memory.h"
#include "../hle/hle.h"
#include "../common/log.h"
#include "../gpu/gpu.h"
#include <windows.h>
#include <iostream>
#include <cstring>

extern "C" u64 g_host_stack_pointer = 0;

namespace Kernel {

    static std::unordered_map<u64, ThreadContext> g_threads;
    static PVOID g_veh_handler = nullptr;
    static LPTOP_LEVEL_EXCEPTION_FILTER g_prev_exception_filter = nullptr;
    static GuestTlsContext g_guest_tls;

    // Forward declaration of the VEH callback
    static LONG CALLBACK VectoredExceptionHandler(PEXCEPTION_POINTERS exception_info);

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
        guest_addr_t tls_alloc = Memory::Map(0, tls_total_size, Memory::PROT_READ | Memory::PROT_WRITE);
        if (!tls_alloc) {
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

            u8* ptr = reinterpret_cast<u8*>(seg.address);
            u64 size = seg.size;

            for (u64 i = 0; i < size - 1; ++i) {
                if (ptr[i] == 0x0F && ptr[i + 1] == 0x05) {
                    // Change memory protection to writable temporarily
                    DWORD old_protect;
                    VirtualProtect(ptr + i, 2, PAGE_EXECUTE_READWRITE, &old_protect);

                    // Patch to INT 3 (0xCC) + NOP (0x90)
                    ptr[i] = 0xCC;
                    ptr[i + 1] = 0x90;

                    // Restore original protection
                    VirtualProtect(ptr + i, 2, old_protect, &old_protect);
                    patched_count++;
                }
            }
        }

        if (patched_count > 0) {
            LOG_INFO(Kernel, "Patched %llu 'syscall' instructions in module executable segments.", patched_count);
        }
    }

    static bool LinkModule(Loader::LoadedModule& module) {
        LOG_INFO(Kernel, "Linking module: %s...", module.name.c_str());

        u64 resolved_count    = 0;
        u64 unresolved_count  = 0;
        u64 relative_count    = 0;
        bool hard_failure     = false;

        // Helper: resolve an external symbol by name, searching all registered HLE modules
        auto resolve_external = [&](const std::string& sym_name) -> guest_addr_t {
            guest_addr_t addr = HLE::ResolveAny(sym_name);
            if (addr != 0) {
                ++resolved_count;
            } else {
                ++unresolved_count;
                LOG_DEBUG(Kernel, "  Unresolved external symbol: %s", sym_name.c_str());
                if (HLE::IsStrictImportMode()) {
                    // In strict-import mode any unresolved symbol is a hard error
                    // so the test harness can detect missing handlers deterministically.
                    hard_failure = true;
                }
            }
            return addr;
        };

        // ── Standard RELA relocations ────────────────────────────────────────
        for (const auto& rel : module.relocations) {
            u32 sym_idx  = static_cast<u32>(ELF64_R_SYM(rel.r_info));
            u32 rel_type = static_cast<u32>(ELF64_R_TYPE(rel.r_info));
            guest_addr_t target_addr = module.base_address + rel.r_offset;

            guest_addr_t resolved_addr = 0;
            if (sym_idx != 0 && sym_idx < module.symbols.size()) {
                const auto& sym = module.symbols[sym_idx];
                if (sym.st_name < module.string_table.size()) {
                    std::string sym_name = &module.string_table[sym.st_name];

                    if (sym.st_shndx != 0 || sym.st_value != 0) {
                        // Internal symbol
                        resolved_addr = module.base_address + sym.st_value;
                    } else {
                        // External symbol — search all registered HLE modules
                        resolved_addr = resolve_external(sym_name);
                    }
                }
            }

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
                    ++relative_count;
                    break;
                default:
                    LOG_DEBUG(Kernel, "Unsupported relocation type %u at 0x%llx", rel_type, target_addr);
                    break;
            }
        }

        // ── PLT / Jump-slot relocations ──────────────────────────────────────
        for (const auto& rel : module.plt_relocations) {
            u32 sym_idx  = static_cast<u32>(ELF64_R_SYM(rel.r_info));
            u32 rel_type = static_cast<u32>(ELF64_R_TYPE(rel.r_info));
            guest_addr_t target_addr = module.base_address + rel.r_offset;

            if (sym_idx != 0 && sym_idx < module.symbols.size()) {
                const auto& sym = module.symbols[sym_idx];
                if (sym.st_name < module.string_table.size()) {
                    std::string sym_name = &module.string_table[sym.st_name];
                    guest_addr_t resolved_addr = resolve_external(sym_name);

                    if (rel_type == Loader::R_X86_64_JUMP_SLOT) {
                        Memory::Write<u64>(target_addr, resolved_addr);
                    } else {
                        LOG_DEBUG(Kernel, "Unexpected PLT reloc type %u for symbol %s", rel_type, sym_name.c_str());
                    }
                }
            }
        }

        LOG_INFO(Kernel, "Linking done: %llu RELATIVE, %llu resolved, %llu unresolved external.",
                 relative_count, resolved_count, unresolved_count);

        if (hard_failure) {
            LOG_ERROR(Kernel, "Strict-import mode: aborting link due to %llu unresolved import(s).",
                      unresolved_count);
            return false;
        }
        return true;
    }

    // Scan the loaded symbol table for a symbol by plain name (e.g. "main").
    // Returns base_address + st_value if found, else 0.
    static guest_addr_t FindSymbolByName(const Loader::LoadedModule& module, const char* name) {
        for (const auto& sym : module.symbols) {
            if (sym.st_name >= module.string_table.size()) continue;
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

        // ── Locate main() for XKRegsFpEpk ─────────────────────────────────────
        // MUST happen BEFORE PatchSyscalls (which overwrites code bytes we scan).
        // Stage 1: ELF dynamic symbol table scan ("main")
        {
            guest_addr_t main_va = FindSymbolByName(out_module, "main");

            if (!main_va) {
                // Stage 2: strtab linear scan for "main\0" byte sequence
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
                // Stage 3: Disassemble _start to find XKRegsFpEpk PLT body,
                //          then scan ITS body for a call to a function-looking address.
                //
                // _start + 0x43 calls the PLT stub for XKRegsFpEpk.
                // The PLT stub at that address (before GOT patching) is the native body.
                // Since we link BEFORE this scan, the GOT was patched; but the PLT CODE
                // at the call target still starts with E8-based internal calls.
                //
                // Walk _start to find the E8 call at offset 0x43, get the PLT va,
                // then look at the next E8 there that hits a function prologue in code.

                guest_addr_t entry_va = out_module.entry_point;
                constexpr u64 MAX_SCAN = 512;
                u8 entry_bytes[MAX_SCAN] = {};
                Memory::ReadBuffer(entry_va, entry_bytes, MAX_SCAN);

                // Identify code segment range
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
                LOG_DEBUG(Kernel, "  entry+0x43 byte = 0x%02X", entry_bytes[0x43]);

                // Scan entire _start for E8 calls; for each one find its target (the PLT),
                // then scan the PLT body for calls into the code segment with fn prologue.
                for (u64 s = 0x20; s < 0x80 && !main_va; ++s) {
                    if (entry_bytes[s] != 0xE8) continue;
                    s32 rel = *reinterpret_cast<s32*>(&entry_bytes[s+1]);
                    guest_addr_t plt_va = entry_va + s + 5 + static_cast<s64>(rel);
                    if (plt_va < code_start || plt_va >= code_end) continue;

                    LOG_DEBUG(Kernel, "  _start+0x%llx: E8 call -> PLT VA 0x%llx", s, plt_va);

                    // Read the PLT/function body
                    u8 xk_bytes[MAX_SCAN] = {};
                    Memory::ReadBuffer(plt_va, xk_bytes, MAX_SCAN);

                    // Scan this body for calls to code-segment addresses with fn prologues
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

                        LOG_DEBUG(Kernel, "    body+0x%llx: call -> 0x%llx, prologue=%02X%02X%02X%02X fn=%d",
                                  i, target, prologue[0], prologue[1], prologue[2], prologue[3], (int)looks_like_fn);

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
            Memory::Protect(seg.address, seg.size, seg.final_protection);
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

        // Allocate a dedicated 1 MB guest stack
        u64 stack_size = 1024 * 1024;
        guest_addr_t stack_base = Memory::Map(0, stack_size, Memory::PROT_READ | Memory::PROT_WRITE);
        if (!stack_base) {
            LOG_ERROR(Kernel, "Failed to allocate guest stack.");
            return false;
        }
        
        // Stack grows downwards, start from the top, subtract space for arguments and align to 16 bytes
        guest_addr_t sp = ALIGN_DOWN(stack_base + stack_size, 16) - 256;

        // Set up the guest stack frame (FreeBSD x86_64 ABI layout) on the dedicated guest stack!
        // Write the program name string at offset sp + 64
        std::string prog_name = main_module.name;
        std::memcpy(reinterpret_cast<void*>(sp + 64), prog_name.c_str(), prog_name.size() + 1);

        Memory::Write<u64>(sp, 1);                    // argc = 1
        Memory::Write<u64>(sp + 8, sp + 64);          // argv[0] -> pointer to prog_name string
        Memory::Write<u64>(sp + 16, 0);               // argv[1] = NULL (end of argv)
        Memory::Write<u64>(sp + 24, 0);               // envp[0] = NULL (end of envp)
        Memory::Write<u64>(sp + 32, 0);               // AT_NULL tag = 0
        Memory::Write<u64>(sp + 40, 0);               // AT_NULL val = 0

        LOG_INFO(Kernel, "Guest stack frame configured on dedicated stack at sp = 0x%llx", sp);

        // Run the entry point natively using our assembly bridge stack switcher
        // We wrap in structured exception handling (SEH) in a sub-function to prevent C2712.
        bool success = TryStartGuest(main_module.entry_point, sp);

        if (!success) {
            return false;
        }

        LOG_INFO(Kernel, "Guest execution finished cleanly.");
        return true;
    }

    void HandleSyscall(u32 syscall_number, PCONTEXT context) {
        // System V Syscall argument registers:
        // Rax = syscall number
        // Rdi = arg1, Rsi = arg2, Rdx = arg3, R10 = arg4, R8 = arg5, R9 = arg6
        u64 arg1 = context->Rdi;
        u64 arg2 = context->Rsi;
        u64 arg3 = context->Rdx;
        u64 arg4 = context->R10;
        u64 arg5 = context->R8;
        u64 arg6 = context->R9;

        LOG_DEBUG(Kernel, "Intercepted Syscall #%u (Args: 0x%llx, 0x%llx, 0x%llx, 0x%llx, 0x%llx, 0x%llx)", 
                  syscall_number, arg1, arg2, arg3, arg4, arg5, arg6);

        u64 result = 0;

        switch (syscall_number) {
            case 1: // sys_exit
                LOG_INFO(Kernel, "Syscall: sys_exit called with code %lld. Terminating process.", arg1);
                ExitProcess((UINT)arg1);
                break;

            case 4: // sys_write
                // arg1 = fd, arg2 = buffer pointer, arg3 = length
                if (arg1 == 1 || arg1 == 2) { // stdout or stderr
                    const char* buf = reinterpret_cast<const char*>(arg2);
                    std::string str(buf, arg3);
                    std::cout << "[GUEST STDOUT] " << str;
                    std::flush(std::cout);
                    result = arg3; // Number of bytes written
                } else {
                    LOG_WARN(Kernel, "sys_write: Unimplemented write to fd %lld", arg1);
                    result = static_cast<u64>(-1);
                }
                break;

            default:
                LOG_WARN(Kernel, "Syscall #%u is unimplemented! Returning 0.", syscall_number);
                result = 0;
                break;
        }

        // Set return value in Rax
        context->Rax = result;
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
            // Check if this breakpoint is our patched syscall (0xCC 0x90)
            u8* ip = reinterpret_cast<u8*>(context->Rip);
            u8 sig[2];
            if (ip && SafeRead(sig, ip, 2) && sig[0] == 0xCC && sig[1] == 0x90) {
                // This is an intercepted guest system call!
                // Read syscall number from Rax
                u32 syscall_number = static_cast<u32>(context->Rax);
                HandleSyscall(syscall_number, context);

                // Skip the INT 3 and NOP (2 bytes total) to resume execution after the syscall
                context->Rip += 2;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }

        // Handle FS segment register overrides (Thread Local Storage accesses)
        if (exception_record->ExceptionCode == STATUS_ACCESS_VIOLATION || exception_record->ExceptionCode == 0xC0000005) {
            u8* rip = reinterpret_cast<u8*>(context->Rip);
            u8* instr = rip;
            u8 b = 0;
            
            // Skip size override prefixes safely
            while (SafeRead(&b, instr, 1) && b == 0x66) {
                instr++;
            }
            
            // Check if FS override is present
            if (SafeRead(&b, instr, 1) && b == 0x64) {
                instr++; // Skip 0x64
                
                // Parse REX prefix if present (0x40 - 0x4F)
                bool is_64bit = false;
                if (SafeRead(&b, instr, 1) && (b & 0xF0) == 0x40) {
                    is_64bit = (b & 0x08) != 0; // Check REX.W
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
                        
                        // Parse ModR/M and SIB to extract displacement
                        if (mod == 0 && rm == 4) { // SIB byte present
                            u8 sib = 0;
                            if (SafeRead(&sib, instr, 1)) {
                                instr++;
                                u8 base = sib & 7;
                                if (base == 5) { // 32-bit displacement only
                                    if (!SafeRead(&displacement, instr, 4)) parse_success = false;
                                    instr += 4;
                                }
                            } else {
                                parse_success = false;
                            }
                        } else if (mod == 0 && rm == 5) { // 32-bit displacement only
                            if (SafeRead(&displacement, instr, 4)) {
                                instr += 4;
                            } else {
                                parse_success = false;
                            }
                        } else if (mod == 1) { // 8-bit displacement
                            if (rm == 4) { // SIB byte
                                instr++; // Skip SIB
                            }
                            s8 disp8 = 0;
                            if (SafeRead(&disp8, instr, 1)) {
                                displacement = static_cast<s32>(disp8);
                                instr += 1;
                            } else {
                                parse_success = false;
                            }
                        } else if (mod == 2) { // 32-bit displacement
                            if (rm == 4) { // SIB byte
                                instr++; // Skip SIB
                            }
                            if (SafeRead(&displacement, instr, 4)) {
                                instr += 4;
                            } else {
                                parse_success = false;
                            }
                        }
                        
                        if (parse_success) {
                            u32 instr_len = static_cast<u32>(instr - rip);
                            
                            // Emulate: mov reg, fs:[displacement]
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
                                    
                                    std::string bytes_str;
                                    for (u32 i = 0; i < instr_len && i < 16; ++i) {
                                        char tmp[8];
                                        u8 b_val = 0;
                                        if (SafeRead(&b_val, rip + i, 1)) {
                                            sprintf_s(tmp, "%02X ", b_val);
                                            bytes_str += tmp;
                                        }
                                    }
                                    LOG_DEBUG(Kernel, "Emulated TLS Read: mov %s, fs:[0x%X] -> Value: 0x%llx (Len: %u, Bytes: %s)",
                                              (reg == 0 ? "RAX" : (reg == 1 ? "RCX" : (reg == 2 ? "RDX" : (reg == 3 ? "RBX" : (reg == 6 ? "RSI" : (reg == 7 ? "RDI" : "REG")))))),
                                              displacement, tls_value, instr_len, bytes_str.c_str());
                                    
                                    context->Rip += instr_len;
                                    return EXCEPTION_CONTINUE_EXECUTION;
                                }
                            }
                            // Emulate: mov fs:[displacement], reg
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
                                    
                                    LOG_DEBUG(Kernel, "Emulated TLS Write: mov fs:[0x%X], %s -> Value: 0x%llx (Len: %u)",
                                              displacement, (reg == 0 ? "RAX" : (reg == 1 ? "RCX" : (reg == 2 ? "RDX" : (reg == 3 ? "RBX" : (reg == 6 ? "RSI" : (reg == 7 ? "RDI" : "REG")))))),
                                              tls_value, instr_len);
                                    
                                    context->Rip += instr_len;
                                    return EXCEPTION_CONTINUE_EXECUTION;
                                }
                            }
                            // Emulate: mov fs:[displacement], imm
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
                                    
                                    LOG_DEBUG(Kernel, "Emulated TLS Write (Imm): mov fs:[0x%X], 0x%X (Len: %u)",
                                              displacement, imm_value, instr_len);
                                              
                                    context->Rip += instr_len;
                                    return EXCEPTION_CONTINUE_EXECUTION;
                                }
                            }
                        }
                    }
                }
            }
        }

        // Intercept guest crashes (Access Violation, Illegal Instruction, Stack Overflow, etc.) 
        // to prevent silent exit or default Windows dialog box.
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

            // Keep window alive so the user can see the final frame state before closing
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
}
// namespace Kernel
