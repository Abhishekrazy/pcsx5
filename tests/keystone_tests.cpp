// Keystone header parser tests.
//
// Verifies HLE::ParseKeystoneHeader against synthetic blobs:
//   - a well-formed blob parses with all fields extracted
//   - bad magic is rejected
//   - truncated buffers (below header size, and below the header's claimed
//     file size) are rejected
//   - unsupported version is rejected
//   - out-of-range data_offset is rejected
//
// Self-contained: links against src/hle/libkeystone.cpp (parser section only;
// the HLE registrations resolve against hle.cpp / memory.cpp like the other
// HLE-linked tests).

#include "hle/keystone.h"
#include "hle/hle.h"
#include "memory/memory.h"
#include "common/types.h"

#include <cstdio>
#include <cstring>
#include <vector>

// Minimal link stubs: libkeystone.cpp's HLE registration block references
// HLE::RegisterSymbol and Memory::ReadBuffer, but the header parser under
// test never calls them, so no-op definitions keep this test self-contained
// (no full hle.cpp / memory.cpp link required).
namespace HLE {
void RegisterSymbol(const std::string& /*module_name*/,
                    const std::string& /*name*/, HleHandler /*handler*/) {}
} // namespace HLE

namespace Memory {
void ReadBuffer(guest_addr_t /*addr*/, void* /*dest*/, u64 /*size*/) {}
} // namespace Memory

namespace {

int g_failures = 0;
int g_checks   = 0;

#define EXPECT(cond, msg) do {                                     \
    ++g_checks;                                                    \
    if (!(cond)) {                                                 \
        ++g_failures;                                              \
        std::fprintf(stderr, "FAIL [%s:%d] %s\n",                  \
                     __FILE__, __LINE__, (msg));                   \
    }                                                              \
} while (0)

#define EXPECT_EQ(a, b, msg) EXPECT((a) == (b), msg)

void WriteLe32(u8* p, u32 v) { std::memcpy(p, &v, sizeof(v)); }
void WriteLe64(u8* p, u64 v) { std::memcpy(p, &v, sizeof(v)); }

// Builds a well-formed keystone blob of `total` bytes with the given header
// fields and a recognizable digest pattern.
std::vector<u8> BuildBlob(u32 version, u32 type, u64 file_size,
                          u32 data_offset, size_t total) {
    std::vector<u8> blob(total, 0xAB);
    std::memcpy(blob.data(), "keystone", 8);
    WriteLe32(blob.data() + 0x08, version);
    WriteLe32(blob.data() + 0x0C, type);
    WriteLe64(blob.data() + 0x10, file_size);
    WriteLe32(blob.data() + 0x18, data_offset);
    for (size_t i = 0; i < 0x20; ++i)
        blob[0x20 + i] = static_cast<u8>(i);
    return blob;
}

} // namespace

int main() {
    using HLE::KeystoneError;
    using HLE::ParseKeystoneHeader;

    // 1. Valid blob: all header fields extracted.
    {
        auto blob = BuildBlob(0x01000000, 2, 0x80, 0x60, 0x80);
        HLE::KeystoneHeader header;
        EXPECT_EQ(ParseKeystoneHeader(blob.data(), blob.size(), &header),
                  KeystoneError::kOk, "valid blob parses");
        EXPECT_EQ(header.version, 0x01000000u, "version extracted");
        EXPECT_EQ(header.type, 2u, "type extracted");
        EXPECT_EQ(header.file_size, 0x80u, "file_size extracted");
        EXPECT_EQ(header.data_offset, 0x60u, "data_offset extracted");
        bool digest_ok = true;
        for (size_t i = 0; i < 0x20; ++i)
            digest_ok = digest_ok && header.data_digest[i] == static_cast<u8>(i);
        EXPECT(digest_ok, "data_digest extracted");
    }

    // 2. Buffer smaller than the public header is rejected.
    {
        auto blob = BuildBlob(0x01000000, 2, 0x60, 0x60, 0x20);
        EXPECT_EQ(ParseKeystoneHeader(blob.data(), blob.size(), nullptr),
                  KeystoneError::kBadArgs, "truncated buffer rejected");
        EXPECT_EQ(ParseKeystoneHeader(nullptr, 0x60, nullptr),
                  KeystoneError::kBadArgs, "null buffer rejected");
    }

    // 3. Bad magic is rejected.
    {
        auto blob = BuildBlob(0x01000000, 2, 0x60, 0x60, 0x60);
        blob[0] = 'X';
        EXPECT_EQ(ParseKeystoneHeader(blob.data(), blob.size(), nullptr),
                  KeystoneError::kBadMagic, "bad magic rejected");
    }

    // 4. Unsupported version is rejected.
    {
        auto blob = BuildBlob(0x02000000, 2, 0x60, 0x60, 0x60);
        EXPECT_EQ(ParseKeystoneHeader(blob.data(), blob.size(), nullptr),
                  KeystoneError::kUnsupportedVersion,
                  "unsupported version rejected");
    }

    // 5. Header claims a file size larger than the supplied buffer.
    {
        auto blob = BuildBlob(0x01000000, 2, 0x100, 0x60, 0x80);
        EXPECT_EQ(ParseKeystoneHeader(blob.data(), blob.size(), nullptr),
                  KeystoneError::kFileSizeMismatch,
                  "claimed size beyond buffer rejected");
    }

    // 6. data_offset outside [header size, file_size] is rejected.
    {
        auto blob = BuildBlob(0x01000000, 2, 0x60, 0x20, 0x60);
        EXPECT_EQ(ParseKeystoneHeader(blob.data(), blob.size(), nullptr),
                  KeystoneError::kBadDataOffset,
                  "data_offset inside header rejected");
    }

    // 7. Error names are stable tokens for log lines.
    EXPECT(std::strcmp(HLE::KeystoneErrorName(KeystoneError::kBadMagic),
                       "bad_magic") == 0, "error name: bad_magic");
    EXPECT(std::strcmp(HLE::KeystoneErrorName(KeystoneError::kUnsupportedVersion),
                       "unsupported_version") == 0, "error name: unsupported_version");

    std::fprintf(stdout, "  %d/%d checks passed\n",
                 g_checks - g_failures, g_checks);
    if (g_failures != 0) {
        std::fprintf(stderr, "  %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::fprintf(stdout, "OK\n");
    return 0;
}
