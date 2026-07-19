#!/usr/bin/env python3
"""Pure-Python read-only extractor for ".ffpkg" images (UFS2 disk images
created by UFS2Tool, as used for PS5 scene game dumps).

Usage:
    python tools/extract_ffpkg.py <image.ffpkg> <output_dir>

The image layout (verified empirically on PPSA07429.ffpkg):
  - UFS2 superblock at file offset 0x10000 (magic 0x19540119 at +0x55C).
    Most struct fs fields are zeroed by the tool, so geometry constants
    below are derived/verified empirically instead of parsed.
  - Inode table is linear: inode N (struct dinode, 0x100 bytes) at
    INODE_BASE + N * 0x100.  Root directory is inode 2.
  - Block pointers (di_db / indirect entries) are in units of 512 bytes
    (fs_fsize); each direct/indirect entry covers BLOCK_SPAN = 4096 bytes
    (fs_bsize).  Byte offset = blockno * 512.
  - Directories use the classic 4.4BSD dirent:
    u32 d_fileno, u16 d_reclen, u8 d_type, u8 d_namlen, char d_name[].
  - Indirection: 12 direct blocks, then single/double/triple indirect
    (u64 pointers, 512 pointers per 4096-byte indirect block).
"""

import os
import struct
import sys

FS_MAGIC = 0x19540119
SBOFF = 0x10000          # superblock file offset
INODE_BASE = 0x15000     # file offset of inode 0
INO_SIZE = 0x100         # sizeof(struct dinode) on disk (UFS2)
FRAG = 512               # fs_fsize: block-pointer unit
BLOCK_SPAN = 4096        # fs_bsize: bytes covered by one block pointer
NDADDR = 12
PTRS_PER_IND = BLOCK_SPAN // 8

IFMT = 0o170000
IFREG = 0o100000
IFDIR = 0o040000
IFLNK = 0o120000

DT_DIR = 4
DT_REG = 8
DT_LNK = 10


class Image:
    def __init__(self, path):
        self.f = open(path, "rb")
        self.f.seek(SBOFF + 0x55C)
        magic = struct.unpack("<I", self.f.read(4))[0]
        if magic != FS_MAGIC:
            sys.exit("error: UFS2 magic not found at superblock (0x%08x)" % magic)

    def read_at(self, off, n):
        self.f.seek(off)
        return self.f.read(n)

    def read_inode(self, ino):
        raw = self.read_at(INODE_BASE + ino * INO_SIZE, INO_SIZE)
        mode = struct.unpack("<H", raw[0:2])[0]
        nlink = struct.unpack("<H", raw[2:4])[0]
        size, blocks = struct.unpack("<QQ", raw[0x10:0x20])
        db = struct.unpack("<12Q", raw[0x70:0xD0])
        ib = struct.unpack("<3Q", raw[0xD0:0xE8])
        return {"mode": mode, "nlink": nlink, "size": size,
                "blocks": blocks, "db": db, "ib": ib, "raw": raw}

    def _ind_block(self, blkno):
        raw = self.read_at(blkno * FRAG, BLOCK_SPAN)
        return struct.unpack("<%dQ" % PTRS_PER_IND, raw)

    def _emit_indirect(self, blkno, level, out):
        """Append data block numbers from an indirect block (level>=1)."""
        if blkno == 0:
            return
        for ptr in self._ind_block(blkno):
            if ptr == 0:
                continue
            if level == 1:
                out.append(ptr)
            else:
                self._ind_block_into(ptr, level - 1, out)

    def _ind_block_into(self, blkno, level, out):
        if blkno == 0:
            return
        for ptr in self._ind_block(blkno):
            if ptr == 0:
                continue
            if level == 1:
                out.append(ptr)
            else:
                self._ind_block_into(ptr, level - 1, out)

    def block_list(self, inode):
        """All data block pointers (in FRAG units) for a file."""
        blks = [b for b in inode["db"] if b]
        for level in range(3):
            ib = inode["ib"][level]
            if ib:
                self._ind_block_into(ib, level + 1, blks)
        return blks

    def iter_file_data(self, inode, chunk=1 << 20):
        """Yield file contents truncated to di_size."""
        remaining = inode["size"]
        buf = b""
        for blk in self.block_list(inode):
            if remaining <= 0:
                break
            want = min(BLOCK_SPAN, remaining)
            buf += self.read_at(blk * FRAG, want)
            remaining -= want
            while len(buf) >= chunk:
                yield buf[:chunk]
                buf = buf[chunk:]
        if buf:
            yield buf

    def list_dir(self, inode):
        """Parse a directory inode into (name, fileno, d_type) entries."""
        data = b"".join(self.iter_file_data(inode))
        entries = []
        off = 0
        while off + 8 <= len(data):
            fileno, reclen, dtype, namlen = struct.unpack(
                "<IHBB", data[off:off + 8])
            if reclen == 0:
                break
            name = data[off + 8:off + 8 + namlen]
            entries.append((name, fileno, dtype))
            off += reclen
        return entries


def sanitize(name):
    # keep names safe for Windows paths
    return name.translate({ord(c): "_" for c in '<>:"/\\|?*'})


def extract(img, outdir):
    stats = {"files": 0, "dirs": 0, "links": 0, "bytes": 0, "skipped": 0}
    seen_dirs = set()

    def walk(ino, path):
        if ino in seen_dirs:
            return
        seen_dirs.add(ino)
        inode = img.read_inode(ino)
        if (inode["mode"] & IFMT) != IFDIR:
            print("warning: inode %d is not a directory, skipping %s"
                  % (ino, path))
            return
        os.makedirs(path, exist_ok=True)
        stats["dirs"] += 1
        for name, fileno, dtype in img.list_dir(inode):
            if name in (b".", b"..") or fileno == 0:
                continue
            try:
                sname = sanitize(name.decode("utf-8", "replace"))
            except Exception:
                sname = "ino_%d" % fileno
            target = os.path.join(path, sname)
            child = img.read_inode(fileno)
            fmt = child["mode"] & IFMT
            if fmt == IFDIR:
                walk(fileno, target)
            elif fmt == IFREG:
                written = 0
                with open(target, "wb") as out:
                    for piece in img.iter_file_data(child):
                        out.write(piece)
                        written += len(piece)
                stats["files"] += 1
                stats["bytes"] += written
                print("%8d bytes  %s" % (written, target))
            elif fmt == IFLNK:
                # fast symlink: target stored in di_db area
                if child["size"] <= NDADDR * 8 + 24 and child["db"][0] == 0:
                    link = child["raw"][0x70:0x70 + child["size"]]
                else:
                    link = b"".join(img.iter_file_data(child))
                try:
                    os.symlink(link.decode("utf-8", "replace"), target)
                except (OSError, NotImplementedError):
                    with open(target, "wb") as out:
                        out.write(link)
                stats["links"] += 1
                print("  symlink  %s -> %s" % (target, link[:80]))
            else:
                stats["skipped"] += 1
                print("  skipped mode %o: %s" % (child["mode"], target))

    walk(2, outdir)
    return stats


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    img = Image(sys.argv[1])
    stats = extract(img, sys.argv[2])
    print("\nDone: %(files)d files, %(dirs)d dirs, %(links)d symlinks, "
          "%(bytes)d bytes, %(skipped)d skipped" % stats)


if __name__ == "__main__":
    main()
