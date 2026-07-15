// Dump the dynamic-tag table of a real PS5 game ELF to identify which
// DT_* tags are actually used.  This helps us understand the gap
// between our loader (only standard DT_* + a few PT_SCE_*) and what
// real PS5 ELFs carry.
//
// Self-contained: only needs the ELF header structs and the file.  No
// Memory / HLE / Loader dependencies.

#include "common/log.h"
#include "common/types.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// Mirror the loader's ELF64 structs.
namespace {

constexpr size_t EI_NIDENT = 16;
constexpr u8 ELFMAG0 = 0x7f, ELFMAG1 = 'E', ELFMAG2 = 'L', ELFMAG3 = 'F';

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
    union { u64 d_val; u64 d_ptr; } d_un;
};

// Sony-specific DT tag name lookup.  Sourced from Kyty's loader
// (https://github.com/InoriRus/Kyty/blob/main/source/emulator/include/Emulator/Loader/Elf.h).
const char* DynTagName(s64 tag) {
    switch (tag) {
        // Standard ELF DT_*
        case 0: return "DT_NULL";
        case 1: return "DT_NEEDED";
        case 2: return "DT_PLTRELSZ";
        case 3: return "DT_PLTGOT";
        case 4: return "DT_HASH";
        case 5: return "DT_STRTAB";
        case 6: return "DT_SYMTAB";
        case 7: return "DT_RELA";
        case 8: return "DT_RELASZ";
        case 9: return "DT_RELAENT";
        case 10: return "DT_STRSZ";
        case 11: return "DT_SYMENT";
        case 12: return "DT_INIT";
        case 13: return "DT_FINI";
        case 14: return "DT_SONAME";
        case 15: return "DT_RPATH";
        case 16: return "DT_SYMBOLIC";
        case 17: return "DT_REL";
        case 18: return "DT_RELSZ";
        case 19: return "DT_RELENT";
        case 20: return "DT_PLTREL";
        case 21: return "DT_DEBUG";
        case 22: return "DT_TEXTREL";
        case 23: return "DT_JMPREL";
        case 24: return "DT_BIND_NOW";
        case 25: return "DT_INIT_ARRAY";
        case 26: return "DT_FINI_ARRAY";
        case 27: return "DT_INIT_ARRAYSZ";
        case 28: return "DT_FINI_ARRAYSZ";
        case 0x6ffffff9: return "DT_RELACOUNT";
        // PT types sometimes re-used as DT (rare but possible)
        case 0x6FFFFF00: return "DT_SCE_PROC_PARAM";
        case 0x6FFFFF01: return "DT_SCE_MODULE_PARAM";
        // Kyty DT_OS_*
        case 0x61000007: return "DT_OS_FINGERPRINT";
        case 0x61000009: return "DT_OS_ORIGINAL_FILENAME";
        case 0x6100000D: return "DT_OS_MODULE_INFO";
        case 0x6100000F: return "DT_OS_NEEDED_MODULE";
        case 0x61000011: return "DT_OS_MODULE_ATTR";
        case 0x61000013: return "DT_OS_EXPORT_LIB";
        case 0x61000015: return "DT_OS_IMPORT_LIB";
        case 0x61000017: return "DT_OS_EXPORT_LIB_ATTR";
        case 0x61000019: return "DT_OS_IMPORT_LIB_ATTR";
        case 0x61000025: return "DT_OS_HASH";
        case 0x61000027: return "DT_OS_PLTGOT";
        case 0x61000029: return "DT_OS_JMPREL";
        case 0x6100002B: return "DT_OS_PLTREL";
        case 0x6100002D: return "DT_OS_PLTRELSZ";
        case 0x6100002F: return "DT_OS_RELA";
        case 0x61000031: return "DT_OS_RELASZ";
        case 0x61000033: return "DT_OS_RELAENT";
        case 0x61000035: return "DT_OS_STRTAB";
        case 0x61000037: return "DT_OS_STRSZ";
        case 0x61000039: return "DT_OS_SYMTAB";
        case 0x6100003B: return "DT_OS_SYMENT";
        case 0x6100003D: return "DT_OS_HASHSZ";
        case 0x6100003F: return "DT_OS_SYMTABSZ";
        case 0x61000041: return "DT_OS_ORIGINAL_FILENAME_1";
        case 0x61000043: return "DT_OS_MODULE_INFO_1";
        case 0x61000045: return "DT_OS_NEEDED_MODULE_1";
        case 0x61000047: return "DT_OS_EXPORT_LIB_1";
        case 0x61000049: return "DT_OS_IMPORT_LIB_1";
        default: return nullptr;
    }
}

// Program-header (PT_*) tag name lookup.  Separated from DynTagName
// because PT_NULL and DT_NULL both equal 0.
const char* PhdrTypeName(u32 tag) {
    switch (tag) {
        case 0: return "PT_NULL";
        case 1: return "PT_LOAD";
        case 2: return "PT_DYNAMIC";
        case 3: return "PT_INTERP";
        case 4: return "PT_NOTE";
        case 5: return "PT_SHLIB";
        case 6: return "PT_PHDR";
        case 7: return "PT_TLS";
        case 0x61000000: return "PT_OS_DYNLIBDATA";
        case 0x61000001: return "PT_OS_PROCPARAM (Kyty) / PT_SCE_RELRO (ours)";
        case 0x61000010: return "PT_OS_RELRO";
        case 0x6474E550: return "PT_GNU_RELRO";
        case 0x6474E551: return "PT_GNU_PROPERTY";
        case 0x6474E552: return "PT_GNU_STACK";
        case 0x6FFFFF00: return "PT_SCE_PROC_PARAM";
        case 0x6FFFFF01: return "PT_SCE_MODULE_PARAM";
        default: return nullptr;
    }
}

constexpr const char* kDefaultEboot =
    "I:/Personal/Windows/pcsx5/Games/PPSA02929-app0/eboot.bin.esbak";

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    LogConfig::SetLevel(LogCategory::Loader, LogLevel::Warn);
    LogConfig::SetLevel(LogCategory::Memory, LogLevel::Warn);

    std::string path = (argc > 1) ? argv[1] : kDefaultEboot;
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) {
        std::fprintf(stderr, "[SKIP] %s not present\n", path.c_str());
        return 0;
    }
    const s64 sz = f.tellg();
    f.seekg(0, std::ios::beg);

    Elf64_Ehdr ehdr{};
    f.read(reinterpret_cast<char*>(&ehdr), sizeof(ehdr));
    if (ehdr.e_ident[0] != ELFMAG0 || ehdr.e_ident[1] != ELFMAG1 ||
        ehdr.e_ident[2] != ELFMAG2 || ehdr.e_ident[3] != ELFMAG3) {
        std::fprintf(stderr, "FATAL: not an ELF\n");
        return 1;
    }

    std::fprintf(stdout, "=== dump_dt of %s ===\n", path.c_str());
    std::fprintf(stdout, "  size       = %lld bytes\n", (long long)sz);
    std::fprintf(stdout, "  e_type     = 0x%04X\n", ehdr.e_type);
    std::fprintf(stdout, "  EI_CLASS   = %u (1=32, 2=64)\n", ehdr.e_ident[4]);
    std::fprintf(stdout, "  EI_DATA    = %u (1=LE, 2=BE)\n", ehdr.e_ident[5]);
    std::fprintf(stdout, "  EI_VERSION = %u\n", ehdr.e_ident[6]);
    std::fprintf(stdout, "  EI_OSABI   = %u\n", ehdr.e_ident[7]);
    std::fprintf(stdout, "  EI_ABIVERS = %u (PS5 == 2)\n", ehdr.e_ident[8]);
    std::fprintf(stdout, "  e_machine  = 0x%X\n", ehdr.e_machine);
    std::fprintf(stdout, "  e_flags    = 0x%X\n", ehdr.e_flags);
    std::fprintf(stdout, "  phnum=%u phoff=0x%llx\n",
                 ehdr.e_phnum, (unsigned long long)ehdr.e_phoff);

    // Read program headers
    std::vector<Elf64_Phdr> phdrs(ehdr.e_phnum);
    f.seekg(ehdr.e_phoff, std::ios::beg);
    f.read(reinterpret_cast<char*>(phdrs.data()),
           ehdr.e_phnum * sizeof(Elf64_Phdr));

    // Tabulate PT_*
    std::map<u32, size_t> pt_counts;
    for (const auto& p : phdrs) ++pt_counts[p.p_type];
    std::fprintf(stdout, "\nProgram-header types (p_type histogram):\n");
    for (const auto& [t, c] : pt_counts) {
        const char* name = PhdrTypeName(t);
        std::fprintf(stdout, "  0x%08X  %-40s  %zu\n", t,
                     name ? name : "(unknown)", c);
    }

    // Find PT_DYNAMIC
    for (const auto& p : phdrs) {
        if (p.p_type != 2) continue;
        std::fprintf(stdout, "\nPT_DYNAMIC: offset=0x%llx size=%llu\n",
                     (unsigned long long)p.p_offset,
                     (unsigned long long)p.p_memsz);

        const size_t n = static_cast<size_t>(p.p_memsz / sizeof(Elf64_Dyn));
        std::vector<Elf64_Dyn> dyns(n);
        f.seekg(p.p_offset, std::ios::beg);
        f.read(reinterpret_cast<char*>(dyns.data()), n * sizeof(Elf64_Dyn));

        std::map<s64, size_t> dt_counts;
        for (const auto& d : dyns) {
            if (d.d_tag == 0) break;
            ++dt_counts[d.d_tag];
        }
        std::fprintf(stdout, "Dynamic-tag histogram (d_tag, count):\n");
        for (const auto& [t, c] : dt_counts) {
            const char* name = DynTagName(t);
            const char* is_sony = (t >= 0x60000000) ? " (Sony)" : "";
            std::fprintf(stdout, "  0x%016llX  %-30s  %zu%s\n",
                         (long long)t,
                         name ? name : "(unknown)",
                         c, is_sony);
        }
        std::fprintf(stdout, "\nFull dynamic-tag list (in order):\n");
        for (size_t i = 0; i < n; ++i) {
            const auto& d = dyns[i];
            if (d.d_tag == 0) {
                std::fprintf(stdout, "  [%zu] DT_NULL\n", i);
                break;
            }
            const char* name = DynTagName(d.d_tag);
            std::fprintf(stdout, "  [%zu] 0x%016llX  %-30s  d_val=0x%llx\n",
                         i, (long long)d.d_tag,
                         name ? name : "(unknown)",
                         (unsigned long long)d.d_un.d_val);
        }
    }
    return 0;
}
