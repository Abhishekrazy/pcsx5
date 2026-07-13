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

    // ELF64 e_type values
    constexpr u16 ET_NONE   = 0;
    constexpr u16 ET_REL    = 1;
    constexpr u16 ET_EXEC   = 2;
    constexpr u16 ET_DYN    = 3;
    constexpr u16 ET_CORE   = 4;

    // Symbol binding (low nibble of st_info)
    constexpr u8 STB_LOCAL   = 0;
    constexpr u8 STB_GLOBAL  = 1;
    constexpr u8 STB_WEAK    = 2;

    // Symbol type (high nibble of st_info)
    constexpr u8 STT_NOTYPE  = 0;
    constexpr u8 STT_OBJECT  = 1;
    constexpr u8 STT_FUNC    = 2;
    constexpr u8 STT_SECTION = 3;
    constexpr u8 STT_FILE    = 4;
    constexpr u8 STT_TLS     = 6;

    // Section index special values
    constexpr u16 SHN_UNDEF  = 0;
    constexpr u16 SHN_ABS    = 0xfff1;

    // Helper macros for st_info
    #define ELF64_ST_BIND(i)   ((i) >> 4)
    #define ELF64_ST_TYPE(i)   ((i) & 0xf)
    #define ELF64_ST_INFO(b,t) (((b) << 4) | ((t) & 0xf))

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
        u16 e_type = 0;             // ELF e_type (ET_EXEC / ET_DYN)
        bool is_pie = false;        // true if PIE (ET_DYN with base 0)

        std::vector<std::string> needed_libraries;
        std::vector<Elf64_Sym> symbols;
        std::string string_table;
        std::vector<Elf64_Rela> relocations;
        std::vector<Elf64_Rela> plt_relocations;

        // PT_DYNAMIC pointers
        guest_addr_t dynamic_table_addr = 0;
        u64 dynamic_table_size = 0;

        // PT_TLS template
        bool has_tls = false;
        u64  tls_file_size = 0;
        u64  tls_mem_size  = 0;
        u64  tls_align     = 0;
        u64  tls_template_offset = 0; // file offset of TLS template

        // DT_INIT / DT_FINI
        guest_addr_t init_address = 0;
        guest_addr_t fini_address = 0;

        // DT_SONAME (only meaningful for ET_DYN)
        std::string soname;

        std::vector<MappedSegment> segments;
    };

    // ------------------------------------------------------------------------
    // Module metadata
    //
    // The raw `LoadedModule` carries every section and program header entry;
    // `ModuleMetadata` is a derived view that classifies what the rest of the
    // emulator cares about: imports vs. exports, TLS template parameters,
    // module dependencies, and the module's runtime type.
    // ------------------------------------------------------------------------
    struct ImportEntry {
        std::string name;          // symbol name as resolved from the strtab
        u32         symbol_index = 0;  // index into LoadedModule::symbols
        u8          sym_type = STT_NOTYPE;   // STT_FUNC / STT_OBJECT / ...
        u8          sym_bind = STB_GLOBAL;  // STB_GLOBAL / STB_WEAK
        bool        is_weak   = false;
        bool        is_tls    = false;  // STT_TLS
        // Number of RELA / PLT entries that target this symbol.  > 0 means
        // the import is actually referenced by the loader.
        u32         rela_refs   = 0;
        u32         plt_refs    = 0;
    };

    struct ExportEntry {
        std::string name;          // symbol name
        guest_addr_t address = 0;  // resolved guest address (base + st_value)
        u64         size   = 0;    // st_size in bytes
        u8          sym_type = STT_NOTYPE;
        u8          sym_bind = STB_GLOBAL;
        u16         section_index = 0;
        bool        is_tls = false;  // STT_TLS
    };

    struct TlsTemplate {
        u64          file_size = 0;
        u64          mem_size  = 0;
        u64          align     = 0;
        u64          file_offset = 0; // file offset of the template blob
    };

    struct ModuleMetadata {
        // Module type
        u16  e_type   = 0;     // ET_EXEC / ET_DYN / ...
        bool is_pie   = false;  // PIE = ET_DYN with vaddr 0 in PT_LOAD
        bool is_shared_object = false;  // ET_DYN that is not PIE

        // Symbol classification
        std::vector<ImportEntry> imports;
        std::vector<ExportEntry> exports;

        // Dependency ordering.  The vector holds the indices of
        // `needed_libraries` in the order they should be loaded.  Right now
        // we surface a topological order based on the loaded set; for
        // simple cases this is just the input order.
        std::vector<std::string> dependencies;

        // TLS template (file offset + sizes + alignment) extracted from
        // PT_TLS, if present.
        TlsTemplate tls;

        // True iff the module has a PT_TLS segment.
        bool has_tls = false;

        // Number of imports that the loader actually references through
        // RELA or JMPREL relocations.  An import can appear in the symbol
        // table but not be referenced (dead code).
        u32 referenced_import_count = 0;
    };

    // Build a `ModuleMetadata` view from an already-loaded module.  The
    // function never fails: it only inspects the data already on the
    // module.  Pass a freshly loaded `LoadedModule` (no further side
    // effects).
    void ParseModuleMetadata(const LoadedModule& module, ModuleMetadata& out);

    // Load an ELF executable into memory and map its PT_LOAD segments
    bool Load(const std::string& filepath, LoadedModule& out_module);
}
// namespace Loader
