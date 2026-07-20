#pragma once

// Parser for unencrypted PFS (Playable File System) images, as found inside
// fake/unsigned PKGs (the "pfs_image.dat" inner image) and savedata images.
//
// Read support covers mounting, directory enumeration and extraction; write
// support (writable MountPfs, WriteFile, CreateFile) covers overwriting and
// growing files inside savedata-style images by allocating unreferenced data
// blocks (the format has no free-space bitmap, so a used-block set is built
// by scanning all inodes once per writable mount).
//
// Format reference: https://www.psdevwiki.com/ps4/PFS
//
// Implemented:
//   - Superblock parsing/validation (magic 20130315, version 1 or 2).
//   - 32-bit inodes (di_d32), which is what inner PKG PFS images use.
//   - Direct block pointers (db[12]) and single-level indirect blocks (ib[5]).
//   - Directory enumeration via dirent blocks, starting at the superroot's
//     "uroot" directory (the real root of the file system).
//   - File extraction (whole file) and recursive ExtractAll.
//   - Case-insensitive name lookup when the superblock mode says so.
//   - Writable mounts: in-place writes, file growth (direct blocks, then the
//     ib[] single-indirect chain), and creation of new files in existing
//     directories (fresh inode + appended dirent). Writable mounts are
//     refused for images flagged read-only (superblock ronly), encrypted, or
//     64-bit-inode.
//
// Deliberately skipped (detected and rejected with a warning):
//   - Sector-encrypted PFS (retail games): superblock mode bit 2. The XTS
//     keys live in the PKG entry table and are out of scope for this parser.
//   - 64-bit inodes (superblock mode bit 1): not used by PKG inner images.
//   - Compressed inodes (inode flags bit 0): rare; listing still works, but
//     extraction of such a file fails with a warning.
//   - Double/triple indirect blocks: PKG PFS files are (nearly) always
//     contiguous; ib[] entries are treated as five independent single-indirect
//     blocks, which is what open-source PFS tools do.
//   - flat_path_table: the hash->inode lookup accelerator is not needed for
//     full enumeration; we always walk dirents.

#include "../common/types.h"

#include <fstream>
#include <string>
#include <vector>

namespace Loader {

#pragma pack(push, 1)

// PFS superblock, stored in block 0 of the image (0x50 bytes used).
struct PfsSuperblock {
    u64 version;       // 1 = PS4, 2 = PS5
    u64 format;        // always kPfsFormatMagic (20130315)
    u32 id[2];
    u8  fmode;
    u8  clean;
    u8  ronly;         // 1 = read-only filesystem
    u8  rsv;
    u16 mode;          // bit0 signed, bit1 64-bit inodes, bit2 encrypted,
                       // bit3 case-insensitive
    u16 unk1;
    u32 blocksz;       // power of two, 4 KiB .. 32 MiB
    u32 nbackup;
    u64 nblock;        // always 1: first inode-table block index
    u64 ndinode;       // number of inodes in the inode table
    u64 ndblock;       // number of data blocks
    u64 ndinodeblock;  // number of blocks the inode table occupies
    u64 superroot_ino; // inode index of the superroot directory
};
static_assert(sizeof(PfsSuperblock) == 0x50);

// 32-bit PFS inode (di_d32), 0xA8 bytes. Inodes never cross a block boundary.
struct PfsInode {
    u16 mode;            // kPfsInodeModeFile / kPfsInodeModeDir | permissions
    u16 nlink;
    u32 flags;           // bit0 compressed, bit4 readonly, bit17 internal
    u64 size;            // size in bytes
    u64 size_compressed; // == size for uncompressed images
    u64 times[4];
    u32 time_nsec[4];
    u32 uid;
    u32 gid;
    u64 spare[2];
    u32 blocks;          // number of data blocks occupied
    s32 db[12];          // direct data block indices
    s32 ib[5];           // single-indirect block indices
};
static_assert(sizeof(PfsInode) == 0xA8);

// Dirent header; a NUL-terminated name of `namelen + 1` bytes follows, then
// padding so the whole entry is exactly `entsize` bytes (8-byte aligned).
struct PfsDirent {
    s32 ino;
    s32 type;    // kPfsDirent*
    s32 namelen; // length excluding the NUL terminator
    s32 entsize; // total size of this dirent in bytes
};
static_assert(sizeof(PfsDirent) == 0x10);

#pragma pack(pop)

constexpr u64 kPfsFormatMagic = 20130315;

// Superblock mode bits.
constexpr u16 kPfsSbModeSigned          = 0x1;
constexpr u16 kPfsSbModeInode64         = 0x2;
constexpr u16 kPfsSbModeEncrypted       = 0x4;
constexpr u16 kPfsSbModeCaseInsensitive = 0x8;

// Inode mode bits.
constexpr u16 kPfsInodeModeDir  = 0x4000;
constexpr u16 kPfsInodeModeFile = 0x8000;

// Inode flag bits.
constexpr u32 kPfsInodeFlagCompressed = 0x1;

// Dirent types.
constexpr s32 kPfsDirentFile = 2;
constexpr s32 kPfsDirentDir  = 3;
constexpr s32 kPfsDirentDot  = 4;  // "."
constexpr s32 kPfsDirentDotDot = 5; // ".."

// One entry of a directory listing produced by ListDirectory().
struct PfsEntry {
    std::string name;
    u32 ino = 0;
    bool is_directory = false;
    u64 size = 0; // bytes; for directories, the size of their dirent block data
};

// A mounted PFS image. Holds the open stream plus the parsed superblock.
// Move-only because of the std::fstream member.
class PfsImage {
public:
    PfsImage() = default;
    PfsImage(const PfsImage&) = delete;
    PfsImage& operator=(const PfsImage&) = delete;
    PfsImage(PfsImage&&) = default;
    PfsImage& operator=(PfsImage&&) = default;

    const PfsSuperblock& Header() const { return header_; }
    bool IsCaseInsensitive() const {
        return (header_.mode & kPfsSbModeCaseInsensitive) != 0;
    }
    bool IsWritable() const { return writable_; }

private:
    friend bool MountPfs(const std::string& path, PfsImage& out_image, bool writable);
    friend bool ListDirectory(PfsImage& image, const std::string& guest_path,
                              std::vector<PfsEntry>& out_entries);
    friend bool ExtractFile(PfsImage& image, const std::string& guest_path,
                            const std::string& out_path);
    friend bool ExtractAll(PfsImage& image, const std::string& out_dir);
    friend bool WriteFile(PfsImage& image, const std::string& guest_path,
                          const std::vector<u8>& data);
    friend bool WriteFile(PfsImage& image, const std::string& guest_path,
                          const std::string& src_path);
    friend bool CreateFile(PfsImage& image, const std::string& guest_path,
                           u64 initial_size);

    bool ReadAt(u64 offset, void* dst, u64 size);
    bool WriteAt(u64 offset, const void* src, u64 size);
    bool ReadInode(u32 ino, PfsInode& out_inode);
    bool WriteInode(u32 ino, const PfsInode& inode);
    // Resolve a data block index to a file offset, bounds-checked.
    bool DataBlockOffset(s32 block, u64& out_offset) const;
    // Resolve one data-slot index of an inode to a data block index, walking
    // the direct and single-indirect block pointers.
    bool InodeSlotBlock(const PfsInode& inode, u64 block_slot, s32& out_block);
    // Read `size` bytes of inode payload starting at `data_offset`, walking
    // the direct and single-indirect block pointers.
    bool ReadInodeData(const PfsInode& inode, u64 data_offset, void* dst, u64 size);
    // Write `size` bytes of inode payload starting at `data_offset`. The range
    // must lie within the inode's already-allocated blocks (see ExtendInode).
    bool WriteInodeData(const PfsInode& inode, u64 data_offset, const void* src, u64 size);
    bool ListDirInode(u32 ino, std::vector<PfsEntry>& out_entries);
    // Resolve a '/'-separated path relative to uroot to an inode index.
    bool ResolvePath(const std::string& guest_path, u32& out_ino);
    // Scan every inode once and record which data blocks are referenced
    // (including single-indirect pointer blocks). Called on writable mounts;
    // the format has no free-space bitmap.
    bool BuildUsedBlockSet();
    // Allocate (and zero) an unreferenced data block from block_used_.
    bool AllocDataBlock(s32& out_block);
    // Grow an inode's block map so it can hold `new_size` bytes, allocating
    // data blocks (and indirect pointer blocks) as needed.
    bool ExtendInode(PfsInode& inode, u64 new_size);

    std::fstream file_;
    u64 file_size_ = 0;
    PfsSuperblock header_{};
    u32 uroot_ino_ = 0;
    bool writable_ = false;
    std::vector<bool> block_used_; // valid only on writable mounts
};

// Open and validate a PFS image. Fails (with a warning) for encrypted or
// 64-bit-inode images, which are out of scope (see file header comment).
// With `writable` the image is opened read-write and WriteFile/CreateFile
// become available; a writable mount is refused when the superblock marks
// the file system read-only (ronly) or uses an out-of-scope mode.
bool MountPfs(const std::string& path, PfsImage& out_image, bool writable = false);

// List the entries of `guest_path` ("" or "/" = root, i.e. the image's uroot
// directory). "." and ".." pseudo-entries are skipped.
bool ListDirectory(PfsImage& image, const std::string& guest_path,
                   std::vector<PfsEntry>& out_entries);

// Extract the regular file at `guest_path` to `out_path` on the host.
bool ExtractFile(PfsImage& image, const std::string& guest_path,
                 const std::string& out_path);

// Recursively extract the whole uroot tree under `out_dir`.
bool ExtractAll(PfsImage& image, const std::string& out_dir);

// Overwrite the regular file at `guest_path` with `data`, growing it by
// allocating free data blocks when `data` is larger than the current size
// (a smaller write shrinks the file; blocks are not freed). Requires a
// writable mount.
bool WriteFile(PfsImage& image, const std::string& guest_path,
               const std::vector<u8>& data);

// Same as above, but the new contents are read from a host file.
bool WriteFile(PfsImage& image, const std::string& guest_path,
               const std::string& src_path);

// Create a new zero-filled regular file `guest_path` (parent directory must
// already exist) with `initial_size` bytes, allocating a fresh inode and
// appending a dirent to the parent. Requires a writable mount.
bool CreateFile(PfsImage& image, const std::string& guest_path, u64 initial_size);

} // namespace Loader
