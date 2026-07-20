#include "pfs.h"
#include "../common/log.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <filesystem>

namespace Loader {

namespace {

// Sanity limits so a corrupt superblock cannot drive huge allocations.
constexpr u64 kMaxBlockSize   = 0x2000000; // 32 MiB, per the format spec
constexpr u64 kMinBlockSize   = 0x1000;    // 4 KiB
constexpr u64 kMaxInodeCount  = 1ull << 24;
constexpr u32 kMaxPathLen     = 1024;

bool IsPowerOfTwo(u64 v) {
    return v != 0 && (v & (v - 1)) == 0;
}

bool NamesEqual(const std::string& a, const std::string& b, bool case_insensitive) {
    if (!case_insensitive) {
        return a == b;
    }
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        const unsigned char ca = static_cast<unsigned char>(a[i]);
        const unsigned char cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) {
            return false;
        }
    }
    return true;
}

} // namespace

bool PfsImage::ReadAt(u64 offset, void* dst, u64 size) {
    if (offset > file_size_ || size > file_size_ - offset) {
        LOG_ERROR(Loader, "PFS: read outside image (offset=0x%llx size=0x%llx file=0x%llx).",
                  static_cast<unsigned long long>(offset),
                  static_cast<unsigned long long>(size),
                  static_cast<unsigned long long>(file_size_));
        return false;
    }
    if (size == 0) {
        return true;
    }
    file_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    file_.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(size));
    if (!file_) {
        LOG_ERROR(Loader, "PFS: failed to read 0x%llx bytes at 0x%llx.",
                  static_cast<unsigned long long>(size),
                  static_cast<unsigned long long>(offset));
        return false;
    }
    return true;
}

bool PfsImage::WriteAt(u64 offset, const void* src, u64 size) {
    if (offset > file_size_ || size > file_size_ - offset) {
        LOG_ERROR(Loader, "PFS: write outside image (offset=0x%llx size=0x%llx file=0x%llx).",
                  static_cast<unsigned long long>(offset),
                  static_cast<unsigned long long>(size),
                  static_cast<unsigned long long>(file_size_));
        return false;
    }
    if (size == 0) {
        return true;
    }
    file_.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    file_.write(reinterpret_cast<const char*>(src), static_cast<std::streamsize>(size));
    if (!file_) {
        LOG_ERROR(Loader, "PFS: failed to write 0x%llx bytes at 0x%llx.",
                  static_cast<unsigned long long>(size),
                  static_cast<unsigned long long>(offset));
        return false;
    }
    return true;
}

bool PfsImage::ReadInode(u32 ino, PfsInode& out_inode) {
    if (ino >= header_.ndinode) {
        LOG_ERROR(Loader, "PFS: inode index %u out of range (ndinode=%llu).", ino,
                  static_cast<unsigned long long>(header_.ndinode));
        return false;
    }
    const u64 inopb = header_.blocksz / sizeof(PfsInode); // inodes per block
    const u64 table_capacity = header_.ndinodeblock * inopb;
    if (ino >= table_capacity) {
        LOG_ERROR(Loader, "PFS: inode index %u beyond inode-table capacity %llu.", ino,
                  static_cast<unsigned long long>(table_capacity));
        return false;
    }
    // The inode table starts at block `nblock` (1 in practice, right after the
    // superblock) and spans ndinodeblock blocks.
    const u64 offset = (header_.nblock + ino / inopb) * header_.blocksz +
                       (ino % inopb) * sizeof(PfsInode);
    return ReadAt(offset, &out_inode, sizeof(PfsInode));
}

bool PfsImage::WriteInode(u32 ino, const PfsInode& inode) {
    if (ino >= header_.ndinode) {
        LOG_ERROR(Loader, "PFS: inode index %u out of range (ndinode=%llu).", ino,
                  static_cast<unsigned long long>(header_.ndinode));
        return false;
    }
    const u64 inopb = header_.blocksz / sizeof(PfsInode); // inodes per block
    const u64 table_capacity = header_.ndinodeblock * inopb;
    if (ino >= table_capacity) {
        LOG_ERROR(Loader, "PFS: inode index %u beyond inode-table capacity %llu.", ino,
                  static_cast<unsigned long long>(table_capacity));
        return false;
    }
    // Same layout as ReadInode: table at block `nblock`, ndinodeblock blocks.
    const u64 offset = (header_.nblock + ino / inopb) * header_.blocksz +
                       (ino % inopb) * sizeof(PfsInode);
    return WriteAt(offset, &inode, sizeof(PfsInode));
}

bool PfsImage::DataBlockOffset(s32 block, u64& out_offset) const {
    // Data block d lives at image block (nblock + ndinodeblock + d).
    if (block < 0 || static_cast<u64>(block) >= header_.ndblock) {
        LOG_ERROR(Loader, "PFS: data block index %d out of range (ndblock=%llu).", block,
                  static_cast<unsigned long long>(header_.ndblock));
        return false;
    }
    const u64 image_block = header_.nblock + header_.ndinodeblock + static_cast<u64>(block);
    const u64 offset = image_block * header_.blocksz;
    if (offset >= file_size_ || header_.blocksz > file_size_ - offset) {
        LOG_ERROR(Loader, "PFS: data block %d lies outside the image.", block);
        return false;
    }
    out_offset = offset;
    return true;
}

bool PfsImage::InodeSlotBlock(const PfsInode& inode, u64 block_slot, s32& out_block) {
    if (block_slot < 12) {
        out_block = inode.db[block_slot];
        return true;
    }
    // Single-indirect: treat each ib[] entry as an independent block
    // of s32 block indices (what open-source PFS tools do). True
    // double/triple indirection is not implemented (see pfs.h).
    const u64 ptrs_per_indirect = header_.blocksz / sizeof(s32);
    const u64 rel = block_slot - 12;
    const u64 slot = rel / ptrs_per_indirect;
    if (slot >= 5) {
        LOG_ERROR(Loader, "PFS: inode requires double-indirect blocks (unsupported).");
        return false;
    }
    if (inode.ib[slot] < 0) {
        LOG_ERROR(Loader, "PFS: inode has a hole in its indirect block chain.");
        return false;
    }
    u64 ib_offset = 0;
    if (!DataBlockOffset(inode.ib[slot], ib_offset)) {
        return false;
    }
    const u64 entry = rel % ptrs_per_indirect;
    return ReadAt(ib_offset + entry * sizeof(s32), &out_block, sizeof(s32));
}

bool PfsImage::ReadInodeData(const PfsInode& inode, u64 data_offset, void* dst, u64 size) {
    if (size == 0) {
        return true;
    }
    if (data_offset > inode.size || size > inode.size - data_offset) {
        LOG_ERROR(Loader, "PFS: inode data range (off=0x%llx size=0x%llx) exceeds inode size 0x%llx.",
                  static_cast<unsigned long long>(data_offset),
                  static_cast<unsigned long long>(size),
                  static_cast<unsigned long long>(inode.size));
        return false;
    }

    const u64 blocksz = header_.blocksz;
    auto* out = static_cast<u8*>(dst);

    u64 done = 0;
    while (done < size) {
        const u64 pos = data_offset + done;
        const u64 block_slot = pos / blocksz;
        const u64 in_block = pos % blocksz;
        const u64 chunk = std::min<u64>(size - done, blocksz - in_block);

        s32 block = -1;
        if (!InodeSlotBlock(inode, block_slot, block)) {
            return false;
        }

        u64 offset = 0;
        if (!DataBlockOffset(block, offset)) {
            return false;
        }
        if (!ReadAt(offset + in_block, out + done, chunk)) {
            return false;
        }
        done += chunk;
    }
    return true;
}

bool PfsImage::WriteInodeData(const PfsInode& inode, u64 data_offset, const void* src, u64 size) {
    if (size == 0) {
        return true;
    }
    // Writes must stay within already-allocated blocks; ExtendInode() grows
    // the block map first. The capacity is the allocated slot count rounded
    // up from the inode size.
    const u64 blocksz = header_.blocksz;
    const u64 capacity = ((inode.size + blocksz - 1) / blocksz) * blocksz;
    if (data_offset > capacity || size > capacity - data_offset) {
        LOG_ERROR(Loader, "PFS: inode write range (off=0x%llx size=0x%llx) exceeds allocated capacity 0x%llx.",
                  static_cast<unsigned long long>(data_offset),
                  static_cast<unsigned long long>(size),
                  static_cast<unsigned long long>(capacity));
        return false;
    }

    const auto* in = static_cast<const u8*>(src);

    u64 done = 0;
    while (done < size) {
        const u64 pos = data_offset + done;
        const u64 block_slot = pos / blocksz;
        const u64 in_block = pos % blocksz;
        const u64 chunk = std::min<u64>(size - done, blocksz - in_block);

        s32 block = -1;
        if (!InodeSlotBlock(inode, block_slot, block)) {
            return false;
        }

        u64 offset = 0;
        if (!DataBlockOffset(block, offset)) {
            return false;
        }
        if (!WriteAt(offset + in_block, in + done, chunk)) {
            return false;
        }
        done += chunk;
    }
    return true;
}

bool PfsImage::ListDirInode(u32 ino, std::vector<PfsEntry>& out_entries) {
    PfsInode inode{};
    if (!ReadInode(ino, inode)) {
        return false;
    }
    if ((inode.mode & kPfsInodeModeDir) == 0) {
        LOG_ERROR(Loader, "PFS: inode %u is not a directory (mode=0x%04X).", ino, inode.mode);
        return false;
    }
    if (inode.size > header_.ndblock * header_.blocksz) {
        LOG_ERROR(Loader, "PFS: directory inode %u has an implausible size 0x%llx.", ino,
                  static_cast<unsigned long long>(inode.size));
        return false;
    }

    std::vector<u8> dir_data(static_cast<size_t>(inode.size));
    if (!ReadInodeData(inode, 0, dir_data.data(), inode.size)) {
        return false;
    }

    out_entries.clear();
    u64 pos = 0;
    while (pos + sizeof(PfsDirent) <= inode.size) {
        PfsDirent de{};
        std::memcpy(&de, dir_data.data() + pos, sizeof(de));

        if (de.entsize < static_cast<s32>(sizeof(PfsDirent)) ||
            (de.entsize & 7) != 0 ||
            static_cast<u64>(de.entsize) > inode.size - pos) {
            LOG_ERROR(Loader, "PFS: malformed dirent at offset 0x%llx (entsize=%d).",
                      static_cast<unsigned long long>(pos), de.entsize);
            return false;
        }
        if (de.namelen < 0 ||
            static_cast<u64>(de.namelen) + 1 > static_cast<u64>(de.entsize) - sizeof(PfsDirent)) {
            LOG_ERROR(Loader, "PFS: malformed dirent at offset 0x%llx (namelen=%d).",
                      static_cast<unsigned long long>(pos), de.namelen);
            return false;
        }
        const char* name_ptr = reinterpret_cast<const char*>(dir_data.data() + pos + sizeof(PfsDirent));
        if (name_ptr[de.namelen] != '\0') {
            LOG_ERROR(Loader, "PFS: dirent name not NUL-terminated at offset 0x%llx.",
                      static_cast<unsigned long long>(pos));
            return false;
        }

        if (de.type == kPfsDirentFile || de.type == kPfsDirentDir) {
            if (de.ino < 0 || static_cast<u64>(de.ino) >= header_.ndinode) {
                LOG_ERROR(Loader, "PFS: dirent '%.*s' references invalid inode %d.",
                          de.namelen, name_ptr, de.ino);
                return false;
            }
            PfsEntry entry;
            entry.name.assign(name_ptr, static_cast<size_t>(de.namelen));
            entry.ino = static_cast<u32>(de.ino);
            entry.is_directory = (de.type == kPfsDirentDir);
            PfsInode child{};
            if (ReadInode(entry.ino, child)) {
                entry.size = child.size;
            }
            out_entries.push_back(std::move(entry));
        } else if (de.type != kPfsDirentDot && de.type != kPfsDirentDotDot) {
            // Unknown dirent types (symlinks etc.): skip with a warning.
            LOG_WARN(Loader, "PFS: skipping dirent '%.*s' of unsupported type %d.",
                     de.namelen, name_ptr, de.type);
        }
        pos += static_cast<u64>(de.entsize);
    }
    return true;
}

bool PfsImage::ResolvePath(const std::string& guest_path, u32& out_ino) {
    if (guest_path.size() > kMaxPathLen) {
        LOG_ERROR(Loader, "PFS: guest path too long.");
        return false;
    }
    u32 current = uroot_ino_;
    size_t pos = 0;
    while (pos < guest_path.size()) {
        const size_t slash = guest_path.find('/', pos);
        const std::string component = guest_path.substr(
            pos, slash == std::string::npos ? std::string::npos : slash - pos);
        pos = (slash == std::string::npos) ? guest_path.size() : slash + 1;
        if (component.empty() || component == ".") {
            continue;
        }

        std::vector<PfsEntry> entries;
        if (!ListDirInode(current, entries)) {
            return false;
        }
        bool found = false;
        for (const auto& e : entries) {
            if (NamesEqual(e.name, component, IsCaseInsensitive())) {
                current = e.ino;
                found = true;
                break;
            }
        }
        if (!found) {
            LOG_ERROR(Loader, "PFS: path component '%s' not found under '%s'.",
                      component.c_str(), guest_path.c_str());
            return false;
        }
    }
    out_ino = current;
    return true;
}

bool PfsImage::BuildUsedBlockSet() {
    block_used_.assign(static_cast<size_t>(header_.ndblock), false);
    const u64 blocksz = header_.blocksz;
    const u64 ptrs_per_indirect = blocksz / sizeof(s32);
    for (u32 ino = 0; ino < header_.ndinode; ++ino) {
        PfsInode inode{};
        if (!ReadInode(ino, inode)) {
            return false;
        }
        if (inode.mode == 0) {
            continue; // unused inode-table slot
        }
        const u64 slots = (inode.size + blocksz - 1) / blocksz;
        for (u64 s = 0; s < slots; ++s) {
            s32 block = -1;
            if (s < 12) {
                block = inode.db[s];
            } else {
                const u64 rel = s - 12;
                const u64 idx = rel / ptrs_per_indirect;
                if (idx >= 5) {
                    break; // double-indirect: out of scope, stop scanning
                }
                const s32 ib = inode.ib[idx];
                if (ib < 0 || static_cast<u64>(ib) >= header_.ndblock) {
                    break; // tolerate a corrupt chain; nothing more to mark
                }
                block_used_[static_cast<size_t>(ib)] = true;
                u64 ib_offset = 0;
                if (!DataBlockOffset(ib, ib_offset)) {
                    return false;
                }
                if (!ReadAt(ib_offset + (rel % ptrs_per_indirect) * sizeof(s32),
                            &block, sizeof(block))) {
                    return false;
                }
            }
            if (block >= 0 && static_cast<u64>(block) < header_.ndblock) {
                block_used_[static_cast<size_t>(block)] = true;
            }
        }
    }
    return true;
}

bool PfsImage::AllocDataBlock(s32& out_block) {
    for (size_t b = 0; b < block_used_.size(); ++b) {
        if (block_used_[b]) {
            continue;
        }
        u64 offset = 0;
        if (!DataBlockOffset(static_cast<s32>(b), offset)) {
            return false;
        }
        // Zero the block so stale bytes never leak into file data, fresh
        // dirent areas, or indirect pointer tables.
        std::vector<u8> zero(static_cast<size_t>(header_.blocksz), 0);
        if (!WriteAt(offset, zero.data(), zero.size())) {
            return false;
        }
        block_used_[b] = true;
        out_block = static_cast<s32>(b);
        return true;
    }
    LOG_ERROR(Loader, "PFS: no free data blocks left (ndblock=%llu); growing the image is not supported.",
              static_cast<unsigned long long>(header_.ndblock));
    return false;
}

bool PfsImage::ExtendInode(PfsInode& inode, u64 new_size) {
    const u64 blocksz = header_.blocksz;
    const u64 ptrs_per_indirect = blocksz / sizeof(s32);
    u64 have = (inode.size + blocksz - 1) / blocksz;
    const u64 need = (new_size + blocksz - 1) / blocksz;
    while (have < need) {
        s32 block = -1;
        if (!AllocDataBlock(block)) {
            return false;
        }
        if (have < 12) {
            inode.db[have] = block;
        } else {
            const u64 rel = have - 12;
            const u64 idx = rel / ptrs_per_indirect;
            if (idx >= 5) {
                LOG_ERROR(Loader, "PFS: growth requires double-indirect blocks (unsupported).");
                return false;
            }
            if (inode.ib[idx] <= 0) {
                // Allocate the indirect pointer block itself too. Unused
                // ib[] entries are 0 (fresh/zero-filled inodes) or -1.
                s32 ib = -1;
                if (!AllocDataBlock(ib)) {
                    return false;
                }
                inode.ib[idx] = ib;
                ++inode.blocks;
            }
            u64 ib_offset = 0;
            if (!DataBlockOffset(inode.ib[idx], ib_offset)) {
                return false;
            }
            if (!WriteAt(ib_offset + (rel % ptrs_per_indirect) * sizeof(s32),
                         &block, sizeof(block))) {
                return false;
            }
        }
        ++inode.blocks;
        ++have;
    }
    return true;
}

bool MountPfs(const std::string& path, PfsImage& out_image, bool writable) {
    LOG_INFO(Loader, "Mounting PFS image%s: %s", writable ? " (writable)" : "", path.c_str());

    PfsImage img;
    img.writable_ = writable;
    // std::fstream with ios::out alone would truncate; in|out opens read-write
    // without truncating. Read-only mounts keep the old ate-seek behavior.
    const std::ios::openmode mode = writable
                                        ? (std::ios::binary | std::ios::in | std::ios::out)
                                        : (std::ios::binary | std::ios::in | std::ios::ate);
    img.file_.open(path, mode);
    if (!img.file_.is_open()) {
        LOG_ERROR(Loader, "PFS: failed to open %s%s.", path.c_str(),
                  writable ? " read-write" : "");
        return false;
    }
    img.file_.seekg(0, std::ios::end);
    const std::streamoff end = img.file_.tellg();
    if (end < 0) {
        LOG_ERROR(Loader, "PFS: failed to determine size of %s.", path.c_str());
        return false;
    }
    img.file_size_ = static_cast<u64>(end);

    if (img.file_size_ < sizeof(PfsSuperblock)) {
        LOG_ERROR(Loader, "PFS: image too small for a superblock.");
        return false;
    }
    if (!img.ReadAt(0, &img.header_, sizeof(PfsSuperblock))) {
        return false;
    }
    const PfsSuperblock& hdr = img.header_;

    if (hdr.format != kPfsFormatMagic) {
        LOG_ERROR(Loader, "PFS: bad format magic 0x%llx (expected %llu).",
                  static_cast<unsigned long long>(hdr.format),
                  static_cast<unsigned long long>(kPfsFormatMagic));
        return false;
    }
    if (hdr.version != 1 && hdr.version != 2) {
        LOG_ERROR(Loader, "PFS: unsupported version %llu.",
                  static_cast<unsigned long long>(hdr.version));
        return false;
    }
    if ((hdr.mode & kPfsSbModeEncrypted) != 0) {
        // Retail PFS images are sector-encrypted (XTS-AES-128); the keys are
        // derived from the PKG entry table. Out of scope for this parser.
        LOG_WARN(Loader, "PFS: image is encrypted (mode bit 2); skipping %s.", path.c_str());
        return false;
    }
    if ((hdr.mode & kPfsSbModeInode64) != 0) {
        LOG_WARN(Loader, "PFS: 64-bit inodes are not supported; skipping %s.", path.c_str());
        return false;
    }
    if ((hdr.mode & kPfsSbModeSigned) != 0) {
        LOG_INFO(Loader, "PFS: signed image; signature is not verified (read-only use).");
    }
    if (writable) {
        if (hdr.ronly != 0) {
            LOG_ERROR(Loader, "PFS: file system is marked read-only; refusing writable mount of %s.",
                      path.c_str());
            return false;
        }
        if (hdr.ndblock > (1ull << 28)) {
            LOG_ERROR(Loader, "PFS: implausible data block count %llu; refusing writable mount.",
                      static_cast<unsigned long long>(hdr.ndblock));
            return false;
        }
        // Encrypted / 64-bit-inode images already failed above.
    }
    if (!IsPowerOfTwo(hdr.blocksz) || hdr.blocksz < kMinBlockSize || hdr.blocksz > kMaxBlockSize) {
        LOG_ERROR(Loader, "PFS: invalid block size %u.", hdr.blocksz);
        return false;
    }
    if (hdr.ndinode == 0 || hdr.ndinode > kMaxInodeCount) {
        LOG_ERROR(Loader, "PFS: invalid inode count %llu.",
                  static_cast<unsigned long long>(hdr.ndinode));
        return false;
    }
    if (hdr.ndinodeblock == 0 || hdr.superroot_ino >= hdr.ndinode) {
        LOG_ERROR(Loader, "PFS: invalid inode table (ndinodeblock=%llu superroot_ino=%llu).",
                  static_cast<unsigned long long>(hdr.ndinodeblock),
                  static_cast<unsigned long long>(hdr.superroot_ino));
        return false;
    }

    // The inode table occupies blocks [nblock, nblock + ndinodeblock); reject
    // images where it would run past EOF.
    const u64 table_end = (hdr.nblock + hdr.ndinodeblock) * hdr.blocksz;
    if (table_end < hdr.nblock * hdr.blocksz || table_end > img.file_size_) {
        LOG_ERROR(Loader, "PFS: inode table extends past the end of the image.");
        return false;
    }
    if (hdr.ndinode > hdr.ndinodeblock * (hdr.blocksz / sizeof(PfsInode))) {
        LOG_ERROR(Loader, "PFS: ndinode exceeds inode-table capacity.");
        return false;
    }

    // Locate uroot: the superroot directory contains "uroot", which is the
    // real root of the file system tree.
    PfsInode superroot{};
    if (!img.ReadInode(static_cast<u32>(hdr.superroot_ino), superroot)) {
        return false;
    }
    if ((superroot.mode & kPfsInodeModeDir) == 0) {
        LOG_ERROR(Loader, "PFS: superroot inode %llu is not a directory.",
                  static_cast<unsigned long long>(hdr.superroot_ino));
        return false;
    }
    std::vector<PfsEntry> superroot_entries;
    if (!img.ListDirInode(static_cast<u32>(hdr.superroot_ino), superroot_entries)) {
        return false;
    }
    bool found_uroot = false;
    for (const auto& e : superroot_entries) {
        if (e.is_directory && NamesEqual(e.name, "uroot", img.IsCaseInsensitive())) {
            img.uroot_ino_ = e.ino;
            found_uroot = true;
            break;
        }
    }
    if (!found_uroot) {
        LOG_ERROR(Loader, "PFS: superroot has no 'uroot' directory.");
        return false;
    }

    if (writable && !img.BuildUsedBlockSet()) {
        LOG_ERROR(Loader, "PFS: failed to build the used-block map; refusing writable mount.");
        return false;
    }

    LOG_INFO(Loader, "PFS mounted: version=%llu blocksz=%u inodes=%llu uroot_ino=%u%s",
             static_cast<unsigned long long>(hdr.version), hdr.blocksz,
             static_cast<unsigned long long>(hdr.ndinode), img.uroot_ino_,
             img.IsCaseInsensitive() ? " (case-insensitive)" : "");

    out_image = std::move(img);
    return true;
}

bool ListDirectory(PfsImage& image, const std::string& guest_path,
                   std::vector<PfsEntry>& out_entries) {
    u32 ino = 0;
    if (!image.ResolvePath(guest_path, ino)) {
        return false;
    }
    return image.ListDirInode(ino, out_entries);
}

bool ExtractFile(PfsImage& image, const std::string& guest_path,
                 const std::string& out_path) {
    u32 ino = 0;
    if (!image.ResolvePath(guest_path, ino)) {
        return false;
    }
    PfsInode inode{};
    if (!image.ReadInode(ino, inode)) {
        return false;
    }
    if ((inode.mode & kPfsInodeModeFile) == 0) {
        LOG_ERROR(Loader, "PFS: '%s' is not a regular file.", guest_path.c_str());
        return false;
    }
    if ((inode.flags & kPfsInodeFlagCompressed) != 0) {
        LOG_WARN(Loader, "PFS: '%s' is compressed; extraction not supported.",
                 guest_path.c_str());
        return false;
    }

    std::vector<u8> data(static_cast<size_t>(inode.size));
    if (!image.ReadInodeData(inode, 0, data.data(), inode.size)) {
        return false;
    }

    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        LOG_ERROR(Loader, "PFS: failed to open output %s.", out_path.c_str());
        return false;
    }
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
    if (!out) {
        LOG_ERROR(Loader, "PFS: failed to write %s.", out_path.c_str());
        return false;
    }
    return true;
}

namespace {

bool ExtractDirRecursive(PfsImage& image, const std::string& guest_path,
                         const std::filesystem::path& host_dir) {
    std::vector<PfsEntry> entries;
    if (!ListDirectory(image, guest_path, entries)) {
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(host_dir, ec);
    if (ec) {
        LOG_ERROR(Loader, "PFS: failed to create directory %s: %s",
                  host_dir.string().c_str(), ec.message().c_str());
        return false;
    }
    for (const auto& e : entries) {
        const std::string child_guest = guest_path.empty() || guest_path == "/"
                                            ? e.name
                                            : guest_path + "/" + e.name;
        if (e.is_directory) {
            if (!ExtractDirRecursive(image, child_guest, host_dir / e.name)) {
                return false;
            }
        } else {
            if (!ExtractFile(image, child_guest, (host_dir / e.name).string())) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

bool ExtractAll(PfsImage& image, const std::string& out_dir) {
    return ExtractDirRecursive(image, "", std::filesystem::path(out_dir));
}

namespace {

void TouchInode(PfsInode& inode) {
    const u64 now = static_cast<u64>(std::time(nullptr));
    for (auto& t : inode.times) {
        t = now;
    }
    for (auto& n : inode.time_nsec) {
        n = 0;
    }
}

bool CheckWritable(PfsImage& image, const char* op) {
    if (!image.IsWritable()) {
        LOG_ERROR(Loader, "PFS: %s requires a writable mount.", op);
        return false;
    }
    return true;
}

} // namespace

bool WriteFile(PfsImage& image, const std::string& guest_path,
               const std::vector<u8>& data) {
    if (!CheckWritable(image, "WriteFile")) {
        return false;
    }
    u32 ino = 0;
    if (!image.ResolvePath(guest_path, ino)) {
        return false;
    }
    PfsInode inode{};
    if (!image.ReadInode(ino, inode)) {
        return false;
    }
    if ((inode.mode & kPfsInodeModeFile) == 0) {
        LOG_ERROR(Loader, "PFS: '%s' is not a regular file.", guest_path.c_str());
        return false;
    }
    if ((inode.flags & kPfsInodeFlagCompressed) != 0) {
        LOG_WARN(Loader, "PFS: '%s' is compressed; writing not supported.",
                 guest_path.c_str());
        return false;
    }

    // Grow the block map first (ExtendInode derives the current slot count
    // from the old size), then bump the size so WriteInodeData's capacity
    // check covers the new range.
    if (!image.ExtendInode(inode, data.size())) {
        return false;
    }
    inode.size = data.size();
    inode.size_compressed = data.size();
    if (!image.WriteInodeData(inode, 0, data.data(), data.size())) {
        return false;
    }
    TouchInode(inode);
    return image.WriteInode(ino, inode);
}

bool WriteFile(PfsImage& image, const std::string& guest_path,
               const std::string& src_path) {
    std::ifstream in(src_path, std::ios::binary | std::ios::ate);
    if (!in.is_open()) {
        LOG_ERROR(Loader, "PFS: failed to open source file %s.", src_path.c_str());
        return false;
    }
    const std::streamoff end = in.tellg();
    if (end < 0) {
        LOG_ERROR(Loader, "PFS: failed to determine size of %s.", src_path.c_str());
        return false;
    }
    std::vector<u8> data(static_cast<size_t>(end));
    in.seekg(0, std::ios::beg);
    if (!data.empty()) {
        in.read(reinterpret_cast<char*>(data.data()), end);
        if (!in) {
            LOG_ERROR(Loader, "PFS: failed to read %s.", src_path.c_str());
            return false;
        }
    }
    return WriteFile(image, guest_path, data);
}

bool CreateFile(PfsImage& image, const std::string& guest_path, u64 initial_size) {
    if (!CheckWritable(image, "CreateFile")) {
        return false;
    }

    // Split into parent directory + final component.
    const size_t slash = guest_path.find_last_of('/');
    const std::string parent_path =
        slash == std::string::npos ? std::string() : guest_path.substr(0, slash);
    const std::string name =
        slash == std::string::npos ? guest_path : guest_path.substr(slash + 1);
    if (name.empty() || name == "." || name == ".." || name.size() > 255) {
        LOG_ERROR(Loader, "PFS: invalid file name in '%s'.", guest_path.c_str());
        return false;
    }

    u32 parent_ino = 0;
    if (!image.ResolvePath(parent_path, parent_ino)) {
        return false;
    }
    PfsInode parent{};
    if (!image.ReadInode(parent_ino, parent)) {
        return false;
    }
    if ((parent.mode & kPfsInodeModeDir) == 0) {
        LOG_ERROR(Loader, "PFS: parent of '%s' is not a directory.", guest_path.c_str());
        return false;
    }

    // The name must not already exist in the parent.
    std::vector<PfsEntry> entries;
    if (!image.ListDirInode(parent_ino, entries)) {
        return false;
    }
    for (const auto& e : entries) {
        if (NamesEqual(e.name, name, image.IsCaseInsensitive())) {
            LOG_ERROR(Loader, "PFS: '%s' already exists.", guest_path.c_str());
            return false;
        }
    }

    // Allocate a fresh inode-table slot (unused slots have mode == 0).
    u32 new_ino = 0;
    bool found = false;
    for (u32 ino = 0; ino < image.Header().ndinode; ++ino) {
        PfsInode probe{};
        if (!image.ReadInode(ino, probe)) {
            return false;
        }
        if (probe.mode == 0) {
            new_ino = ino;
            found = true;
            break;
        }
    }
    if (!found) {
        LOG_ERROR(Loader, "PFS: inode table is full; cannot create '%s'.", guest_path.c_str());
        return false;
    }

    PfsInode inode{};
    inode.mode = kPfsInodeModeFile | 0644;
    inode.nlink = 1;
    if (!image.ExtendInode(inode, initial_size)) {
        return false;
    }
    inode.size = initial_size;
    inode.size_compressed = initial_size;
    TouchInode(inode);
    if (!image.WriteInode(new_ino, inode)) {
        return false;
    }

    // Append the dirent to the parent's dirent data, growing the parent's
    // block map when the entry crosses into a fresh block.
    PfsDirent de{};
    de.ino = static_cast<s32>(new_ino);
    de.type = kPfsDirentFile;
    de.namelen = static_cast<s32>(name.size());
    const u64 raw = sizeof(PfsDirent) + name.size() + 1;
    de.entsize = static_cast<s32>((raw + 7) & ~u64(7));
    std::vector<u8> dirent_bytes(static_cast<size_t>(de.entsize), 0);
    std::memcpy(dirent_bytes.data(), &de, sizeof(de));
    std::memcpy(dirent_bytes.data() + sizeof(de), name.data(), name.size());

    const u64 write_at = parent.size;
    if (!image.ExtendInode(parent, parent.size + static_cast<u64>(de.entsize))) {
        return false;
    }
    parent.size += static_cast<u64>(de.entsize);
    parent.size_compressed = parent.size;
    if (!image.WriteInodeData(parent, write_at, dirent_bytes.data(), dirent_bytes.size())) {
        return false;
    }
    TouchInode(parent);
    if (!image.WriteInode(parent_ino, parent)) {
        return false;
    }

    LOG_INFO(Loader, "PFS: created '%s' (ino=%u size=%llu).", guest_path.c_str(), new_ino,
             static_cast<unsigned long long>(initial_size));
    return true;
}

} // namespace Loader
