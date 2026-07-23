#define _CRT_SECURE_NO_WARNINGS
// PS5 PKG parser unit tests — synthetic minimal PS5 PKG header.
#include "loader/pkg_ps5.h"
#include "common/log.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Helper: write a big-endian u32 at a byte offset in a buffer.
static void WrBE32(u8* buf, size_t off, u32 v) {
    buf[off + 0] = static_cast<u8>((v >> 24) & 0xFF);
    buf[off + 1] = static_cast<u8>((v >> 16) & 0xFF);
    buf[off + 2] = static_cast<u8>((v >> 8) & 0xFF);
    buf[off + 3] = static_cast<u8>(v & 0xFF);
}
static void WrBE64(u8* buf, size_t off, u64 v) {
    WrBE32(buf, off,     static_cast<u32>((v >> 32) & 0xFFFFFFFFu));
    WrBE32(buf, off + 4, static_cast<u32>(v & 0xFFFFFFFFu));
}
static void WrStr(u8* buf, size_t off, const char* s, size_t max_len) {
    size_t len = std::strlen(s);
    if (len > max_len) len = max_len;
    std::memcpy(buf + off, s, len);
}

int main() {
    LogConfig::SetLevel(LogCategory::Loader, LogLevel::Info);

    // Build a PS5 PKG header at known offsets (kPs5Off* constants).
    // We use a larger-than-necessary file so entry offsets are valid.
    u8 hdr[0x2000] = {};
    WrBE32(hdr, 0x00, 0x7F464948u);        // magic
    WrBE32(hdr, 0x0C, 2);                   // file_count = 2
    WrBE32(hdr, 0x10, 0x1000);              // entry_table_offset
    WrBE32(hdr, 0x14, 64);                  // entry_table_size
    WrBE64(hdr, 0x18, 0x1040);              // body_offset
    WrBE64(hdr, 0x20, 0x1000);              // body_size (plenty of room)
    WrStr(hdr, 0x30, "UP0001-TEST00000_00-TEST00000000-A00", 36);
    WrBE32(hdr, 0x60, 1);                   // drm_type = fake
    WrBE32(hdr, 0x64, 1);                   // content_type
    // Layout table at 0x400 stays zeroed (no real PFS image).

    const char* test_path = "test_ps5_minimal.pkg";
    FILE* f = fopen(test_path, "wb");
    if (!f) { std::fprintf(stderr, "FAIL: create\n"); return 1; }
    fwrite(hdr, 1, sizeof(hdr), f);  // 0x2000 bytes — covers all offsets

    // Seek to 0x1000 and write the entry table there.
    fseek(f, 0x1000, SEEK_SET);
    // Entry data starts at offset 0x1100 to stay well within the file.
    u8 entries[64] = {};
    // Entry 0: PFS image (type 0x01)
    WrBE32(entries, 0,  1);          // id = 1
    WrBE32(entries, 4,  0x01);       // type = PFS image
    WrBE64(entries, 8,  0x1100);     // offset (within the 0x2000 file)
    WrBE64(entries, 16, 0x20);       // size = 32
    // Entry 1: param.json (type 0x04)
    WrBE32(entries, 32, 2);          // id = 2
    WrBE32(entries, 36, 0x04);       // type = param.json
    WrBE64(entries, 40, 0x1120);     // offset
    WrBE64(entries, 48, 0x10);       // size = 16
    fwrite(entries, 1, sizeof(entries), f);

    // Write PFS image data at offset 0x1100.
    fseek(f, 0x1100, SEEK_SET);
    const char* pfs_dummy = "PFS_IMAGE_DATA";
    fwrite(pfs_dummy, 1, 16, f);

    // Write param.json at offset 0x1120.
    fseek(f, 0x1120, SEEK_SET);
    const char* param = R"({"titleId":"TEST"})";
    fwrite(param, 1, std::strlen(param) + 1, f);
    fclose(f);

    // --- Parse and verify ---
    Loader::PkgPs5Image image;
    if (!Loader::ParsePkgPs5(test_path, image)) {
        std::fprintf(stderr, "FAIL: ParsePkgPs5 returned false\n");
        std::remove(test_path);
        return 1;
    }

    bool ok = true;
    if (image.content_id != "UP0001-TEST00000_00-TEST00000000-A00") {
        std::fprintf(stderr, "FAIL: content_id='%s'\n", image.content_id.c_str());
        ok = false;
    }
    if (image.drm_type != 1) {
        std::fprintf(stderr, "FAIL: drm_type=%u\n", image.drm_type);
        ok = false;
    }
    if (image.entries.size() != 2) {
        std::fprintf(stderr, "FAIL: %zu entries\n", image.entries.size());
        ok = false;
    } else {
        if (image.entries[0].TypeId() != 0x01) {
            std::fprintf(stderr, "FAIL: entry[0] type=0x%X\n", image.entries[0].TypeId());
            ok = false;
        }
        if (image.entries[1].TypeId() != 0x04) {
            std::fprintf(stderr, "FAIL: entry[1] type=0x%X\n", image.entries[1].TypeId());
            ok = false;
        }
        if (image.entries[0].size != 0x20) {
            std::fprintf(stderr, "FAIL: entry[0] size=0x%llx\n",
                         static_cast<unsigned long long>(image.entries[0].size));
            ok = false;
        }
    }
    if (image.IsRetail()) {
        std::fprintf(stderr, "FAIL: drm_type=1 should not be retail\n");
        ok = false;
    }

    std::remove(test_path);
    if (ok) { std::fprintf(stdout, "PASS: PS5 PKG parser test\n"); return 0; }
    return 1;
}
