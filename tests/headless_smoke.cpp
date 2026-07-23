#define _CRT_SECURE_NO_WARNINGS
// Headless smoke test — pure CPU, no GPU required.
#include "loader/pkg_ps5.h"
#include "common/log.h"
#include <cstdio>
#include <cstring>
#include <vector>

static void Wr32(u8* p, size_t o, u32 v) {
    p[o+0]=(u8)(v>>24); p[o+1]=(u8)(v>>16); p[o+2]=(u8)(v>>8); p[o+3]=(u8)v;
}
static void Wr64(u8* p, size_t o, u64 v) {
    Wr32(p, o, (u32)(v>>32)); Wr32(p, o+4, (u32)v);
}

int main() {
    LogConfig::SetLevel(LogCategory::Loader, LogLevel::Info);

    // Build synthetic PS5 PKG in a file.
    std::vector<u8> hdr(0x2000, 0);
    Wr32(hdr.data(), 0x00, 0x7F464948u);  // magic
    Wr32(hdr.data(), 0x0C, 1);             // file_count = 1
    Wr32(hdr.data(), 0x10, 0x1000);        // entry_table_offset
    Wr32(hdr.data(), 0x14, 32);            // entry_table_size
    // single entry at offset 0x1000
    Wr32(&hdr[0x1000], 0, 1);              // id = 1
    Wr32(&hdr[0x1000], 4, 1);              // type = PFS
    Wr64(&hdr[0x1000], 8, 0x1100);         // offset
    Wr64(&hdr[0x1000], 16, 0x20);          // size

    const char* path = "headless_test.pkg";
    FILE* f = fopen(path, "wb");
    if (!f) { std::fprintf(stderr, "FAIL: create\n"); return 1; }
    fwrite(hdr.data(), 1, hdr.size(), f);
    fclose(f);

    Loader::PkgPs5Image image;
    if (!Loader::ParsePkgPs5(path, image)) {
        std::fprintf(stderr, "FAIL: ParsePkgPs5\n");
        std::remove(path); return 1;
    }
    std::remove(path);

    if (image.entries.size() != 1) {
        std::fprintf(stderr, "FAIL: %zu entries (expected 1)\n", image.entries.size());
        return 1;
    }
    std::fprintf(stdout, "PASS: headless PS5 PKG (%zu entry, content_id='%s')\n",
                 image.entries.size(), image.content_id.c_str());
    return 0;
}
