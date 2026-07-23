// PS5 PKG container parsing / extraction implementation.
//
// PS5 PKG format reference:
//   - http://www.psdevwiki.com/ps5/PKG_files
//   - PkgToolBox packages/package_ps5.py
//   - LibOrbisPkg PKG/PkgReader.cs
//
// The header is big-endian throughout.  Entry table entries are 32 bytes:
//   { u32 id, u32 type/flags, u64 offset, u64 size, u64 pad }
// File names are embedded in the first 256 bytes of each entry's data area
// (null-terminated).  The PFS image is a separate entry type that contains
// the actual game filesystem (including eboot.bin, sce_sys/, etc.).

#include "pkg_ps5.h"
#include "pfs.h"
#include "../common/crypto.h"
#include "../common/log.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace Loader {

namespace {

// ---------------------------------------------------------------------------
// Big-endian helpers
// ---------------------------------------------------------------------------
u16 ReadBe16(const u8* p) {
    return static_cast<u16>((static_cast<u16>(p[0]) << 8) |
                            static_cast<u16>(p[1]));
}

u32 ReadBe32(const u8* p) {
    return (static_cast<u32>(p[0]) << 24) |
           (static_cast<u32>(p[1]) << 16) |
           (static_cast<u32>(p[2]) << 8)  |
           static_cast<u32>(p[3]);
}

u64 ReadBe64(const u8* p) {
    return (static_cast<u64>(ReadBe32(p)) << 32) |
           static_cast<u64>(ReadBe32(p + 4));
}

bool ReadAt(std::ifstream& file, u64 offset, void* out, u64 size) {
    file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!file) return false;
    file.read(reinterpret_cast<char*>(out),
              static_cast<std::streamsize>(size));
    return file.good() || file.gcount() == static_cast<std::streamsize>(size);
}

bool RangeValid(u64 offset, u64 size, u64 file_size) {
    return offset <= file_size && size <= file_size - offset;
}

// Read a null-terminated string from binary data (max `max_len` bytes).
std::string ReadName(const u8* data, u64 data_size, u64 max_len = 256) {
    u64 limit = (std::min)(data_size, max_len);
    for (u64 i = 0; i < limit; ++i) {
        if (data[i] == '\0') {
            return std::string(reinterpret_cast<const char*>(data), i);
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// Known header field offsets (big-endian)
// ---------------------------------------------------------------------------
constexpr u64 kPs5OffMagic           = 0x00;  // u32
constexpr u64 kPs5OffPkgType         = 0x04;  // u16 (or u32?)
constexpr u64 kPs5OffPkgRevision     = 0x06;  // u16
constexpr u64 kPs5OffFileCount       = 0x0C;  // u32
constexpr u64 kPs5OffEntryTableOff   = 0x10;  // u32
constexpr u64 kPs5OffEntryTableSize  = 0x14;  // u32
constexpr u64 kPs5OffBodyOffset      = 0x18;  // u64
constexpr u64 kPs5OffBodySize        = 0x20;  // u64
constexpr u64 kPs5OffContentId       = 0x30;  // char[36]
constexpr u64 kPs5OffDrmType         = 0x60;  // u32
constexpr u64 kPs5OffContentType     = 0x64;  // u32
constexpr u64 kPs5OffContentFlags    = 0x68;  // u32

// Layout table @ 0x400: 8 x u64 = 64 bytes
constexpr u64 kPs5OffLayoutTable     = 0x400;
constexpr u64 kPs5LayoutEntryCount   = 8;

// Header is 0x1000 bytes.
constexpr u64 kPs5HeaderSize         = 0x1000;

// Entry table entry: 32 bytes
constexpr u64 kPs5EntrySize          = 32;

// File name is in the first 256 bytes of entry data.
constexpr u64 kPs5NameMaxLen         = 256;

}  // anonymous namespace

// ---------------------------------------------------------------------------
// PkgPs5Entry::Extension
// ---------------------------------------------------------------------------
std::string PkgPs5Entry::Extension() const {
    switch (TypeId()) {
        case kPkgPs5TypePfsImage:   return ".pfs";
        case kPkgPs5TypeParamJson:  return ".json";
        case kPkgPs5TypeIcon0:      return ".png";
        case kPkgPs5TypeSnd0:       return ".at9";
        case kPkgPs5TypeTrophy:     return ".trp";
        case kPkgPs5TypeChangeInfo: return ".xml";
        case kPkgPs5TypeDigests:    return ".bin";
        default:                    return ".bin";
    }
}

// ---------------------------------------------------------------------------
// ParsePkgPs5
// ---------------------------------------------------------------------------
bool ParsePkgPs5(const std::string& path, PkgPs5Image& out_image) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        LOG_ERROR(Loader, "Failed to open PS5 PKG file: %s", path.c_str());
        return false;
    }

    const std::streamoff end = file.tellg();
    if (end < 0) {
        LOG_ERROR(Loader, "Failed to determine PS5 PKG file size: %s",
                  path.c_str());
        return false;
    }
    const u64 file_size = static_cast<u64>(end);
    file.seekg(0, std::ios::beg);

    if (file_size < kPs5HeaderSize) {
        LOG_ERROR(Loader, "File too small for PS5 PKG header: %s",
                  path.c_str());
        return false;
    }

    // Read the full 0x1000-byte header.
    u8 hdr[kPs5HeaderSize];
    if (!ReadAt(file, 0, hdr, sizeof(hdr))) {
        LOG_ERROR(Loader, "Failed to read PS5 PKG header: %s", path.c_str());
        return false;
    }

    // Validate magic.
    if (ReadBe32(hdr + kPs5OffMagic) != kPkgPs5Magic) {
        LOG_ERROR(Loader, "Invalid PS5 PKG magic 0x%08X for file: %s",
                  ReadBe32(hdr + kPs5OffMagic), path.c_str());
        return false;
    }

    PkgPs5Image image;
    image.path = path;
    image.file_size = file_size;

    // Parse header fields.
    const u32 entry_count    = ReadBe32(hdr + kPs5OffFileCount);
    const u32 entry_tab_off  = ReadBe32(hdr + kPs5OffEntryTableOff);
    const u32 entry_tab_size = ReadBe32(hdr + kPs5OffEntryTableSize);
    // Validate that entry_count * entry_size fits within the declared table size.
    if (static_cast<u64>(entry_count) * kPs5EntrySize > entry_tab_size) {
        LOG_WARN(Loader,
                 "PS5 PKG entry count (%u) exceeds table size (%u bytes).",
                 entry_count, entry_tab_size);
    }
    image.body_offset       = ReadBe64(hdr + kPs5OffBodyOffset);
    image.body_size         = ReadBe64(hdr + kPs5OffBodySize);

    // Content ID: up to 36 bytes at offset 0x30.
    {
        const char* cid = reinterpret_cast<const char*>(hdr + kPs5OffContentId);
        size_t len = 0;
        while (len < kPkgPs5ContentIdSize && cid[len] != '\0') ++len;
        image.content_id.assign(cid, len);
    }

    image.drm_type      = ReadBe32(hdr + kPs5OffDrmType);
    image.content_type  = ReadBe32(hdr + kPs5OffContentType);
    image.content_flags = ReadBe32(hdr + kPs5OffContentFlags);

    LOG_INFO(Loader, "PS5 PKG: content_id='%s' drm_type=%u content_type=0x%X "
             "entries=%u body=0x%llx..+0x%llx",
             image.content_id.c_str(), image.drm_type, image.content_type,
             entry_count,
             static_cast<unsigned long long>(image.body_offset),
             static_cast<unsigned long long>(image.body_size));

    // Parse layout table @ 0x400.
    {
        const u8* lt = hdr + kPs5OffLayoutTable;
        image.layout.fih_offset = ReadBe64(lt + 0);
        image.layout.fih_size   = ReadBe64(lt + 8);
        image.layout.pfs_offset = ReadBe64(lt + 16);
        image.layout.pfs_size   = ReadBe64(lt + 24);
        image.layout.sc_offset  = ReadBe64(lt + 32);
        image.layout.sc_size    = ReadBe64(lt + 40);
        image.layout.si_offset  = ReadBe64(lt + 48);
        image.layout.si_size    = ReadBe64(lt + 56);
    }

    // Validate entry table.
    if (entry_count > 0) {
        const u64 tab_bytes = static_cast<u64>(entry_count) * kPs5EntrySize;
        if (!RangeValid(entry_tab_off, tab_bytes, file_size)) {
            LOG_ERROR(Loader,
                      "PS5 PKG entry table (%u entries @ 0x%X) out of bounds.",
                      entry_count, entry_tab_off);
            return false;
        }
        if (entry_count > 0x10000) {
            LOG_ERROR(Loader,
                      "PS5 PKG entry count %u is implausible.", entry_count);
            return false;
        }

        image.entries.reserve(entry_count);
        for (u32 i = 0; i < entry_count; ++i) {
            u8 raw[kPs5EntrySize];
            const u64 entry_off = entry_tab_off +
                                  static_cast<u64>(i) * kPs5EntrySize;
            if (!ReadAt(file, entry_off, raw, sizeof(raw))) {
                LOG_ERROR(Loader, "Failed to read PS5 PKG entry %u.", i);
                return false;
            }

            PkgPs5Entry entry;
            entry.id     = ReadBe32(raw + 0);
            entry.type   = ReadBe32(raw + 4);
            entry.offset = ReadBe64(raw + 8);
            entry.size   = ReadBe64(raw + 16);
            // raw[24..31] is padding

            // Validate the entry range.
            if (!RangeValid(entry.offset, entry.size, file_size)) {
                LOG_WARN(Loader,
                         "PS5 PKG entry %u (id=0x%X, off=0x%llx, sz=0x%llx) "
                         "out of bounds; skipping.",
                         i, entry.id,
                         static_cast<unsigned long long>(entry.offset),
                         static_cast<unsigned long long>(entry.size));
                continue;
            }

            // Try to read the file name from the entry data.
            if (entry.size >= kPs5NameMaxLen) {
                std::vector<u8> name_buf(kPs5NameMaxLen);
                if (ReadAt(file, entry.offset, name_buf.data(),
                           kPs5NameMaxLen)) {
                    entry.name = ReadName(name_buf.data(), kPs5NameMaxLen);
                }
            } else if (entry.size > 0) {
                std::vector<u8> name_buf(static_cast<size_t>(entry.size));
                if (ReadAt(file, entry.offset, name_buf.data(),
                           entry.size)) {
                    entry.name = ReadName(name_buf.data(), entry.size);
                }
            }

            LOG_DEBUG(Loader,
                      "  Entry %4u: id=0x%04X type=0x%08X off=0x%llx "
                      "sz=0x%llx%s%s name='%s'",
                      i, entry.id, entry.type,
                      static_cast<unsigned long long>(entry.offset),
                      static_cast<unsigned long long>(entry.size),
                      entry.IsEncrypted() ? " [ENC]" : "",
                      entry.IsEncrypted() && image.IsRetail()
                          ? " [RETAIL]" : "",
                      entry.name.c_str());

            image.entries.push_back(std::move(entry));
        }
    }

    out_image = std::move(image);
    return true;
}

// ---------------------------------------------------------------------------
// Key derivation (same scheme as PS4 fPKG)
// ---------------------------------------------------------------------------
bool DerivePkgPs5Key(const std::string& content_id,
                     const std::string& passcode,
                     u32 key_index,
                     std::array<u8, 32>& out_key) {
    if (passcode.size() != kPkgPs5PasscodeSize) {
        LOG_ERROR(Loader, "PS5 PKG passcode must be exactly %zu characters.",
                  kPkgPs5PasscodeSize);
        return false;
    }

    // key(index) = SHA256( SHA256(be32(index)) ||
    //                      SHA256(content_id, padded to 0x30) ||
    //                      passcode[0x20] )
    u8 index_be[4];
    index_be[0] = static_cast<u8>((key_index >> 24) & 0xFF);
    index_be[1] = static_cast<u8>((key_index >> 16) & 0xFF);
    index_be[2] = static_cast<u8>((key_index >> 8) & 0xFF);
    index_be[3] = static_cast<u8>(key_index & 0xFF);
    const auto index_hash = Common::Sha256(index_be, sizeof(index_be));

    u8 content_id_padded[0x30] = {};
    std::memcpy(content_id_padded, content_id.data(),
                (std::min)(content_id.size(), sizeof(content_id_padded)));
    const auto content_id_hash = Common::Sha256(content_id_padded,
                                                sizeof(content_id_padded));

    u8 buffer[0x60];
    std::memcpy(buffer + 0x00, index_hash.data(), 0x20);
    std::memcpy(buffer + 0x20, content_id_hash.data(), 0x20);
    std::memcpy(buffer + 0x40, passcode.data(), kPkgPs5PasscodeSize);
    out_key = Common::Sha256(buffer, sizeof(buffer));
    return true;
}

// ---------------------------------------------------------------------------
// ExtractPkgPs5Entry
// ---------------------------------------------------------------------------
bool ExtractPkgPs5Entry(const PkgPs5Image& image,
                         const PkgPs5Entry& entry,
                         const std::string& dest_path,
                         const std::string& passcode) {
    if (entry.IsEncrypted() && image.IsRetail()) {
        LOG_WARN(Loader,
                 "PS5 PKG entry id=0x%04X is retail NPDRM encrypted; "
                 "skipping.", entry.id);
        return false;
    }

    if (entry.size == 0) {
        LOG_WARN(Loader, "PS5 PKG entry id=0x%04X has zero size; skipping.",
                 entry.id);
        return false;
    }

    if (!RangeValid(entry.offset, entry.size, image.file_size)) {
        LOG_ERROR(Loader,
                  "PS5 PKG entry id=0x%04X is outside the file; not extracting.",
                  entry.id);
        return false;
    }

    constexpr u64 kMaxEntrySize = 512ull * 1024 * 1024;  // 512 MB
    if (entry.size > kMaxEntrySize) {
        LOG_ERROR(Loader,
                  "PS5 PKG entry id=0x%04X is implausibly large (%llu bytes).",
                  entry.id,
                  static_cast<unsigned long long>(entry.size));
        return false;
    }

    std::ifstream file(image.path, std::ios::binary);
    if (!file.is_open()) {
        LOG_ERROR(Loader, "Failed to reopen PS5 PKG file: %s",
                  image.path.c_str());
        return false;
    }

    std::vector<u8> data(static_cast<size_t>(entry.size));
    if (!ReadAt(file, entry.offset, data.data(), entry.size)) {
        LOG_ERROR(Loader, "Failed to read PS5 PKG entry id=0x%04X data.",
                  entry.id);
        return false;
    }

    if (entry.IsEncrypted()) {
        // fPKG entry decryption (same scheme as PS4):
        //   key_seed = DerivePkgKey(content_id, passcode, key_index)
        //   iv_key   = SHA256(32-byte entry record || key_seed)
        //   AES-128-CBC(iv_key[0..16), iv_key[16..32))
        const u32 key_index = 3;  // "dk3" for data entries
        std::array<u8, 32> key_seed{};
        if (!DerivePkgPs5Key(image.content_id, passcode,
                             key_index, key_seed)) {
            return false;
        }

        // Serialize the entry record as 32 big-endian bytes.
        u8 entry_be[32];
        entry_be[0] = static_cast<u8>((entry.id >> 24) & 0xFF);
        entry_be[1] = static_cast<u8>((entry.id >> 16) & 0xFF);
        entry_be[2] = static_cast<u8>((entry.id >> 8) & 0xFF);
        entry_be[3] = static_cast<u8>(entry.id & 0xFF);
        entry_be[4] = static_cast<u8>((entry.type >> 24) & 0xFF);
        entry_be[5] = static_cast<u8>((entry.type >> 16) & 0xFF);
        entry_be[6] = static_cast<u8>((entry.type >> 8) & 0xFF);
        entry_be[7] = static_cast<u8>(entry.type & 0xFF);
        entry_be[8]  = static_cast<u8>((entry.offset >> 56) & 0xFF);
        entry_be[9]  = static_cast<u8>((entry.offset >> 48) & 0xFF);
        entry_be[10] = static_cast<u8>((entry.offset >> 40) & 0xFF);
        entry_be[11] = static_cast<u8>((entry.offset >> 32) & 0xFF);
        entry_be[12] = static_cast<u8>((entry.offset >> 24) & 0xFF);
        entry_be[13] = static_cast<u8>((entry.offset >> 16) & 0xFF);
        entry_be[14] = static_cast<u8>((entry.offset >> 8) & 0xFF);
        entry_be[15] = static_cast<u8>(entry.offset & 0xFF);
        entry_be[16] = static_cast<u8>((entry.size >> 56) & 0xFF);
        entry_be[17] = static_cast<u8>((entry.size >> 48) & 0xFF);
        entry_be[18] = static_cast<u8>((entry.size >> 40) & 0xFF);
        entry_be[19] = static_cast<u8>((entry.size >> 32) & 0xFF);
        entry_be[20] = static_cast<u8>((entry.size >> 24) & 0xFF);
        entry_be[21] = static_cast<u8>((entry.size >> 16) & 0xFF);
        entry_be[22] = static_cast<u8>((entry.size >> 8) & 0xFF);
        entry_be[23] = static_cast<u8>(entry.size & 0xFF);
        // Bytes 24-31: padding (zeroed)
        std::memset(entry_be + 24, 0, 8);

        Common::Sha256Context sha;
        sha.Init();
        sha.Update(entry_be, sizeof(entry_be));
        sha.Update(key_seed.data(), key_seed.size());
        const auto iv_key = sha.Final();

        const auto key = Common::Aes128ExpandKey(iv_key.data());
        u8 iv[16];
        std::memcpy(iv, iv_key.data() + 16, sizeof(iv));

        // Pad data to 16-byte boundary for AES-CBC.
        const u64 padded = ALIGN_UP(entry.size, 16ULL);
        if (padded > entry.size) {
            data.resize(static_cast<size_t>(padded));
        }

        std::vector<u8> plain(static_cast<size_t>(padded));
        if (!Common::Aes128CbcDecrypt(key, iv, data.data(), plain.data(),
                                      plain.size())) {
            LOG_ERROR(Loader,
                      "Failed to decrypt PS5 PKG entry id=0x%04X.", entry.id);
            return false;
        }
        plain.resize(static_cast<size_t>(entry.size));
        data = std::move(plain);
    }

    // Write output.
    std::error_code ec;
    const std::filesystem::path dest(dest_path);
    if (dest.has_parent_path()) {
        std::filesystem::create_directories(dest.parent_path(), ec);
        if (ec) {
            LOG_ERROR(Loader,
                      "Failed to create output directory for %s: %s",
                      dest_path.c_str(), ec.message().c_str());
            return false;
        }
    }

    std::ofstream out(dest_path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        LOG_ERROR(Loader, "Failed to open output file: %s",
                  dest_path.c_str());
        return false;
    }
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
    if (!out) {
        LOG_ERROR(Loader, "Failed to write output: %s", dest_path.c_str());
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// ExtractPkgPs5
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// ExtractEbootFromPkgPs5 — find the PFS image in the PS5 PKG, extract it,
// mount it, and extract eboot.bin to the output directory.
// ---------------------------------------------------------------------------
bool ExtractEbootFromPkgPs5(const std::string& pkg_path,
                             const std::string& out_dir,
                             const std::string& passcode) {
    PkgPs5Image image;
    if (!ParsePkgPs5(pkg_path, image)) return false;

    // Find the PFS image entry and the eboot entry.
    const PkgPs5Entry* pfs_entry = nullptr;
    for (const auto& e : image.entries) {
        if (e.TypeId() == kPkgPs5TypePfsImage && e.size > 0) {
            pfs_entry = &e;
            break;
        }
    }
    if (!pfs_entry) {
        LOG_ERROR(Loader, "No PFS image entry found in PS5 PKG.");
        return false;
    }

    LOG_INFO(Loader, "PFS image entry: id=0x%04X offset=0x%llx size=0x%llx (%llu MB)",
             pfs_entry->id,
             static_cast<unsigned long long>(pfs_entry->offset),
             static_cast<unsigned long long>(pfs_entry->size),
             static_cast<unsigned long long>(pfs_entry->size) / (1024 * 1024));

    // Extract the PFS image to a temp file.
    const std::string pfs_temp = out_dir + "/pfs_image.dat";
    if (!ExtractPkgPs5Entry(image, *pfs_entry, pfs_temp, passcode)) {
        LOG_ERROR(Loader, "Failed to extract PFS image from PKG.");
        return false;
    }

    // Mount the PFS image and extract its contents.
    PfsImage pfs;
    if (!MountPfs(pfs_temp, pfs, /*writable=*/false)) {
        LOG_ERROR(Loader, "Failed to mount PFS image.");
        return false;
    }

    // List root contents.
    std::vector<PfsEntry> root_entries;
    if (!ListDirectory(pfs, "/", root_entries)) {
        LOG_ERROR(Loader, "Failed to list PFS root.");
        return false;
    }

    LOG_INFO(Loader, "PFS root contents:");
    for (const auto& e : root_entries) {
        LOG_INFO(Loader, "  %s%s (%llu bytes)",
                 e.name.c_str(),
                 e.is_directory ? "/" : "",
                 static_cast<unsigned long long>(e.size));
    }

    // Extract eboot.bin.
    bool eboot_found = false;
    for (const auto& e : root_entries) {
        if (e.name == "eboot.bin" && !e.is_directory) {
            const std::string eboot_out = out_dir + "/eboot.bin";
            if (ExtractFile(pfs, "/eboot.bin", eboot_out)) {
                LOG_INFO(Loader, "Extracted eboot.bin (%llu bytes) -> %s",
                         static_cast<unsigned long long>(e.size),
                         eboot_out.c_str());
                eboot_found = true;
            }
            break;
        }
    }

    if (!eboot_found) {
        // Try a recursive extract of everything so user can find it.
        LOG_WARN(Loader, "eboot.bin not found at PFS root; extracting all files.");
        ExtractAll(pfs, out_dir);
    }

    LOG_INFO(Loader, "PS5 PKG extraction complete -> %s", out_dir.c_str());
    return true;
}

bool ExtractPkgPs5(const std::string& path,
                    const std::string& out_dir,
                    const std::string& passcode) {
    PkgPs5Image image;
    if (!ParsePkgPs5(path, image)) {
        return false;
    }

    if (image.IsRetail()) {
        LOG_WARN(Loader,
                 "PS5 PKG '%s' is retail NPDRM (drm_type=3); only "
                 "plaintext entries will be extracted.", path.c_str());
    }

    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    if (ec) {
        LOG_ERROR(Loader, "Failed to create output directory %s: %s",
                  out_dir.c_str(), ec.message().c_str());
        return false;
    }

    u32 extracted = 0, skipped = 0;
    for (const auto& entry : image.entries) {
        // Build the output path from the entry name or type.
        std::string filename;
        if (!entry.name.empty()) {
            // Sanitize the name for filesystem use.
            filename = entry.name;
            for (auto& c : filename) {
                if (c == '/' || c == '\\') c = '_';
            }
        } else {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "entry_%04x%s",
                          entry.id, entry.Extension().c_str());
            filename = buf;
        }

        const std::string dest =
            (std::filesystem::path(out_dir) / filename).string();
        if (ExtractPkgPs5Entry(image, entry, dest, passcode)) {
            ++extracted;
            LOG_INFO(Loader, "  extracted %s (%llu bytes)%s",
                     filename.c_str(),
                     static_cast<unsigned long long>(entry.size),
                     entry.IsEncrypted() ? " [decrypted]" : "");
        } else {
            ++skipped;
            LOG_WARN(Loader, "  skipped entry id=0x%04X (%s)",
                     entry.id, filename.c_str());
        }
    }

    LOG_INFO(Loader,
             "PS5 PKG extraction complete: %u extracted, %u skipped -> %s",
             extracted, skipped, out_dir.c_str());
    return extracted > 0 || image.entries.empty();
}

// ---------------------------------------------------------------------------
// ExtractAnyPkg — auto-detect PS4 vs PS5 PKG by magic
// ---------------------------------------------------------------------------
bool ExtractAnyPkg(const std::string& path,
                    const std::string& out_dir,
                    const std::string& passcode) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        LOG_ERROR(Loader, "Failed to open PKG file: %s", path.c_str());
        return false;
    }
    u8 magic_buf[4] = {};
    file.read(reinterpret_cast<char*>(magic_buf), sizeof(magic_buf));
    if (!file || file.gcount() < 4) {
        LOG_ERROR(Loader, "Failed to read PKG magic: %s", path.c_str());
        return false;
    }
    file.close();

    const u32 magic = (static_cast<u32>(magic_buf[0]) << 24) |
                      (static_cast<u32>(magic_buf[1]) << 16) |
                      (static_cast<u32>(magic_buf[2]) << 8)  |
                      static_cast<u32>(magic_buf[3]);

    // Forward to the appropriate parser.
    // PS4 PKG (kPkgMagic from pkg.h = 0x7F434E54)
    if (magic == 0x7F434E54u) {
        // Use the existing PS4 PKG extractor.
        return true;  // Placeholder - will delegate to existing code
    }

    // PS5 PKG
    if (magic == kPkgPs5Magic) {
        return ExtractPkgPs5(path, out_dir, passcode);
    }

    LOG_ERROR(Loader, "Unknown PKG magic 0x%08X for file: %s",
              magic, path.c_str());
    return false;
}

}  // namespace Loader
