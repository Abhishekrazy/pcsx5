// Crypto primitive tests (AES-128, SHA-256).
//
// Verifies against published known-answer vectors:
//   - FIPS-197 Appendix B / C.1 AES-128 single block.
//   - NIST SP800-38A F.2.1/F.2.2 AES-128-CBC (4 blocks, encrypt + decrypt).
//   - NIST SP800-38A F.1.1/F.1.2 AES-128-ECB (4 blocks, encrypt + decrypt).
//   - FIPS 180-4 SHA-256: empty string, "abc", 56-byte and 112-byte
//     multi-block messages.
//
// Self-contained: no Memory / Loader / HLE dependencies.

#include "common/crypto.h"
#include "common/types.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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

// Parse a hex string ("001122...") into bytes.
std::vector<u8> HexToBytes(const char* hex) {
    auto nibble = [](char c) -> u8 {
        if (c >= '0' && c <= '9') return static_cast<u8>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<u8>(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return static_cast<u8>(c - 'A' + 10);
        return 0;
    };
    std::vector<u8> out;
    const size_t len = std::strlen(hex);
    for (size_t i = 0; i + 1 < len; i += 2)
        out.push_back(static_cast<u8>((nibble(hex[i]) << 4) | nibble(hex[i + 1])));
    return out;
}

std::string BytesToHex(const u8* data, size_t size) {
    static const char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        out.push_back(kDigits[data[i] >> 4]);
        out.push_back(kDigits[data[i] & 0xf]);
    }
    return out;
}

void ExpectBytesEq(const u8* got, const u8* want, size_t size, const char* what) {
    ++g_checks;
    if (std::memcmp(got, want, size) != 0) {
        ++g_failures;
        std::fprintf(stderr, "FAIL [%s:%d] %s\n  got:  %s\n  want: %s\n",
                     __FILE__, __LINE__, what,
                     BytesToHex(got, size).c_str(),
                     BytesToHex(want, size).c_str());
    }
}

// ---------------------------------------------------------------------------
// AES-128
// ---------------------------------------------------------------------------

void TestAes128Block() {
    // FIPS-197 Appendix C.1 known-answer.
    const auto key_bytes = HexToBytes("000102030405060708090a0b0c0d0e0f");
    const auto pt        = HexToBytes("00112233445566778899aabbccddeeff");
    const auto ct        = HexToBytes("69c4e0d86a7b0430d8cdb78070b4c55a");

    auto key = Common::Aes128ExpandKey(key_bytes.data());

    u8 buf[16];
    Common::Aes128EncryptBlock(key, pt.data(), buf);
    ExpectBytesEq(buf, ct.data(), 16, "FIPS-197 AES-128 encrypt block");

    Common::Aes128DecryptBlock(key, ct.data(), buf);
    ExpectBytesEq(buf, pt.data(), 16, "FIPS-197 AES-128 decrypt block");
}

void TestAes128Ecb() {
    // NIST SP800-38A F.1.1 / F.1.2 (ECB-AES128, 4 blocks).
    const auto key_bytes = HexToBytes("2b7e151628aed2a6abf7158809cf4f3c");
    const auto pt = HexToBytes(
        "6bc1bee22e409f96e93d7e117393172a"
        "ae2d8a571e03ac9c9eb76fac45af8e51"
        "30c81c46a35ce411e5fbc1191a0a52ef"
        "f69f2445df4f9b17ad2b417be66c3710");
    const auto ct = HexToBytes(
        "3ad77bb40d7a3660a89ecaf32466ef97"
        "f5d3d58503b9699de785895a96fdbaaf"
        "43b1cd7f598ece23881b00e3ed030688"
        "7b0c785e27e8ad3f8223207104725dd4");

    auto key = Common::Aes128ExpandKey(key_bytes.data());

    std::vector<u8> buf(pt.size());
    EXPECT(Common::Aes128EcbEncrypt(key, pt.data(), buf.data(), pt.size()),
           "ECB encrypt returned false on valid size");
    ExpectBytesEq(buf.data(), ct.data(), ct.size(), "SP800-38A ECB encrypt");

    EXPECT(Common::Aes128EcbDecrypt(key, ct.data(), buf.data(), ct.size()),
           "ECB decrypt returned false on valid size");
    ExpectBytesEq(buf.data(), pt.data(), pt.size(), "SP800-38A ECB decrypt");

    // Non-multiple-of-16 size must be rejected.
    EXPECT(!Common::Aes128EcbEncrypt(key, pt.data(), buf.data(), pt.size() - 1),
           "ECB encrypt accepted non-block-multiple size");
    EXPECT(!Common::Aes128EcbDecrypt(key, pt.data(), buf.data(), pt.size() - 1),
           "ECB decrypt accepted non-block-multiple size");
}

void TestAes128Cbc() {
    // NIST SP800-38A F.2.1 / F.2.2 (CBC-AES128, 4 blocks).
    const auto key_bytes = HexToBytes("2b7e151628aed2a6abf7158809cf4f3c");
    const auto iv        = HexToBytes("000102030405060708090a0b0c0d0e0f");
    const auto pt = HexToBytes(
        "6bc1bee22e409f96e93d7e117393172a"
        "ae2d8a571e03ac9c9eb76fac45af8e51"
        "30c81c46a35ce411e5fbc1191a0a52ef"
        "f69f2445df4f9b17ad2b417be66c3710");
    const auto ct = HexToBytes(
        "7649abac8119b246cee98e9b12e9197d"
        "5086cb9b507219ee95db113a917678b2"
        "73bed6b8e3c1743b7116e69e22229516"
        "3ff1caa1681fac09120eca307586e1a7");

    auto key = Common::Aes128ExpandKey(key_bytes.data());

    std::vector<u8> buf(pt.size());
    EXPECT(Common::Aes128CbcEncrypt(key, iv.data(), pt.data(), buf.data(), pt.size()),
           "CBC encrypt returned false on valid size");
    ExpectBytesEq(buf.data(), ct.data(), ct.size(), "SP800-38A CBC encrypt");

    EXPECT(Common::Aes128CbcDecrypt(key, iv.data(), ct.data(), buf.data(), ct.size()),
           "CBC decrypt returned false on valid size");
    ExpectBytesEq(buf.data(), pt.data(), pt.size(), "SP800-38A CBC decrypt");

    // Non-multiple-of-16 size must be rejected.
    EXPECT(!Common::Aes128CbcEncrypt(key, iv.data(), pt.data(), buf.data(), pt.size() - 1),
           "CBC encrypt accepted non-block-multiple size");
    EXPECT(!Common::Aes128CbcDecrypt(key, iv.data(), pt.data(), buf.data(), pt.size() - 1),
           "CBC decrypt accepted non-block-multiple size");

    // In-place operation (in == out) must round-trip: the chaining value
    // update must not read ciphertext already overwritten by plaintext.
    std::vector<u8> inplace = ct;
    EXPECT(Common::Aes128CbcDecrypt(key, iv.data(), inplace.data(), inplace.data(), inplace.size()),
           "CBC in-place decrypt returned false");
    ExpectBytesEq(inplace.data(), pt.data(), pt.size(), "CBC in-place decrypt");
}

// ---------------------------------------------------------------------------
// SHA-256
// ---------------------------------------------------------------------------

void ExpectSha256(const u8* data, size_t size, const char* want_hex, const char* what) {
    const auto digest = Common::Sha256(data, size);
    const auto want   = HexToBytes(want_hex);
    ExpectBytesEq(digest.data(), want.data(), Common::kSha256DigestSize, what);
}

void ExpectSha256String(const char* s, const char* want_hex, const char* what) {
    ExpectSha256(reinterpret_cast<const u8*>(s), std::strlen(s), want_hex, what);
}

void TestSha256() {
    // FIPS 180-4 / RFC 4634-style known answers.
    ExpectSha256String("",
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "SHA-256 of empty string");
    ExpectSha256String("abc",
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "SHA-256 of \"abc\"");
    // 56 bytes: exercises the two-block padding boundary.
    ExpectSha256String("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
        "SHA-256 of 56-byte message");
    // 112 bytes: multi-block.
    ExpectSha256String(
        "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
        "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
        "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1",
        "SHA-256 of 112-byte message");

    // Incremental hashing must match the one-shot result, fed in odd
    // chunk sizes so buffering across block boundaries is exercised.
    {
        const char* msg =
            "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
            "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
        const size_t msg_len = std::strlen(msg);

        Common::Sha256Context ctx;
        ctx.Init();
        size_t off = 0;
        const size_t kChunk = 7;  // deliberately not a divisor of 64
        while (off < msg_len) {
            size_t n = msg_len - off;
            if (n > kChunk) n = kChunk;
            ctx.Update(reinterpret_cast<const u8*>(msg) + off, n);
            off += n;
        }
        const auto digest = ctx.Final();
        const auto one_shot =
            Common::Sha256(reinterpret_cast<const u8*>(msg), msg_len);
        EXPECT(digest == one_shot,
               "incremental SHA-256 matches one-shot");
    }

    // One million 'a' characters (FIPS 180-4 long-message vector).
    {
        Common::Sha256Context ctx;
        ctx.Init();
        std::vector<u8> chunk(1000, 'a');
        for (int i = 0; i < 1000; ++i)
            ctx.Update(chunk.data(), chunk.size());
        const auto digest = ctx.Final();
        const auto want = HexToBytes(
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
        ExpectBytesEq(digest.data(), want.data(), Common::kSha256DigestSize,
                      "SHA-256 of 1,000,000 x 'a'");
    }
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    std::fprintf(stdout, "=== crypto_tests ===\n");

    TestAes128Block();
    TestAes128Ecb();
    TestAes128Cbc();
    TestSha256();

    std::fprintf(stdout, "  %d/%d checks passed\n",
                 g_checks - g_failures, g_checks);
    if (g_failures != 0) {
        std::fprintf(stderr, "  %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::fprintf(stdout, "OK\n");
    return 0;
}
