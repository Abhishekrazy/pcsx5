// PS5 PKG container parsing / extraction for fake-signed packages.
//
// PS5 PKGs use a different format from PS4 PKGs:
//   - Magic 0x7F464948 ("?FIH") vs PS4's 0x7F434E54 ("\x7FCNT")
//   - Different header layout (big-endian throughout)
//   - Entry table: {u32 id, u32 type/flags, u64 offset, u64 size, u64 pad}
//   - File names embedded in the first 256 bytes of each entry's data
//   - PFS (PlayStation File System) image contains the game filesystem
//
// Scope: fake-signed (fPKG) PS5 PKGs with a known passcode.  Retail
// NPDRM PKGs use keys we do not have; their encrypted entries are
// detected, logged, and skipped.

#pragma once
#include "../common/types.h"

#include <array>
#include <string>
#include <vector>

namespace Loader {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
constexpr u32 kPkgPs5Magic = 0x7F464948u;   // "?FIH"

// PS5 uses the same scene fPKG passcode standard as PS4.
constexpr const char* kPkgPs5ScenePasscode = "00000000000000000000000000000000";
constexpr size_t kPkgPs5PasscodeSize = 32;
constexpr size_t kPkgPs5ContentIdSize = 36;

// Entry type flags
constexpr u32 kPkgPs5EncryptedFlag = 0x80000000u;
constexpr u32 kPkgPs5TypeMask      = 0x0000FFFFu;

// Well-known entry type values
constexpr u32 kPkgPs5TypePfsImage   = 0x0001;   // PFS image
constexpr u32 kPkgPs5TypeParamJson  = 0x0004;   // sce_sys/param.json
constexpr u32 kPkgPs5TypeIcon0      = 0x0005;   // sce_sys/icon0.png
constexpr u32 kPkgPs5TypePic0       = 0x0006;   // sce_sys/pic0.png
constexpr u32 kPkgPs5TypePic1       = 0x0007;   // sce_sys/pic1.png
constexpr u32 kPkgPs5TypeSnd0       = 0x0008;   // sce_sys/snd0.at9
constexpr u32 kPkgPs5TypeTrophy     = 0x000A;   // sce_sys/trophy/trophy00.trp
constexpr u32 kPkgPs5TypeChangeInfo = 0x000C;   // change info
constexpr u32 kPkgPs5TypeEboot      = 0x0012;   // eboot.bin (inside PFS, not here)
constexpr u32 kPkgPs5TypeDigests    = 0x0080;   // package digests

// ---------------------------------------------------------------------------
// DRM types (same values as PS4 PKG)
// ---------------------------------------------------------------------------
constexpr u32 kPkgPs5DrmTypeNone  = 0;
constexpr u32 kPkgPs5DrmTypeFake  = 1;
constexpr u32 kPkgPs5DrmTypeFree  = 2;
constexpr u32 kPkgPs5DrmTypeNpdrm = 3;   // retail; encrypted

// ---------------------------------------------------------------------------
// Parsed structures (host-endian)
// ---------------------------------------------------------------------------

struct PkgPs5Entry {
    u32 id = 0;
    u32 type = 0;           // raw type field (includes encryption flag)
    u64 offset = 0;         // absolute file offset of entry data
    u64 size = 0;           // entry data size in bytes
    std::string name;       // resolved from the first 256 bytes

    bool IsEncrypted() const { return (type & kPkgPs5EncryptedFlag) != 0; }
    u32 TypeId() const { return type & kPkgPs5TypeMask; }
    std::string Extension() const;  // e.g. ".bin", ".png"
};

struct PkgPs5Layout {
    u64 fih_offset = 0, fih_size = 0;   // File Integrity Hash
    u64 pfs_offset = 0, pfs_size = 0;   // PFS image
    u64 sc_offset  = 0, sc_size  = 0;   // Security Certificate
    u64 si_offset  = 0, si_size  = 0;   // Supplementary Information
};

struct PkgPs5Image {
    std::string path;
    u64 file_size = 0;

    // Header fields
    std::string content_id;          // up to 36 chars
    u32 drm_type = 0;
    u32 content_type = 0;
    u32 content_flags = 0;
    u64 body_offset = 0;
    u64 body_size = 0;

    // Layout
    PkgPs5Layout layout;

    // Entry table
    std::vector<PkgPs5Entry> entries;

    // Convenience
    bool IsRetail() const { return drm_type == kPkgPs5DrmTypeNpdrm; }
};

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

// Parse the PS5 PKG header and entry table.  Returns false on malformed
// files (bounds-checked offset/size, validated magic).
bool ParsePkgPs5(const std::string& path, PkgPs5Image& out_image);

// Derive a 32-byte key from content_id + passcode for entry decryption.
// Uses the same scheme as PS4: SHA256(content_id || passcode || index).
bool DerivePkgPs5Key(const std::string& content_id,
                     const std::string& passcode,
                     u32 key_index,
                     std::array<u8, 32>& out_key);

// Extract a single PS5 PKG entry to `dest_path`.  Encrypted entries are
// decrypted with the fPKG scheme when possible; retail-encrypted entries
// are skipped with a warning.
bool ExtractPkgPs5Entry(const PkgPs5Image& image,
                        const PkgPs5Entry& entry,
                        const std::string& dest_path,
                        const std::string& passcode = kPkgPs5ScenePasscode);

// Extract every entry of the PS5 PKG at `path` into `out_dir`.
// Unextractable entries are skipped with a warning.
bool ExtractPkgPs5(const std::string& path,
                   const std::string& out_dir,
                   const std::string& passcode = kPkgPs5ScenePasscode);

// Extract eboot.bin from a PS5 PKG by extracting the PFS image, mounting
// it, and reading eboot.bin from the PFS filesystem.  If eboot.bin is not
// at the PFS root, all PFS files are extracted to `out_dir`.
bool ExtractEbootFromPkgPs5(const std::string& pkg_path,
                             const std::string& out_dir,
                             const std::string& passcode = kPkgPs5ScenePasscode);

// High-level dispatch: auto-detect PS4 vs PS5 PKG by magic and extract.
// Returns false for unknown/unparseable formats.
bool ExtractAnyPkg(const std::string& path,
                   const std::string& out_dir,
                   const std::string& passcode = "00000000000000000000000000000000");

}  // namespace Loader
