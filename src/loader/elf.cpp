#include "elf.h"
#include "../memory/memory.h"
#include "../common/log.h"
#include <fstream>
#include <cstring>
#include <algorithm>
#include <limits>

namespace Loader {

    bool Load(const std::string& filepath, LoadedModule& out_module) {
        LOG_INFO(Loader, "Loading ELF binary: %s", filepath.c_str());

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
        for (const auto& phdr : phdrs) {
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
        guest_addr_t base_address = 0;
        if (load_base == 0) {
            base_address = 0x800000000;
            LOG_INFO(Loader, "Position Independent Executable (PIE) detected. Mapping at base base: 0x%llx", base_address);
        } else {
            base_address = 0; // Loaded at their absolute addresses
            LOG_INFO(Loader, "Fixed-position executable detected. Mapping at absolute addresses.");
        }

        out_module.base_address = base_address;
        out_module.image_size = total_size;
        out_module.entry_point = base_address + ehdr.e_entry;

        // Reserve the entire virtual address range for the image first
        guest_addr_t reserved_base = Memory::Reserve(base_address + load_base, total_size);
        if (!reserved_base) {
            LOG_ERROR(Loader, "Failed to reserve virtual address range for the module (size: %llu)", total_size);
            return false;
        }
        
        // Update base address if OS allocated dynamically (PIE)
        if (load_base == 0) {
            base_address = reserved_base;
            out_module.base_address = base_address;
            out_module.entry_point = base_address + ehdr.e_entry;
        }

        // Map segments
        for (const auto& phdr : phdrs) {
            if (phdr.p_type != PT_LOAD) continue;

            guest_addr_t seg_start = base_address + phdr.p_vaddr;
            u64 seg_size = phdr.p_memsz;
            
            // Translate protection flags (PF_X=1, PF_W=2, PF_R=4)
            u32 final_protection = Memory::PROT_NONE;
            if (phdr.p_flags & 4) final_protection |= Memory::PROT_READ;
            if (phdr.p_flags & 2) final_protection |= Memory::PROT_WRITE;
            if (phdr.p_flags & 1) final_protection |= Memory::PROT_EXEC;

            // Default zero-flag PT_LOAD segments to Read+Write (representing BSS/global variables)
            if (final_protection == Memory::PROT_NONE) {
                final_protection = Memory::PROT_READ | Memory::PROT_WRITE;
            }

            // Map memory region with temporary Read+Write (and Exec if final is Exec) so we can write and link
            u32 map_protection = Memory::PROT_READ | Memory::PROT_WRITE;
            if (final_protection & Memory::PROT_EXEC) {
                map_protection |= Memory::PROT_EXEC;
            }

            guest_addr_t mapped = Memory::Commit(seg_start, seg_size, map_protection);
            if (mapped == 0) {
                LOG_ERROR(Loader, "Failed to commit segment at 0x%llx (size: %llu)", seg_start, seg_size);
                return false;
            }

            // Save mapped segment info for post-link protection configuration
            MappedSegment seg;
            seg.address = seg_start;
            seg.size = seg_size;
            seg.final_protection = final_protection;
            out_module.segments.push_back(seg);

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

        for (const auto& phdr : phdrs) {
            if (phdr.p_type == PT_DYNAMIC) {
                out_module.dynamic_table_addr = base_address + phdr.p_vaddr;
                out_module.dynamic_table_size = phdr.p_memsz;
                
                u64 num_entries = phdr.p_memsz / sizeof(Elf64_Dyn);
                std::vector<Elf64_Dyn> dyn_entries(num_entries);
                
                file.seekg(phdr.p_offset, std::ios::beg);
                file.read(reinterpret_cast<char*>(dyn_entries.data()), phdr.p_memsz);

                for (const auto& dyn : dyn_entries) {
                    if (dyn.d_tag == DT_NULL) break;

                    switch (dyn.d_tag) {
                        case DT_NEEDED:
                            needed_offsets.push_back(dyn.d_un.d_val);
                            break;
                        case DT_STRTAB:
                            // Dynamic pointer tables inside loaded files could be absolute or relative.
                            // In standard ELF, they are guest addresses.
                            strtab_addr = base_address + dyn.d_un.d_ptr;
                            break;
                        case DT_SYMTAB:
                            symtab_addr = base_address + dyn.d_un.d_ptr;
                            break;
                        case DT_STRSZ:
                            strsz = dyn.d_un.d_val;
                            break;
                        case DT_SYMENT:
                            syment = dyn.d_un.d_val;
                            break;
                        case DT_RELA:
                            rela_addr = base_address + dyn.d_un.d_ptr;
                            break;
                        case DT_RELASZ:
                            relasz = dyn.d_un.d_val;
                            break;
                        case DT_JMPREL:
                            jmprel_addr = base_address + dyn.d_un.d_ptr;
                            break;
                        case DT_PLTRELSZ:
                            pltrelsz = dyn.d_un.d_val;
                            break;
                    }
                }
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
        }

        // Load Symbol Table
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
}
// namespace Loader
