#pragma once

// Keystone header parser — shared between the HLE validator (libkeystone.cpp),
// the per-title boot-time loader (main.cpp), and tests/keystone_tests.cpp.
//
// Public keystone blob layout (see psdevwiki "Keystone"; the blob is the
// per-title anti-tamper ticket stored as ".keystone" inside the PFS image):
//
//   offset 0x00  char[8]   magic "keystone"
//   offset 0x08  u32le     version (expected 0x01000000, i.e. 1.00)
//   offset 0x0C  u32le     type
//   offset 0x10  u64le     file_size   total blob size claimed by the header
//   offset 0x18  u32le     data_offset offset of the ticket data (typically 0x60)
//   offset 0x1C  u32le     reserved
//   offset 0x20  u8[0x20]  data_digest SHA-256 of the ticket data
//   offset 0x40  u8[0x20]  reserved / per-console signature material
//   (0x60 bytes total; data_offset bytes beyond this point are not parsed)

#include "../common/types.h"

#include <cstddef>

namespace HLE {

constexpr u32 KEYSTONE_SUPPORTED_VERSION = 0x01000000;
constexpr size_t KEYSTONE_HEADER_SIZE = 0x60;

struct KeystoneHeader {
    u32 version = 0;
    u32 type = 0;
    u64 file_size = 0;      // total blob size claimed by the header
    u32 data_offset = 0;    // offset of the ticket data within the blob
    u8  data_digest[0x20] = {};
};

enum class KeystoneError {
    kOk = 0,
    kBadArgs,              // null data pointer or buffer smaller than the header
    kBadMagic,             // first 8 bytes are not "keystone"
    kUnsupportedVersion,   // version != KEYSTONE_SUPPORTED_VERSION
    kFileSizeMismatch,     // header file_size exceeds the supplied buffer
    kBadDataOffset,        // data_offset outside [header size, file_size]
};

// Parses the public keystone header from `data` (a host buffer of `size`
// bytes) into `out`.  Returns KeystoneError::kOk on success; every failure
// path reports the specific reason via the return value so callers can log
// KEYSTONE_INVALID reason=<name>.
KeystoneError ParseKeystoneHeader(const u8* data, size_t size, KeystoneHeader* out);

// Stable lowercase token for log lines ("bad_magic", "truncated", ...).
const char* KeystoneErrorName(KeystoneError err);

} // namespace HLE
