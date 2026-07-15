// Positive ELF corpus tests for the loader.
//
// Each test hand-crafts a minimal valid ELF64/x86-64 binary, hands it to
// Loader::Load, and asserts the post-load invariants the rest of the emulator
// relies on (entry point, base address, segment count, segment mapping).
//
// Categories exercised:
//   - valid PIE       (e_type = ET_DYN, PT_LOAD p_vaddr = 0)
//   - valid fixed     (e_type = ET_EXEC, PT_LOAD p_vaddr = 0x400000)
//   - multi-segment   (two PT_LOAD entries at distinct vaddrs)
//   - BSS             (PT_LOAD with p_memsz > p_filesz)
//   - PT_DYNAMIC      (DT_STRTAB / DT_SYMTAB / DT_RELA dynamic table)
//
// These tests do not need an external compiler — the binaries are built
// directly from the struct definitions in loader/elf.h.  The freestanding
// guest ELFs that exercise TLS and syscalls are built separately by
// `tests/test_elf/build_corpus.bat` and exercised by the smoke tests.
//
// Build target: loader_corpus_tests (see CMakeLists.txt).

#include "loader/elf.h"
#include "memory/memory.h"
#include "common/log.h"
#include "common/types.h"

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
    } while (0)

// ---------------------------------------------------------------------------
// Helpers: build a writable vector of bytes that represents a complete ELF.
//
// File layout used by this builder:
//   [Ehdr]   [Phdr * N]   [segment 0 data]   [segment 1 data]   ...
//   0         64            64 + N*56         ...
//
// The phdr table is placed immediately after the Ehdr so that data offsets
// remain stable.  The caller declares all program headers first (via
// AddPhdr), then appends each segment's file data (via AddData, which fills
// the matching phdr's p_offset and p_filesz).
// ---------------------------------------------------------------------------
struct ElfBuilder {
    u16 phdr_type = 0;            // e_type of the binary
    guest_addr_t entry = 0;        // e_entry
    std::vector<Loader::Elf64_Phdr> phdrs;
    std::vector<u8> bytes;
    u64 phdr_table_offset;         // where the phdr table will live in the file
    bool finalized = false;

    ElfBuilder(u16 type, guest_addr_t entry_)
        : phdr_type(type), entry(entry_) {
        // Reserve space for the Ehdr; the phdr table is patched in at Finalize
        // time once we know how many phdrs the user added.
        bytes.resize(sizeof(Loader::Elf64_Ehdr));
        phdr_table_offset = sizeof(Loader::Elf64_Ehdr);
    }

    // Record a program header.  p_offset is filled in by AddData; the caller
    // supplies p_vaddr / p_memsz / p_align / p_flags / p_type.  Returns a
    // reference to the recorded phdr so AddData can patch it.
    Loader::Elf64_Phdr& AddPhdr(u32 p_type, u32 p_flags, u64 p_vaddr,
                                u64 p_memsz, u64 p_align) {
        Loader::Elf64_Phdr ph{};
        ph.p_type   = p_type;
        ph.p_flags  = p_flags;
        ph.p_offset = 0; // filled in by AddData
        ph.p_vaddr  = p_vaddr;
        ph.p_paddr  = p_vaddr;
        ph.p_filesz = 0; // filled in by AddData
        ph.p_memsz  = p_memsz;
        ph.p_align  = p_align;
        phdrs.push_back(ph);
        return phdrs.back();
    }

    // Append `n` bytes from `src` to the file, and patch `ph` with the file
    // offset and on-disk size of the just-appended data.  The first call to
    // AddData also reserves the program-header table area so that subsequent
    // data does not overlap it.
    void AddData(Loader::Elf64_Phdr& ph, const void* src, size_t n) {
        if (finalized) {
            std::fprintf(stderr,
                         "[INTERNAL] ElfBuilder::AddData called after Finalize\n");
            std::abort();
        }
        // First data append: ensure space for the phdr table right after the
        // Ehdr, then append the data after that.  This avoids overwriting the
        // phdr table when Finalize runs.
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

    // Append `n` zero bytes (used to materialize BSS-style padding inside the
    // file when we want the segment to span a known length).
    void AddZeros(Loader::Elf64_Phdr& ph, size_t n) {
        std::vector<u8> zeros(n, 0);
        AddData(ph, zeros.data(), n);
    }

    // Patch the Ehdr and write the phdr table at phdr_table_offset.  Does NOT
    // truncate any data that AddData already appended after the phdr table.
    void Finalize() {
        Loader::Elf64_Ehdr ehdr{};
        ehdr.e_ident[0]  = Loader::ELFMAG0;
        ehdr.e_ident[1]  = Loader::ELFMAG1;
        ehdr.e_ident[2]  = Loader::ELFMAG2;
        ehdr.e_ident[3]  = Loader::ELFMAG3;
        ehdr.e_ident[4]  = 2;            // ELFCLASS64
        ehdr.e_ident[5]  = 1;            // ELFDATA2LSB
        ehdr.e_ident[6]  = 1;            // EV_CURRENT
        ehdr.e_ident[7]  = 0;            // ELFOSABI_NONE
        ehdr.e_type      = phdr_type;
        ehdr.e_machine   = 0x3E;         // EM_X86_64
        ehdr.e_version   = 1;            // EV_CURRENT
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
        // Ensure the phdr table region exists (AddData already grows the
        // buffer past it for data, but a no-data phdr table may not be
        // allocated yet).
        if (bytes.size() < phdr_table_offset + phdr_bytes) {
            bytes.resize(phdr_table_offset + phdr_bytes, 0);
        }
        std::memcpy(bytes.data() + phdr_table_offset, phdrs.data(), phdr_bytes);

        finalized = true;
    }

    // Write the assembled ELF to a unique temp file and return the path.
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

// A small piece of x86-64 code: `mov rax, 60; xor rdi, rdi; syscall; ret`.
// This is the sys_exit(0) call the freestanding ELFs use to terminate.
// Embedding real code (rather than random bytes) makes it safe for the
// loader test to *also* be executed manually as a sanity check.
static const u8 kExitCode[] = {
    0x48, 0xC7, 0xC0, 0x3C, 0x00, 0x00, 0x00, // mov rax, 60
    0x48, 0x31, 0xFF,                          // xor rdi, rdi
    0x0F, 0x05,                                // syscall
    0xC3                                       // ret
};

static const std::filesystem::path kCorpusDir() {
    return std::filesystem::temp_directory_path() / "pcsx5_loader_corpus";
}

// Distinct base addresses for each test.  The loader does not currently
// unmap a module on its way out, so each test must use a range that does not
// overlap any earlier test's range, otherwise Win32 VirtualAlloc will fail
// with ERROR_INVALID_ADDRESS (487) on the second call.
constexpr u64 ADDR_FIXED         = 0x100000;
constexpr u64 ADDR_MULTI_TEXT    = 0x200000;
constexpr u64 ADDR_MULTI_DATA    = 0x204000;  // 16KB-aligned so Memory::Commit accepts it
constexpr u64 ADDR_BSS           = 0x300000;
constexpr u64 ADDR_DYN           = 0x400000;

// ---------------------------------------------------------------------------
// 1. Valid PIE binary: e_type = ET_DYN, single PT_LOAD with p_vaddr = 0
// ---------------------------------------------------------------------------
void TestValidPIE() {
    std::fprintf(stdout, "[TEST] Valid PIE (ET_DYN, p_vaddr=0)\n");

    ElfBuilder b(/*type=*/3 /*ET_DYN*/, /*entry=*/0);
    auto& ph = b.AddPhdr(Loader::PT_LOAD,
                         /*p_flags=*/5 /*PF_R|PF_X*/,
                         /*p_vaddr=*/0,
                         /*p_memsz=*/sizeof(kExitCode),
                         /*p_align=*/0x1000);
    b.AddData(ph, kExitCode, sizeof(kExitCode));
    b.Finalize();

    auto path = b.Write(kCorpusDir(), "valid_pie.elf");

    Loader::LoadedModule m{};
    EXPECT(Loader::Load(path.string(), m), "Valid PIE must load");
    EXPECT_EQ(m.entry_point, (guest_addr_t)0x800000000ULL,
              "PIE entry rebased to 0x800000000");
    EXPECT_EQ(m.base_address, (guest_addr_t)0x800000000ULL,
              "PIE base rebased to 0x800000000");
    EXPECT_EQ(m.segments.size(), (size_t)1, "PIE has one mapped segment");
    EXPECT(m.segments[0].address == 0x800000000ULL,
           "PIE segment rebased to 0x800000000");
}

// ---------------------------------------------------------------------------
// 2. Valid fixed-address binary: e_type = ET_EXEC, p_vaddr = 0x100000
// ---------------------------------------------------------------------------
void TestValidFixedAddress() {
    std::fprintf(stdout, "[TEST] Valid fixed-address (ET_EXEC, p_vaddr=0x100000)\n");

    constexpr u64 TEXT_VADDR = ADDR_FIXED;

    ElfBuilder b(/*type=*/2 /*ET_EXEC*/, /*entry=*/TEXT_VADDR);
    auto& ph = b.AddPhdr(Loader::PT_LOAD, 5 /*PF_R|PF_X*/,
                         TEXT_VADDR,
                         sizeof(kExitCode), 0x200000);
    b.AddData(ph, kExitCode, sizeof(kExitCode));
    b.Finalize();

    auto path = b.Write(kCorpusDir(), "valid_fixed.elf");

    Loader::LoadedModule m{};
    EXPECT(Loader::Load(path.string(), m), "Valid fixed-address must load");
    EXPECT_EQ(m.entry_point, (guest_addr_t)TEXT_VADDR,
              "Fixed-address entry is absolute");
    EXPECT_EQ(m.base_address, (guest_addr_t)0,
              "Fixed-address base is 0 (absolute addresses)");
    EXPECT_EQ(m.segments.size(), (size_t)1, "Fixed-address has one segment");
    EXPECT_EQ(m.segments[0].address, (guest_addr_t)TEXT_VADDR,
              "Fixed-address segment is at the requested vaddr");
}

// ---------------------------------------------------------------------------
// 3. Multi-segment binary: text + data within a single contiguous range
//    (text at 0x200000, data at 0x201000).  The loader reserves the full
//    [min_vaddr, max_vaddr] range up front, so the segments must be adjacent.
//    A separate-range layout would require per-segment reservation, which is
//    scheduled for Phase 1.
// ---------------------------------------------------------------------------
void TestMultiSegment() {
    std::fprintf(stdout, "[TEST] Multi-segment (text + data at distinct vaddrs)\n");

    constexpr u64 TEXT_VADDR = ADDR_MULTI_TEXT;
    constexpr u64 DATA_VADDR = ADDR_MULTI_DATA; // right after text

    const u8 data_init[4] = {0xDE, 0xAD, 0xBE, 0xEF};

    ElfBuilder b(/*type=*/2 /*ET_EXEC*/, /*entry=*/TEXT_VADDR);
    auto& text_ph = b.AddPhdr(Loader::PT_LOAD, 5 /*PF_R|PF_X*/,
                              TEXT_VADDR,
                              sizeof(kExitCode), 0x200000);
    auto& data_ph = b.AddPhdr(Loader::PT_LOAD, 6 /*PF_R|PF_W*/,
                              DATA_VADDR,
                              sizeof(data_init), 0x200000);
    b.AddData(text_ph, kExitCode, sizeof(kExitCode));
    b.AddData(data_ph, data_init, sizeof(data_init));
    b.Finalize();

    auto path = b.Write(kCorpusDir(), "multi_segment.elf");

    Loader::LoadedModule m{};
    EXPECT(Loader::Load(path.string(), m), "Multi-segment must load");
    EXPECT_EQ(m.segments.size(), (size_t)2, "Two segments mapped");
    EXPECT(m.segments[0].address == TEXT_VADDR, "Segment 0 is text");
    EXPECT(m.segments[1].address == DATA_VADDR, "Segment 1 is data");

    // Data should be readable from guest memory.
    u8 readback[4] = {};
    Memory::ReadBuffer(DATA_VADDR, readback, sizeof(readback));
    EXPECT(std::memcmp(readback, data_init, sizeof(data_init)) == 0,
           "Data segment contents match file");
}

// ---------------------------------------------------------------------------
// 4. BSS: PT_LOAD with p_memsz > p_filesz.  The gap must be zero-filled.
// ---------------------------------------------------------------------------
void TestBSS() {
    std::fprintf(stdout, "[TEST] BSS (p_memsz > p_filesz must be zeroed)\n");

    constexpr u64 TEXT_VADDR = ADDR_BSS;
    constexpr u64 BSS_PADDING = 64; // extra zero-filled bytes after file data
    constexpr u64 TOTAL_MEMSZ  = sizeof(kExitCode) + BSS_PADDING;

    ElfBuilder b(/*type=*/2 /*ET_EXEC*/, /*entry=*/TEXT_VADDR);
    auto& ph = b.AddPhdr(Loader::PT_LOAD, 6 /*PF_R|PF_W*/,
                         TEXT_VADDR, TOTAL_MEMSZ, 0x200000);
    b.AddData(ph, kExitCode, sizeof(kExitCode));
    b.Finalize();

    auto path = b.Write(kCorpusDir(), "with_bss.elf");

    Loader::LoadedModule m{};
    EXPECT(Loader::Load(path.string(), m), "BSS-bearing ELF must load");
    EXPECT_EQ(m.segments.size(), (size_t)1, "BSS counted as one segment");
    EXPECT_EQ(m.segments[0].size, TOTAL_MEMSZ,
              "Segment size is the memory size, not the file size");

    // The BSS tail must read as zero.
    u8 zeros[BSS_PADDING];
    Memory::ReadBuffer(TEXT_VADDR + sizeof(kExitCode), zeros, sizeof(zeros));
    for (size_t i = 0; i < BSS_PADDING; ++i) {
        EXPECT_EQ(zeros[i], (u8)0, "BSS byte must be zero");
    }
}

// ---------------------------------------------------------------------------
// 5. PT_DYNAMIC: dynamic table with DT_STRTAB / DT_SYMTAB / DT_RELA.
//    This exercises the dynamic-loader code path of the ELF parser.
// ---------------------------------------------------------------------------
void TestDynamic() {
    std::fprintf(stdout, "[TEST] PT_DYNAMIC (DT_STRTAB / DT_SYMTAB / DT_RELA)\n");

    // Layout:
    //   [Ehdr]   [Phdr * 2]  [text+dyn+strtab+symtab+rela]
    //
    // We collapse everything into a single PT_LOAD at 0x400000 and a separate
    // PT_DYNAMIC whose vaddr points at the dynamic table inside the PT_LOAD.
    constexpr u64 TEXT_VADDR = ADDR_DYN;

    // Build a string table containing a single empty string (just a NUL byte)
    // and a single null symbol.  The linker's `string_table` is required to be
    // non-empty for the parser to extract needed library names.
    const u8 strtab_bytes[2] = {0x00, 0x00};
    const Loader::Elf64_Sym symtab_bytes[1] = {}; // one null entry terminates

    // One R_X86_64_NONE relocation (so RELA is non-empty).
    Loader::Elf64_Rela rela_bytes[1] = {};
    rela_bytes[0].r_info = (0ULL << 32) | Loader::R_X86_64_NONE;

    // File contents: [text][dynamic table][strtab][symtab][rela]
    // All packed into one PT_LOAD; PT_DYNAMIC vaddr points at the dynamic table.
    std::vector<u8> payload;
    auto append = [&](const void* src, size_t n) {
        const size_t off = payload.size();
        payload.resize(off + n);
        std::memcpy(payload.data() + off, src, n);
    };
    append(kExitCode, sizeof(kExitCode));
    const u64 dyn_off = payload.size();
    Loader::Elf64_Dyn dyn[5] = {};
    // Placeholders: real vaddrs are patched in after we know the load base.
    dyn[0].d_tag = Loader::DT_STRTAB;
    dyn[1].d_tag = Loader::DT_STRSZ;  dyn[1].d_un.d_val = sizeof(strtab_bytes);
    dyn[2].d_tag = Loader::DT_SYMTAB;
    dyn[3].d_tag = Loader::DT_RELA;
    dyn[4].d_tag = Loader::DT_RELASZ; dyn[4].d_un.d_val = sizeof(rela_bytes);
    append(dyn, sizeof(dyn));
    const u64 strtab_off = payload.size();
    append(strtab_bytes, sizeof(strtab_bytes));
    const u64 symtab_off = payload.size();
    append(symtab_bytes, sizeof(symtab_bytes));
    const u64 rela_off = payload.size();
    append(rela_bytes,   sizeof(rela_bytes));

    const u64 payload_size = payload.size();
    const u64 dyn_vaddr    = TEXT_VADDR + dyn_off;
    const u64 strtab_vaddr = TEXT_VADDR + strtab_off;
    const u64 symtab_vaddr = TEXT_VADDR + symtab_off;
    const u64 rela_vaddr   = TEXT_VADDR + rela_off;
    dyn[0].d_un.d_ptr = strtab_vaddr;
    dyn[2].d_un.d_ptr = symtab_vaddr;
    dyn[3].d_un.d_ptr = rela_vaddr;
    // Patch the dynamic table back into the payload now that the vaddrs are
    // known.
    std::memcpy(payload.data() + dyn_off, dyn, sizeof(dyn));

    ElfBuilder b(/*type=*/2 /*ET_EXEC*/, /*entry=*/TEXT_VADDR);
    auto& load_ph = b.AddPhdr(Loader::PT_LOAD, 6 /*PF_R|PF_W*/,
                              TEXT_VADDR, payload_size, 0x200000);
    auto& dyn_ph  = b.AddPhdr(Loader::PT_DYNAMIC, 6 /*PF_R|PF_W*/,
                              dyn_vaddr, sizeof(dyn), 8);
    b.AddData(load_ph, payload.data(), payload.size());
    // Patch the dynamic phdr so its p_offset points at the dynamic table
    // inside the file.  AddData pads the phdr table area, then appends our
    // payload; the dynamic table sits at (load_ph.p_offset + dyn_off).
    dyn_ph.p_offset = load_ph.p_offset + dyn_off;
    dyn_ph.p_filesz = sizeof(dyn);
    b.Finalize();

    auto path = b.Write(kCorpusDir(), "with_dynamic.elf");

    Loader::LoadedModule m{};
    EXPECT(Loader::Load(path.string(), m), "PT_DYNAMIC-bearing ELF must load");
    EXPECT_EQ(m.dynamic_table_size, (u64)sizeof(dyn), "Dynamic table size parsed");
    EXPECT(m.string_table.size() == sizeof(strtab_bytes), "String table parsed");
    EXPECT_EQ(m.symbols.size(), (size_t)1, "One (null) symbol parsed");
    EXPECT_EQ(m.relocations.size(), (size_t)1, "One R_X86_64_NONE relocation parsed");
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    if (!Memory::Initialize()) {
        std::fprintf(stderr, "FATAL: Memory::Initialize failed\n");
        return 2;
    }

    TestValidPIE();
    TestValidFixedAddress();
    TestMultiSegment();
    TestBSS();
    TestDynamic();

    Memory::Shutdown();

    std::fprintf(stdout, "Loader corpus: %d check(s), %d failure(s)\n",
                 g_checks, g_failures);
    if (g_failures == 0) {
        std::fprintf(stdout, "Loader corpus tests passed.\n");
        // Best-effort cleanup of fixtures
        std::error_code ec;
        std::filesystem::remove_all(kCorpusDir(), ec);
        return 0;
    }
    std::fprintf(stderr, "Loader corpus tests FAILED with %d failure(s).\n", g_failures);
    return 1;
}
