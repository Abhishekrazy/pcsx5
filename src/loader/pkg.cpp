#include "pkg.h"
#include "../common/crypto.h"
#include "../common/log.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

// PKG container extraction for fake-signed (fPKG) Prospero/Orbis packages.
//
// Confirmed format facts:
//   - Header magic 0x7F434E54 ("\x7FCNT") at offset 0; header is big-endian
//     and 0x1000 bytes long.
//     [http://www.psdevwiki.com/ps4/PKG_files,
//      LibOrbisPkg PKG/PkgReader.cs ReadHeader]
//   - entry_count @ 0x10, sc_entry_count @ 0x14 (u16), entry_table_offset
//     @ 0x18; each table entry is 32 bytes:
//       u32 id, u32 filename_offset, u32 flags1, u32 flags2,
//       u32 offset, u32 size, u64 pad
//     flags1 & 0x80000000 => encrypted; (flags2 & 0xF000) >> 12 => key index.
//     [psdevwiki "Entries", LibOrbisPkg PKG/Entry.cs MetaEntry]
//   - Entry ids: 0x0001 digests, 0x0010 entry keys, 0x0020 image key,
//     0x0080 general digests, 0x0100 metadata, 0x0200 entry names;
//     0x0400..0x0409 license/npbind/nptitle; 0x1000 param.sfo, 0x1001
//     playgo-chunk.dat, ... 0x1200 icon0.png, 0x1400 trophy/trophy00.trp.
//     [psdevwiki "Entry IDs"]
//   - fPKG key derivation (psdevwiki "Entry Keys", LibOrbisPkg
//     Util/Crypto.cs ComputeKeys):
//       key(index) = SHA256( SHA256(be32(index)) ||
//                            SHA256(content_id, 36 chars padded to 0x30) ||
//                            passcode[0x20] )
//     index 1 = EKPFS, index 3 = "dk3" (used for encrypted table entries).
//   - Encrypted entry scheme (LibOrbisPkg PKG/Entry.cs Decrypt +
//     Util/Crypto.cs AesCbcCfb128Decrypt):
//       iv_key = SHA256( 32-byte big-endian meta entry || key_seed )
//       AES-128-CBC decrypt with key = iv_key[0..16), iv = iv_key[16..32).
//     Entry data is stored padded up to a multiple of 16 bytes; the extra
//     padding is discarded after decryption.
//
// Implemented vs skipped:
//   - Fake-signed PKGs with a known passcode (scene standard 32x'0'): fully
//     supported, both plaintext and encrypted table entries.
//   - Retail NPDRM PKGs (drm_type == 3): their passcodes are not public, so
//     encrypted entries are detected, logged and skipped.  The retail-only
//     RSA-2048 entry-key unwrap (LibOrbisPkg RSAKeyset.PkgDerivedKey3Keyset
//     is only known for index 3) and the PFS image XTS layer are out of
//     scope for this task; the PFS image offset/size are exposed in PkgImage
//     for the follow-up PFS task.

namespace Loader {

namespace {

// --- big-endian helpers -----------------------------------------------------

u16 ReadBe16(const u8* p) {
    return static_cast<u16>((static_cast<u16>(p[0]) << 8) |
                            static_cast<u16>(p[1]));
}

u32 ReadBe32(const u8* p) {
    return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) |
           (static_cast<u32>(p[2]) << 8) | static_cast<u32>(p[3]);
}

u64 ReadBe64(const u8* p) {
    return (static_cast<u64>(ReadBe32(p)) << 32) |
           static_cast<u64>(ReadBe32(p + 4));
}

void WriteBe32(u8* p, u32 v) {
    p[0] = static_cast<u8>(v >> 24);
    p[1] = static_cast<u8>(v >> 16);
    p[2] = static_cast<u8>(v >> 8);
    p[3] = static_cast<u8>(v);
}

// Read exactly `size` bytes from `file` at absolute `offset` (already
// bounds-checked by the caller).  Returns false on any short read.
bool ReadAt(std::ifstream& file, u64 offset, void* out, u64 size) {
    file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!file) {
        return false;
    }
    file.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(size));
    return file.good() || file.gcount() == static_cast<std::streamsize>(size);
}

// True if [offset, offset+size) lies inside a file of `file_size` bytes.
bool RangeValid(u64 offset, u64 size, u64 file_size) {
    return offset <= file_size && size <= file_size - offset;
}

}  // namespace

// ---------------------------------------------------------------------------
// Key derivation / entry serialisation (public; also used by the tests)
// ---------------------------------------------------------------------------

bool DerivePkgKey(const std::string& content_id, const std::string& passcode,
                  u32 key_index, std::array<u8, 32>& out_key) {
    if (passcode.size() != kPkgPasscodeSize) {
        LOG_ERROR(Loader, "PKG passcode must be exactly %zu characters.",
                  kPkgPasscodeSize);
        return false;
    }
    if (content_id.size() > kPkgContentIdSize) {
        LOG_ERROR(Loader, "PKG content id is too long (%zu chars).",
                  content_id.size());
        return false;
    }

    // data = SHA256(be32(index)) || SHA256(content_id padded to 0x30) || passcode
    u8 index_be[4];
    WriteBe32(index_be, key_index);
    const auto index_hash = Common::Sha256(index_be, sizeof(index_be));

    u8 content_id_padded[0x30] = {};
    std::memcpy(content_id_padded, content_id.data(), content_id.size());
    const auto content_id_hash = Common::Sha256(content_id_padded,
                                                sizeof(content_id_padded));

    u8 buffer[0x60];
    std::memcpy(buffer + 0x00, index_hash.data(), 0x20);
    std::memcpy(buffer + 0x20, content_id_hash.data(), 0x20);
    std::memcpy(buffer + 0x40, passcode.data(), kPkgPasscodeSize);
    out_key = Common::Sha256(buffer, sizeof(buffer));
    return true;
}

bool SerializePkgEntryBe(const PkgEntry& entry, u8 out[32]) {
    WriteBe32(out + 0x00, entry.id);
    WriteBe32(out + 0x04, entry.filename_offset);
    WriteBe32(out + 0x08, entry.flags1);
    WriteBe32(out + 0x0C, entry.flags2);
    WriteBe32(out + 0x10, entry.offset);
    WriteBe32(out + 0x14, entry.size);
    std::memset(out + 0x18, 0, 8);
    return true;
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

bool ParsePkg(const std::string& path, PkgImage& out_image) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        LOG_ERROR(Loader, "Failed to open PKG file: %s", path.c_str());
        return false;
    }
    const std::streamoff end = file.tellg();
    if (end < 0) {
        LOG_ERROR(Loader, "Failed to determine PKG file size: %s", path.c_str());
        return false;
    }
    const u64 file_size = static_cast<u64>(end);
    file.seekg(0, std::ios::beg);

    if (file_size < kPkgHeaderSize) {
        LOG_ERROR(Loader, "File too small to contain a PKG header: %s",
                  path.c_str());
        return false;
    }

    // The fields we use all live in the first 0x440 bytes; read the full
    // 0x1000 header so bounds below are trivially satisfied.
    u8 hdr[kPkgHeaderSize];
    if (!ReadAt(file, 0, hdr, sizeof(hdr))) {
        LOG_ERROR(Loader, "Failed to read PKG header: %s", path.c_str());
        return false;
    }

    if (ReadBe32(hdr + 0x00) != kPkgMagic) {
        LOG_ERROR(Loader, "Invalid PKG magic 0x%08X for file: %s",
                  ReadBe32(hdr + 0x00), path.c_str());
        return false;
    }

    const u32 entry_count = ReadBe32(hdr + 0x10);
    const u32 entry_table_offset = ReadBe32(hdr + 0x18);

    PkgImage image;
    image.path = path;
    image.file_size = file_size;

    // Content ID: 36-byte ASCII string at 0x40 (may be NUL-padded).
    {
        const char* cid = reinterpret_cast<const char*>(hdr + 0x40);
        size_t len = 0;
        while (len < kPkgContentIdSize && cid[len] != '\0') {
            ++len;
        }
        image.content_id.assign(cid, len);
    }
    image.drm_type = ReadBe32(hdr + 0x70);
    image.content_type = ReadBe32(hdr + 0x74);
    image.content_flags = ReadBe32(hdr + 0x78);
    image.body_offset = ReadBe64(hdr + 0x20);
    image.body_size = ReadBe64(hdr + 0x28);
    image.pfs_image_offset = ReadBe64(hdr + 0x410);
    image.pfs_image_size = ReadBe64(hdr + 0x418);

    // Validate the entry table before seeking/allocating.
    if (entry_count > 0) {
        if (!RangeValid(entry_table_offset,
                        static_cast<u64>(entry_count) * sizeof(PkgTableEntryDisk),
                        file_size)) {
            LOG_ERROR(Loader,
                      "PKG entry table (%u entries @ 0x%X) is outside the file.",
                      entry_count, entry_table_offset);
            return false;
        }
        // Sanity cap: even huge retail PKGs have a few dozen entries.
        if (entry_count > 0x1000) {
            LOG_ERROR(Loader, "PKG entry count %u is implausible; rejecting.",
                      entry_count);
            return false;
        }
    }

    LOG_INFO(Loader,
             "PKG: content_id='%s' drm_type=%u content_type=0x%X entries=%u",
             image.content_id.c_str(), image.drm_type, image.content_type,
             entry_count);

    image.entries.reserve(entry_count);
    for (u32 i = 0; i < entry_count; ++i) {
        u8 raw[sizeof(PkgTableEntryDisk)];
        if (!ReadAt(file, entry_table_offset + i * sizeof(raw), raw, sizeof(raw))) {
            LOG_ERROR(Loader, "Failed to read PKG entry %u.", i);
            return false;
        }
        PkgEntry entry;
        entry.id = ReadBe32(raw + 0x00);
        entry.filename_offset = ReadBe32(raw + 0x04);
        entry.flags1 = ReadBe32(raw + 0x08);
        entry.flags2 = ReadBe32(raw + 0x0C);
        entry.offset = ReadBe32(raw + 0x10);
        entry.size = ReadBe32(raw + 0x14);

        // Validate the entry data range.  Encrypted entries are padded up to
        // a multiple of 16 bytes on disk, so check that padded range too.
        const u64 stored_size = entry.IsEncrypted()
                                    ? static_cast<u64>(ALIGN_UP(entry.size, 16))
                                    : static_cast<u64>(entry.size);
        if (!RangeValid(entry.offset, stored_size, file_size)) {
            LOG_ERROR(Loader,
                      "PKG entry 0x%04X (offset 0x%X size 0x%X) is outside the file.",
                      entry.id, entry.offset, entry.size);
            return false;
        }
        image.entries.push_back(entry);
    }

    // Resolve entry names from the ENTRY_NAMES table (id 0x200) when present.
    for (auto& names_entry : image.entries) {
        if (names_entry.id != kPkgEntryEntryNames || names_entry.size == 0) {
            continue;
        }
        std::vector<char> names(names_entry.size);
        if (!ReadAt(file, names_entry.offset, names.data(), names.size())) {
            LOG_WARN(Loader, "Failed to read PKG entry-name table; names skipped.");
            break;
        }
        for (auto& entry : image.entries) {
            if (entry.filename_offset == 0 ||
                entry.filename_offset >= names_entry.size) {
                continue;
            }
            const char* base = names.data() + entry.filename_offset;
            const size_t max_len = names_entry.size - entry.filename_offset;
            const void* nul = std::memchr(base, '\0', max_len);
            if (nul == nullptr) {
                continue;  // unterminated string in a corrupt table; ignore
            }
            entry.name.assign(base, static_cast<const char*>(nul) - base);
        }
        break;
    }

    out_image = std::move(image);
    return true;
}

// ---------------------------------------------------------------------------
// Entry id -> host path mapping
// ---------------------------------------------------------------------------

std::string PkgEntryPath(const PkgEntry& entry) {
    // Prefer the name stored in the ENTRY_NAMES table when the PKG has one.
    if (!entry.name.empty()) {
        return entry.name;
    }
    switch (entry.id) {
        case kPkgEntryParamSfo:         return "param.sfo";
        case kPkgEntryPlaygoChunkDat:   return "playgo-chunk.dat";
        case kPkgEntryPlaygoChunkSha:   return "playgo-chunk.sha";
        case kPkgEntryPlaygoManifest:   return "playgo-manifest.xml";
        case kPkgEntryPronunciationXml: return "pronunciation.xml";
        case kPkgEntryPronunciationSig: return "pronunciation.sig";
        case kPkgEntryIcon0Png:         return "icon0.png";
        case kPkgEntryPic0Png:          return "pic0.png";
        case kPkgEntrySnd0At9:          return "snd0.at9";
        case kPkgEntryTrophy:           return "trophy/trophy00.trp";
        case kPkgEntryLicenseDat:       return "license.dat";
        case kPkgEntryLicenseInfo:      return "license.info";
        case kPkgEntryNpTitleDat:       return "nptitle.dat";
        case kPkgEntryNpBindDat:        return "npbind.dat";
        case kPkgEntrySelfInfoDat:      return "selfinfo.dat";
        case kPkgEntryImageInfoDat:     return "imageinfo.dat";
        case kPkgEntryPsReservedDat:    return "psreserved.dat";
        // Internal bookkeeping entries: keep them out of the way under
        // entries/ instead of dropping them at the top level.
        case kPkgEntryDigests:          return "entries/digests.bin";
        case kPkgEntryEntryKeys:        return "entries/entry_keys.bin";
        case kPkgEntryImageKey:         return "entries/image_key.bin";
        case kPkgEntryGeneralDigests:   return "entries/general_digests.bin";
        case kPkgEntryMetadata:         return "entries/metadata.bin";
        case kPkgEntryEntryNames:       return "entries/entry_names.bin";
        default: break;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "entries/0x%04X.bin", entry.id);
    return buf;
}

// ---------------------------------------------------------------------------
// Extraction
// ---------------------------------------------------------------------------

bool ExtractPkgEntry(const PkgImage& image, const PkgEntry& entry,
                     const std::string& dest_path, const std::string& passcode) {
    const bool encrypted = entry.IsEncrypted();
    const u64 stored_size = encrypted
                                ? static_cast<u64>(ALIGN_UP(entry.size, 16))
                                : static_cast<u64>(entry.size);

    if (!RangeValid(entry.offset, stored_size, image.file_size)) {
        LOG_ERROR(Loader,
                  "PKG entry 0x%04X range is outside the file; not extracting.",
                  entry.id);
        return false;
    }
    // Entries are 32-bit sized; cap the allocation defensively.
    constexpr u64 kMaxEntrySize = 256ull * 1024 * 1024;
    if (stored_size > kMaxEntrySize) {
        LOG_ERROR(Loader, "PKG entry 0x%04X is implausibly large (%llu bytes).",
                  entry.id, static_cast<unsigned long long>(stored_size));
        return false;
    }

    if (encrypted && image.IsRetail()) {
        LOG_WARN(Loader,
                 "PKG entry 0x%04X is retail-NPDRM encrypted; skipping "
                 "(retail PKG crypto is out of scope).", entry.id);
        return false;
    }

    std::ifstream file(image.path, std::ios::binary);
    if (!file.is_open()) {
        LOG_ERROR(Loader, "Failed to reopen PKG file: %s", image.path.c_str());
        return false;
    }

    std::vector<u8> data(static_cast<size_t>(stored_size));
    if (stored_size > 0 && !ReadAt(file, entry.offset, data.data(), stored_size)) {
        LOG_ERROR(Loader, "Failed to read PKG entry 0x%04X data.", entry.id);
        return false;
    }

    if (encrypted) {
        // fPKG entry crypto (see header comment):
        //   key_seed = DerivePkgKey(content_id, passcode, key_index)
        //   iv_key   = SHA256(meta_entry_be[32] || key_seed)
        //   AES-128-CBC decrypt, key = iv_key[0..16), iv = iv_key[16..32).
        std::array<u8, 32> key_seed{};
        if (!DerivePkgKey(image.content_id, passcode, entry.KeyIndex(), key_seed)) {
            return false;
        }
        u8 entry_be[32];
        SerializePkgEntryBe(entry, entry_be);

        Common::Sha256Context sha;
        sha.Init();
        sha.Update(entry_be, sizeof(entry_be));
        sha.Update(key_seed.data(), key_seed.size());
        const auto iv_key = sha.Final();

        const auto key = Common::Aes128ExpandKey(iv_key.data());
        u8 iv[16];
        std::memcpy(iv, iv_key.data() + 16, sizeof(iv));
        // NB: Common::Aes128CbcDecrypt does not support in-place operation
        // (it re-reads the input block as the next chaining value after
        // writing the output), so decrypt into a separate buffer.
        std::vector<u8> plain(data.size());
        if (!Common::Aes128CbcDecrypt(key, iv, data.data(), plain.data(),
                                      plain.size())) {
            LOG_ERROR(Loader, "Failed to decrypt PKG entry 0x%04X.", entry.id);
            return false;
        }
        plain.resize(entry.size);  // drop the 16-byte alignment padding
        data = std::move(plain);
    }

    std::error_code ec;
    const std::filesystem::path dest(dest_path);
    if (dest.has_parent_path()) {
        std::filesystem::create_directories(dest.parent_path(), ec);
        if (ec) {
            LOG_ERROR(Loader, "Failed to create output directory for %s: %s",
                      dest_path.c_str(), ec.message().c_str());
            return false;
        }
    }
    std::ofstream out(dest_path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        LOG_ERROR(Loader, "Failed to open output file: %s", dest_path.c_str());
        return false;
    }
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
    if (!out) {
        LOG_ERROR(Loader, "Failed to write output file: %s", dest_path.c_str());
        return false;
    }
    return true;
}

bool ExtractPkg(const std::string& path, const std::string& out_dir,
                const std::string& passcode) {
    PkgImage image;
    if (!ParsePkg(path, image)) {
        return false;
    }

    if (image.IsRetail()) {
        LOG_WARN(Loader,
                 "PKG '%s' is retail NPDRM (drm_type=3); only plaintext "
                 "entries will be extracted.", path.c_str());
    }

    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    if (ec) {
        LOG_ERROR(Loader, "Failed to create output directory %s: %s",
                  out_dir.c_str(), ec.message().c_str());
        return false;
    }

    u32 extracted = 0;
    u32 skipped = 0;
    for (const auto& entry : image.entries) {
        const std::string rel = PkgEntryPath(entry);
        const std::string dest =
            (std::filesystem::path(out_dir) / rel).string();
        if (ExtractPkgEntry(image, entry, dest, passcode)) {
            ++extracted;
            LOG_INFO(Loader, "  extracted %s (%u bytes)%s", rel.c_str(),
                     entry.size, entry.IsEncrypted() ? " [decrypted]" : "");
        } else {
            ++skipped;
            LOG_WARN(Loader, "  skipped entry 0x%04X (%s)", entry.id,
                     rel.c_str());
        }
    }

    LOG_INFO(Loader, "PKG extraction complete: %u extracted, %u skipped -> %s",
             extracted, skipped, out_dir.c_str());
    return extracted > 0 || image.entries.empty();
}

}  // namespace Loader
