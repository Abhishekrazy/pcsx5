// Crypto primitives needed for PKG / PFS handling.
//
// AES-128 (block, ECB, CBC) per FIPS-197 and SHA-256 per FIPS 180-4,
// written from scratch as a compact, self-contained implementation
// (no external dependencies).  Verified against published NIST test
// vectors in tests/crypto_tests.cpp.

#include "crypto.h"

#include <cstring>

namespace Common {
namespace {

// ---------------------------------------------------------------------------
// AES-128 (FIPS-197)
// ---------------------------------------------------------------------------

// AES S-box.
constexpr u8 kSbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};

// AES inverse S-box.
constexpr u8 kInvSbox[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d,
};

// Round constants for the key schedule.
constexpr u8 kRcon[11] = {
    0x00,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36,
};

// Multiply by x (i.e. by {02}) in GF(2^8).
constexpr u8 Xtime(u8 x) {
    return static_cast<u8>((x << 1) ^ ((x >> 7) * 0x1b));
}

// Multiply two bytes in GF(2^8) (Russian peasant multiplication).
constexpr u8 Gmul(u8 a, u8 b) {
    u8 r = 0;
    while (b != 0) {
        if (b & 1) r ^= a;
        a = Xtime(a);
        b >>= 1;
    }
    return r;
}

using AesState = u8[4][4];  // column-major, as in FIPS-197

void StateFromBlock(AesState s, const u8* block) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            s[r][c] = block[c * 4 + r];
}

void BlockFromState(u8* block, const AesState s) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            block[c * 4 + r] = s[r][c];
}

void AddRoundKey(AesState s, const u8* rk) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            s[r][c] ^= rk[c * 4 + r];
}

void SubBytes(AesState s) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            s[r][c] = kSbox[s[r][c]];
}

void InvSubBytes(AesState s) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            s[r][c] = kInvSbox[s[r][c]];
}

void ShiftRows(AesState s) {
    u8 t;
    // Row 1: rotate left by 1.
    t = s[1][0];
    s[1][0] = s[1][1]; s[1][1] = s[1][2]; s[1][2] = s[1][3]; s[1][3] = t;
    // Row 2: rotate left by 2.
    t = s[2][0];
    s[2][0] = s[2][2]; s[2][2] = t;
    t = s[2][1];
    s[2][1] = s[2][3]; s[2][3] = t;
    // Row 3: rotate left by 3 (== right by 1).
    t = s[3][3];
    s[3][3] = s[3][2]; s[3][2] = s[3][1]; s[3][1] = s[3][0]; s[3][0] = t;
}

void InvShiftRows(AesState s) {
    u8 t;
    // Row 1: rotate right by 1.
    t = s[1][3];
    s[1][3] = s[1][2]; s[1][2] = s[1][1]; s[1][1] = s[1][0]; s[1][0] = t;
    // Row 2: rotate right by 2.
    t = s[2][0];
    s[2][0] = s[2][2]; s[2][2] = t;
    t = s[2][1];
    s[2][1] = s[2][3]; s[2][3] = t;
    // Row 3: rotate right by 3 (== left by 1).
    t = s[3][0];
    s[3][0] = s[3][1]; s[3][1] = s[3][2]; s[3][2] = s[3][3]; s[3][3] = t;
}

void MixColumns(AesState s) {
    for (int c = 0; c < 4; ++c) {
        const u8 a0 = s[0][c], a1 = s[1][c], a2 = s[2][c], a3 = s[3][c];
        s[0][c] = static_cast<u8>(Xtime(a0) ^ (Xtime(a1) ^ a1) ^ a2 ^ a3);
        s[1][c] = static_cast<u8>(a0 ^ Xtime(a1) ^ (Xtime(a2) ^ a2) ^ a3);
        s[2][c] = static_cast<u8>(a0 ^ a1 ^ Xtime(a2) ^ (Xtime(a3) ^ a3));
        s[3][c] = static_cast<u8>((Xtime(a0) ^ a0) ^ a1 ^ a2 ^ Xtime(a3));
    }
}

void InvMixColumns(AesState s) {
    for (int c = 0; c < 4; ++c) {
        const u8 a0 = s[0][c], a1 = s[1][c], a2 = s[2][c], a3 = s[3][c];
        s[0][c] = static_cast<u8>(Gmul(a0,14) ^ Gmul(a1,11) ^ Gmul(a2,13) ^ Gmul(a3,9));
        s[1][c] = static_cast<u8>(Gmul(a0,9)  ^ Gmul(a1,14) ^ Gmul(a2,11) ^ Gmul(a3,13));
        s[2][c] = static_cast<u8>(Gmul(a0,13) ^ Gmul(a1,9)  ^ Gmul(a2,14) ^ Gmul(a3,11));
        s[3][c] = static_cast<u8>(Gmul(a0,11) ^ Gmul(a1,13) ^ Gmul(a2,9)  ^ Gmul(a3,14));
    }
}

// ---------------------------------------------------------------------------
// SHA-256 (FIPS 180-4)
// ---------------------------------------------------------------------------

constexpr u32 kSha256K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
};

constexpr u32 Rotr(u32 x, u32 n) {
    return (x >> n) | (x << (32 - n));
}

void Sha256Compress(u32 state[8], const u8 block[64]) {
    u32 w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<u32>(block[i * 4 + 0]) << 24) |
               (static_cast<u32>(block[i * 4 + 1]) << 16) |
               (static_cast<u32>(block[i * 4 + 2]) <<  8) |
               (static_cast<u32>(block[i * 4 + 3]) <<  0);
    }
    for (int i = 16; i < 64; ++i) {
        const u32 s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const u32 s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    u32 a = state[0], b = state[1], c = state[2], d = state[3];
    u32 e = state[4], f = state[5], g = state[6], h = state[7];

    for (int i = 0; i < 64; ++i) {
        const u32 s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
        const u32 ch = (e & f) ^ (~e & g);
        const u32 t1 = h + s1 + ch + kSha256K[i] + w[i];
        const u32 s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
        const u32 maj = (a & b) ^ (a & c) ^ (b & c);
        const u32 t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

}  // namespace

// ---------------------------------------------------------------------------
// AES-128 public API
// ---------------------------------------------------------------------------

Aes128Key Aes128ExpandKey(const u8 key[kAes128KeySize]) noexcept {
    Aes128Key out;
    std::memcpy(out.round_keys.data(), key, kAes128KeySize);

    u32 bytes = kAes128KeySize;  // bytes generated so far
    u32 rcon_iter = 1;
    u8 temp[4];

    while (bytes < kAes128RoundKeySize) {
        for (int i = 0; i < 4; ++i)
            temp[i] = out.round_keys[bytes - 4 + i];

        if (bytes % kAes128KeySize == 0) {
            // RotWord + SubWord + Rcon.
            const u8 t = temp[0];
            temp[0] = static_cast<u8>(kSbox[temp[1]] ^ kRcon[rcon_iter++]);
            temp[1] = kSbox[temp[2]];
            temp[2] = kSbox[temp[3]];
            temp[3] = kSbox[t];
        }
        for (int i = 0; i < 4; ++i) {
            out.round_keys[bytes] = static_cast<u8>(out.round_keys[bytes - kAes128KeySize] ^ temp[i]);
            ++bytes;
        }
    }
    return out;
}

void Aes128EncryptBlock(const Aes128Key& key,
                        const u8 in[kAes128BlockSize],
                        u8 out[kAes128BlockSize]) noexcept {
    AesState s;
    StateFromBlock(s, in);

    AddRoundKey(s, key.round_keys.data());
    for (int round = 1; round <= 9; ++round) {
        SubBytes(s);
        ShiftRows(s);
        MixColumns(s);
        AddRoundKey(s, key.round_keys.data() + round * 16);
    }
    SubBytes(s);
    ShiftRows(s);
    AddRoundKey(s, key.round_keys.data() + 10 * 16);

    BlockFromState(out, s);
}

void Aes128DecryptBlock(const Aes128Key& key,
                        const u8 in[kAes128BlockSize],
                        u8 out[kAes128BlockSize]) noexcept {
    AesState s;
    StateFromBlock(s, in);

    AddRoundKey(s, key.round_keys.data() + 10 * 16);
    for (int round = 9; round >= 1; --round) {
        InvShiftRows(s);
        InvSubBytes(s);
        AddRoundKey(s, key.round_keys.data() + round * 16);
        InvMixColumns(s);
    }
    InvShiftRows(s);
    InvSubBytes(s);
    AddRoundKey(s, key.round_keys.data());

    BlockFromState(out, s);
}

bool Aes128EcbEncrypt(const Aes128Key& key,
                      const u8* in, u8* out, size_t size) noexcept {
    if (size % kAes128BlockSize != 0)
        return false;
    for (size_t off = 0; off < size; off += kAes128BlockSize)
        Aes128EncryptBlock(key, in + off, out + off);
    return true;
}

bool Aes128EcbDecrypt(const Aes128Key& key,
                      const u8* in, u8* out, size_t size) noexcept {
    if (size % kAes128BlockSize != 0)
        return false;
    for (size_t off = 0; off < size; off += kAes128BlockSize)
        Aes128DecryptBlock(key, in + off, out + off);
    return true;
}

bool Aes128CbcEncrypt(const Aes128Key& key, const u8 iv[kAes128BlockSize],
                      const u8* in, u8* out, size_t size) noexcept {
    if (size % kAes128BlockSize != 0)
        return false;
    u8 chain[kAes128BlockSize];
    std::memcpy(chain, iv, kAes128BlockSize);
    for (size_t off = 0; off < size; off += kAes128BlockSize) {
        u8 block[kAes128BlockSize];
        for (size_t i = 0; i < kAes128BlockSize; ++i)
            block[i] = static_cast<u8>(in[off + i] ^ chain[i]);
        Aes128EncryptBlock(key, block, out + off);
        std::memcpy(chain, out + off, kAes128BlockSize);
    }
    return true;
}

bool Aes128CbcDecrypt(const Aes128Key& key, const u8 iv[kAes128BlockSize],
                      const u8* in, u8* out, size_t size) noexcept {
    if (size % kAes128BlockSize != 0)
        return false;
    u8 chain[kAes128BlockSize];
    std::memcpy(chain, iv, kAes128BlockSize);
    for (size_t off = 0; off < size; off += kAes128BlockSize) {
        // Capture the ciphertext block first: in-place operation (in == out)
        // would overwrite it before the chaining-value update below.
        u8 next_chain[kAes128BlockSize];
        std::memcpy(next_chain, in + off, kAes128BlockSize);
        u8 block[kAes128BlockSize];
        Aes128DecryptBlock(key, next_chain, block);
        for (size_t i = 0; i < kAes128BlockSize; ++i)
            out[off + i] = static_cast<u8>(block[i] ^ chain[i]);
        std::memcpy(chain, next_chain, kAes128BlockSize);
    }
    return true;
}

// ---------------------------------------------------------------------------
// SHA-256 public API
// ---------------------------------------------------------------------------

void Sha256Context::Init() noexcept {
    state_ = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
              0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    total_len_ = 0;
    block_len_ = 0;
}

void Sha256Context::Update(const u8* data, size_t size) noexcept {
    total_len_ += size;
    while (size > 0) {
        const size_t take = (size < 64 - block_len_) ? size : (64 - block_len_);
        std::memcpy(block_.data() + block_len_, data, take);
        block_len_ += take;
        data += take;
        size -= take;
        if (block_len_ == 64) {
            Sha256Compress(state_.data(), block_.data());
            block_len_ = 0;
        }
    }
}

Sha256Digest Sha256Context::Final() noexcept {
    const u64 bit_len = total_len_ * 8;

    // Append 0x80 then zeros until 56 bytes mod 64, then the length.
    u8 pad = 0x80;
    Update(&pad, 1);
    pad = 0x00;
    while (block_len_ != 56)
        Update(&pad, 1);

    u8 len_bytes[8];
    for (int i = 0; i < 8; ++i)
        len_bytes[i] = static_cast<u8>(bit_len >> (56 - i * 8));
    Update(len_bytes, 8);

    Sha256Digest digest;
    for (int i = 0; i < 8; ++i) {
        digest[i * 4 + 0] = static_cast<u8>(state_[i] >> 24);
        digest[i * 4 + 1] = static_cast<u8>(state_[i] >> 16);
        digest[i * 4 + 2] = static_cast<u8>(state_[i] >>  8);
        digest[i * 4 + 3] = static_cast<u8>(state_[i] >>  0);
    }
    return digest;
}

Sha256Digest Sha256(const u8* data, size_t size) noexcept {
    Sha256Context ctx;
    ctx.Init();
    ctx.Update(data, size);
    return ctx.Final();
}

}  // namespace Common
