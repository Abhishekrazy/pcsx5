// PKG container parser / extractor tests.
//
// Verifies:
//   - A synthetic minimal fPKG parses and plaintext entries extract byte-exact.
//   - Entry ids map to the conventional file names (param.sfo, icon0.png).
//   - fPKG-encrypted entries (scene passcode derivation + per-entry
//     AES-128-CBC) round-trip through encryption -> extraction -> plaintext.
//   - Malformed inputs fail cleanly: bad magic, truncated header, entry table
//     outside the file, entry data outside the file.
//   - Retail-NPDRM encrypted entries are detected and skipped (not crashed).
//
// Self-contained: freestanding main, no gtest (same style as nid_tests.cpp).

#include "loader/pkg.h"
#include "common/crypto.h"
#include "common/log.h"
#include "common/types.h"

#include <algorithm>
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
int g_checks = 0;

#define EXPECT(cond, msg)                                                          \
    do {                                                                           \
        ++g_checks;                                                                \
        if (!(cond)) {                                                             \
            std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, msg);   \
            ++g_failures;                                                          \
        }                                                                          \
    } while (0)

#define EXPECT_EQ(a, b, msg)                                                       \
    do {                                                                           \
        ++g_checks;                                                                \
        auto _lhs = (a);                                                           \
        auto _rhs = (b);                                                           \
        if (!(_lhs == _rhs)) {                                                     \
            std::fprintf(stderr, "[FAIL] %s:%d: %s  (lhs=%lld rhs=%lld)\n",        \
                         __FILE__, __LINE__, msg, (long long)_lhs,                 \
                         (long long)_rhs);                                         \
            ++g_failures;                                                          \
        }                                                                          \
    } while (0)

// ---------------------------------------------------------------------------
// PkgBuilder: mirrors the ElfBuilder pattern from tests/loader_corpus.cpp.
//
// Layout produced:
//   [header 0x1000] [entry table 32*N] [entry data ...]
// ---------------------------------------------------------------------------

constexpr char kTestContentId[] = "EP0000-PPSA00000_00-TEST000000000000";  // 36 chars

struct PkgBuilder {
    u32 drm_type = Loader::kPkgDrmTypeFake;
    std::vector<Loader::PkgEntry> entries;
    std::vector<std::vector<u8>> payloads;  // on-disk (possibly encrypted) data
    std::string content_id = kTestContentId;

    static void WriteBe16(std::vector<u8>& b, size_t off, u16 v) {
        b[off] = static_cast<u8>(v >> 8);
        b[off + 1] = static_cast<u8>(v);
    }
    static void WriteBe32(std::vector<u8>& b, size_t off, u32 v) {
        b[off] = static_cast<u8>(v >> 24);
        b[off + 1] = static_cast<u8>(v >> 16);
        b[off + 2] = static_cast<u8>(v >> 8);
        b[off + 3] = static_cast<u8>(v);
    }
    static void WriteBe64(std::vector<u8>& b, size_t off, u64 v) {
        WriteBe32(b, off, static_cast<u32>(v >> 32));
        WriteBe32(b, off + 4, static_cast<u32>(v));
    }

    // Add a plaintext entry.  `data` is stored as-is.
    void AddPlain(u32 id, const void* data, size_t size) {
        Loader::PkgEntry e{};
        e.id = id;
        e.flags1 = 0;
        e.flags2 = 0;
        e.size = static_cast<u32>(size);
        entries.push_back(e);
        payloads.emplace_back(static_cast<const u8*>(data),
                              static_cast<const u8*>(data) + size);
    }

    // Add an fPKG-encrypted entry: the plaintext is padded up to a multiple
    // of 16 and AES-128-CBC encrypted with the standard scheme (key seed
    // from DerivePkgKey with the scene passcode, key/IV from
    // SHA256(meta_entry_be || key_seed)).  Entry offsets/sizes are patched
    // during Finalize; encryption happens there because the meta bytes that
    // feed the IV derivation include the final offset.
    void AddEncrypted(u32 id, u32 key_index, const void* data, size_t size) {
        Loader::PkgEntry e{};
        e.id = id;
        e.flags1 = Loader::kPkgEntryFlagEncrypted;
        e.flags2 = key_index << 12;
        e.size = static_cast<u32>(size);
        entries.push_back(e);
        payloads.emplace_back(static_cast<const u8*>(data),
                              static_cast<const u8*>(data) + size);
    }

    std::vector<u8> Finalize() {
        const u32 entry_count = static_cast<u32>(entries.size());
        const u32 table_offset = Loader::kPkgHeaderSize;

        // Patch entry offsets and lay out entry data after the table.
        u64 data_cursor = table_offset + entry_count * 32u;
        std::vector<u8> bytes(static_cast<size_t>(data_cursor), 0);
        for (size_t i = 0; i < entries.size(); ++i) {
            Loader::PkgEntry& e = entries[i];
            e.offset = static_cast<u32>(data_cursor);
            size_t stored = e.size;
            if (e.IsEncrypted()) {
                stored = ALIGN_UP(e.size, 16);
                payloads[i].resize(stored, 0);
                // Serialize the final meta entry and encrypt in place.
                u8 entry_be[32];
                Loader::SerializePkgEntryBe(e, entry_be);
                std::array<u8, 32> key_seed{};
                if (!Loader::DerivePkgKey(content_id, Loader::kPkgScenePasscode,
                                          e.KeyIndex(), key_seed)) {
                    std::fprintf(stderr, "[INTERNAL] DerivePkgKey failed\n");
                    std::abort();
                }
                Common::Sha256Context sha;
                sha.Init();
                sha.Update(entry_be, sizeof(entry_be));
                sha.Update(key_seed.data(), key_seed.size());
                const auto iv_key = sha.Final();
                const auto key = Common::Aes128ExpandKey(iv_key.data());
                u8 iv[16];
                std::memcpy(iv, iv_key.data() + 16, sizeof(iv));
                if (!Common::Aes128CbcEncrypt(key, iv, payloads[i].data(),
                                              payloads[i].data(), stored)) {
                    std::fprintf(stderr, "[INTERNAL] encrypt failed\n");
                    std::abort();
                }
            }
            bytes.resize(static_cast<size_t>(data_cursor) + stored, 0);
            std::memcpy(bytes.data() + data_cursor, payloads[i].data(), stored);
            data_cursor += stored;
        }

        // Header.
        WriteBe32(bytes, 0x00, Loader::kPkgMagic);
        WriteBe32(bytes, 0x04, 0);                    // pkg_type
        WriteBe32(bytes, 0x0C, 0xF);                  // file_count-ish
        WriteBe32(bytes, 0x10, entry_count);
        WriteBe16(bytes, 0x14, static_cast<u16>(entry_count));  // sc entries
        WriteBe16(bytes, 0x16, static_cast<u16>(entry_count));  // entry_count_2
        WriteBe32(bytes, 0x18, table_offset);
        WriteBe64(bytes, 0x20, table_offset);         // body_offset
        WriteBe64(bytes, 0x28, data_cursor - table_offset);     // body_size
        std::memcpy(bytes.data() + 0x40, content_id.data(),
                    std::min<size_t>(content_id.size(), Loader::kPkgContentIdSize));
        WriteBe32(bytes, 0x70, drm_type);
        WriteBe32(bytes, 0x74, 0);                    // content_type
        WriteBe64(bytes, 0x430, data_cursor);         // pkg_size

        // Entry table (big-endian meta entries).
        for (size_t i = 0; i < entries.size(); ++i) {
            u8 entry_be[32];
            Loader::SerializePkgEntryBe(entries[i], entry_be);
            std::memcpy(bytes.data() + table_offset + i * 32, entry_be, 32);
        }
        return bytes;
    }
};

// ---------------------------------------------------------------------------
// Filesystem helpers
// ---------------------------------------------------------------------------

std::filesystem::path g_tmp_root;

bool WriteFile(const std::string& path, const std::vector<u8>& bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) return false;
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    return f.good();
}

bool ReadFile(const std::string& path, std::vector<u8>& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return false;
    const auto size = f.tellg();
    if (size < 0) return false;
    out.resize(static_cast<size_t>(size));
    f.seekg(0, std::ios::beg);
    f.read(reinterpret_cast<char*>(out.data()), size);
    return f.good() || f.gcount() == size;
}

bool BytesEqual(const std::vector<u8>& a, const void* b, size_t n) {
    return a.size() == n && std::memcmp(a.data(), b, n) == 0;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void TestPlaintextRoundTrip() {
    PkgBuilder b;
    const char sfo[] = "\x00PSF fake param.sfo payload 12345";
    const u8 icon[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 1, 2, 3};
    const u8 misc[] = {0xDE, 0xAD, 0xBE, 0xEF};
    b.AddPlain(Loader::kPkgEntryParamSfo, sfo, sizeof(sfo) - 1);
    b.AddPlain(Loader::kPkgEntryIcon0Png, icon, sizeof(icon));
    b.AddPlain(0x9000, misc, sizeof(misc));  // unknown id
    const auto bytes = b.Finalize();

    const std::string pkg_path = (g_tmp_root / "plain.pkg").string();
    EXPECT(WriteFile(pkg_path, bytes), "write plain.pkg");

    Loader::PkgImage image;
    EXPECT(Loader::ParsePkg(pkg_path, image), "ParsePkg plaintext pkg");
    EXPECT_EQ(image.entries.size(), 3u, "entry count");
    EXPECT(image.content_id == kTestContentId, "content id parsed");
    EXPECT_EQ(image.entries[0].id, Loader::kPkgEntryParamSfo, "entry 0 id");
    EXPECT(!image.entries[0].IsEncrypted(), "entry 0 not encrypted");

    // Name mapping.
    EXPECT(Loader::PkgEntryPath(image.entries[0]) == "param.sfo",
           "param.sfo path mapping");
    EXPECT(Loader::PkgEntryPath(image.entries[1]) == "icon0.png",
           "icon0.png path mapping");
    EXPECT(Loader::PkgEntryPath(image.entries[2]) == "entries/0x9000.bin",
           "unknown id path mapping");

    // Whole-package extraction.
    const std::string out_dir = (g_tmp_root / "plain_out").string();
    EXPECT(Loader::ExtractPkg(pkg_path, out_dir), "ExtractPkg plaintext");

    std::vector<u8> got;
    EXPECT(ReadFile((g_tmp_root / "plain_out" / "param.sfo").string(), got),
           "read extracted param.sfo");
    EXPECT(BytesEqual(got, sfo, sizeof(sfo) - 1), "param.sfo bytes round-trip");
    EXPECT(ReadFile((g_tmp_root / "plain_out" / "icon0.png").string(), got),
           "read extracted icon0.png");
    EXPECT(BytesEqual(got, icon, sizeof(icon)), "icon0.png bytes round-trip");
    EXPECT(ReadFile((g_tmp_root / "plain_out" / "entries" / "0x9000.bin").string(), got),
           "read extracted unknown entry");
    EXPECT(BytesEqual(got, misc, sizeof(misc)), "unknown entry bytes round-trip");
}

void TestEncryptedRoundTrip() {
    PkgBuilder b;
    // 38 bytes: deliberately not a multiple of 16 to exercise the
    // padding-discard after CBC decryption.
    const u8 secret[] = "fake-signed encrypted entry payload!!!";
    const u8 plain[] = {1, 2, 3, 4};
    b.AddEncrypted(Loader::kPkgEntryLicenseDat, 3, secret, sizeof(secret) - 1);
    b.AddPlain(Loader::kPkgEntryParamSfo, plain, sizeof(plain));
    const auto bytes = b.Finalize();

    const std::string pkg_path = (g_tmp_root / "enc.pkg").string();
    EXPECT(WriteFile(pkg_path, bytes), "write enc.pkg");

    Loader::PkgImage image;
    EXPECT(Loader::ParsePkg(pkg_path, image), "ParsePkg encrypted pkg");
    EXPECT_EQ(image.entries.size(), 2u, "encrypted pkg entry count");
    EXPECT(image.entries[0].IsEncrypted(), "entry 0 flagged encrypted");
    EXPECT_EQ(image.entries[0].KeyIndex(), 3u, "entry 0 key index");

    const std::string out_dir = (g_tmp_root / "enc_out").string();
    EXPECT(Loader::ExtractPkg(pkg_path, out_dir), "ExtractPkg encrypted");

    std::vector<u8> got;
    EXPECT(ReadFile((g_tmp_root / "enc_out" / "license.dat").string(), got),
           "read decrypted license.dat");
    EXPECT(BytesEqual(got, secret, sizeof(secret) - 1),
           "encrypted entry decrypts to original plaintext (padding dropped)");
    EXPECT(ReadFile((g_tmp_root / "enc_out" / "param.sfo").string(), got),
           "read param.sfo next to encrypted entry");
    EXPECT(BytesEqual(got, plain, sizeof(plain)), "plaintext entry intact");
}

void TestBadMagic() {
    PkgBuilder b;
    const u8 d[] = {0x42};
    b.AddPlain(Loader::kPkgEntryParamSfo, d, sizeof(d));
    auto bytes = b.Finalize();
    bytes[0] = 0x7F;
    bytes[1] = 'X';  // break "\x7FCNT"
    const std::string path = (g_tmp_root / "badmagic.pkg").string();
    EXPECT(WriteFile(path, bytes), "write badmagic.pkg");
    Loader::PkgImage image;
    EXPECT(!Loader::ParsePkg(path, image), "bad magic rejected");
}

void TestTruncatedHeader() {
    PkgBuilder b;
    const u8 d[] = {0x42};
    b.AddPlain(Loader::kPkgEntryParamSfo, d, sizeof(d));
    auto bytes = b.Finalize();
    bytes.resize(0x800);  // shorter than the 0x1000 header
    const std::string path = (g_tmp_root / "trunc.pkg").string();
    EXPECT(WriteFile(path, bytes), "write trunc.pkg");
    Loader::PkgImage image;
    EXPECT(!Loader::ParsePkg(path, image), "truncated header rejected");
}

void TestEntryTableOutOfBounds() {
    PkgBuilder b;
    const u8 d[] = {0x42};
    b.AddPlain(Loader::kPkgEntryParamSfo, d, sizeof(d));
    auto bytes = b.Finalize();
    // Point the table past EOF.
    bytes[0x18] = 0xFF; bytes[0x19] = 0xFF;
    bytes[0x1A] = 0xFF; bytes[0x1B] = 0xF0;
    const std::string path = (g_tmp_root / "badtable.pkg").string();
    EXPECT(WriteFile(path, bytes), "write badtable.pkg");
    Loader::PkgImage image;
    EXPECT(!Loader::ParsePkg(path, image), "entry table past EOF rejected");

    // Variant: plausible offset, absurd entry count.
    auto bytes2 = b.Finalize();
    bytes2[0x10] = 0x10; bytes2[0x11] = 0x00;
    bytes2[0x12] = 0x00; bytes2[0x13] = 0x00;  // 0x10000000 entries
    const std::string path2 = (g_tmp_root / "badcount.pkg").string();
    EXPECT(WriteFile(path2, bytes2), "write badcount.pkg");
    EXPECT(!Loader::ParsePkg(path2, image), "absurd entry count rejected");
}

void TestEntryDataOutOfBounds() {
    PkgBuilder b;
    const u8 d[] = {0x42};
    b.AddPlain(Loader::kPkgEntryParamSfo, d, sizeof(d));
    auto bytes = b.Finalize();
    // Corrupt the entry's data offset to near EOF (table is at 0x1000,
    // entry offset field at +0x10 within the first meta entry).
    const size_t off_field = Loader::kPkgHeaderSize + 0x10;
    bytes[off_field] = 0xFF; bytes[off_field + 1] = 0xFF;
    bytes[off_field + 2] = 0xFF; bytes[off_field + 3] = 0xF0;
    const std::string path = (g_tmp_root / "baddata.pkg").string();
    EXPECT(WriteFile(path, bytes), "write baddata.pkg");
    Loader::PkgImage image;
    EXPECT(!Loader::ParsePkg(path, image), "entry data past EOF rejected");
}

void TestRetailEntrySkipped() {
    PkgBuilder b;
    b.drm_type = Loader::kPkgDrmTypeNpdrm;  // retail
    const u8 secret[] = "retail blob";
    const u8 plain[] = {9, 9, 9};
    b.AddEncrypted(Loader::kPkgEntryLicenseDat, 3, secret, sizeof(secret) - 1);
    b.AddPlain(Loader::kPkgEntryParamSfo, plain, sizeof(plain));
    const auto bytes = b.Finalize();

    const std::string pkg_path = (g_tmp_root / "retail.pkg").string();
    EXPECT(WriteFile(pkg_path, bytes), "write retail.pkg");

    Loader::PkgImage image;
    EXPECT(Loader::ParsePkg(pkg_path, image), "ParsePkg retail pkg");
    EXPECT(image.IsRetail(), "retail pkg detected");

    // The encrypted entry must be skipped, not crash or emit garbage.
    const std::string dest = (g_tmp_root / "retail_license.dat").string();
    EXPECT(!Loader::ExtractPkgEntry(image, image.entries[0], dest),
           "retail encrypted entry skipped");
    // Plaintext entries of a retail PKG still extract.
    const std::string dest2 = (g_tmp_root / "retail_param.sfo").string();
    EXPECT(Loader::ExtractPkgEntry(image, image.entries[1], dest2),
           "retail plaintext entry still extracts");
    std::vector<u8> got;
    EXPECT(ReadFile(dest2, got), "read retail param.sfo");
    EXPECT(BytesEqual(got, plain, sizeof(plain)), "retail plaintext bytes ok");
}

}  // namespace

int main() {
    std::error_code ec;
    g_tmp_root = std::filesystem::temp_directory_path(ec) /
                 "pcsx5_pkg_tests";
    if (ec) {
        std::fprintf(stderr, "no temp directory available\n");
        return 2;
    }
    std::filesystem::remove_all(g_tmp_root, ec);
    ec.clear();
    std::filesystem::create_directories(g_tmp_root, ec);
    if (ec) {
        std::fprintf(stderr, "failed to create temp test directory\n");
        return 2;
    }

    TestPlaintextRoundTrip();
    TestEncryptedRoundTrip();
    TestBadMagic();
    TestTruncatedHeader();
    TestEntryTableOutOfBounds();
    TestEntryDataOutOfBounds();
    TestRetailEntrySkipped();

    std::filesystem::remove_all(g_tmp_root, ec);

    std::printf("pkg_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
