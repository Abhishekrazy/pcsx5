#pragma once

#include <cstddef>
#include <cstdint>

namespace pcsx5::core {

// ELF Header identification indexes and values.
inline constexpr std::size_t ei_nident = 16;
inline constexpr std::uint8_t elfmag0 = 0x7f;
inline constexpr std::uint8_t elfmag1 = 'E';
inline constexpr std::uint8_t elfmag2 = 'L';
inline constexpr std::uint8_t elfmag3 = 'F';

inline constexpr std::uint8_t elfclassnone = 0;
inline constexpr std::uint8_t elfclass32   = 1;
inline constexpr std::uint8_t elfclass64   = 2;

inline constexpr std::uint8_t elfdata2lsb  = 1; // 2's complement, little endian
inline constexpr std::uint8_t ev_current   = 1;

// Architecture machine type for x86-64.
inline constexpr std::uint16_t em_x86_64 = 0x3E;

// Object file types (e_type).
inline constexpr std::uint16_t et_none = 0;
inline constexpr std::uint16_t et_rel  = 1;
inline constexpr std::uint16_t et_exec = 2;
inline constexpr std::uint16_t et_dyn  = 3;
inline constexpr std::uint16_t et_core = 4;

// Sony PS5 SDK e_type values (0xFE00..0xFEFF).
inline constexpr std::uint16_t et_sce_exec     = 0xFE00;
inline constexpr std::uint16_t et_sce_relexec  = 0xFE04;
inline constexpr std::uint16_t et_sce_stublib  = 0xFE0C;
inline constexpr std::uint16_t et_sce_dynamic  = 0xFE18;

[[nodiscard]] constexpr bool is_ps5_module_type(std::uint16_t e_type) noexcept {
    return (e_type >= 0xFE00 && e_type <= 0xFEFF);
}

// Program header types (p_type).
inline constexpr std::uint32_t pt_null    = 0;
inline constexpr std::uint32_t pt_load    = 1;
inline constexpr std::uint32_t pt_dynamic = 2;
inline constexpr std::uint32_t pt_interp  = 3;
inline constexpr std::uint32_t pt_note    = 4;
inline constexpr std::uint32_t pt_shlib   = 5;
inline constexpr std::uint32_t pt_phdr    = 6;
inline constexpr std::uint32_t pt_tls     = 7;

// PS5 SDK & GNU program header extensions.
inline constexpr std::uint32_t pt_sce_proc_param   = 0x61000001;
inline constexpr std::uint32_t pt_sce_module_param = 0x61000002;
inline constexpr std::uint32_t pt_sce_relro        = 0x61000010;
inline constexpr std::uint32_t pt_sce_path         = 0x6FFFFF00;
inline constexpr std::uint32_t pt_gnu_eh_frame     = 0x6474E550;
inline constexpr std::uint32_t pt_gnu_relro        = 0x6474E550;
inline constexpr std::uint32_t pt_gnu_property     = 0x6474E551;
inline constexpr std::uint32_t pt_gnu_stack        = 0x6474E552;

[[nodiscard]] constexpr bool is_ps5_segment_type(std::uint32_t p_type) noexcept {
    switch (p_type) {
    case pt_sce_proc_param:
    case pt_sce_module_param:
    case pt_sce_relro:
    case pt_sce_path:
    case pt_gnu_eh_frame:
    case pt_gnu_property:
    case pt_gnu_stack:
        return true;
    default:
        return false;
    }
}

// Program header flags (p_flags).
inline constexpr std::uint32_t pf_x = 0x1;
inline constexpr std::uint32_t pf_w = 0x2;
inline constexpr std::uint32_t pf_r = 0x4;

// Dynamic array tags (d_tag).
inline constexpr std::int64_t dt_null         = 0;
inline constexpr std::int64_t dt_needed       = 1;
inline constexpr std::int64_t dt_pltrelsz     = 2;
inline constexpr std::int64_t dt_pltgot       = 3;
inline constexpr std::int64_t dt_hash         = 4;
inline constexpr std::int64_t dt_strtab       = 5;
inline constexpr std::int64_t dt_symtab       = 6;
inline constexpr std::int64_t dt_rela         = 7;
inline constexpr std::int64_t dt_relasz       = 8;
inline constexpr std::int64_t dt_relaent      = 9;
inline constexpr std::int64_t dt_strsz        = 10;
inline constexpr std::int64_t dt_syment       = 11;
inline constexpr std::int64_t dt_init         = 12;
inline constexpr std::int64_t dt_fini         = 13;
inline constexpr std::int64_t dt_soname       = 14;
inline constexpr std::int64_t dt_rpath        = 15;
inline constexpr std::int64_t dt_symbolic     = 16;
inline constexpr std::int64_t dt_rel          = 17;
inline constexpr std::int64_t dt_relsz        = 18;
inline constexpr std::int64_t dt_relent       = 19;
inline constexpr std::int64_t dt_pltrel       = 20;
inline constexpr std::int64_t dt_debug        = 21;
inline constexpr std::int64_t dt_textrel      = 22;
inline constexpr std::int64_t dt_jmprel       = 23;
inline constexpr std::int64_t dt_init_array   = 25;
inline constexpr std::int64_t dt_fini_array   = 26;
inline constexpr std::int64_t dt_init_arraysz = 27;
inline constexpr std::int64_t dt_fini_arraysz = 28;
inline constexpr std::int64_t dt_preinit_array   = 32;
inline constexpr std::int64_t dt_preinit_arraysz = 33;

// Relocation types for x86-64 (r_info >> 32 = sym, r_info & 0xFFFFFFFF = type).
inline constexpr std::uint32_t r_x86_64_none      = 0;
inline constexpr std::uint32_t r_x86_64_64        = 1;
inline constexpr std::uint32_t r_x86_64_pc32      = 2;
inline constexpr std::uint32_t r_x86_64_got32     = 3;
inline constexpr std::uint32_t r_x86_64_plt32     = 4;
inline constexpr std::uint32_t r_x86_64_copy      = 5;
inline constexpr std::uint32_t r_x86_64_glob_dat  = 6;
inline constexpr std::uint32_t r_x86_64_jump_slot = 7;
inline constexpr std::uint32_t r_x86_64_relative  = 8;


struct elf64_ehdr {
    std::uint8_t  e_ident[ei_nident];
    std::uint16_t e_type;
    std::uint16_t e_machine;
    std::uint32_t e_version;
    std::uint64_t e_entry;
    std::uint64_t e_phoff;
    std::uint64_t e_shoff;
    std::uint32_t e_flags;
    std::uint16_t e_ehsize;
    std::uint16_t e_phentsize;
    std::uint16_t e_phnum;
    std::uint16_t e_shentsize;
    std::uint16_t e_shnum;
    std::uint16_t e_shstrndx;
};
static_assert(sizeof(elf64_ehdr) == 64, "elf64_ehdr size must be exactly 64 bytes");

struct elf64_phdr {
    std::uint32_t p_type;
    std::uint32_t p_flags;
    std::uint64_t p_offset;
    std::uint64_t p_vaddr;
    std::uint64_t p_paddr;
    std::uint64_t p_filesz;
    std::uint64_t p_memsz;
    std::uint64_t p_align;
};
static_assert(sizeof(elf64_phdr) == 56, "elf64_phdr size must be exactly 56 bytes");

struct elf64_shdr {
    std::uint32_t sh_name;
    std::uint32_t sh_type;
    std::uint64_t sh_flags;
    std::uint64_t sh_addr;
    std::uint64_t sh_offset;
    std::uint64_t sh_size;
    std::uint32_t sh_link;
    std::uint32_t sh_info;
    std::uint64_t sh_addralign;
    std::uint64_t sh_entsize;
};
static_assert(sizeof(elf64_shdr) == 64, "elf64_shdr size must be exactly 64 bytes");

struct elf64_dyn {
    std::int64_t d_tag;
    union {
        std::uint64_t d_val;
        std::uint64_t d_ptr;
    } d_un;
};
static_assert(sizeof(elf64_dyn) == 16, "elf64_dyn size must be exactly 16 bytes");

struct elf64_rela {
    std::uint64_t r_offset;
    std::uint64_t r_info;
    std::int64_t  r_addend;
};
static_assert(sizeof(elf64_rela) == 24, "elf64_rela size must be exactly 24 bytes");

struct elf64_sym {
    std::uint32_t st_name;
    std::uint8_t  st_info;
    std::uint8_t  st_other;
    std::uint16_t st_shndx;
    std::uint64_t st_value;
    std::uint64_t st_size;
};
static_assert(sizeof(elf64_sym) == 24, "elf64_sym size must be exactly 24 bytes");


} // namespace pcsx5::core

