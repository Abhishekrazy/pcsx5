#pragma once
//
// Crypto primitives needed for PKG / PFS handling.
//
// AES-128 (block, ECB, CBC) and SHA-256, implemented from scratch in
// crypto.cpp (self-contained, no external dependencies).  The AES core
// is the classic FIPS-197 table-based cipher; SHA-256 follows FIPS
// 180-4.  Both are verified against published NIST test vectors in
// tests/crypto_tests.cpp.
//
#include "types.h"

#include <array>
#include <cstddef>

namespace Common {

// ---------------------------------------------------------------------------
// AES-128
// ---------------------------------------------------------------------------

constexpr size_t kAes128BlockSize = 16;  // bytes
constexpr size_t kAes128KeySize   = 16;  // bytes
constexpr size_t kAes128RoundKeySize = 176;  // 11 round keys * 16 bytes

// Expanded key schedule.  Construct once, reuse for many blocks.
struct Aes128Key {
    std::array<u8, kAes128RoundKeySize> round_keys{};
};

// Expand a 16-byte key into the AES-128 key schedule.
Aes128Key Aes128ExpandKey(const u8 key[kAes128KeySize]) noexcept;

// Encrypt/decrypt a single 16-byte block.
void Aes128EncryptBlock(const Aes128Key& key,
                        const u8 in[kAes128BlockSize],
                        u8 out[kAes128BlockSize]) noexcept;
void Aes128DecryptBlock(const Aes128Key& key,
                        const u8 in[kAes128BlockSize],
                        u8 out[kAes128BlockSize]) noexcept;

// ECB mode over a whole buffer.  `size` must be a multiple of 16;
// returns false (and touches nothing) otherwise.
bool Aes128EcbEncrypt(const Aes128Key& key,
                      const u8* in, u8* out, size_t size) noexcept;
bool Aes128EcbDecrypt(const Aes128Key& key,
                      const u8* in, u8* out, size_t size) noexcept;

// CBC mode over a whole buffer.  `size` must be a multiple of 16;
// returns false (and touches nothing) otherwise.  `iv` is the 16-byte
// initialisation vector; it is not modified.
bool Aes128CbcEncrypt(const Aes128Key& key, const u8 iv[kAes128BlockSize],
                      const u8* in, u8* out, size_t size) noexcept;
bool Aes128CbcDecrypt(const Aes128Key& key, const u8 iv[kAes128BlockSize],
                      const u8* in, u8* out, size_t size) noexcept;

// XTS mode (IEEE 1619) over a whole data unit.  `key1` encrypts the data,
// `key2` encrypts the tweak.  `data_unit` is the 16-byte little-endian
// data-unit number (e.g. a sector index); it is not modified.  `size` must
// be a multiple of 16; returns false (and touches nothing) otherwise.
// No ciphertext stealing: partial trailing blocks are not supported.
bool Aes128XtsEncrypt(const Aes128Key& key1, const Aes128Key& key2,
                      const u8 data_unit[kAes128BlockSize],
                      const u8* in, u8* out, size_t size) noexcept;
bool Aes128XtsDecrypt(const Aes128Key& key1, const Aes128Key& key2,
                      const u8 data_unit[kAes128BlockSize],
                      const u8* in, u8* out, size_t size) noexcept;

// ---------------------------------------------------------------------------
// SHA-256 (FIPS 180-4)
// ---------------------------------------------------------------------------

constexpr size_t kSha256DigestSize = 32;  // bytes

using Sha256Digest = std::array<u8, kSha256DigestSize>;

// One-shot hash of a byte buffer.
Sha256Digest Sha256(const u8* data, size_t size) noexcept;

// Incremental hashing: Init, feed with Update any number of times,
// then Final to obtain the digest.  The context must not be reused
// after Final without another Init.
class Sha256Context {
public:
    void Init() noexcept;
    void Update(const u8* data, size_t size) noexcept;
    Sha256Digest Final() noexcept;

private:
    std::array<u32, 8> state_{};
    std::array<u8, 64> block_{};
    u64 total_len_ = 0;      // total bytes fed so far
    size_t block_len_ = 0;   // bytes currently buffered in block_
};

}  // namespace Common
