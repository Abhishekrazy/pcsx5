#pragma once
//
// PKG container parsing / extraction for fake-signed (fPKG) Prospero/Orbis
// packages.
//
// Format references (big-endian header, "\x7FCNT" magic):
//   - http://www.psdevwiki.com/ps4/PKG_files
//   - http://www.psdevwiki.com/ps5/PKG_files
//   - OpenOrbis/LibOrbisPkg (PKG/PkgReader.cs, PKG/Entry.cs, Util/Crypto.cs)
//
// Scope: fake-signed PKGs built with a known passcode (scene standard is 32
// ASCII '0' characters).  Retail NPDRM PKGs use passcodes we do not have;
// their encrypted entries are detected, logged and skipped (see pkg.cpp).
//
#include "../common/types.h"

#include <array>
#include <string>
#include <vector>

namespace Loader {

// ---------------------------------------------------------------------------
// On-disk structures (all integer fields are big-endian in the file).
// ---------------------------------------------------------------------------

constexpr u32 kPkgMagic = 0x7F434E54u;   // "\x7FCNT"
constexpr u64 kPkgHeaderSize = 0x1000;

// pkg_table_entry: 32 bytes per entry in the entry table.
struct PkgTableEntryDisk {
    u32 id;
    u32 filename_offset;  // offset into the ENTRY_NAMES (0x200) table, 0 = none
    u32 flags1;           // bit 31 (0x80000000): entry data is encrypted
    u32 flags2;           // bits 12-15: encryption key index
    u32 offset;           // absolute file offset of entry data
    u32 size;             // entry data size in bytes
    u64 padding;
};
static_assert(sizeof(PkgTableEntryDisk) == 32, "PKG table entry must be 32 bytes");

// Well-known entry ids (psdevwiki "Entry IDs").
enum PkgEntryId : u32 {
    kPkgEntryDigests        = 0x0001,
    kPkgEntryEntryKeys      = 0x0010,
    kPkgEntryImageKey       = 0x0020,
    kPkgEntryGeneralDigests = 0x0080,
    kPkgEntryMetadata       = 0x0100,
    kPkgEntryEntryNames     = 0x0200,
    kPkgEntryLicenseDat     = 0x0400,
    kPkgEntryLicenseInfo    = 0x0401,
    kPkgEntryNpTitleDat     = 0x0402,
    kPkgEntryNpBindDat      = 0x0403,
    kPkgEntrySelfInfoDat    = 0x0404,
    kPkgEntryImageInfoDat   = 0x0406,
    kPkgEntryPsReservedDat  = 0x0409,
    kPkgEntryParamSfo       = 0x1000,
    kPkgEntryPlaygoChunkDat = 0x1001,
    kPkgEntryPlaygoChunkSha = 0x1002,
    kPkgEntryPlaygoManifest = 0x1003,
    kPkgEntryPronunciationXml = 0x1004,
    kPkgEntryPronunciationSig = 0x1005,
    kPkgEntryIcon0Png       = 0x1200,
    kPkgEntryPic0Png        = 0x1220,
    kPkgEntrySnd0At9        = 0x1240,
    kPkgEntryTrophy         = 0x1400,
};

// drm_type field values (LibOrbisPkg DrmType).
constexpr u32 kPkgDrmTypeNone  = 0;
constexpr u32 kPkgDrmTypeFake  = 1;
constexpr u32 kPkgDrmTypeFree  = 2;
constexpr u32 kPkgDrmTypeNpdrm = 3;   // retail; encrypted entries out of scope

constexpr u32 kPkgEntryFlagEncrypted = 0x80000000u;

// The passcode used by scene fPKG tooling ("fake" signing).
constexpr char kPkgScenePasscode[] = "00000000000000000000000000000000";  // 32 chars
constexpr size_t kPkgPasscodeSize = 32;
constexpr size_t kPkgContentIdSize = 0x24;  // 36 characters

// ---------------------------------------------------------------------------
// Parsed, host-endian representation.
// ---------------------------------------------------------------------------

struct PkgEntry {
    u32 id = 0;
    u32 filename_offset = 0;
    u32 flags1 = 0;
    u32 flags2 = 0;
    u32 offset = 0;   // absolute file offset
    u32 size = 0;
    std::string name; // resolved from the ENTRY_NAMES table when present

    bool IsEncrypted() const { return (flags1 & kPkgEntryFlagEncrypted) != 0; }
    u32 KeyIndex() const { return (flags2 & 0xF000u) >> 12; }
};

struct PkgImage {
    std::string path;
    u64 file_size = 0;
    std::string content_id;          // up to 36 chars, from header @ 0x40
    u32 drm_type = 0;
    u32 content_type = 0;
    u32 content_flags = 0;
    u64 body_offset = 0;
    u64 body_size = 0;
    u64 pfs_image_offset = 0;        // header @ 0x410
    u64 pfs_image_size = 0;          // header @ 0x418
    std::vector<PkgEntry> entries;

    // True for retail NPDRM packages whose entry crypto we cannot handle.
    bool IsRetail() const { return drm_type == kPkgDrmTypeNpdrm; }
};

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

// Parse the PKG header and entry table.  Every offset/size is validated
// against the actual file size; malformed files fail cleanly with false.
bool ParsePkg(const std::string& path, PkgImage& out_image);

// Conventional host-relative file name for a well-known entry id
// (e.g. 0x1000 -> "param.sfo", 0x1400 -> "trophy/trophy00.trp").
// Unknown ids map to "entries/0x%04X.bin".
std::string PkgEntryPath(const PkgEntry& entry);

// Derive a 32-byte PKG key from the content id and passcode:
//   key = SHA256( SHA256(be32(index)) || SHA256(content_id padded to 0x30)
//                 || passcode[0x20] )
// Index 1 is the EKPFS; index 3 ("dk3") encrypts most encrypted entries.
// Returns false if the passcode is not exactly 32 bytes.
bool DerivePkgKey(const std::string& content_id, const std::string& passcode,
                  u32 key_index, std::array<u8, 32>& out_key);

// Serialise an entry back to its 32-byte big-endian on-disk form.  The
// encrypted-entry scheme hashes these exact bytes, so tests (and any future
// writer) need the identical layout.  Returns false on overflow.
bool SerializePkgEntryBe(const PkgEntry& entry, u8 out[32]);

// Extract a single entry to `dest_path`.  Plaintext entries are copied as-is;
// encrypted entries are decrypted with the fPKG scheme (AES-128-CBC with a
// per-entry key/IV derived from the entry record and the passcode-derived
// key seed).  Returns false for entries we cannot handle (retail crypto) or
// on any bounds/IO error.
bool ExtractPkgEntry(const PkgImage& image, const PkgEntry& entry,
                     const std::string& dest_path,
                     const std::string& passcode = kPkgScenePasscode);

// Extract every entry of the PKG at `path` into `out_dir`, using the
// conventional layout produced by PkgEntryPath (param.sfo, icon0.png,
// license.dat, ... under the output directory).  Unextractable entries
// (retail-encrypted) are skipped with a warning and do not fail the run.
// Returns false only on hard errors (unparseable PKG, output dir problems).
bool ExtractPkg(const std::string& path, const std::string& out_dir,
                const std::string& passcode = kPkgScenePasscode);

}  // namespace Loader
