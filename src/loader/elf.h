#pragma once
#include "../common/types.h"
#include <string>
#include <vector>

namespace Loader {

    // ELF Identification Constants
    constexpr size_t EI_NIDENT = 16;
    constexpr u8 ELFMAG0 = 0x7f;
    constexpr u8 ELFMAG1 = 'E';
    constexpr u8 ELFMAG2 = 'L';
    constexpr u8 ELFMAG3 = 'F';

    // ELF Program Header Types
    constexpr u32 PT_NULL    = 0;
    constexpr u32 PT_LOAD    = 1;
    constexpr u32 PT_DYNAMIC = 2;
    constexpr u32 PT_INTERP  = 3;
    constexpr u32 PT_NOTE    = 4;
    constexpr u32 PT_SHLIB   = 5;
    constexpr u32 PT_PHDR    = 6;
    constexpr u32 PT_TLS     = 7;

    // ELF Dynamic Entry Tags
    constexpr s64 DT_NULL         = 0;
    constexpr s64 DT_NEEDED       = 1;
    constexpr s64 DT_PLTRELSZ     = 2;
    constexpr s64 DT_PLTGOT       = 3;
    constexpr s64 DT_HASH         = 4;
    constexpr s64 DT_STRTAB       = 5;
    constexpr s64 DT_SYMTAB       = 6;
    constexpr s64 DT_RELA         = 7;
    constexpr s64 DT_RELASZ       = 8;
    constexpr s64 DT_RELAENT      = 9;
    constexpr s64 DT_STRSZ        = 10;
    constexpr s64 DT_SYMENT       = 11;
    constexpr s64 DT_INIT         = 12;
    constexpr s64 DT_FINI         = 13;
    constexpr s64 DT_SONAME       = 14;
    constexpr s64 DT_RPATH        = 15;
    constexpr s64 DT_SYMBOLIC     = 16;
    constexpr s64 DT_REL          = 17;
    constexpr s64 DT_RELSZ        = 18;
    constexpr s64 DT_RELENT       = 19;
    constexpr s64 DT_PLTREL       = 20;
    constexpr s64 DT_DEBUG        = 21;
    constexpr s64 DT_TEXTREL      = 22;
    constexpr s64 DT_JMPREL       = 23;
    constexpr s64 DT_BIND_NOW     = 24;

    // Relocation Types for x86-64
    constexpr u32 R_X86_64_NONE      = 0;
    constexpr u32 R_X86_64_64        = 1;
    constexpr u32 R_X86_64_PC32      = 2;
    constexpr u32 R_X86_64_GOT32     = 3;
    constexpr u32 R_X86_64_PLT32     = 4;
    constexpr u32 R_X86_64_COPY      = 5;
    constexpr u32 R_X86_64_GLOB_DAT  = 6;
    constexpr u32 R_X86_64_JUMP_SLOT = 7;
    constexpr u32 R_X86_64_RELATIVE  = 8;

    // Standard ELF64 Headers
    #pragma pack(push, 1)
    struct Elf64_Ehdr {
        u8  e_ident[EI_NIDENT];
        u16 e_type;
        u16 e_machine;
        u32 e_version;
        u64 e_entry;
        u64 e_phoff;
        u64 e_shoff;
        u32 e_flags;
        u16 e_ehsize;
        u16 e_phentsize;
        u16 e_phnum;
        u16 e_shentsize;
        u16 e_shnum;
        u16 e_shstrndx;
    };

    struct Elf64_Phdr {
        u32 p_type;
        u32 p_flags;
        u64 p_offset;
        u64 p_vaddr;
        u64 p_paddr;
        u64 p_filesz;
        u64 p_memsz;
        u64 p_align;
    };

    struct Elf64_Dyn {
        s64 d_tag;
        union {
            u64 d_val;
            u64 d_ptr;
        } d_un;
    };

    struct Elf64_Sym {
        u32 st_name;
        u8  st_info;
        u8  st_other;
        u16 st_shndx;
        u64 st_value;
        u64 st_size;
    };

    struct Elf64_Rela {
        u64 r_offset;
        u64 r_info;
        s64 r_addend;
    };
    #pragma pack(pop)

    #define ELF64_R_SYM(i)    ((i) >> 32)
    #define ELF64_R_TYPE(i)   ((i) & 0xffffffffL)

    struct MappedSegment {
        guest_addr_t address;
        u64 size;
        u32 final_protection;
    };

    // Loaded Module details
    struct LoadedModule {
        std::string name;
        guest_addr_t base_address = 0;
        u64 image_size = 0;
        guest_addr_t entry_point = 0;

        std::vector<std::string> needed_libraries;
        std::vector<Elf64_Sym> symbols;
        std::string string_table;
        std::vector<Elf64_Rela> relocations;
        std::vector<Elf64_Rela> plt_relocations;

        guest_addr_t dynamic_table_addr = 0;
        u64 dynamic_table_size = 0;
        
        std::vector<MappedSegment> segments;
    };

    // Load an ELF executable into memory and map its PT_LOAD segments
    bool Load(const std::string& filepath, LoadedModule& out_module);
}
// namespace Loader
