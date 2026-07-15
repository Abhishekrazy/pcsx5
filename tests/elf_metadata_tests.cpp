// ELF/SELF metadata parsing tests (Phase 1, item 1).
//
// The base `Loader::Load` returns a `LoadedModule` that contains the raw
// program-header and symbol-table data.  These tests focus on the derived
// `ModuleMetadata` view: imports vs. exports, TLS template, DT_NEEDED
// dependencies, DT_SONAME, and the module's ELF type.
//
// All scratch files live in a unique subdirectory of the process temp
// directory so the test is independent of cwd.

#include "loader/elf.h"
#include "memory/memory.h"
#include "common/log.h"
#include "common/types.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// Undefine macros from <windows.h> that collide with names in our own
// namespaces (see memory_query_tests.cpp for the full rationale).  We
// re-define PAGE_SIZE to 0x4000 to match the rest of the project.
#ifdef PAGE_SIZE
#  undef PAGE_SIZE
#endif
#ifdef ALIGN_UP
#  undef ALIGN_UP
#endif
#ifdef ALIGN_DOWN
#  undef ALIGN_DOWN
#endif
#ifndef PAGE_SIZE
#  define PAGE_SIZE 0x4000
#endif

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks   = 0;

#define EXPECT(cond, msg)                                                            \
    do {                                                                            \
        ++g_checks;                                                                 \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, msg);    \
            ++g_failures;                                                           \
        }                                                                           \
        (void)0;                                                                    \
    } while (0)

#define EXPECT_EQ(a, b, msg)                                                                    \
    do {                                                                                        \
        ++g_checks;                                                                            \
        auto _lhs = (a);                                                                        \
        auto _rhs = (b);                                                                        \
        if (!(_lhs == _rhs)) {                                                                  \
            std::fprintf(stderr, "[FAIL] %s:%d: %s  (lhs=%lld rhs=%lld)\n",                     \
                         __FILE__, __LINE__, msg,                                               \
                         (long long)_lhs, (long long)_rhs);                                     \
            ++g_failures;                                                                       \
        }                                                                                       \
        (void)0;                                                                                \
    } while (0)

// ---------------------------------------------------------------------------
// Minimal ElfBuilder — same wire format as the one in loader_corpus.cpp.
// We inline it here (rather than extract it) so this test target stays
// self-contained and easy to evolve independently.
// ---------------------------------------------------------------------------
struct ElfBuilder {
    u16 phdr_type = 0;
    guest_addr_t entry = 0;
    std::vector<Loader::Elf64_Phdr> phdrs;
    std::vector<u8> bytes;
    u64 phdr_table_offset = 0;
    bool finalized = false;

    ElfBuilder(u16 type, guest_addr_t entry_)
        : phdr_type(type), entry(entry_) {
        bytes.resize(sizeof(Loader::Elf64_Ehdr));
        phdr_table_offset = sizeof(Loader::Elf64_Ehdr);
        // Pre-reserve enough phdr capacity so references returned from
        // AddPhdr are NOT invalidated by subsequent AddPhdr calls (the
        // tests hold these references across AddPhdr / AddData sequences,
        // and a vector reallocation would turn them into dangling
        // references — the exact bug that produced p_filesz=0 in the
        // PT_LOAD segment).
        phdrs.reserve(16);
    }

    Loader::Elf64_Phdr& AddPhdr(u32 p_type, u32 p_flags, u64 p_vaddr,
                                u64 p_memsz, u64 p_align) {
        Loader::Elf64_Phdr ph{};
        ph.p_type   = p_type;
        ph.p_flags  = p_flags;
        ph.p_offset = 0;
        ph.p_vaddr  = p_vaddr;
        ph.p_paddr  = p_vaddr;
        ph.p_filesz = 0;
        ph.p_memsz  = p_memsz;
        ph.p_align  = p_align;
        phdrs.push_back(ph);
        return phdrs.back();
    }

    void AddData(Loader::Elf64_Phdr& ph, const void* src, size_t n) {
        const size_t phdr_table_bytes = phdrs.size() * sizeof(Loader::Elf64_Phdr);
        const size_t min_offset = phdr_table_offset + phdr_table_bytes;
        if (bytes.size() < min_offset) {
            bytes.resize(min_offset, 0);
        }
        const u64 off = bytes.size();
        bytes.resize(off + n);
        std::memcpy(bytes.data() + off, src, n);
        ph.p_offset = off;
        ph.p_filesz = n;
    }

    void Finalize() {
        Loader::Elf64_Ehdr ehdr{};
        ehdr.e_ident[0]  = Loader::ELFMAG0;
        ehdr.e_ident[1]  = Loader::ELFMAG1;
        ehdr.e_ident[2]  = Loader::ELFMAG2;
        ehdr.e_ident[3]  = Loader::ELFMAG3;
        ehdr.e_ident[4]  = 2;            // ELFCLASS64
        ehdr.e_ident[5]  = 1;            // ELFDATA2LSB
        ehdr.e_ident[6]  = 1;            // EV_CURRENT
        ehdr.e_ident[7]  = 0;
        ehdr.e_type      = phdr_type;
        ehdr.e_machine   = 0x3E;
        ehdr.e_version   = 1;
        ehdr.e_entry     = entry;
        ehdr.e_phoff     = phdr_table_offset;
        ehdr.e_ehsize    = sizeof(Loader::Elf64_Ehdr);
        ehdr.e_phentsize = sizeof(Loader::Elf64_Phdr);
        ehdr.e_phnum     = static_cast<u16>(phdrs.size());
        ehdr.e_flags     = 0;
        ehdr.e_shoff     = 0;
        ehdr.e_shentsize = 0;
        ehdr.e_shnum     = 0;
        ehdr.e_shstrndx  = 0;
        std::memcpy(bytes.data(), &ehdr, sizeof(ehdr));

        const size_t phdr_bytes = phdrs.size() * sizeof(Loader::Elf64_Phdr);
        if (bytes.size() < phdr_table_offset + phdr_bytes) {
            bytes.resize(phdr_table_offset + phdr_bytes, 0);
        }
        std::memcpy(bytes.data() + phdr_table_offset, phdrs.data(), phdr_bytes);
        finalized = true;
    }

    std::filesystem::path Write(const std::filesystem::path& dir,
                                const std::string& name) {
        std::filesystem::create_directories(dir);
        std::filesystem::path p = dir / name;
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        return p;
    }
};

// Tiny x86-64 exit sequence — `mov rax, 60; xor rdi, rdi; syscall; ret`.
static const u8 kExitCode[] = {
    0x48, 0xC7, 0xC0, 0x3C, 0x00, 0x00, 0x00,
    0x48, 0x31, 0xFF,
    0x0F, 0x05,
    0xC3
};

// Helper that builds a st_info byte.  Defined as a function (not a macro)
// because MSVC's parser emits C2589 when it sees `Loader::MACRO(...)` — the
// parser cannot handle a `(` token on the right side of `::`.  The semantics
// match the standard ELF64_ST_INFO macro: bind << 4 | (type & 0xf).
static u8 MakeStInfo(u8 bind, u8 type) {
    return static_cast<u8>((bind << 4) | (type & 0xf));
}

static const std::filesystem::path kDir() {
    return std::filesystem::temp_directory_path() / "pcsx5_elf_metadata";
}

// ---------------------------------------------------------------------------
// 1. e_type / is_pie classification
// ---------------------------------------------------------------------------
void TestETypeClassification() {
    std::fprintf(stdout, "[TEST] e_type / is_pie classification\n");

    // PIE (ET_DYN) with PT_LOAD vaddr = 0
    {
        ElfBuilder b(/*type=*/Loader::ET_DYN, /*entry=*/0);
        auto& ph = b.AddPhdr(Loader::PT_LOAD, 5 /*PF_R|PF_X*/,
                             0, sizeof(kExitCode), 0x1000);
        b.AddData(ph, kExitCode, sizeof(kExitCode));
        b.Finalize();
        auto path = b.Write(kDir(), "pie.elf");
        Loader::LoadedModule m{};
        EXPECT(Loader::Load(path.string(), m), "PIE loads");
        EXPECT_EQ(m.e_type, (u16)Loader::ET_DYN, "PIE e_type is ET_DYN");
        EXPECT(m.is_pie, "PIE is_pie flag is true");
    }

    // Fixed-address (ET_EXEC)
    {
        ElfBuilder b(/*type=*/Loader::ET_EXEC, /*entry=*/0x500000);
        auto& ph = b.AddPhdr(Loader::PT_LOAD, 5,
                             0x500000, sizeof(kExitCode), 0x1000);
        b.AddData(ph, kExitCode, sizeof(kExitCode));
        b.Finalize();
        auto path = b.Write(kDir(), "fixed.elf");
        Loader::LoadedModule m{};
        EXPECT(Loader::Load(path.string(), m), "Fixed loads");
        EXPECT_EQ(m.e_type, (u16)Loader::ET_EXEC, "Fixed e_type is ET_EXEC");
        EXPECT(!m.is_pie, "Fixed is_pie flag is false");
    }
}

// ---------------------------------------------------------------------------
// 2. PT_TLS extraction
// ---------------------------------------------------------------------------
void TestPts() {
    std::fprintf(stdout, "[TEST] PT_TLS template extraction\n");

    constexpr u64 TLS_FILESZ = 16;
    constexpr u64 TLS_MEMSZ  = 32;  // p_memsz > p_filesz → template + zero padding
    constexpr u64 TLS_ALIGN  = 0x20;

    // Build a payload of [text | dyn | strtab | symtab | rela | tls_template]
    // We will point PT_TLS at the tls_template offset inside the PT_LOAD.
    std::vector<u8> tls_template(TLS_FILESZ, 0xCD);

    std::vector<u8> payload;
    auto append = [&](const void* src, size_t n) {
        const size_t off = payload.size();
        payload.resize(off + n);
        std::memcpy(payload.data() + off, src, n);
    };
    append(kExitCode, sizeof(kExitCode));
    const u64 dyn_off = payload.size();

    // strtab with a NUL byte and a SONAME we'll reference.
    const char strtab_bytes[] = "\0libfoo.so.1";
    const u64 soname_off = 1;  // index of "libfoo.so.1" in strtab

    // Build a dynamic table with DT_STRTAB, DT_STRSZ, DT_SONAME.
    Loader::Elf64_Dyn dyn[4] = {};
    dyn[0].d_tag = Loader::DT_STRTAB;
    dyn[1].d_tag = Loader::DT_STRSZ;   dyn[1].d_un.d_val = sizeof(strtab_bytes);
    dyn[2].d_tag = Loader::DT_SONAME;  dyn[2].d_un.d_val = soname_off;
    dyn[3].d_tag = Loader::DT_NULL;
    append(dyn, sizeof(dyn));
    const u64 strtab_off = payload.size();
    append(strtab_bytes, sizeof(strtab_bytes));
    const u64 tls_off = payload.size();
    append(tls_template.data(), tls_template.size());
    const u64 payload_size = payload.size();

    constexpr u64 BASE = 0x600000;
    const u64 dyn_vaddr    = BASE + dyn_off;
    const u64 strtab_vaddr = BASE + strtab_off;
    const u64 tls_vaddr    = BASE + tls_off;
    dyn[0].d_un.d_ptr = strtab_vaddr;
    std::memcpy(payload.data() + dyn_off, dyn, sizeof(dyn));

    ElfBuilder b(Loader::ET_DYN, 0);
    auto& load_ph = b.AddPhdr(Loader::PT_LOAD, 6 /*PF_R|PF_W*/,
                              BASE, payload_size, 0x200000);
    auto& dyn_ph  = b.AddPhdr(Loader::PT_DYNAMIC, 6,
                              dyn_vaddr, sizeof(dyn), 8);
    auto& tls_ph  = b.AddPhdr(Loader::PT_TLS, 4 /*PF_R*/,
                              tls_vaddr, TLS_MEMSZ, TLS_ALIGN);
    b.AddData(load_ph, payload.data(), payload.size());
    dyn_ph.p_offset = load_ph.p_offset + dyn_off;
    dyn_ph.p_filesz = sizeof(dyn);
    // PT_TLS uses the same file offset as the template blob
    tls_ph.p_offset = load_ph.p_offset + tls_off;
    tls_ph.p_filesz = TLS_FILESZ;
    b.Finalize();

    auto path = b.Write(kDir(), "with_tls.elf");
    Loader::LoadedModule m{};
    EXPECT(Loader::Load(path.string(), m), "TLS-bearing ELF loads");
    EXPECT(m.has_tls, "has_tls flag set");
    EXPECT_EQ(m.tls_file_size, TLS_FILESZ, "tls_file_size parsed");
    EXPECT_EQ(m.tls_mem_size,  TLS_MEMSZ,  "tls_mem_size parsed");
    EXPECT_EQ(m.tls_align,     TLS_ALIGN,  "tls_align parsed");
    EXPECT_EQ(m.tls_template_offset, tls_ph.p_offset, "tls file offset parsed");

    // The ModuleMetadata view should mirror the raw fields.
    Loader::ModuleMetadata meta{};
    Loader::ParseModuleMetadata(m, meta);
    EXPECT(meta.has_tls, "metadata has_tls set");
    EXPECT_EQ(meta.tls.file_size, TLS_FILESZ, "metadata tls file size");
    EXPECT_EQ(meta.tls.mem_size,  TLS_MEMSZ,  "metadata tls mem size");
    EXPECT_EQ(meta.tls.align,     TLS_ALIGN,  "metadata tls align");

    // DT_SONAME was extracted from the dynamic table.
    EXPECT(m.soname == "libfoo.so.1", "DT_SONAME parsed");
}

// ---------------------------------------------------------------------------
// 3. DT_NEEDED + dependency list
// ---------------------------------------------------------------------------
void TestDtNeeded() {
    std::fprintf(stdout, "[TEST] DT_NEEDED dependency extraction\n");

    constexpr u64 BASE = 0x700000;

    // strtab: "\0libc.so.6\0libpthread.so.0"
    const char strtab_bytes[] = "\0libc.so.6\0libpthread.so.0";
    const u64 needed_a_off = 1;  // index of "libc.so.6"
    const u64 needed_b_off = 1 + sizeof("libc.so.6");  // index of "libpthread.so.0"

    std::vector<u8> payload;
    auto append = [&](const void* src, size_t n) {
        const size_t off = payload.size();
        payload.resize(off + n);
        std::memcpy(payload.data() + off, src, n);
    };
    append(kExitCode, sizeof(kExitCode));
    const u64 dyn_off = payload.size();

    Loader::Elf64_Dyn dyn[5] = {};
    dyn[0].d_tag = Loader::DT_STRTAB;
    dyn[1].d_tag = Loader::DT_STRSZ;   dyn[1].d_un.d_val = sizeof(strtab_bytes);
    dyn[2].d_tag = Loader::DT_NEEDED;  dyn[2].d_un.d_val = needed_a_off;
    dyn[3].d_tag = Loader::DT_NEEDED;  dyn[3].d_un.d_val = needed_b_off;
    dyn[4].d_tag = Loader::DT_NULL;
    append(dyn, sizeof(dyn));
    const u64 strtab_off = payload.size();
    append(strtab_bytes, sizeof(strtab_bytes));
    const u64 payload_size = payload.size();

    const u64 dyn_vaddr    = BASE + dyn_off;
    const u64 strtab_vaddr = BASE + strtab_off;
    dyn[0].d_un.d_ptr = strtab_vaddr;
    std::memcpy(payload.data() + dyn_off, dyn, sizeof(dyn));

    std::fprintf(stderr, "[DEBUG-with_needed] sizeof(Elf64_Dyn)=%zu  payload_size=%zu  strtab_off=%zu  strtab_vaddr=0x%llx\n",
                 sizeof(Loader::Elf64_Dyn), (size_t)payload_size, (size_t)strtab_off, (unsigned long long)strtab_vaddr);
    std::fprintf(stderr, "[DEBUG-with_needed] sizeof(strtab_bytes)=%zu  sizeof(dyn)=%zu  sizeof(kExitCode)=%zu\n",
                 sizeof(strtab_bytes), sizeof(dyn), sizeof(kExitCode));
    std::fprintf(stderr, "[DEBUG-with_needed] kExitCode is at offset 0, dyn at %zu, strtab at %zu\n",
                 (size_t)dyn_off, (size_t)strtab_off);

    ElfBuilder b(Loader::ET_DYN, 0);
    auto& load_ph = b.AddPhdr(Loader::PT_LOAD, 6, BASE, payload_size, 0x200000);
    auto& dyn_ph  = b.AddPhdr(Loader::PT_DYNAMIC, 6, dyn_vaddr, sizeof(dyn), 8);
    b.AddData(load_ph, payload.data(), payload.size());
    dyn_ph.p_offset = load_ph.p_offset + dyn_off;
    dyn_ph.p_filesz = sizeof(dyn);
    b.Finalize();

    auto path = b.Write(kDir(), "with_needed.elf");
    Loader::LoadedModule m{};
    EXPECT(Loader::Load(path.string(), m), "DT_NEEDED-bearing ELF loads");

    // Targeted debug: read strtab back as both raw bytes and as a string
    std::fprintf(stderr, "[DBG] strtab host-addr=%p size=%zu  strtab_off=%zu\n",
                 (const void*)(uintptr_t)(BASE + strtab_off),
                 sizeof(strtab_bytes), (size_t)strtab_off);
    {
        u8 raw[64] = {};
        const size_t want = (sizeof(strtab_bytes) < sizeof(raw)) ? sizeof(strtab_bytes) : sizeof(raw);
        Memory::ReadBuffer(BASE + strtab_off, raw, want);
        std::fprintf(stderr, "[DBG] raw hex:");
        for (size_t i = 0; i < want; ++i) std::fprintf(stderr, " %02x", raw[i]);
        std::fprintf(stderr, "\n");
        std::fprintf(stderr, "[DBG] raw as string: '%.27s'\n", (const char*)raw);
    }
    std::fprintf(stderr, "[DBG] m.needed_libraries.size() = %zu\n", m.needed_libraries.size());
    for (size_t i = 0; i < m.needed_libraries.size(); ++i) {
        std::fprintf(stderr, "[DBG] m.needed_libraries[%zu] = '%s'\n",
                     i, m.needed_libraries[i].c_str());
    }
    EXPECT_EQ(m.needed_libraries.size(), (size_t)2, "two DT_NEEDED libs parsed");
    EXPECT(m.needed_libraries[0] == "libc.so.6",       "first dep is libc.so.6");
    EXPECT(m.needed_libraries[1] == "libpthread.so.0", "second dep is libpthread.so.0");

    Loader::ModuleMetadata meta{};
    Loader::ParseModuleMetadata(m, meta);
    EXPECT_EQ(meta.dependencies.size(), (size_t)2, "metadata dep count");
    EXPECT(meta.dependencies[0] == "libc.so.6", "metadata dep[0]");
    EXPECT(meta.dependencies[1] == "libpthread.so.0", "metadata dep[1]");
}

// ---------------------------------------------------------------------------
// 4. Imports vs. exports classification
//
// We synthesise a tiny dynamic symbol table with:
//   - one STB_GLOBAL STT_FUNC export (`my_func`)
//   - one STB_GLOBAL STT_NOTYPE import (`imp_func`)
//   - one STB_WEAK STT_FUNC weak import (`weak_func`)
//   - one STB_LOCAL symbol (should be ignored)
// and assert ParseModuleMetadata splits them correctly.
// ---------------------------------------------------------------------------
void TestImportExportClassification() {
    std::fprintf(stdout, "[TEST] Import / export classification\n");

    constexpr u64 BASE = 0x800000;

    // String table: "\0my_func\0imp_func\0weak_func"
    const char strtab_bytes[] = "\0my_func\0imp_func\0weak_func";
    const u64 my_func_off  = 1;
    const u64 imp_func_off = 1 + sizeof("my_func");
    const u64 weak_off     = imp_func_off + sizeof("imp_func");

    // Symbol table:
    //   [0] null entry (mandatory)
    //   [1] STB_GLOBAL STT_FUNC   my_func     (shndx = 1, value = 0x1000)
    //   [2] STB_GLOBAL STT_NOTYPE imp_func    (shndx = 0, value = 0)
    //   [3] STB_WEAK   STT_FUNC   weak_func   (shndx = 0, value = 0)
    //   [4] STB_LOCAL  STT_FUNC   hidden_fn   (shndx = 1, value = 0x2000)
    Loader::Elf64_Sym syms[5] = {};
    syms[1].st_name  = static_cast<u32>(my_func_off);
    syms[1].st_info  = MakeStInfo(Loader::STB_GLOBAL, Loader::STT_FUNC);
    syms[1].st_other = 0;
    syms[1].st_shndx = 1;
    syms[1].st_value = 0x1000;
    syms[1].st_size  = 16;
    syms[2].st_name  = static_cast<u32>(imp_func_off);
    syms[2].st_info  = MakeStInfo(Loader::STB_GLOBAL, Loader::STT_NOTYPE);
    syms[2].st_other = 0;
    syms[2].st_shndx = 0;  // SHN_UNDEF
    syms[2].st_value = 0;
    syms[2].st_size  = 0;
    syms[3].st_name  = static_cast<u32>(weak_off);
    syms[3].st_info  = MakeStInfo(Loader::STB_WEAK, Loader::STT_FUNC);
    syms[3].st_other = 0;
    syms[3].st_shndx = 0;
    syms[3].st_value = 0;
    syms[3].st_size  = 0;
    syms[4].st_name  = 0;  // unnamed local — must be ignored
    syms[4].st_info  = MakeStInfo(Loader::STB_LOCAL, Loader::STT_FUNC);
    syms[4].st_other = 0;
    syms[4].st_shndx = 1;
    syms[4].st_value = 0x2000;
    syms[4].st_size  = 8;

    std::vector<u8> payload;
    auto append = [&](const void* src, size_t n) {
        const size_t off = payload.size();
        payload.resize(off + n);
        std::memcpy(payload.data() + off, src, n);
    };
    append(kExitCode, sizeof(kExitCode));
    const u64 dyn_off = payload.size();

    Loader::Elf64_Dyn dyn[4] = {};
    dyn[0].d_tag = Loader::DT_STRTAB;
    dyn[1].d_tag = Loader::DT_STRSZ;  dyn[1].d_un.d_val = sizeof(strtab_bytes);
    dyn[2].d_tag = Loader::DT_SYMTAB;
    dyn[3].d_tag = Loader::DT_NULL;
    append(dyn, sizeof(dyn));
    const u64 strtab_off = payload.size();
    append(strtab_bytes, sizeof(strtab_bytes));
    const u64 symtab_off = payload.size();
    append(syms, sizeof(syms));
    const u64 payload_size = payload.size();

    const u64 dyn_vaddr    = BASE + dyn_off;
    const u64 strtab_vaddr = BASE + strtab_off;
    const u64 symtab_vaddr = BASE + symtab_off;
    dyn[0].d_un.d_ptr = strtab_vaddr;
    dyn[2].d_un.d_ptr = symtab_vaddr;
    std::memcpy(payload.data() + dyn_off, dyn, sizeof(dyn));

    ElfBuilder b(Loader::ET_DYN, 0);
    auto& load_ph = b.AddPhdr(Loader::PT_LOAD, 6, BASE, payload_size, 0x200000);
    auto& dyn_ph  = b.AddPhdr(Loader::PT_DYNAMIC, 6, dyn_vaddr, sizeof(dyn), 8);
    b.AddData(load_ph, payload.data(), payload.size());
    dyn_ph.p_offset = load_ph.p_offset + dyn_off;
    dyn_ph.p_filesz = sizeof(dyn);
    b.Finalize();

    auto path = b.Write(kDir(), "with_syms.elf");
    Loader::LoadedModule m{};
    EXPECT(Loader::Load(path.string(), m), "Symbol-bearing ELF loads");
    EXPECT_EQ(m.symbols.size(), (size_t)5, "All five symbols loaded");

    Loader::ModuleMetadata meta{};
    Loader::ParseModuleMetadata(m, meta);

    // Two exports: my_func and (any STB_GLOBAL/WEAK + shndx!=0 + name) only
    // counts `my_func` here because the local symbol has st_name == 0 and is
    // filtered out.
    EXPECT_EQ(meta.exports.size(), (size_t)1, "One export");
    EXPECT(meta.exports[0].name == "my_func", "Export is my_func");
    EXPECT(meta.exports[0].sym_type == Loader::STT_FUNC, "Export type is FUNC");
    EXPECT_EQ(meta.exports[0].address, (guest_addr_t)(m.base_address + 0x1000),
              "Export address rebased by base");
    EXPECT_EQ(meta.exports[0].size, (u64)16, "Export size preserved");

    // Two imports: imp_func (GLOBAL) and weak_func (WEAK).
    EXPECT_EQ(meta.imports.size(), (size_t)2, "Two imports");
    bool saw_global = false, saw_weak = false;
    for (const auto& imp : meta.imports) {
        if (imp.name == "imp_func") {
            saw_global = true;
            EXPECT(imp.sym_bind == Loader::STB_GLOBAL, "imp_func is GLOBAL");
            EXPECT(!imp.is_weak, "imp_func is not weak");
        } else if (imp.name == "weak_func") {
            saw_weak = true;
            EXPECT(imp.sym_bind == Loader::STB_WEAK, "weak_func is WEAK");
            EXPECT(imp.is_weak, "weak_func weak flag");
        }
    }
    EXPECT(saw_global, "imp_func found");
    EXPECT(saw_weak,   "weak_func found");
}

// ---------------------------------------------------------------------------
// 5. Referenced import counting (RELA + JMPREL)
// ---------------------------------------------------------------------------
void TestReferencedImports() {
    std::fprintf(stdout, "[TEST] Referenced import counting\n");

    constexpr u64 BASE = 0x900000;

    const char strtab_bytes[] = "\0imp_a\0imp_b";
    const u64 a_off = 1;
    const u64 b_off = 1 + sizeof("imp_a");

    Loader::Elf64_Sym syms[3] = {};
    syms[1].st_name  = static_cast<u32>(a_off);
    syms[1].st_info  = MakeStInfo(Loader::STB_GLOBAL, Loader::STT_FUNC);
    syms[1].st_shndx = 0;  // import
    syms[2].st_name  = static_cast<u32>(b_off);
    syms[2].st_info  = MakeStInfo(Loader::STB_GLOBAL, Loader::STT_FUNC);
    syms[2].st_shndx = 0;  // import

    // Two RELA relocations: one targeting imp_a, one targeting imp_b.
    // One PLT relocation: one targeting imp_a.
    Loader::Elf64_Rela relas[2] = {};
    relas[0].r_info = (1ULL << 32) | Loader::R_X86_64_GLOB_DAT;  // -> sym[1] = imp_a
    relas[1].r_info = (2ULL << 32) | Loader::R_X86_64_GLOB_DAT;  // -> sym[2] = imp_b
    Loader::Elf64_Rela plts[1]  = {};
    plts[0].r_info  = (1ULL << 32) | Loader::R_X86_64_JUMP_SLOT; // -> sym[1] = imp_a

    std::vector<u8> payload;
    auto append = [&](const void* src, size_t n) {
        const size_t off = payload.size();
        payload.resize(off + n);
        std::memcpy(payload.data() + off, src, n);
    };
    append(kExitCode, sizeof(kExitCode));
    const u64 dyn_off = payload.size();

    Loader::Elf64_Dyn dyn[8] = {};
    dyn[0].d_tag = Loader::DT_STRTAB;
    dyn[1].d_tag = Loader::DT_STRSZ;   dyn[1].d_un.d_val = sizeof(strtab_bytes);
    dyn[2].d_tag = Loader::DT_SYMTAB;
    dyn[3].d_tag = Loader::DT_RELA;     // will be patched
    dyn[4].d_tag = Loader::DT_RELASZ;   dyn[4].d_un.d_val = sizeof(relas);
    dyn[5].d_tag = Loader::DT_JMPREL;   // will be patched
    dyn[6].d_tag = Loader::DT_PLTRELSZ; dyn[6].d_un.d_val = sizeof(plts);
    dyn[7].d_tag = Loader::DT_NULL;
    append(dyn, sizeof(dyn));
    const u64 strtab_off = payload.size();
    append(strtab_bytes, sizeof(strtab_bytes));
    const u64 symtab_off = payload.size();
    append(syms, sizeof(syms));
    const u64 rela_off = payload.size();
    append(relas, sizeof(relas));
    const u64 plt_off = payload.size();
    append(plts, sizeof(plts));
    const u64 payload_size = payload.size();

    const u64 dyn_vaddr    = BASE + dyn_off;
    const u64 strtab_vaddr = BASE + strtab_off;
    const u64 symtab_vaddr = BASE + symtab_off;
    const u64 rela_vaddr   = BASE + rela_off;
    const u64 plt_vaddr    = BASE + plt_off;
    dyn[0].d_un.d_ptr = strtab_vaddr;
    dyn[2].d_un.d_ptr = symtab_vaddr;
    dyn[3].d_un.d_ptr = rela_vaddr;
    dyn[5].d_un.d_ptr = plt_vaddr;
    std::memcpy(payload.data() + dyn_off, dyn, sizeof(dyn));

    ElfBuilder b(Loader::ET_DYN, 0);
    auto& load_ph = b.AddPhdr(Loader::PT_LOAD, 6, BASE, payload_size, 0x200000);
    auto& dyn_ph  = b.AddPhdr(Loader::PT_DYNAMIC, 6, dyn_vaddr, sizeof(dyn), 8);
    b.AddData(load_ph, payload.data(), payload.size());
    dyn_ph.p_offset = load_ph.p_offset + dyn_off;
    dyn_ph.p_filesz = sizeof(dyn);
    b.Finalize();

    auto path = b.Write(kDir(), "with_refs.elf");
    Loader::LoadedModule m{};
    EXPECT(Loader::Load(path.string(), m), "Ref-bearing ELF loads");
    EXPECT_EQ(m.relocations.size(),    (size_t)2, "Two RELA relocs");
    EXPECT_EQ(m.plt_relocations.size(),(size_t)1, "One PLT reloc");

    Loader::ModuleMetadata meta{};
    Loader::ParseModuleMetadata(m, meta);

    EXPECT_EQ(meta.imports.size(), (size_t)2, "Two imports");
    // imp_a: 1 RELA + 1 PLT  → rela_refs=1, plt_refs=1, referenced=true
    // imp_b: 1 RELA + 0 PLT   → rela_refs=1, plt_refs=0, referenced=true
    for (const auto& imp : meta.imports) {
        if (imp.name == "imp_a") {
            EXPECT_EQ(imp.rela_refs, (u32)1, "imp_a RELA refs");
            EXPECT_EQ(imp.plt_refs,  (u32)1, "imp_a PLT refs");
        } else if (imp.name == "imp_b") {
            EXPECT_EQ(imp.rela_refs, (u32)1, "imp_b RELA refs");
            EXPECT_EQ(imp.plt_refs,  (u32)0, "imp_b PLT refs");
        }
    }
    EXPECT_EQ(meta.referenced_import_count, (u32)2, "Both imports are referenced");
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    // Enable debug-level logging from Loader so we can see segment-copy
    // diagnostics.
    LogConfig::SetLevel(LogCategory::Loader, LogLevel::Debug);

    if (!Memory::Initialize()) {
        std::fprintf(stderr, "FATAL: Memory::Initialize failed\n");
        return 2;
    }

    TestETypeClassification();
    TestPts();
    TestDtNeeded();
    TestImportExportClassification();
    TestReferencedImports();

    Memory::Shutdown();

    // Cleanup
    std::error_code ec;
    std::filesystem::remove_all(kDir(), ec);

    std::fprintf(stdout, "ELF metadata: %d check(s), %d failure(s)\n",
                 g_checks, g_failures);
    if (g_failures == 0) {
        std::fprintf(stdout, "ELF metadata tests passed.\n");
        return 0;
    }
    std::fprintf(stderr, "ELF metadata tests FAILED with %d failure(s).\n", g_failures);
    return 1;
}
