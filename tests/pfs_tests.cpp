// PFS (Playable File System) parser tests.
//
// Verifies:
//   - A synthetic minimal inner-PFS image (superroot -> uroot -> files/dirs)
//     mounts, lists directories, and extracts byte-exact files.
//   - ExtractAll reproduces the whole tree on the host filesystem.
//   - Malformed images are rejected cleanly: bad magic, truncated image,
//     encrypted image (out of scope), bad superroot inode, dirent pointing at
//     an out-of-range inode, and a file inode with an out-of-range data block.
//   - Write support: writable mounts (refused for ronly images), round-trip
//     write -> remount -> read, growth across block boundaries and into the
//     single-indirect chain, CreateFile + list + extract, and negative tests
//     for writes on read-only mounts and corrupt images.
//
// The PfsBuilder below mirrors the ElfBuilder approach in loader_corpus.cpp:
// a byte vector is assembled block by block and the superblock is patched in
// at Finalize() time.
//
// Self-contained: links against src/loader/pfs.cpp and src/common/log.cpp.

#include "loader/pfs.h"
#include "common/log.h"
#include "common/types.h"

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

#define EXPECT(cond, msg) do {                                     \
    ++g_checks;                                                    \
    if (!(cond)) {                                                 \
        ++g_failures;                                              \
        std::fprintf(stderr, "[FAIL] %s:%d %s\n",                  \
                     __FILE__, __LINE__, (msg));                   \
    }                                                              \
} while (0)

#define EXPECT_EQ(a, b, msg) EXPECT((a) == (b), msg)

// ---------------------------------------------------------------------------
// Synthetic PFS image builder.
//
// Layout (block size 0x1000, the spec minimum):
//   block 0: superblock (patched by Finalize)
//   block 1: inode table (up to blocksz / sizeof(PfsInode) inodes)
//   blocks 2..: data blocks, allocated sequentially
// ---------------------------------------------------------------------------
struct PfsBuilder {
    struct DirentSpec {
        u32 ino;
        s32 type;
        std::string name;
    };

    static constexpr u32 kBlockSz = 0x1000;
    static constexpr u32 kInodeTableBlocks = 1;

    std::vector<u8> bytes;
    u32 next_ino = 0;
    u32 next_data_block = 0;
    u32 inode_count_pad = 0; // extra unused (zeroed) inode-table slots
    u32 superroot_ino = 0;
    u64 format_magic = Loader::kPfsFormatMagic;
    u16 sb_mode = 0; // superblock mode bits; bit2 = encrypted

    PfsBuilder() {
        // Reserve superblock + inode table blocks.
        bytes.resize((1 + kInodeTableBlocks) * kBlockSz, 0);
    }

    u32 AllocInode() { return next_ino++; }

    // Allocate a data block and return its data-block index.
    u32 AllocDataBlock() {
        bytes.resize(bytes.size() + kBlockSz, 0);
        return next_data_block++;
    }

    // Append `n` zeroed data blocks that no inode references, i.e. free
    // space a writable mount can allocate from.
    void AddFreeDataBlocks(u32 n) {
        for (u32 i = 0; i < n; ++i) {
            AllocDataBlock();
        }
    }

    u64 DataBlockOffset(u32 data_block) const {
        return (1 + kInodeTableBlocks + data_block) * kBlockSz;
    }

    void WriteData(u32 data_block, const void* src, size_t n) {
        std::memcpy(bytes.data() + DataBlockOffset(data_block), src, n);
    }

    // Serialize a dirent list into `data_block`. Each dirent is 8-byte
    // aligned and padded to its entsize. Returns the number of bytes used,
    // which is what the directory inode's size field must be set to.
    size_t WriteDirents(u32 data_block, const std::vector<DirentSpec>& entries) {
        u8* base = bytes.data() + DataBlockOffset(data_block);
        size_t pos = 0;
        for (const auto& e : entries) {
            Loader::PfsDirent de{};
            de.ino = static_cast<s32>(e.ino);
            de.type = e.type;
            de.namelen = static_cast<s32>(e.name.size());
            const size_t raw = sizeof(Loader::PfsDirent) + e.name.size() + 1;
            de.entsize = static_cast<s32>((raw + 7) & ~size_t(7));
            if (pos + static_cast<size_t>(de.entsize) > kBlockSz) {
                std::fprintf(stderr, "[INTERNAL] PfsBuilder dirent block overflow\n");
                std::abort();
            }
            std::memcpy(base + pos, &de, sizeof(de));
            std::memcpy(base + pos + sizeof(de), e.name.data(), e.name.size());
            pos += static_cast<size_t>(de.entsize);
        }
        return pos;
    }

    // Write a directory inode (ino already allocated) whose dirent data lives
    // in a freshly allocated data block.
    void WriteDirInode(u32 ino, const std::vector<DirentSpec>& entries, u16 nlink) {
        const u32 blk = AllocDataBlock();
        const size_t used = WriteDirents(blk, entries);

        Loader::PfsInode inode{};
        inode.mode = Loader::kPfsInodeModeDir | 0755;
        inode.nlink = nlink;
        inode.size = used;
        inode.size_compressed = inode.size;
        inode.blocks = 1;
        inode.db[0] = static_cast<s32>(blk);
        WriteInode(ino, inode);
    }

    // Create a directory inode whose dirent data lives in one data block.
    // Returns the inode index.
    u32 AddDir(const std::vector<DirentSpec>& entries) {
        const u32 ino = AllocInode();
        WriteDirInode(ino, entries, 2);
        return ino;
    }

    // Create a regular-file inode backed by a single data block.
    u32 AddFile(const void* src, size_t n) {
        const u32 ino = AllocInode();
        const u32 blk = AllocDataBlock();
        WriteData(blk, src, n);

        Loader::PfsInode inode{};
        inode.mode = Loader::kPfsInodeModeFile | 0644;
        inode.nlink = 1;
        inode.size = n;
        inode.size_compressed = n;
        inode.blocks = 1;
        inode.db[0] = static_cast<s32>(blk);
        WriteInode(ino, inode);
        return ino;
    }

    void WriteInode(u32 ino, const Loader::PfsInode& inode) {
        const u64 off = kBlockSz + static_cast<u64>(ino) * sizeof(Loader::PfsInode);
        if (off + sizeof(Loader::PfsInode) > kBlockSz * (1 + kInodeTableBlocks)) {
            std::fprintf(stderr, "[INTERNAL] PfsBuilder inode table overflow\n");
            std::abort();
        }
        std::memcpy(bytes.data() + off, &inode, sizeof(inode));
    }

    void Finalize() {
        Loader::PfsSuperblock sb{};
        sb.version = 1;
        sb.format = format_magic;
        sb.fmode = 1;
        sb.clean = 1;
        sb.ronly = 1;
        sb.mode = sb_mode;
        sb.blocksz = kBlockSz;
        sb.nbackup = 0;
        sb.nblock = 1;
        sb.ndinode = next_ino + inode_count_pad;
        sb.ndblock = next_data_block;
        sb.ndinodeblock = kInodeTableBlocks;
        sb.superroot_ino = superroot_ino;
        std::memcpy(bytes.data(), &sb, sizeof(sb));
    }
};

// Build the standard valid test image:
//   superroot(ino0): ".", "..", "uroot" -> ino1
//   uroot(ino1):     ".", "..", "hello.txt" -> ino2, "subdir" -> ino3
//   subdir(ino3):    ".", "..", "nested.bin" -> ino4
std::vector<u8> BuildValidImage(const std::string& hello_data,
                                const std::vector<u8>& nested_data,
                                u32 free_data_blocks = 0,
                                u32 spare_inodes = 0) {
    PfsBuilder b;
    b.inode_count_pad = spare_inodes;
    const u32 ino_superroot = b.AllocInode(); // 0
    const u32 ino_uroot = b.AllocInode();     // 1
    const u32 ino_hello = b.AddFile(hello_data.data(), hello_data.size()); // 2
    const u32 ino_subdir = b.AllocInode();    // 3
    const u32 ino_nested = b.AddFile(nested_data.data(), nested_data.size()); // 4

    // Write the dir inodes now that child inodes are known (AddDir would
    // allocate fresh inode slots, so use WriteDirInode directly).
    b.WriteDirInode(ino_superroot,
                    {{ino_superroot, Loader::kPfsDirentDot, "."},
                     {ino_superroot, Loader::kPfsDirentDotDot, ".."},
                     {ino_uroot, Loader::kPfsDirentDir, "uroot"}},
                    2);
    b.WriteDirInode(ino_uroot,
                    {{ino_uroot, Loader::kPfsDirentDot, "."},
                     {ino_superroot, Loader::kPfsDirentDotDot, ".."},
                     {ino_hello, Loader::kPfsDirentFile, "hello.txt"},
                     {ino_subdir, Loader::kPfsDirentDir, "subdir"}},
                    3);
    b.WriteDirInode(ino_subdir,
                    {{ino_subdir, Loader::kPfsDirentDot, "."},
                     {ino_uroot, Loader::kPfsDirentDotDot, ".."},
                     {ino_nested, Loader::kPfsDirentFile, "nested.bin"}},
                    2);

    b.superroot_ino = ino_superroot;
    b.AddFreeDataBlocks(free_data_blocks);
    b.Finalize();
    return b.bytes;
}

bool WriteFile(const std::filesystem::path& path, const std::vector<u8>& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
    return out.good();
}

std::vector<u8> ReadWholeFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in.is_open()) {
        return {};
    }
    const auto size = in.tellg();
    std::vector<u8> data(static_cast<size_t>(size));
    in.seekg(0, std::ios::beg);
    in.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

bool MountBytes(const std::filesystem::path& path, const std::vector<u8>& bytes,
                Loader::PfsImage& image, bool writable = false) {
    if (!WriteFile(path, bytes)) {
        std::fprintf(stderr, "[INTERNAL] failed to write temp image\n");
        return false;
    }
    return Loader::MountPfs(path.string(), image, writable);
}

// Offset of the ronly byte inside the superblock (after version, format, id,
// fmode, clean).
constexpr size_t kSbRonlyOffset = 0x1A;

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    LogConfig::SetLevel(LogCategory::Loader, LogLevel::Error);

    std::fprintf(stdout, "=== pfs_tests ===\n");

    const std::filesystem::path tmp =
        std::filesystem::temp_directory_path() / "pcsx5_pfs_tests";
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
    std::filesystem::create_directories(tmp, ec);

    const std::string hello_data = "Hello from a synthetic inner PFS image!";
    std::vector<u8> nested_data(3000);
    for (size_t i = 0; i < nested_data.size(); ++i) {
        nested_data[i] = static_cast<u8>((i * 31 + 7) & 0xFF);
    }

    const std::filesystem::path img_path = tmp / "test.pfs";

    // 1. Mount a valid image and enumerate the tree.
    {
        Loader::PfsImage image;
        EXPECT(MountBytes(img_path, BuildValidImage(hello_data, nested_data), image),
               "MountPfs accepted the valid image");
        EXPECT_EQ(image.Header().blocksz, PfsBuilder::kBlockSz, "block size parsed");
        EXPECT_EQ(image.Header().superroot_ino, 0u, "superroot inode parsed");

        std::vector<Loader::PfsEntry> root_entries;
        EXPECT(Loader::ListDirectory(image, "", root_entries), "ListDirectory root");
        EXPECT_EQ(root_entries.size(), size_t(2), "root has two entries");
        bool saw_hello = false, saw_subdir = false;
        for (const auto& e : root_entries) {
            if (e.name == "hello.txt") {
                saw_hello = true;
                EXPECT(!e.is_directory, "hello.txt is a file");
                EXPECT_EQ(e.size, hello_data.size(), "hello.txt size");
            }
            if (e.name == "subdir") {
                saw_subdir = true;
                EXPECT(e.is_directory, "subdir is a directory");
            }
        }
        EXPECT(saw_hello, "root lists hello.txt");
        EXPECT(saw_subdir, "root lists subdir");

        std::vector<Loader::PfsEntry> sub_entries;
        EXPECT(Loader::ListDirectory(image, "subdir", sub_entries),
               "ListDirectory subdir");
        EXPECT_EQ(sub_entries.size(), size_t(1), "subdir has one entry");
        if (!sub_entries.empty()) {
            EXPECT(sub_entries[0].name == "nested.bin", "subdir entry name");
            EXPECT_EQ(sub_entries[0].size, nested_data.size(), "nested.bin size");
        }

        // Missing paths are rejected.
        std::vector<Loader::PfsEntry> dummy;
        EXPECT(!Loader::ListDirectory(image, "nosuchdir", dummy),
               "ListDirectory of missing path fails");
        EXPECT(!Loader::ListDirectory(image, "hello.txt", dummy),
               "ListDirectory of a file fails");
    }

    // 2. ExtractFile produces byte-exact output.
    {
        Loader::PfsImage image;
        EXPECT(MountBytes(img_path, BuildValidImage(hello_data, nested_data), image),
               "remount for extraction");

        const auto hello_out = tmp / "hello.out";
        EXPECT(Loader::ExtractFile(image, "hello.txt", hello_out.string()),
               "ExtractFile hello.txt");
        EXPECT(ReadWholeFile(hello_out) ==
                   std::vector<u8>(hello_data.begin(), hello_data.end()),
               "hello.txt bytes match");

        const auto nested_out = tmp / "nested.out";
        EXPECT(Loader::ExtractFile(image, "subdir/nested.bin", nested_out.string()),
               "ExtractFile nested.bin");
        EXPECT(ReadWholeFile(nested_out) == nested_data, "nested.bin bytes match");

        EXPECT(!Loader::ExtractFile(image, "nosuchfile.bin", (tmp / "x.out").string()),
               "ExtractFile of missing path fails");
        EXPECT(!Loader::ExtractFile(image, "subdir", (tmp / "y.out").string()),
               "ExtractFile of a directory fails");
    }

    // 3. ExtractAll reproduces the tree.
    {
        Loader::PfsImage image;
        EXPECT(MountBytes(img_path, BuildValidImage(hello_data, nested_data), image),
               "remount for ExtractAll");
        const auto out_dir = tmp / "extracted";
        EXPECT(Loader::ExtractAll(image, out_dir.string()), "ExtractAll succeeds");
        EXPECT(ReadWholeFile(out_dir / "hello.txt") ==
                   std::vector<u8>(hello_data.begin(), hello_data.end()),
               "ExtractAll hello.txt bytes");
        EXPECT(ReadWholeFile(out_dir / "subdir" / "nested.bin") == nested_data,
               "ExtractAll nested.bin bytes");
    }

    // 4. Bad format magic is rejected.
    {
        auto bytes = BuildValidImage(hello_data, nested_data);
        // Patch the format field (offset 0x08) to garbage.
        const u64 bad_magic = 0xDEADBEEF;
        std::memcpy(bytes.data() + 0x08, &bad_magic, sizeof(bad_magic));
        Loader::PfsImage image;
        EXPECT(!MountBytes(img_path, bytes, image), "bad magic rejected");
    }

    // 5. Truncated images are rejected.
    {
        auto bytes = BuildValidImage(hello_data, nested_data);
        Loader::PfsImage image;

        std::vector<u8> tiny(bytes.begin(), bytes.begin() + 0x20);
        EXPECT(!MountBytes(img_path, tiny, image),
               "image smaller than a superblock rejected");

        // Header intact but inode table cut off.
        std::vector<u8> truncated(bytes.begin(), bytes.begin() + 0x1800);
        EXPECT(!MountBytes(img_path, truncated, image),
               "truncated inode table rejected");
    }

    // 6. Encrypted images are detected and skipped.
    {
        PfsBuilder b;
        (void)b.AllocInode();
        b.superroot_ino = 0;
        b.sb_mode = Loader::kPfsSbModeEncrypted;
        b.Finalize();
        Loader::PfsImage image;
        EXPECT(!MountBytes(img_path, b.bytes, image), "encrypted image rejected");
    }

    // 7. Superroot inode index out of range is rejected.
    {
        auto bytes = BuildValidImage(hello_data, nested_data);
        const u64 bad_ino = 0xFFFF;
        std::memcpy(bytes.data() + 0x48, &bad_ino, sizeof(bad_ino)); // superroot_ino
        Loader::PfsImage image;
        EXPECT(!MountBytes(img_path, bytes, image), "bad superroot_ino rejected");
    }

    // 8. A dirent referencing an out-of-range inode fails listing cleanly.
    {
        auto bytes = BuildValidImage(hello_data, nested_data);
        // uroot's dirent block is data block 3 (blocks 0,1 = files' data,
        // 2..4 = superroot/uroot/subdir dirents, allocated in that order).
        // Corrupt the "hello.txt" dirent's ino inside the uroot block.
        const u64 uroot_blk_off = (1 + 1 + 3) * PfsBuilder::kBlockSz;
        bool patched = false;
        for (u64 off = uroot_blk_off; off + sizeof(Loader::PfsDirent) <=
             uroot_blk_off + PfsBuilder::kBlockSz && !patched;) {
            Loader::PfsDirent de{};
            std::memcpy(&de, bytes.data() + off, sizeof(de));
            if (de.type == Loader::kPfsDirentFile) {
                const s32 bad_ino = 0x7FFF;
                std::memcpy(bytes.data() + off, &bad_ino, sizeof(bad_ino));
                patched = true;
                break;
            }
            off += static_cast<u64>(de.entsize);
        }
        EXPECT(patched, "test setup: found uroot file dirent to corrupt");
        Loader::PfsImage image;
        EXPECT(MountBytes(img_path, bytes, image),
               "image with corrupt dirent still mounts");
        std::vector<Loader::PfsEntry> entries;
        EXPECT(!Loader::ListDirectory(image, "", entries),
               "dirent with out-of-range inode rejected");
    }

    // 9. A file inode with an out-of-range data block fails extraction.
    {
        auto bytes = BuildValidImage(hello_data, nested_data);
        // hello.txt is inode 2; corrupt db[0] (offset 0x64 within the inode).
        const u64 inode_off = PfsBuilder::kBlockSz + 2 * sizeof(Loader::PfsInode);
        const s32 bad_block = 0x7FFFFF;
        std::memcpy(bytes.data() + inode_off + 0x64, &bad_block, sizeof(bad_block));
        Loader::PfsImage image;
        EXPECT(MountBytes(img_path, bytes, image),
               "image with corrupt block pointer still mounts");
        EXPECT(!Loader::ExtractFile(image, "hello.txt", (tmp / "z.out").string()),
               "out-of-range data block rejected");
    }

    // 10. A writable mount of an image flagged read-only (ronly) is refused.
    {
        auto bytes = BuildValidImage(hello_data, nested_data);
        Loader::PfsImage image;
        EXPECT(!MountBytes(img_path, bytes, image, true),
               "writable mount of a read-only image fails");
        // Read-only mounting the same image still works.
        EXPECT(MountBytes(img_path, bytes, image), "read-only mount of ronly image works");
        EXPECT(!image.IsWritable(), "read-only mount reports non-writable");
    }

    // 11. Round trip: writable mount -> overwrite (shrink + grow) -> remount
    // read-only -> extract byte-exact.
    {
        auto bytes = BuildValidImage(hello_data, nested_data, /*free_data_blocks=*/4);
        bytes[kSbRonlyOffset] = 0; // clear ronly so writable mount is allowed
        EXPECT(WriteFile(img_path, bytes), "test setup: wrote writable image");

        const std::vector<u8> bye = {'b', 'y', 'e', '!'};
        std::vector<u8> big(9000); // 3 blocks, nested.bin starts with 1
        for (size_t i = 0; i < big.size(); ++i) {
            big[i] = static_cast<u8>((i * 17 + 3) & 0xFF);
        }
        {
            Loader::PfsImage image;
            EXPECT(Loader::MountPfs(img_path.string(), image, true),
                   "writable mount succeeds");
            EXPECT(image.IsWritable(), "writable mount reports writable");
            EXPECT(Loader::WriteFile(image, "hello.txt", bye),
                   "WriteFile overwrite+shrink");
            EXPECT(Loader::WriteFile(image, "subdir/nested.bin", big),
                   "WriteFile growth across block boundaries");
        } // stream closes with the image

        Loader::PfsImage image;
        EXPECT(Loader::MountPfs(img_path.string(), image),
               "remount read-only after writes");
        EXPECT(Loader::ExtractFile(image, "hello.txt", (tmp / "hello.out").string()),
               "extract shrunk hello.txt");
        EXPECT(ReadWholeFile(tmp / "hello.out") == bye, "shrunk hello.txt bytes match");
        EXPECT(Loader::ExtractFile(image, "subdir/nested.bin", (tmp / "nested.out").string()),
               "extract grown nested.bin");
        EXPECT(ReadWholeFile(tmp / "nested.out") == big, "grown nested.bin bytes match");
    }

    // 12. CreateFile + growth into the single-indirect chain + listing.
    {
        // 50000 bytes need 13 data slots -> db[12] + ib[0] with 1 entry,
        // i.e. 13 data blocks + 1 indirect pointer block, plus 1 block for
        // the initially-sized file in subdir. Base image uses 5 blocks.
        auto bytes = BuildValidImage(hello_data, nested_data,
                                     /*free_data_blocks=*/20, /*spare_inodes=*/4);
        bytes[kSbRonlyOffset] = 0;
        EXPECT(WriteFile(img_path, bytes), "test setup: wrote writable image");

        std::vector<u8> save(50000);
        for (size_t i = 0; i < save.size(); ++i) {
            save[i] = static_cast<u8>((i * 7 + i / 251) & 0xFF);
        }
        {
            Loader::PfsImage image;
            EXPECT(Loader::MountPfs(img_path.string(), image, true),
                   "writable mount for create test");
            EXPECT(Loader::CreateFile(image, "savegame.dat", 0), "CreateFile in root");
            EXPECT(Loader::WriteFile(image, "savegame.dat", save),
                   "WriteFile grows into single-indirect blocks");
            EXPECT(Loader::CreateFile(image, "subdir/extra.bin", 0x100),
                   "CreateFile with initial size in subdir");
            EXPECT(!Loader::CreateFile(image, "hello.txt", 0),
                   "CreateFile of an existing name fails");

            std::vector<Loader::PfsEntry> root_entries;
            EXPECT(Loader::ListDirectory(image, "", root_entries),
                   "list root after create");
            EXPECT_EQ(root_entries.size(), size_t(3), "root gained one entry");
            bool saw_save = false;
            for (const auto& e : root_entries) {
                if (e.name == "savegame.dat") {
                    saw_save = true;
                    EXPECT(!e.is_directory, "savegame.dat is a file");
                    EXPECT_EQ(e.size, save.size(), "savegame.dat listed size");
                }
            }
            EXPECT(saw_save, "root lists savegame.dat");

            std::vector<Loader::PfsEntry> sub_entries;
            EXPECT(Loader::ListDirectory(image, "subdir", sub_entries),
                   "list subdir after create");
            EXPECT_EQ(sub_entries.size(), size_t(2), "subdir gained one entry");
        }

        Loader::PfsImage image;
        EXPECT(Loader::MountPfs(img_path.string(), image),
               "remount after create+write");
        EXPECT(Loader::ExtractFile(image, "savegame.dat", (tmp / "save.out").string()),
               "extract created file");
        EXPECT(ReadWholeFile(tmp / "save.out") == save,
               "created file bytes match (incl. indirect blocks)");
        EXPECT(Loader::ExtractFile(image, "subdir/extra.bin", (tmp / "extra.out").string()),
               "extract initially-sized file");
        EXPECT(ReadWholeFile(tmp / "extra.out") == std::vector<u8>(0x100, 0),
               "initially-sized file is zero-filled");
        // The pre-existing files are untouched by the writes.
        EXPECT(Loader::ExtractFile(image, "hello.txt", (tmp / "hello2.out").string()),
               "pre-existing file still extracts");
        EXPECT(ReadWholeFile(tmp / "hello2.out") ==
                   std::vector<u8>(hello_data.begin(), hello_data.end()),
               "pre-existing file bytes unchanged");
    }

    // 13. Write operations on a read-only mount fail.
    {
        auto bytes = BuildValidImage(hello_data, nested_data);
        bytes[kSbRonlyOffset] = 0; // image itself is writable; the mount is not
        Loader::PfsImage image;
        EXPECT(MountBytes(img_path, bytes, image), "read-only mount for negative tests");
        const std::vector<u8> data = {'x'};
        EXPECT(!Loader::WriteFile(image, "hello.txt", data),
               "WriteFile on a read-only mount fails");
        EXPECT(!Loader::CreateFile(image, "new.bin", 0),
               "CreateFile on a read-only mount fails");
    }

    // 14. Writable mount of a corrupt image fails.
    {
        auto bytes = BuildValidImage(hello_data, nested_data);
        bytes[kSbRonlyOffset] = 0;
        const u64 bad_magic = 0xDEADBEEF;
        std::memcpy(bytes.data() + 0x08, &bad_magic, sizeof(bad_magic));
        Loader::PfsImage image;
        EXPECT(!MountBytes(img_path, bytes, image, true),
               "writable mount of a corrupt image fails");
    }

    std::filesystem::remove_all(tmp, ec);

    std::fprintf(stdout, "  %d/%d checks passed\n",
                 g_checks - g_failures, g_checks);
    if (g_failures != 0) {
        std::fprintf(stderr, "  %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::fprintf(stdout, "OK\n");
    return 0;
}
