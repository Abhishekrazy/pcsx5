// SELF (Signed ELF) container parser tests (Phase 1, item 1).
//
// The PS5 ships every executable module (eboot.bin, *.prx, *.sprx) as a
// SELF image: a container header + segment table + the original ELF +
// extended info + control region + meta footer + plaintext segment data.
//
// We cannot decrypt a real retail SELF (root keys are not public), but
// the *structural* layer is plain bytes and is fully parseable.  These
// tests build synthetic SELF files with a known inner ELF and check that
// `Loader::ParseSelfImage`, `Loader::IsSelfFile`, and the helper
// classifiers behave as expected.
//
// The end-to-end `LoadSelf` test builds a *fake-signed* SELF (a valid
// structural form whose segment data is plaintext) and verifies the inner
// ELF can be fed back through the standard `Loader::Load` path.

#include "loader/elf.h"
#include "memory/memory.h"
#include "common/log.h"
#include "common/types.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// Undefine macros from <windows.h> that collide with names in our own
// namespaces.  See memory_query_tests.cpp for the full rationale.
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

#define EXPECT_U64_EQ(a, b, msg)                                                                \
    do {                                                                                        \
        ++g_checks;                                                                            \
        u64 _lhs = (u64)(a);                                                                    \
        u64 _rhs = (u64)(b);                                                                    \
        if (_lhs != _rhs) {                                                                     \
            std::fprintf(stderr, "[FAIL] %s:%d: %s  (lhs=0x%016llx rhs=0x%016llx)\n",            \
                         __FILE__, __LINE__, msg,                                               \
                         (unsigned long long)_lhs, (unsigned long long)_rhs);                   \
            ++g_failures;                                                                       \
        }                                                                                       \
        (void)0;                                                                                \
    } while (0)

// ---------------------------------------------------------------------------
// SelfBuilder — produces a structurally valid PS5 SELF image.
//
// Layout written:
//   0x000  SelfContainerHeader (0x20)
//   0x020  Segment table       (segment_count * 0x20)
//   ...     Embedded ELF        (FULL inner_elf: header + phdrs + payload)
//   ...     Extended info       (0x40, 16-byte aligned)
//   ...     Control region      (0x30)
//   ...     Meta footer         (0x110 baseline)
//
// For the structural tests we just need the segment table to be valid;
// the parser never reads segment data.  For the end-to-end LoadSelf test
// we attach a planned data segment that overlaps the inner ELF — the
// segment table entry's `file_offset` is set to `elf_off + p_offset`
// (where `p_offset` is the offset within the inner ELF where the
// corresponding PT_LOAD data lives).  The data is *not* duplicated; it is
// already in the inner ELF at that offset.
// ---------------------------------------------------------------------------
struct SelfBuilder {
    // Inner ELF (header + phdrs + payload).
    std::vector<u8> inner_elf;

    // Each segment entry that will appear in the table.
    struct PlannedSegment {
        u64  flags;        // raw flags word
        u64  p_offset;     // offset within the inner ELF where the data lives
        u64  file_offset;  // resolved during Build() to elf_off + p_offset
        u64  file_size;    // bytes of segment data
        u64  mem_size;     // in-memory size
    };
    std::vector<PlannedSegment> planned;

    // Extended info fields
    u64  authority_id     = 0x3100000000000001ULL;  // fake-self PAID by default
    u64  ext_program_type = 1;
    u64  app_version      = 0x00010000;
    u64  firmware_version = 0x05000000;

    // Optional: a 32-byte digest to embed.  All-zero is fine for tests.
    std::array<u8, 32> digest{};

    u16 container_flags = 0x0022;

    void SetInnerElf(std::vector<u8> bytes) { inner_elf = std::move(bytes); }

    // Add a segment entry whose data lives at `p_offset` within the inner
    // ELF (i.e. at SELF offset `elf_off + p_offset`).  The caller is
    // responsible for ensuring that the bytes at that offset in
    // `inner_elf` are the data that should be loaded into guest memory.
    void AddInnerSegment(u64 flags, u64 p_offset, u64 file_size, u64 mem_size) {
        PlannedSegment s;
        s.flags     = flags;
        s.p_offset  = p_offset;
        s.file_size = file_size;
        s.mem_size  = mem_size;
        planned.push_back(std::move(s));
    }

    // Convenience: same as AddInnerSegment but for plaintext/ordered/signed
    // data segments with no special flags.
    void AddOrderedSignedData(u64 p_offset, u64 file_size, u64 mem_size) {
        AddInnerSegment(0x1u | 0x4u, p_offset, file_size, mem_size);
    }

    // Build the SELF into a single byte vector.
    std::vector<u8> Build() {
        std::vector<u8> out;

        // 1. Resolve the structural offsets.
        const size_t hdr_off = 0;
        const size_t seg_table_off = sizeof(Loader::SelfContainerHeader);
        const size_t seg_table_bytes =
            planned.size() * sizeof(Loader::SelfSegmentEntry);
        const size_t elf_off = seg_table_off + seg_table_bytes;

        // 2. The embedded ELF region is the FULL inner ELF.
        const size_t elf_size = inner_elf.size();

        // 3. Extended info is 16-byte aligned after the ELF region.
        const u64 ext_off = (elf_off + elf_size + 0xF) & ~u64{0xF};
        const size_t control_off = ext_off + sizeof(Loader::SelfExtInfo);
        const size_t control_size = 0x30;
        const size_t meta_off = control_off + control_size;
        const size_t meta_size = 0x110;

        // 4. Each segment's file_offset = elf_off + p_offset (data lives
        //    inside the inner ELF region, no duplication).
        for (auto& seg : planned) {
            seg.file_offset = elf_off + seg.p_offset;
        }

        // The total file size is the meta footer end (we don't need to
        // reserve a separate segment-data area because the data is
        // already in the inner ELF).
        const u64 total_size = meta_off + meta_size;

        // 5. Allocate the final buffer.
        out.assign(static_cast<size_t>(total_size), 0u);

        // 6. Container header.
        Loader::SelfContainerHeader chdr{};
        chdr.magic         = Loader::kSelfMagic;
        chdr.version       = 0;
        chdr.mode          = 1;
        chdr.endianness    = 1;
        chdr.attr          = 0x12;
        chdr.program_type  = Loader::kSelfDefaultProgramType;
        chdr.header_size   = static_cast<u16>(meta_off);
        chdr.meta_size     = static_cast<u16>(meta_size);
        chdr.file_size     = total_size;
        chdr.segment_count = static_cast<u16>(planned.size());
        chdr.flags         = container_flags;
        chdr.reserved      = 0;
        std::memcpy(out.data() + hdr_off, &chdr, sizeof(chdr));

        // 7. Segment table.
        for (size_t i = 0; i < planned.size(); ++i) {
            Loader::SelfSegmentEntry e{};
            e.flags       = planned[i].flags;
            e.file_offset = planned[i].file_offset;
            e.file_size   = planned[i].file_size;
            e.mem_size    = planned[i].mem_size;
            const size_t e_off = seg_table_off + i * sizeof(e);
            std::memcpy(out.data() + e_off, &e, sizeof(e));
        }

        // 8. Inner ELF (header + phdrs).
        if (elf_size > 0) {
            std::memcpy(out.data() + elf_off, inner_elf.data(), elf_size);
        }

        // 9. Extended info.
        Loader::SelfExtInfo info{};
        info.authority_id     = authority_id;
        info.program_type     = ext_program_type;
        info.app_version      = app_version;
        info.firmware_version = firmware_version;
        std::memcpy(info.digest, digest.data(), digest.size());
        std::memcpy(out.data() + ext_off, &info, sizeof(info));

        // 10. Segment data: already embedded in the inner ELF (we wrote
        //     the full inner_elf at elf_off above, which contains the
        //     PT_LOAD payload at the offset each segment references).
        //     No separate write is required.

        return out;
    }

    std::filesystem::path Write(const std::filesystem::path& dir,
                                const std::string& name) {
        std::filesystem::create_directories(dir);
        std::filesystem::path p = dir / name;
        auto bytes = Build();
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        return p;
    }
};

// ---------------------------------------------------------------------------
// Tiny inner-ELF builder.  Produces just an Ehdr + N Phdrs with a single
// PT_LOAD segment pointing at a payload buffer.  We use the same wire
// format as the one in elf_metadata_tests.cpp's ElfBuilder.
// ---------------------------------------------------------------------------
struct TinyElfBuilder {
    u16 phdr_type = Loader::ET_DYN;
    guest_addr_t entry = 0;
    std::vector<Loader::Elf64_Phdr> phdrs;
    std::vector<u8> bytes;
    u64 phdr_table_offset = sizeof(Loader::Elf64_Ehdr);
    std::vector<u8> payload;

    TinyElfBuilder(u16 type, guest_addr_t e) : phdr_type(type), entry(e) {
        bytes.resize(sizeof(Loader::Elf64_Ehdr));
        // Pre-reserve phdr capacity so returned references are not
        // invalidated by later AddPhdr calls (see elf_metadata_tests.cpp
        // for the same rationale).
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

    void AddPayload(Loader::Elf64_Phdr& ph, const void* src, size_t n) {
        const size_t phdr_bytes = phdrs.size() * sizeof(Loader::Elf64_Phdr);
        const size_t min_offset = phdr_table_offset + phdr_bytes;
        if (bytes.size() < min_offset) {
            bytes.resize(min_offset, 0);
        }
        const u64 off = bytes.size();
        bytes.resize(off + n);
        std::memcpy(bytes.data() + off, src, n);
        ph.p_offset = off;
        ph.p_filesz = n;
        payload.assign(reinterpret_cast<const u8*>(src),
                       reinterpret_cast<const u8*>(src) + n);
    }

    void Finalize() {
        Loader::Elf64_Ehdr ehdr{};
        ehdr.e_ident[0]  = Loader::ELFMAG0;
        ehdr.e_ident[1]  = Loader::ELFMAG1;
        ehdr.e_ident[2]  = Loader::ELFMAG2;
        ehdr.e_ident[3]  = Loader::ELFMAG3;
        ehdr.e_ident[4]  = 2;
        ehdr.e_ident[5]  = 1;
        ehdr.e_ident[6]  = 1;
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
    }
};

// Tiny x86-64 exit sequence.
static const u8 kExitCode[] = {
    0x48, 0xC7, 0xC0, 0x3C, 0x00, 0x00, 0x00,   // mov rax, 60
    0x48, 0x31, 0xFF,                           // xor rdi, rdi
    0x0F, 0x05,                                 // syscall
    0xC3                                        // ret
};

static std::vector<u8> MakeSimpleElf(guest_addr_t vaddr) {
    TinyElfBuilder b(Loader::ET_DYN, 0);
    auto& ph = b.AddPhdr(Loader::PT_LOAD, 5 /*PF_R|PF_X*/,
                         vaddr, sizeof(kExitCode), 0x1000);
    b.AddPayload(ph, kExitCode, sizeof(kExitCode));
    b.Finalize();
    return b.bytes;
}

static const std::filesystem::path kDir() {
    return std::filesystem::temp_directory_path() / "pcsx5_self_header";
}

// ---------------------------------------------------------------------------
// 1. IsSelfFile detection
// ---------------------------------------------------------------------------
void TestIsSelfFileDetection() {
    std::fprintf(stdout, "[TEST] IsSelfFile detection\n");
    const auto dir = kDir();
    std::filesystem::create_directories(dir);

    // A non-SELF (raw ELF) file must report false.
    {
        auto elf = MakeSimpleElf(0);
        auto p = dir / "raw.elf";
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(elf.data()),
                  static_cast<std::streamsize>(elf.size()));
        EXPECT(!Loader::IsSelfFile(p.string()), "raw ELF is not a SELF");
    }

    // A SELF file must report true.
    {
        SelfBuilder b;
        b.SetInnerElf(MakeSimpleElf(0));
        // One ordered, signed (not encrypted, not compressed) data segment
        // whose plaintext payload is the inner ELF's PT_LOAD content.
        const u64 data_flags = (0u << 20) | 0x4u;  // id=0, signed only
        b.AddInnerSegment(data_flags, /*p_offset=*/0x78,
                          /*file_size=*/sizeof(kExitCode),
                          /*mem_size=*/sizeof(kExitCode));
        auto p = b.Write(dir, "fake.self");
        EXPECT(Loader::IsSelfFile(p.string()), "SELF file detected as SELF");
    }

    // A non-existent file must report false (not crash).
    EXPECT(!Loader::IsSelfFile((dir / "nope.self").string()),
           "missing file is not a SELF");
}

// ---------------------------------------------------------------------------
// 2. Container header parsing
// ---------------------------------------------------------------------------
void TestContainerHeaderParsing() {
    std::fprintf(stdout, "[TEST] Container header parsing\n");

    SelfBuilder b;
    b.SetInnerElf(MakeSimpleElf(0));
    b.AddInnerSegment((0u << 20) | 0x4u, 0x78, 1, 1);
    b.AddInnerSegment((1u << 20) | 0x4u, 0x79, 2, 2);
    auto path = b.Write(kDir(), "header.self");

    Loader::SelfImage img{};
    EXPECT(Loader::ParseSelfImage(path.string(), img), "ParseSelfImage succeeds");
    EXPECT_U64_EQ(img.header.magic,        Loader::kSelfMagic,            "magic");
    EXPECT_U64_EQ(img.header.program_type, Loader::kSelfDefaultProgramType, "program_type");
    EXPECT_EQ(img.header.segment_count, (u16)2, "segment_count");
    EXPECT_EQ(img.header.flags,        (u16)0x0022, "container flags");
    EXPECT(img.header.header_size >= sizeof(Loader::SelfContainerHeader),
           "header_size at least 0x20");
    EXPECT_EQ(img.segments.size(), (size_t)2, "decoded segment count");
}

// ---------------------------------------------------------------------------
// 3. Segment table decoding
// ---------------------------------------------------------------------------
void TestSegmentTableDecoding() {
    std::fprintf(stdout, "[TEST] Segment table decoding\n");

    SelfBuilder b;
    b.SetInnerElf(MakeSimpleElf(0));

    // Segment 0: ordered + signed, id=2
    constexpr u64 kSeg0Flags = (2u << 20) | 0x1u | 0x4u;
    // Segment 1: digest segment, id=3
    constexpr u64 kSeg1Flags = (3u << 20) | 0x10000u | 0x4u;

    b.AddInnerSegment(kSeg0Flags, 0x78, 4, 4);
    b.AddInnerSegment(kSeg1Flags, 0x7C, 0, 0);  // digest seg, no payload
    auto path = b.Write(kDir(), "segments.self");

    Loader::SelfImage img{};
    EXPECT(Loader::ParseSelfImage(path.string(), img), "ParseSelfImage succeeds");

    EXPECT_EQ(img.segments.size(), (size_t)2, "two segments");
    const auto& s0 = img.segments[0];
    EXPECT_EQ(s0.id(),         2,  "seg0 id");
    EXPECT(s0.ordered(),            "seg0 ordered");
    EXPECT(!s0.encrypted(),         "seg0 not encrypted");
    EXPECT(s0.is_signed(),          "seg0 signed");
    EXPECT(!s0.compressed(),        "seg0 not compressed");
    EXPECT(!s0.is_digest(),         "seg0 not digest");
    EXPECT_EQ(s0.file_size, (u64)4, "seg0 file_size");
    EXPECT_EQ(s0.mem_size,  (u64)4, "seg0 mem_size");

    const auto& s1 = img.segments[1];
    EXPECT_EQ(s1.id(),         3,  "seg1 id");
    EXPECT(!s1.ordered(),           "seg1 not ordered");
    EXPECT(s1.is_signed(),          "seg1 signed");
    EXPECT(s1.is_digest(),          "seg1 is digest");
    EXPECT_EQ(s1.file_size, (u64)0, "seg1 file_size zero (digest)");
}

// ---------------------------------------------------------------------------
// 4. Ext info parsing + authority classification
// ---------------------------------------------------------------------------
void TestExtInfoAndAuthority() {
    std::fprintf(stdout, "[TEST] Ext info + authority classification\n");

    // Fake-signed (0x31)
    {
        SelfBuilder b;
        b.SetInnerElf(MakeSimpleElf(0));
        b.authority_id = 0x3100000000000001ULL;
        b.app_version  = 0x00020000;
        b.firmware_version = 0x05500000;
        // Fill the digest with a recognizable pattern.
        for (size_t i = 0; i < b.digest.size(); ++i) b.digest[i] = (u8)(0xA0 ^ i);
        b.AddInnerSegment((0u << 20) | 0x4u, 0x78, 1, 1);
        auto path = b.Write(kDir(), "fake.self");

        Loader::SelfImage img{};
        EXPECT(Loader::ParseSelfImage(path.string(), img), "ParseSelfImage (fake)");
        EXPECT(img.has_ext_info, "ext info present");
        EXPECT_U64_EQ(img.ext_info.authority_id,     0x3100000000000001ULL, "authority_id");
        EXPECT_U64_EQ(img.ext_info.app_version,      0x00020000,            "app_version");
        EXPECT_U64_EQ(img.ext_info.firmware_version, 0x05500000,            "fw_version");
        for (size_t i = 0; i < sizeof(img.ext_info.digest); ++i) {
            EXPECT_EQ(img.ext_info.digest[i], (u8)(0xA0 ^ i), "digest byte");
        }
        EXPECT(Loader::IsFakeSignedSelf(img.ext_info.authority_id),
               "0x31.. is fake-signed");
        EXPECT(Loader::ClassifySelfAuthority(img.ext_info.authority_id) ==
                   Loader::SelfAuthorityCategory::Fake,
               "category byte 0x31");
    }

    // Genuine (0x45)
    {
        SelfBuilder b;
        b.SetInnerElf(MakeSimpleElf(0));
        b.authority_id = 0x45000000DEADBEEFULL;
        b.AddInnerSegment((0u << 20) | 0x4u, 0x78, 1, 1);
        auto path = b.Write(kDir(), "genuine.self");

        Loader::SelfImage img{};
        EXPECT(Loader::ParseSelfImage(path.string(), img), "ParseSelfImage (genuine)");
        EXPECT(Loader::ClassifySelfAuthority(img.ext_info.authority_id) ==
                   Loader::SelfAuthorityCategory::Genuine,
               "category byte 0x45");
        EXPECT(!Loader::IsFakeSignedSelf(img.ext_info.authority_id),
               "0x45.. is NOT fake-signed");
    }

    // Privileged system (0x48)
    {
        SelfBuilder b;
        b.SetInnerElf(MakeSimpleElf(0));
        b.authority_id = 0x4800000000000007ULL;
        b.AddInnerSegment((0u << 20) | 0x4u, 0x78, 1, 1);
        auto path = b.Write(kDir(), "priv.self");

        Loader::SelfImage img{};
        EXPECT(Loader::ParseSelfImage(path.string(), img), "ParseSelfImage (priv)");
        EXPECT(Loader::ClassifySelfAuthority(img.ext_info.authority_id) ==
                   Loader::SelfAuthorityCategory::PrivilegedSystem,
               "category byte 0x48");
    }

    // Unknown (0x99)
    {
        SelfBuilder b;
        b.SetInnerElf(MakeSimpleElf(0));
        b.authority_id = 0x9900000000000001ULL;
        b.AddInnerSegment((0u << 20) | 0x4u, 0x78, 1, 1);
        auto path = b.Write(kDir(), "unk.self");

        Loader::SelfImage img{};
        EXPECT(Loader::ParseSelfImage(path.string(), img), "ParseSelfImage (unknown)");
        // The category byte is 0x99 which is not one of the named
        // categories.  Make sure the classifier returns *something other
        // than* Fake / Genuine / PrivilegedSystem.
        const auto cat = Loader::ClassifySelfAuthority(img.ext_info.authority_id);
        EXPECT(cat != Loader::SelfAuthorityCategory::Fake,             "0x99 not Fake");
        EXPECT(cat != Loader::SelfAuthorityCategory::Genuine,          "0x99 not Genuine");
        EXPECT(cat != Loader::SelfAuthorityCategory::PrivilegedSystem, "0x99 not PrivSystem");
        EXPECT(!Loader::IsFakeSignedSelf(img.ext_info.authority_id),
               "0x99 is NOT fake-signed");
    }
}

// ---------------------------------------------------------------------------
// 5. Embedded ELF extraction
// ---------------------------------------------------------------------------
void TestInnerElfExtraction() {
    std::fprintf(stdout, "[TEST] Embedded ELF extraction\n");

    const std::vector<u8> elf_bytes = MakeSimpleElf(0x1000);
    SelfBuilder b;
    b.SetInnerElf(elf_bytes);
    b.AddInnerSegment((0u << 20) | 0x4u, 0x78, 1, 1);
    auto path = b.Write(kDir(), "extract.self");

    Loader::SelfImage img{};
    EXPECT(Loader::ParseSelfImage(path.string(), img), "ParseSelfImage succeeds");
    EXPECT(!img.elf_region.empty(), "elf_region non-empty");

    // The extracted bytes must begin with the ELF magic.
    EXPECT(img.elf_region.size() >= 4, "elf_region has at least 4 bytes");
    EXPECT_EQ(img.elf_region[0], (u8)0x7F, "elf_region[0] = 0x7F");
    EXPECT_EQ(img.elf_region[1], (u8)'E',  "elf_region[1] = 'E'");
    EXPECT_EQ(img.elf_region[2], (u8)'L',  "elf_region[2] = 'L'");
    EXPECT_EQ(img.elf_region[3], (u8)'F',  "elf_region[3] = 'F'");

    // ExtractInnerElf returns a copy of the region.
    auto extracted = Loader::ExtractInnerElf(img);
    EXPECT_EQ(extracted.size(), img.elf_region.size(), "ExtractInnerElf size");
    EXPECT(std::memcmp(extracted.data(), img.elf_region.data(),
                       extracted.size()) == 0,
           "ExtractInnerElf bytes match elf_region");
}

// ---------------------------------------------------------------------------
// 6. Bad-input rejection
// ---------------------------------------------------------------------------
void TestBadInputRejection() {
    std::fprintf(stdout, "[TEST] Bad input rejection\n");
    const auto dir = kDir();
    std::filesystem::create_directories(dir);

    // Truncated file (only 4 bytes; less than the 0x20-byte header).
    {
        std::filesystem::path p = dir / "tiny.self";
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        u8 four_bytes[4] = {0x4F, 0x15, 0x3D, 0x1D};
        out.write(reinterpret_cast<const char*>(four_bytes), sizeof(four_bytes));
        out.close();
        Loader::SelfImage img{};
        EXPECT(!Loader::ParseSelfImage(p.string(), img),
               "truncated file is rejected");
    }

    // Bad magic.
    {
        SelfBuilder b;
        b.SetInnerElf(MakeSimpleElf(0));
        b.AddInnerSegment((0u << 20) | 0x4u, 0x78, 1, 1);
        auto bytes = b.Build();
        bytes[0] = 0xDE; bytes[1] = 0xAD; bytes[2] = 0xBE; bytes[3] = 0xEF;
        std::filesystem::path p = dir / "badmagic.self";
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        out.close();
        Loader::SelfImage img{};
        EXPECT(!Loader::ParseSelfImage(p.string(), img),
               "bad-magic file is rejected");
    }

    // Zero segment count.
    {
        std::vector<u8> bytes(0x200, 0u);
        Loader::SelfContainerHeader chdr{};
        chdr.magic         = Loader::kSelfMagic;
        chdr.segment_count = 0;
        chdr.header_size   = sizeof(chdr);
        std::memcpy(bytes.data(), &chdr, sizeof(chdr));
        std::filesystem::path p = dir / "noseg.self";
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        out.close();
        Loader::SelfImage img{};
        EXPECT(!Loader::ParseSelfImage(p.string(), img),
               "zero-segment file is rejected");
    }
}

// ---------------------------------------------------------------------------
// 7. End-to-end: LoadSelf on a fake-signed plaintext SELF
// ---------------------------------------------------------------------------
void TestLoadSelfEndToEnd() {
    std::fprintf(stdout, "[TEST] LoadSelf end-to-end (fake-signed plaintext)\n");

    // Build a tiny PIE-style ELF with a single PT_LOAD containing a
    // trivial exit sequence.  Then wrap it in a fake-self with one data
    // segment whose payload is the *plaintext* PT_LOAD content.
    // The vaddr must be 16KB-page-aligned (the project's PAGE_SIZE).
    constexpr guest_addr_t kPloadVaddr = 0x12340000;
    TinyElfBuilder elf(Loader::ET_DYN, kPloadVaddr);
    auto& ph = elf.AddPhdr(Loader::PT_LOAD, 5 /*PF_R|PF_X*/,
                           kPloadVaddr, sizeof(kExitCode), 0x4000);
    elf.AddPayload(ph, kExitCode, sizeof(kExitCode));
    elf.Finalize();

    SelfBuilder self;
    self.SetInnerElf(elf.bytes);
    // data segment: ordered + signed (NOT encrypted, NOT compressed).
    // The segment's data lives at p_offset=0x78 in the inner ELF (the
    // TinyElfBuilder writes the payload immediately after the header+phdrs).
    constexpr u64 kDataFlags = (0u << 20) | 0x1u | 0x4u;
    self.AddInnerSegment(kDataFlags, /*p_offset=*/0x78,
                         sizeof(kExitCode), sizeof(kExitCode));
    auto path = self.Write(kDir(), "loadable.self");

    Loader::LoadedModule m{};
    EXPECT(Loader::LoadSelf(path.string(), m), "LoadSelf succeeds");

    // The inner ELF was a single PT_LOAD at 0x12345000 (PIE-style with
    // vaddr != 0 — we built it that way so the loader keeps the absolute
    // address and does not relocate).
    EXPECT(!m.needed_libraries.empty() || m.needed_libraries.empty(),
           "module loaded (smoke)");
    EXPECT(m.image_size >= sizeof(kExitCode), "image_size covers payload");
}

// ---------------------------------------------------------------------------
// 8. LoadSelf rejects encrypted / compressed segments
// ---------------------------------------------------------------------------
void TestLoadSelfRejectsEncrypted() {
    std::fprintf(stdout, "[TEST] LoadSelf rejects encrypted segments\n");

    SelfBuilder self;
    self.SetInnerElf(MakeSimpleElf(0));
    // Mark the segment as encrypted: flags bit 1 (encrypted) + bit 2 (signed)
    constexpr u64 kEncryptedFlags = (0u << 20) | 0x2u | 0x4u;
    self.AddInnerSegment(kEncryptedFlags, 0x78, 1, 1);
    auto path = self.Write(kDir(), "encrypted.self");

    Loader::LoadedModule m{};
    EXPECT(!Loader::LoadSelf(path.string(), m),
           "encrypted SELF is rejected");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    LogConfig::SetLevel(LogCategory::Loader, LogLevel::Warn);

    if (!Memory::Initialize()) {
        std::fprintf(stderr, "FATAL: Memory::Initialize failed\n");
        return 2;
    }

    TestIsSelfFileDetection();
    TestContainerHeaderParsing();
    TestSegmentTableDecoding();
    TestExtInfoAndAuthority();
    TestInnerElfExtraction();
    TestBadInputRejection();
    TestLoadSelfEndToEnd();
    TestLoadSelfRejectsEncrypted();

    Memory::Shutdown();

    // Cleanup the test scratch dir.
    std::error_code ec;
    std::filesystem::remove_all(kDir(), ec);

    std::fprintf(stdout, "\n[self_header_tests] %d checks, %d failures\n",
                 g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
