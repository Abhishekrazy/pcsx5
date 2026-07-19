#include "elf.h"
#include "../memory/memory.h"
#include "../common/log.h"
#include <fstream>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <limits>
#include <filesystem>

namespace Loader {

    bool Load(const std::string& filepath, LoadedModule& out_module) {
        LOG_INFO(Loader, "Loading ELF binary: %s", filepath.c_str());

        // Dispatch through the SELF container parser when the file starts
        // with one of Sony's SELF magics (see elf.h::IsSelfMagic).  The
        // extracted inner ELF is materialised to a temp file and then run
        // through the regular ELF64 loader.
        if (IsSelfFile(filepath)) {
            return LoadSelf(filepath, out_module);
        }

        std::ifstream file(filepath, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            LOG_ERROR(Loader, "Failed to open ELF file: %s", filepath.c_str());
            return false;
        }

        std::streamsize file_size = file.tellg();
        file.seekg(0, std::ios::beg);

        if (file_size < sizeof(Elf64_Ehdr)) {
            LOG_ERROR(Loader, "File too small to contain ELF64 Header: %s", filepath.c_str());
            return false;
        }

        // Read ELF header
        Elf64_Ehdr ehdr;
        file.read(reinterpret_cast<char*>(&ehdr), sizeof(Elf64_Ehdr));

        // Verify ELF Magic
        if (ehdr.e_ident[0] != ELFMAG0 || ehdr.e_ident[1] != ELFMAG1 ||
            ehdr.e_ident[2] != ELFMAG2 || ehdr.e_ident[3] != ELFMAG3) {
            LOG_ERROR(Loader, "Invalid ELF magic for file: %s", filepath.c_str());
            return false;
        }

        // Verify 64-bit architecture
        if (ehdr.e_ident[4] != 2) { // 2 = ELFCLASS64
            LOG_ERROR(Loader, "Only 64-bit ELF binaries are supported.");
            return false;
        }

        // Verify machine type is x86-64 (0x3E = EM_X86_64)
        if (ehdr.e_machine != 0x3E) {
            LOG_ERROR(Loader, "Invalid machine architecture: 0x%X (expected x86-64).", ehdr.e_machine);
            return false;
        }

        LOG_INFO(Loader, "Valid ELF64 x86-64 binary detected. Entry Point: 0x%llx", ehdr.e_entry);

        // Record e_type and PIE flag in the module before any other state is
        // mutated so downstream code (including ParseModuleMetadata) can use
        // it.  PS5 SDK module types (0xFE00..0xFEFF) are accepted and treated
        // as PIE-style dynamic modules, the same as ET_DYN with a base of 0.
        out_module.e_type = ehdr.e_type;
        out_module.is_pie = (ehdr.e_type == ET_DYN) || IsPs5ModuleType(ehdr.e_type);
        if (IsPs5ModuleType(ehdr.e_type)) {
            LOG_INFO(Loader, "PS5 SDK module type 0x%04X detected; treating as PIE.",
                     ehdr.e_type);
        }

        // Read program headers. Validate the table before seeking or allocating so
        // malformed inputs cannot make the loader read outside the file.
        if (ehdr.e_phnum == 0) {
            LOG_ERROR(Loader, "No program headers found in ELF.");
            return false;
        }
        if (ehdr.e_phentsize != sizeof(Elf64_Phdr)) {
            LOG_ERROR(Loader, "Unsupported program-header entry size: %u.", ehdr.e_phentsize);
            return false;
        }

        const u64 file_size_u64 = static_cast<u64>(file_size);
        if (ehdr.e_phoff > file_size_u64 ||
            ehdr.e_phnum > (file_size_u64 - ehdr.e_phoff) / sizeof(Elf64_Phdr)) {
            LOG_ERROR(Loader, "Program-header table is outside the ELF file.");
            return false;
        }

        std::vector<Elf64_Phdr> phdrs(ehdr.e_phnum);
        file.seekg(ehdr.e_phoff, std::ios::beg);
        file.read(reinterpret_cast<char*>(phdrs.data()), static_cast<std::streamsize>(ehdr.e_phnum * sizeof(Elf64_Phdr)));
        if (!file) {
            LOG_ERROR(Loader, "Failed to read the complete program-header table.");
            return false;
        }

        // Validate all file-backed ranges and PT_LOAD address ranges before
        // reserving guest memory or copying any segment data.
        int inner_idx = 0;
        for (const auto& phdr : phdrs) {
            LOG_INFO(Loader, "  Inner Phdr[%d]: type=%u, p_offset=0x%llx, p_filesz=0x%llx, file_size=0x%llx",
                     inner_idx++, phdr.p_type,
                     static_cast<unsigned long long>(phdr.p_offset),
                     static_cast<unsigned long long>(phdr.p_filesz),
                     static_cast<unsigned long long>(file_size_u64));
            if (phdr.p_offset > file_size_u64 || phdr.p_filesz > file_size_u64 - phdr.p_offset) {
                LOG_ERROR(Loader, "Segment file range is outside the ELF file.");
                return false;
            }
            if (phdr.p_type == PT_LOAD) {
                if (phdr.p_filesz > phdr.p_memsz) {
                    LOG_ERROR(Loader, "PT_LOAD file size exceeds memory size.");
                    return false;
                }
                if (phdr.p_vaddr > std::numeric_limits<u64>::max() - phdr.p_memsz) {
                    LOG_ERROR(Loader, "PT_LOAD virtual address range overflows.");
                    return false;
                }
            }
            // Sony PS5 SDK p_types (PT_SCE_*, PT_GNU_RELRO, etc.) carry
            // no PT_LOAD data, so we don't need to validate them.  A
            // debug log makes it easy to see what extensions the module
            // is using.
            if (IsPs5SegmentType(phdr.p_type)) {
                LOG_DEBUG(Loader, "Skipping PS5 SDK segment p_type=0x%08X "
                          "p_vaddr=0x%llx p_filesz=%llu p_memsz=%llu",
                          phdr.p_type,
                          (unsigned long long)phdr.p_vaddr,
                          (unsigned long long)phdr.p_filesz,
                          (unsigned long long)phdr.p_memsz);
            }
        }

        // Determine base address mapping requirements and total size
        u64 min_vaddr = ~0ULL;
        u64 max_vaddr = 0;
        bool has_load_segments = false;

        for (const auto& phdr : phdrs) {
            if (phdr.p_type == PT_LOAD) {
                min_vaddr = std::min(min_vaddr, phdr.p_vaddr);
                max_vaddr = std::max(max_vaddr, phdr.p_vaddr + phdr.p_memsz);
                has_load_segments = true;
            }
        }

        if (!has_load_segments) {
            LOG_ERROR(Loader, "No PT_LOAD segments found in ELF.");
            return false;
        }

        // Round min_vaddr down and max_vaddr up to Page Size
        u64 load_base = ALIGN_DOWN(min_vaddr, PAGE_SIZE);
        u64 load_limit = ALIGN_UP(max_vaddr, PAGE_SIZE);
        u64 total_size = load_limit - load_base;

        LOG_INFO(Loader, "Required memory footprint: [0x%llx - 0x%llx] (size: %llu bytes)", 
                 load_base, load_limit, total_size);

        // For PS5, if base vaddr is 0 (PIE binary), we assign a base guest address.
        // Let's choose 0x800000000 (32GB line) for dynamic base mapping.
        constexpr guest_addr_t kPieBaseHint    = 0x800000000;
        constexpr guest_addr_t kGuestWindowEnd = 0x900000000; // guest modules live in [kPieBaseHint, kGuestWindowEnd)
        constexpr guest_addr_t kPieBaseStep    = 0x10000000;  // 256 MB between module base candidates
        guest_addr_t base_address = 0;
        if (load_base == 0) {
            base_address = kPieBaseHint;
            LOG_INFO(Loader, "Position Independent Executable (PIE) detected. Mapping at base base: 0x%llx", base_address);
        } else {
            base_address = 0; // Loaded at their absolute addresses
            LOG_INFO(Loader, "Fixed-position executable detected. Mapping at absolute addresses.");
        }

        out_module.base_address = base_address;
        out_module.image_size = total_size;
        out_module.entry_point = base_address + ehdr.e_entry;

        // Reserve the entire virtual address range for the image first
        guest_addr_t reserved_base = 0;
        if (Memory::Reserve(base_address + load_base, total_size, &reserved_base) != Memory::Status::Ok) {
            // The preferred base is taken (e.g. the main module already sits
            // at 0x800000000).  PIE modules are relocatable: walk upward in
            // the guest window until a free range is found so that several
            // PRX modules can coexist with the main module.
            bool reserved = false;
            if (load_base == 0) {
                for (guest_addr_t hint = kPieBaseHint + kPieBaseStep;
                     hint + total_size < kGuestWindowEnd;
                     hint += kPieBaseStep) {
                    if (Memory::Reserve(hint, total_size, &reserved_base) == Memory::Status::Ok) {
                        LOG_INFO(Loader, "Preferred PIE base busy; relocated module to guest base 0x%llx", hint);
                        reserved = true;
                        break;
                    }
                }
            }
            if (!reserved) {
                LOG_ERROR(Loader, "Failed to reserve virtual address range for the module (size: %llu)", total_size);
                return false;
            }
        }
        
        // Update base address if OS allocated dynamically (PIE)
        if (out_module.is_pie) {
            base_address = reserved_base - load_base;
            out_module.base_address = base_address;
            out_module.entry_point = base_address + ehdr.e_entry;
        }

        // Map segments
        for (const auto& phdr : phdrs) {
            if (phdr.p_type != PT_LOAD) continue;

            guest_addr_t seg_start = base_address + phdr.p_vaddr;
            u64 seg_size = phdr.p_memsz;

            // Some PS5 SDK ELFs (notably e_type 0xFE10) carry PT_LOAD
            // segments whose p_vaddr is NOT 16 KB-page-aligned (the
            // segment sits in the middle of a page alongside a sibling
            // segment).  Memory::Commit / VirtualAlloc require
            // page-aligned addresses, so we:
            //   1. round the start *down* to the page boundary,
            //   2. grow the size to cover the full unaligned range,
            //   3. keep the unaligned `seg_start` for data copy so the
            //      bytes land at the correct vaddr.
            const u64 unaligned_offset = seg_start & (PAGE_SIZE - 1);
            const guest_addr_t commit_start = seg_start - unaligned_offset;
            const u64 commit_size = (unaligned_offset + seg_size + PAGE_SIZE - 1)
                                    & ~(u64)(PAGE_SIZE - 1);

            // Translate protection flags (PF_X=1, PF_W=2, PF_R=4)
            u32 final_protection = Memory::PROT_NONE;
            if (phdr.p_flags & 4) final_protection |= Memory::PROT_READ;
            if (phdr.p_flags & 2) final_protection |= Memory::PROT_WRITE;
            if (phdr.p_flags & 1) {
                final_protection |= Memory::PROT_EXEC;
                final_protection |= Memory::PROT_READ; // Always allow reading code segments on host
            }

            // Default zero-flag PT_LOAD segments to Read+Write (representing BSS/global variables)
            if (final_protection == Memory::PROT_NONE) {
                final_protection = Memory::PROT_READ | Memory::PROT_WRITE;
            }

            // Map memory region with temporary Read+Write (and Exec if final is Exec) so we can write and link
            u32 map_protection = Memory::PROT_READ | Memory::PROT_WRITE;
            if (final_protection & Memory::PROT_EXEC) {
                map_protection |= Memory::PROT_EXEC;
            }

            if (Memory::Commit(commit_start, commit_size, map_protection) != Memory::Status::Ok) {
                LOG_ERROR(Loader, "Failed to commit segment at 0x%llx (size: %llu)",
                          commit_start, commit_size);
                return false;
            }

            // Save mapped segment info for post-link protection configuration
            MappedSegment seg;
            seg.address = seg_start;
            seg.size = seg_size;
            seg.final_protection = final_protection;
            // Full ELF program header fields (for boot parser / debugging)
            seg.type = phdr.p_type;
            seg.file_offset = phdr.p_offset;
            seg.file_size = phdr.p_filesz;
            seg.mem_size = phdr.p_memsz;
            seg.vaddr = phdr.p_vaddr;
            seg.flags = phdr.p_flags;
            out_module.segments.push_back(seg);

            // Always log the phdr fields we are about to use so a
            // p_filesz=0 anomaly is visible even when the data copy is
            // skipped.
            LOG_INFO(
                Loader,
                "PT_LOAD p_offset=0x%llx p_vaddr=0x%llx "
                "p_filesz=%llu p_memsz=%llu sizeof(Elf64_Phdr)=%zu",
                (unsigned long long)phdr.p_offset,
                (unsigned long long)phdr.p_vaddr,
                (unsigned long long)phdr.p_filesz,
                (unsigned long long)phdr.p_memsz,
                sizeof(Elf64_Phdr)
            );

            // Copy data from file
            if (phdr.p_filesz > 0) {
                file.seekg(phdr.p_offset, std::ios::beg);
                std::vector<u8> seg_data(phdr.p_filesz);
                file.read(reinterpret_cast<char*>(seg_data.data()), phdr.p_filesz);

                // Debug print first 16 bytes in hex to inspect segment content
                char hex_buf[128] = {0};
                u64 bytes_to_print = phdr.p_filesz > 16 ? 16 : phdr.p_filesz;
                for (u64 idx = 0; idx < bytes_to_print; ++idx) {
                    sprintf_s(hex_buf + idx * 3, sizeof(hex_buf) - idx * 3, "%02X ", seg_data[idx]);
                }
                LOG_DEBUG(Loader, "Segment p_offset: 0x%llx, VAddr: 0x%llx, First bytes: %s",
                          phdr.p_offset, seg_start, hex_buf);

                Memory::WriteBuffer(seg_start, seg_data.data(), phdr.p_filesz);
            }

            // Zero-fill remaining space (BSS)
            if (phdr.p_memsz > phdr.p_filesz) {
                u64 zero_size = phdr.p_memsz - phdr.p_filesz;
                guest_addr_t zero_start = seg_start + phdr.p_filesz;
                
                std::vector<u8> zero_buf(zero_size, 0);
                Memory::WriteBuffer(zero_start, zero_buf.data(), zero_size);
            }

            LOG_DEBUG(Loader, "Loaded PT_LOAD segment: [0x%llx - 0x%llx] (File Size: %llu, Mem Size: %llu, Flags: %s%s%s)",
                      seg_start, seg_start + seg_size, phdr.p_filesz, phdr.p_memsz,
                      (phdr.p_flags & 4) ? "R" : "-",
                      (phdr.p_flags & 2) ? "W" : "-",
                      (phdr.p_flags & 1) ? "X" : "-");
        }

        // Parse PT_DYNAMIC segment
        guest_addr_t strtab_addr = 0;
        guest_addr_t symtab_addr = 0;
        guest_addr_t rela_addr = 0;
        guest_addr_t jmprel_addr = 0;
        u64 strsz = 0;
        u64 syment = sizeof(Elf64_Sym);
        u64 relasz = 0;
        u64 pltrelsz = 0;
        std::vector<u64> needed_offsets;
        u64 soname_offset = 0;
        bool has_soname = false;

        for (const auto& phdr : phdrs) {
            if (phdr.p_type == PT_DYNAMIC) {
                LOG_INFO(Loader, "Found PT_DYNAMIC: offset=0x%llx memsz=0x%llx vaddr=0x%llx", phdr.p_offset, phdr.p_memsz, phdr.p_vaddr);
                out_module.dynamic_table_addr = base_address + phdr.p_vaddr;
                out_module.dynamic_table_size = phdr.p_memsz;

                u64 num_entries = phdr.p_memsz / sizeof(Elf64_Dyn);
                LOG_INFO(Loader, "PT_DYNAMIC num_entries: %llu", num_entries);
                std::vector<Elf64_Dyn> dyn_entries(num_entries);

                // Read from mapped guest memory instead of file stream to handle truncated files
                Memory::ReadBuffer(base_address + phdr.p_vaddr, dyn_entries.data(), phdr.p_memsz);
                LOG_INFO(Loader, "PT_DYNAMIC loaded from guest memory at 0x%llx", base_address + phdr.p_vaddr);

                int dyn_idx = 0;
                for (const auto& dyn : dyn_entries) {
                    LOG_INFO(Loader, "  Dyn[%d]: tag=%lld, val=0x%llx", dyn_idx++, dyn.d_tag, dyn.d_un.d_val);
                    if (dyn.d_tag == DT_NULL) break;

                    switch (dyn.d_tag) {
                        case DT_NEEDED:
                            needed_offsets.push_back(dyn.d_un.d_val);
                            break;
                        case DT_STRTAB:
                            // Dynamic pointer tables inside loaded files could be absolute or relative.
                            // In standard ELF, for ET_DYN (PIE/shared) they are relative to load base.
                            // For ET_EXEC (non-PIE), they are absolute virtual addresses.
                            if (out_module.is_pie) {
                                strtab_addr = base_address + dyn.d_un.d_ptr;
                            } else {
                                strtab_addr = dyn.d_un.d_ptr;
                            }
                            break;
                        case DT_SYMTAB:
                            if (out_module.is_pie) {
                                symtab_addr = base_address + dyn.d_un.d_ptr;
                            } else {
                                symtab_addr = dyn.d_un.d_ptr;
                            }
                            break;
                        case DT_STRSZ:
                            strsz = dyn.d_un.d_val;
                            break;
                        case DT_SYMENT:
                            syment = dyn.d_un.d_val;
                            break;
                        case DT_RELA:
                            if (out_module.is_pie) {
                                rela_addr = base_address + dyn.d_un.d_ptr;
                            } else {
                                rela_addr = dyn.d_un.d_ptr;
                            }
                            break;
                        case DT_RELASZ:
                            relasz = dyn.d_un.d_val;
                            break;
                        case DT_JMPREL:
                            if (out_module.is_pie) {
                                jmprel_addr = base_address + dyn.d_un.d_ptr;
                            } else {
                                jmprel_addr = dyn.d_un.d_ptr;
                            }
                            break;
                        case DT_PLTRELSZ:
                            pltrelsz = dyn.d_un.d_val;
                            break;
                        case DT_SONAME:
                            soname_offset = dyn.d_un.d_val;
                            has_soname    = true;
                            break;
                        case DT_INIT:
                            out_module.init_address =
                                base_address + dyn.d_un.d_ptr;
                            break;
                        case DT_FINI:
                            out_module.fini_address =
                                base_address + dyn.d_un.d_ptr;
                            break;
                    }
                }
                break;
            }
        }

        // Extract PT_TLS template parameters (file size, mem size, alignment,
        // and file offset of the template blob).  We need this even when the
        // module has no symbol-table imports so that the kernel can correctly
        // lay out TLS at load time.
        for (const auto& phdr : phdrs) {
            if (phdr.p_type == PT_TLS) {
                out_module.has_tls            = true;
                out_module.tls_file_size      = phdr.p_filesz;
                out_module.tls_mem_size       = phdr.p_memsz;
                out_module.tls_align          = phdr.p_align;
                out_module.tls_template_offset = phdr.p_offset;
                break;
            }
        }

        // Load String Table
        if (strtab_addr && strsz) {
            out_module.string_table.resize(strsz);
            Memory::ReadBuffer(strtab_addr, &out_module.string_table[0], strsz);

            // Extract needed libraries using string table offsets
            for (u64 offset : needed_offsets) {
                if (offset < strsz) {
                    std::string lib_name = &out_module.string_table[offset];
                    out_module.needed_libraries.push_back(lib_name);
                    LOG_INFO(Loader, "Dependency: %s", lib_name.c_str());
                }
            }

            // Extract DT_SONAME if the dynamic table referenced it.
            if (has_soname && soname_offset < strsz) {
                out_module.soname = &out_module.string_table[soname_offset];
                if (!out_module.soname.empty()) {
                    LOG_INFO(Loader, "SONAME: %s", out_module.soname.c_str());
                }
            }
        }

        // Load Symbol Table
        LOG_INFO(Loader, "Symbol table address: 0x%llx, String table address: 0x%llx, strsz: %llu", symtab_addr, strtab_addr, strsz);
        if (symtab_addr && strtab_addr) {
            // Estimate number of symbols (until we hit the string limit or section bounds, or using hash size)
            // A simple approximation is to read until we hit a symbol with an invalid name offset.
            guest_addr_t curr_sym = symtab_addr;
            while (true) {
                Elf64_Sym sym;
                Memory::ReadBuffer(curr_sym, &sym, sizeof(Elf64_Sym));
                
                // End of symtab is usually marked by zero name if it's the first, 
                // or if we exceed string limits.
                if (sym.st_name >= strsz && sym.st_name != 0) {
                    break;
                }
                
                // Break after a certain threshold or if name is empty and value is empty (excluding first symbol)
                if (curr_sym != symtab_addr && sym.st_name == 0 && sym.st_value == 0 && sym.st_info == 0) {
                    break;
                }

                out_module.symbols.push_back(sym);
                curr_sym += sizeof(Elf64_Sym);
                
                // Safety guard (arbitrary high symbol limit to prevent infinite loops on corrupted headers)
                if (out_module.symbols.size() > 100000) break;
            }
            LOG_INFO(Loader, "Loaded %zu dynamic symbols.", out_module.symbols.size());
        }

        // Load Relocations
        if (rela_addr && relasz) {
            u64 num_relas = relasz / sizeof(Elf64_Rela);
            out_module.relocations.resize(num_relas);
            Memory::ReadBuffer(rela_addr, out_module.relocations.data(), relasz);
            LOG_INFO(Loader, "Loaded %zu standard relocations.", num_relas);
        }

        // Load PLT/JMP Relocations
        if (jmprel_addr && pltrelsz) {
            u64 num_plts = pltrelsz / sizeof(Elf64_Rela);
            out_module.plt_relocations.resize(num_plts);
            Memory::ReadBuffer(jmprel_addr, out_module.plt_relocations.data(), pltrelsz);
            LOG_INFO(Loader, "Loaded %zu PLT/Jump relocations.", num_plts);
        }

        out_module.name = filepath.substr(filepath.find_last_of("/\\") + 1);
        LOG_INFO(Loader, "Successfully mapped module: %s", out_module.name.c_str());
        return true;
    }

    // ===========================================================================
    // ParseModuleMetadata
    //
    // Walks an already-loaded module and produces a structured view of the
    // metadata the rest of the emulator needs:
    //   - imports vs. exports split by `st_shndx` and `STB_*`
    //   - the set of imports referenced by RELA / JMPREL relocations
    //   - the dependency list (DT_NEEDED)
    //   - the TLS template (PT_TLS)
    //   - the module's ELF type (ET_EXEC / ET_DYN, PIE vs. shared object)
    // ===========================================================================
    void ParseModuleMetadata(const LoadedModule& module, ModuleMetadata& out) {
        out = ModuleMetadata{};
        out.e_type          = module.e_type;
        out.is_pie          = module.is_pie;
        out.is_shared_object = (module.e_type == ET_DYN) && !module.is_pie;
        out.has_tls         = module.has_tls;
        out.tls.file_size   = module.tls_file_size;
        out.tls.mem_size    = module.tls_mem_size;
        out.tls.align       = module.tls_align;
        out.tls.file_offset = module.tls_template_offset;

        // ── Symbol classification ────────────────────────────────────────────
        //
        // Walk the symbol table once.  For each entry:
        //   - `st_shndx == 0`  (SHN_UNDEF) and a non-empty name → import.
        //   - `st_shndx != 0`,  binding is GLOBAL/WEAK, name is set  → export.
        //   - everything else (file/section/local/null) is ignored here.
        for (size_t i = 0; i < module.symbols.size(); ++i) {
            const auto& sym = module.symbols[i];
            const u8  bind = ELF64_ST_BIND(sym.st_info);
            const u8  type = ELF64_ST_TYPE(sym.st_info);
            const bool is_undef = (sym.st_shndx == SHN_UNDEF);

            // Resolve the name.  st_name == 0 means the symbol has no name
            // (mandatory for the first table entry) so we treat it as empty.
            std::string name;
            if (sym.st_name < module.string_table.size()) {
                name = &module.string_table[sym.st_name];
            }

            if (is_undef && !name.empty()) {
                ImportEntry imp;
                imp.name         = name;
                imp.symbol_index = static_cast<u32>(i);
                imp.sym_type     = type;
                imp.sym_bind     = bind;
                imp.is_weak      = (bind == STB_WEAK);
                imp.is_tls       = (type == STT_TLS);
                out.imports.push_back(std::move(imp));
            } else if (!is_undef && !name.empty() &&
                       (bind == STB_GLOBAL || bind == STB_WEAK)) {
                ExportEntry exp;
                exp.name          = name;
                exp.address       = module.base_address + sym.st_value;
                exp.size          = sym.st_size;
                exp.sym_type      = type;
                exp.sym_bind      = bind;
                exp.section_index = sym.st_shndx;
                exp.is_tls        = (type == STT_TLS);
                out.exports.push_back(std::move(exp));
            }
        }

        // ── Reference count: which imports are actually used ────────────────
        for (const auto& rel : module.relocations) {
            const u32 sym_idx = static_cast<u32>(ELF64_R_SYM(rel.r_info));
            if (sym_idx == 0 || sym_idx >= module.symbols.size()) continue;
            const auto& sym = module.symbols[sym_idx];
            if (sym.st_shndx != SHN_UNDEF) continue;  // ignore internal
            if (sym.st_name == 0 || sym.st_name >= module.string_table.size()) continue;
            const std::string name = &module.string_table[sym.st_name];
            for (auto& imp : out.imports) {
                if (imp.name == name) { imp.rela_refs++; break; }
            }
        }
        for (const auto& rel : module.plt_relocations) {
            const u32 sym_idx = static_cast<u32>(ELF64_R_SYM(rel.r_info));
            if (sym_idx == 0 || sym_idx >= module.symbols.size()) continue;
            const auto& sym = module.symbols[sym_idx];
            if (sym.st_shndx != SHN_UNDEF) continue;
            if (sym.st_name == 0 || sym.st_name >= module.string_table.size()) continue;
            const std::string name = &module.string_table[sym.st_name];
            for (auto& imp : out.imports) {
                if (imp.name == name) { imp.plt_refs++; break; }
            }
        }
        // Count unique referenced imports (either RELA or PLT referenced).
        out.referenced_import_count = 0;
        for (const auto& imp : out.imports) {
            if (imp.rela_refs > 0 || imp.plt_refs > 0) {
                out.referenced_import_count++;
            }
        }

        // ── Dependency list (DT_NEEDED) ─────────────────────────────────────
        //
        // For now we just copy the names in the order they appeared in the
        // file.  A future change can do real topological ordering against a
        // set of known libkernel/libc/etc. dependencies.
        out.dependencies = module.needed_libraries;
    }

    // ===========================================================================
    // SELF (Signed ELF) container parser
    //
    // The parser below only reads the structural layer of a PS5 SELF image:
    // container header, segment table, embedded ELF region (header + phdrs),
    // and extended info.  It does NOT decrypt or verify segment data; that
    // requires root keys we do not have.
    //
    // Layout (little-endian, all offsets in bytes):
    //
    //   0x000  SelfContainerHeader (0x20)
    //   0x020  Segment table       (segment_count * 0x20)
    //   ...     Embedded ELF        (header 0x40 + phdr[] 0x38 each)
    //   ...     Extended info       (0x40, 16-byte aligned)
    //   ...     Control region      (0x30)
    //   ...     Meta footer         (meta_size)
    //   ...     Segment data        (file_offset/file_size per segment)
    //
    // Reference: SvenGDK/LibProsperoPKG Content/ProsperoFself.cs (2026).
    // ===========================================================================

    bool IsSelfFile(const std::string& filepath) {
        std::ifstream file(filepath, std::ios::binary);
        if (!file.is_open()) return false;
        u32 magic = 0;
        file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
        return file.gcount() == sizeof(magic) && IsSelfMagic(magic);
    }

    bool ParseSelfHeader(std::ifstream& file, SelfImage& out) {
        if (!file) return false;
        file.clear();
        file.seekg(0, std::ios::beg);

        SelfContainerHeader hdr{};
        file.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
        if (file.gcount() != sizeof(hdr)) {
            LOG_ERROR(Loader, "SELF: file too small for container header.");
            return false;
        }
        if (!IsSelfMagic(hdr.magic)) {
            LOG_ERROR(Loader, "SELF: bad magic 0x%08X (expected 0x%08X or 0x%08X).",
                      hdr.magic, kSelfMagic, kSelfMagicRetail);
            return false;
        }
        if (hdr.segment_count == 0) {
            LOG_ERROR(Loader, "SELF: container has zero segments.");
            return false;
        }
        if (hdr.header_size < sizeof(hdr)) {
            LOG_ERROR(Loader, "SELF: header_size (%u) smaller than container header (%zu).",
                      hdr.header_size, sizeof(hdr));
            return false;
        }

        // Read the segment table.  Each entry is 0x20 bytes; the table
        // starts immediately after the container header.
        const u64 seg_table_off = sizeof(hdr);
        const u64 seg_table_bytes =
            static_cast<u64>(hdr.segment_count) * sizeof(SelfSegmentEntry);
        std::vector<SelfSegmentEntry> raw_segs(hdr.segment_count);
        file.seekg(static_cast<std::streamoff>(seg_table_off), std::ios::beg);
        file.read(reinterpret_cast<char*>(raw_segs.data()),
                  static_cast<std::streamsize>(seg_table_bytes));
        if (static_cast<u64>(file.gcount()) != seg_table_bytes) {
            LOG_ERROR(Loader, "SELF: segment table truncated (need %llu bytes).",
                      static_cast<unsigned long long>(seg_table_bytes));
            return false;
        }

        out.header = hdr;
        out.segments.clear();
        out.segments.reserve(hdr.segment_count);
        for (const auto& raw : raw_segs) {
            SelfSegment s;
            s.flags       = raw.flags;
            s.file_offset = raw.file_offset;
            s.file_size   = raw.file_size;
            s.mem_size    = raw.mem_size;
            out.segments.push_back(s);
        }
        for (size_t i = 0; i < out.segments.size(); ++i) {
            const auto& seg = out.segments[i];
            LOG_INFO(Loader, "  SELF Seg[%zu]: id=%d offset=0x%llx size=0x%llx memsz=0x%llx encrypted=%d compressed=%d",
                     i, seg.id(), seg.file_offset, seg.file_size, seg.mem_size, seg.encrypted(), seg.compressed());
        }
        return true;
    }

    bool ParseSelfImage(const std::string& filepath, SelfImage& out) {
        std::ifstream file(filepath, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            LOG_ERROR(Loader, "SELF: failed to open %s", filepath.c_str());
            return false;
        }
        const std::streamsize file_size = file.tellg();
        file.seekg(0, std::ios::beg);
        if (file_size <= 0) {
            LOG_ERROR(Loader, "SELF: file is empty: %s", filepath.c_str());
            return false;
        }

        if (!ParseSelfHeader(file, out)) return false;

        // Locate the embedded ELF.  The ELF region begins immediately after
        // the segment table and contains an ELF64 Ehdr (0x40) followed by
        // phnum program headers.  We do not require the ELF to be 16-byte
        // aligned here; the SvenGDK builder aligns `extInfoStart` later.
        const u64 elf_start = sizeof(SelfContainerHeader) +
            static_cast<u64>(out.header.segment_count) * sizeof(SelfSegmentEntry);

        // Probe for an ELF magic.  Some builders pad; we accept the first
        // ELF-looking position within the first 0x40 bytes after the
        // segment table.
        u8 probe[4] = {0, 0, 0, 0};
        u64 elf_off = 0;
        for (u64 delta = 0; delta < 0x40; delta += 0x8) {
            file.seekg(static_cast<std::streamoff>(elf_start + delta), std::ios::beg);
            file.read(reinterpret_cast<char*>(probe), sizeof(probe));
            if (probe[0] == 0x7F && probe[1] == 'E' && probe[2] == 'L' && probe[3] == 'F') {
                elf_off = elf_start + delta;
                break;
            }
        }
        if (elf_off == 0) {
            LOG_WARN(Loader, "SELF: no embedded ELF magic found near offset 0x%llx.",
                     static_cast<unsigned long long>(elf_start));
            // Not fatal: the SELF is still structurally valid, it just
            // does not contain a parseable embedded ELF.
            out.elf_region.clear();
            out.elf_region_offset = 0;
        } else {
            // Read the ELF Ehdr to learn the phnum.
            file.seekg(static_cast<std::streamoff>(elf_off), std::ios::beg);
            Elf64_Ehdr ehdr{};
            file.read(reinterpret_cast<char*>(&ehdr), sizeof(ehdr));
            if (file.gcount() != sizeof(ehdr)) {
                LOG_ERROR(Loader, "SELF: embedded ELF header truncated.");
                return false;
            }

            char ehdr_hex[256] = {0};
            for (int i = 0; i < 64; ++i) {
                sprintf_s(ehdr_hex + i * 3, sizeof(ehdr_hex) - i * 3, "%02X ", reinterpret_cast<u8*>(&ehdr)[i]);
            }
            LOG_INFO(Loader, "SELF inner ELF Header hex: %s", ehdr_hex);

            // Read the phdr table so we can compute the *actual* extent of
            // the embedded ELF (some SELFs embed the full ELF, payload and
            // all, in the structural layer; we need to read all of it so
            // that LoadSelf gets a loadable binary).  We compute the max
            // extent as `max(p_offset + p_filesz)`, falling back to the
            // header+phdr table size if no phdrs are present.
            const u64 phdr_table_size =
                static_cast<u64>(ehdr.e_phnum) * sizeof(Elf64_Phdr);
            std::vector<Elf64_Phdr> phdrs(ehdr.e_phnum);
            if (ehdr.e_phnum > 0) {
                file.seekg(static_cast<std::streamoff>(elf_off + ehdr.e_phoff),
                           std::ios::beg);
                file.read(reinterpret_cast<char*>(phdrs.data()),
                          static_cast<std::streamsize>(phdr_table_size));
            if (static_cast<u64>(file.gcount()) != phdr_table_size) {
                    LOG_ERROR(Loader, "SELF: embedded ELF phdr table truncated.");
                    return false;
                }
            }
            u64 max_extent = sizeof(ehdr) + phdr_table_size;
            int phdr_idx = 0;
            for (const auto& p : phdrs) {
                LOG_INFO(Loader, "  Phdr[%d]: type=%u, p_offset=0x%llx, p_filesz=0x%llx (sum=0x%llx)",
                         phdr_idx++, p.p_type,
                         static_cast<unsigned long long>(p.p_offset),
                         static_cast<unsigned long long>(p.p_filesz),
                         static_cast<unsigned long long>(p.p_offset + p.p_filesz));
                if (p.p_offset + p.p_filesz > max_extent) {
                    max_extent = p.p_offset + p.p_filesz;
                }
            }
            // Grow max_extent to cover any larger segment payloads actually written
            for (const auto& seg : out.segments) {
                int id = seg.id();
                if (id >= 0 && id < ehdr.e_phnum) {
                    const auto& phdr = phdrs[id];
                    if (seg.file_size > 0) {
                        if (phdr.p_offset + seg.file_size > max_extent) {
                            max_extent = phdr.p_offset + seg.file_size;
                        }
                    }
                }
            }
 
            u64 elf_region_size = max_extent;
            out.elf_region.assign(static_cast<size_t>(elf_region_size), 0);
 
            // 1. Copy Elf64_Ehdr to the start of elf_region
            std::memcpy(out.elf_region.data(), &ehdr, sizeof(ehdr));
 
            // 2. Copy Elf64_Phdrs to elf_region + ehdr.e_phoff
            if (ehdr.e_phnum > 0) {
                std::memcpy(out.elf_region.data() + ehdr.e_phoff, phdrs.data(), phdr_table_size);
            }
 
            // 3. Copy each loadable/metadata segment from its SELF file offset to its ELF offset
            for (const auto& seg : out.segments) {
                int id = seg.id();
                if (id >= 0 && id < ehdr.e_phnum) {
                    const auto& phdr = phdrs[id];
                    if (seg.file_size > 0) {
                        if (seg.file_offset + seg.file_size <= static_cast<u64>(file_size)) {
                            file.clear();
                            file.seekg(static_cast<std::streamoff>(seg.file_offset), std::ios::beg);
                            file.read(reinterpret_cast<char*>(out.elf_region.data() + phdr.p_offset),
                                      static_cast<std::streamsize>(seg.file_size));
                            LOG_INFO(Loader, "Reconstructed SELF segment id=%d: file_off=0x%llx -> ELF off=0x%llx size=0x%llx",
                                     id, seg.file_offset, phdr.p_offset, seg.file_size);
                        } else {
                            LOG_WARN(Loader, "SELF segment id=%d file extent (0x%llx) exceeds file size (0x%llx)",
                                     id, seg.file_offset + seg.file_size, file_size);
                        }
                    }
                }
            }
            out.elf_region_offset = elf_off;
            LOG_INFO(Loader,
                     "SELF: embedded ELF at 0x%llx, phnum=%u, region size=%llu",
                     static_cast<unsigned long long>(elf_off),
                     ehdr.e_phnum,
                     static_cast<unsigned long long>(elf_region_size));

            // The extended info sits 16-byte aligned after the ELF region,
            // but only if it falls within the header region.
            const u64 ext_info_start = (elf_off + elf_region_size + 0xF) & ~u64{0xF};
            if (ext_info_start + sizeof(SelfExtInfo) <=
                    static_cast<u64>(out.header.header_size) &&
                ext_info_start + sizeof(SelfExtInfo) <= static_cast<u64>(file_size)) {
                file.seekg(static_cast<std::streamoff>(ext_info_start), std::ios::beg);
                file.read(reinterpret_cast<char*>(&out.ext_info), sizeof(out.ext_info));
                if (file.gcount() == sizeof(out.ext_info)) {
                    out.has_ext_info = true;
                    out.ext_info_offset = ext_info_start;
                    LOG_INFO(Loader,
                             "SELF: ext info at 0x%llx, authority_id=0x%016llx, "
                             "app_ver=0x%llx, fw_ver=0x%llx, category=0x%02X",
                             static_cast<unsigned long long>(ext_info_start),
                             static_cast<unsigned long long>(out.ext_info.authority_id),
                             static_cast<unsigned long long>(out.ext_info.app_version),
                             static_cast<unsigned long long>(out.ext_info.firmware_version),
                             static_cast<unsigned int>(
                                 (out.ext_info.authority_id >> 56) & 0xFFu));
                }
            }
        }

        LOG_INFO(Loader,
                 "SELF: program_type=0x%08X, header_size=%u, meta_size=%u, "
                 "file_size=%llu, segment_count=%u, flags=0x%04X",
                 out.header.program_type, out.header.header_size,
                 out.header.meta_size,
                 static_cast<unsigned long long>(out.header.file_size),
                 out.header.segment_count, out.header.flags);
        return true;
    }

    std::vector<u8> ExtractInnerElf(const SelfImage& self) {
        return self.elf_region;
    }

    bool LoadSelf(const std::string& filepath, LoadedModule& out_module) {
        LOG_INFO(Loader, "Loading SELF container: %s", filepath.c_str());

        SelfImage self;
        if (!ParseSelfImage(filepath, self)) {
            LOG_ERROR(Loader, "SELF: parse failed for %s", filepath.c_str());
            return false;
        }

        if (self.elf_region.empty()) {
            LOG_ERROR(Loader, "SELF: no embedded ELF to load.");
            return false;
        }

        // If any data segment is encrypted or compressed, we cannot
        // reconstruct a valid ELF binary; we just have the inner header.
        // Detect this and bail out before attempting to load.
        for (const auto& seg : self.segments) {
            if (seg.encrypted()) {
                LOG_ERROR(Loader,
                          "SELF: segment id=%d is encrypted — root keys required.",
                          seg.id());
                return false;
            }
            if (seg.compressed()) {
                LOG_ERROR(Loader,
                          "SELF: segment id=%d is compressed — not supported yet.",
                          seg.id());
                return false;
            }
        }

        // Materialize the inner ELF bytes in a temp file so the existing
        // `Loader::Load` path can do its job.  The temp file is deleted by
        // the OS once the handle is closed (or on next reboot if it isn't).
        std::filesystem::path tmp_dir =
            std::filesystem::temp_directory_path() / "pcsx5_self";
        std::error_code ec;
        std::filesystem::create_directories(tmp_dir, ec);

        // Build a unique temp filename.  std::tmpnam is unsafe in general
        // but acceptable here because we own the directory and only the
        // current process writes to it.
        char tmp_name[L_tmpnam_s];
        if (tmpnam_s(tmp_name, sizeof(tmp_name)) != 0) {
            LOG_ERROR(Loader, "SELF: failed to allocate temp name.");
            return false;
        }
        std::filesystem::path tmp_path = tmp_dir / tmp_name;
        {
            std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
            if (!out.is_open()) {
                LOG_ERROR(Loader, "SELF: failed to write temp ELF.");
                return false;
            }
            out.write(reinterpret_cast<const char*>(self.elf_region.data()),
                      static_cast<std::streamsize>(self.elf_region.size()));
        }

        const bool ok = Load(tmp_path.string(), out_module);

        // Best-effort cleanup; ignore errors.
        std::filesystem::remove(tmp_path, ec);

        if (ok) {
            LOG_INFO(Loader,
                     "SELF: loaded inner ELF '%s' (entry=0x%llx, base=0x%llx).",
                     out_module.name.c_str(),
                     static_cast<unsigned long long>(out_module.entry_point),
                     static_cast<unsigned long long>(out_module.base_address));
        }
        return ok;
    }
}
// namespace Loader
