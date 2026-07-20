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

    // Sony PS5 SDK-specific program-header types.  These appear in real
    // PS5 game executables and in SELF-extracted inner ELFs.  The loader
    // skips them with a debug log so a real game ELF can be loaded
    // without crashing on unknown p_type values.
    //   0x61000001 - PT_SCE_RELRO (read-only relocations)
    //   0x61000010 - PT_SCE_COMMENT
    //   0x6FFFFF00 - PT_SCE_PROC_PARAM
    //   0x6FFFFF01 - PT_SCE_MODULE_PARAM
    //   0x6474E550 - PT_GNU_RELRO (read-only-after-relocations)
    //   0x6474E551 - PT_GNU_PROPERTY
    //   0x6474E552 - PT_GNU_STACK (executable-stack note)
    // We accept (and ignore) the common ones we know about.
    constexpr u32 PT_SCE_PROC_PARAM   = 0x6FFFFF00;
    constexpr u32 PT_SCE_MODULE_PARAM = 0x6FFFFF01;
    constexpr u32 PT_SCE_RELRO        = 0x61000001;
    constexpr u32 PT_GNU_RELRO        = 0x6474E550;
    constexpr u32 PT_GNU_PROPERTY     = 0x6474E551;
    constexpr u32 PT_GNU_STACK        = 0x6474E552;
    // Standard GNU value (same numeric constant Sony uses for the
    // .eh_frame_hdr segment on PS5; the PT_GNU_RELRO alias above predates
    // this and is kept for IsPs5SegmentType compatibility).
    constexpr u32 PT_GNU_EH_FRAME     = 0x6474E550;

    // True if `p_type` is a known PS5 SDK extension we should silently
    // skip during PT_LOAD / validation / dynamic-table processing.
    inline bool IsPs5SegmentType(u32 p_type) {
        switch (p_type) {
            case PT_SCE_PROC_PARAM:
            case PT_SCE_MODULE_PARAM:
            case PT_SCE_RELRO:
            case PT_GNU_RELRO:
            case PT_GNU_PROPERTY:
            case PT_GNU_STACK:
                return true;
            default:
                return false;
        }
    }

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

    // OS-/processor-specific e_type values used by the Sony PS5 SDK
    // (range 0xFE00..0xFEFF).  Real PS5 eboot.bin / *.prx modules carry
    // one of these instead of the standard ET_DYN/ET_EXEC.  We accept
    // them and treat them all as loadable dynamic modules (effectively
    // the same as ET_DYN with a PIE-style base of 0).
    constexpr u16 ET_PS5_MODULE_EXEC         = 0xFE00;  // module executable
    constexpr u16 ET_PS5_REPLAY_EXEC         = 0xFE01;  // replay executable
    constexpr u16 ET_PS5_RELOCATABLE_EXEC    = 0xFE04;  // relocatable module exec
    constexpr u16 ET_PS5_STUB_LIBRARY        = 0xFE0C;  // stub library
    // 0xFE10 is used by retail game executables (the inner ELF extracted
    // from a SELF).  We accept it the same way as the other PS5 types.
    constexpr u16 ET_PS5_GAME_EXEC           = 0xFE10;
    // 0xFE18 is used by PS5 PRX/SPRX shared modules (the *.prx files under
    // sce_module/).  Like the other module types it is a PIE-style dynamic
    // image with a load base of 0.
    constexpr u16 ET_PS5_PRX_MODULE          = 0xFE18;

    // True if `e_type` is one of the PS5 SDK module types we accept.
    inline bool IsPs5ModuleType(u16 e_type) {
        switch (e_type) {
            case ET_PS5_MODULE_EXEC:
            case ET_PS5_REPLAY_EXEC:
            case ET_PS5_RELOCATABLE_EXEC:
            case ET_PS5_STUB_LIBRARY:
            case ET_PS5_GAME_EXEC:
            case ET_PS5_PRX_MODULE:
                return true;
            default:
                return false;
        }
    }

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
        // Full ELF program header fields (for boot parser / debugging)
        u32 type = 0;           // p_type
        u64 file_offset = 0;    // p_offset
        u64 file_size = 0;      // p_filesz
        u64 mem_size = 0;       // p_memsz
        guest_addr_t vaddr = 0; // p_vaddr
        u64 flags = 0;          // p_flags
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

        // PT_GNU_EH_FRAME (0x6474E550) — guest address of the module's
        // .eh_frame_hdr and its size in bytes (0 when the module has none).
        // Consumed by the HLE guest C++ exception unwinder (liblibc.cpp).
        guest_addr_t eh_frame_hdr_addr = 0;
        u64          eh_frame_hdr_size = 0;

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

    // ------------------------------------------------------------------------
    // SELF (Signed ELF) container
    //
    // PS5 stores every executable module (eboot.bin, *.prx, *.sprx) as a
    // *SELF* image: a Sony-proprietary container that wraps an ELF64 module
    // with a header, a per-segment table, an extended-info block, and
    // (optionally) signed/encrypted per-segment data.
    //
    // Decrypting a real retail SELF requires PS5 root keys that are not
    // publicly available.  The parser below, however, only reads the
    // *structural* layer: container header, segment table, and extended
    // info.  This lets us:
    //   - inspect a SELF image (segment count, file layout, program type),
    //   - classify whether a SELF is a fake-signed debug image,
    //   - extract the embedded ELF header + program-header bytes and hand
    //     them to the regular `Loader::Load` path (which works as long as
    //     the segment data is plaintext, as in fPKG / fake-self modules).
    //
    // Layout reference: see LibProsperoPkg/Content/ProsperoFself.cs
    // (SvenGDK, 2026).  All scalars are little-endian.
    // ------------------------------------------------------------------------

    // Magic word at file offset 0x00.
    // Sony uses multiple SELF container magics.  In our experience:
    //   0x1D3D154F - fake-signed / fPKG / debug builds (KEPLER, retail devkit)
    //   0xEEF51454 - genuine / retail SELFs (newer games, e.g. PPSA23885)
    // Both share the same on-disk layout; we accept either.
    constexpr u32 kSelfMagic       = 0x1D3D154F;
    constexpr u32 kSelfMagicRetail = 0xEEF51454;
    constexpr u32 IsSelfMagic(u32 m) { return m == kSelfMagic || m == kSelfMagicRetail; }

    // Default program-type value written by the Sony signing toolchain and
    // by fPKG / fake-self builders.  We accept any value at parse time but
    // keep this constant for diagnostic comparison.
    constexpr u32 kSelfDefaultProgramType = 0x00000101;

    // Container header: 0x20 bytes total.
    #pragma pack(push, 1)
    struct SelfContainerHeader {
        u32 magic;          // 0x00  - must equal kSelfMagic
        u8  version;        // 0x04
        u8  mode;           // 0x05
        u8  endianness;     // 0x06
        u8  attr;           // 0x07
        u32 program_type;   // 0x08
        u16 header_size;    // 0x0C
        u16 meta_size;      // 0x0E
        u64 file_size;      // 0x10
        u16 segment_count;  // 0x18
        u16 flags;          // 0x1A
        u32 reserved;       // 0x1C
    };
    static_assert(sizeof(SelfContainerHeader) == 0x20,
                  "SelfContainerHeader must be exactly 0x20 bytes");

    // One segment-table entry: 0x20 bytes total.
    struct SelfSegmentEntry {
        u64 flags;        // 0x00
        u64 file_offset;  // 0x08
        u64 file_size;    // 0x10
        u64 mem_size;     // 0x18
    };
    static_assert(sizeof(SelfSegmentEntry) == 0x20,
                  "SelfSegmentEntry must be exactly 0x20 bytes");

    // Extended info block: 0x40 bytes total.  Located after the embedded
    // ELF (header + phdrs), 16-byte aligned.
    struct SelfExtInfo {
        u64 authority_id;      // 0x00  - program authority id (PAID)
        u64 program_type;      // 0x08
        u64 app_version;       // 0x10
        u64 firmware_version;  // 0x18
        u8  digest[32];        // 0x20  - SHA-256 of the inner ELF bytes
    };
    static_assert(sizeof(SelfExtInfo) == 0x40,
                  "SelfExtInfo must be exactly 0x40 bytes");
    #pragma pack(pop)

    // Decoded view of a SELF segment-table entry.
    struct SelfSegment {
        u64 flags       = 0;
        u64 file_offset = 0;
        u64 file_size   = 0;
        u64 mem_size    = 0;

        // Program-header index encoded in bits 20..35 of `flags`.
        int  id()           const { return static_cast<int>((flags >> 20) & 0xFFFFu); }
        bool ordered()      const { return (flags & 0x1u) != 0; }
        bool encrypted()    const { return (flags & 0x2u) != 0; }
        bool is_signed()    const { return (flags & 0x4u) != 0; }
        bool compressed()   const { return (flags & 0x8u) != 0; }
        bool is_digest()    const { return (flags & 0x10000u) != 0; }
        bool blocked()      const { return (flags & 0x800u) != 0; }
    };

    // Authority-id category: the high byte of the 8-byte PAID.  PS5 tools
    // commonly use 0x31 for fake-signed debug modules, 0x45 for genuine
    // modules, and 0x48 for privileged system modules.
    enum class SelfAuthorityCategory : u8 {
        Unknown          = 0x00,
        Fake             = 0x31,  // fake-self / fPKG
        Genuine          = 0x45,  // normal retail / genuine auth_info
        PrivilegedSystem = 0x48,  // system modules
    };

    // Authority-id helpers.
    inline SelfAuthorityCategory ClassifySelfAuthority(u64 authority_id) {
        return static_cast<SelfAuthorityCategory>(
            static_cast<u8>((authority_id >> 56) & 0xFFu));
    }
    inline bool IsFakeSignedSelf(u64 authority_id) {
        return ClassifySelfAuthority(authority_id) == SelfAuthorityCategory::Fake;
    }

    // Parsed SELF image.  Does NOT contain segment data — only the
    // structural layer.  Use ExtractInnerElf to recover the embedded ELF.
    struct SelfImage {
        SelfContainerHeader          header;       // parsed container header
        std::vector<SelfSegment>     segments;     // decoded segment table

        // The embedded ELF region (header + program headers only, copied
        // verbatim from the SELF body).  May be empty if the inner file
        // could not be located / read.
        std::vector<u8>              elf_region;
        u64                          elf_region_offset = 0;  // file offset of the region

        // Extended info, when present.
        bool                         has_ext_info = false;
        SelfExtInfo                  ext_info;
        u64                          ext_info_offset = 0;
    };

    // Returns true if the file at `filepath` starts with the SELF magic.
    // Does not validate the rest of the header.
    bool IsSelfFile(const std::string& filepath);

    // Reads just the SELF container header and segment table from a file
    // already opened for binary reading.  On failure, leaves `out` in an
    // unspecified state and returns false.
    bool ParseSelfHeader(std::ifstream& file, SelfImage& out);

    // Reads the full structural layer (header + segment table + embedded
    // ELF region + optional extended info) from a file on disk.
    bool ParseSelfImage(const std::string& filepath, SelfImage& out);

    // Returns a copy of the embedded ELF (header + program headers).
    // Returns an empty vector if the SELF did not contain a parseable ELF
    // region.
    std::vector<u8> ExtractInnerElf(const SelfImage& self);

    // High-level helper: load a SELF file, extract the inner ELF bytes,
    // materialize them in a temp file, and run the standard ELF loader.
    // This works for *fake-signed* / fPKG SELF images whose segment data
    // is plaintext.  Real retail SELFs with encrypted segments cannot be
    // loaded without the root keys.
    bool LoadSelf(const std::string& filepath, LoadedModule& out_module);
}
// namespace Loader
