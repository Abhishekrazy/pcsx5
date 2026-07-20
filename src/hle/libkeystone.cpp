// Keystone HLE — header validation + per-title state (ROADMAP Phase 7).
//
// A keystone blob is the per-title anti-tamper ticket found inside the PFS
// image (".keystone"); on real hardware the kernel parses and verifies it.
// Here we parse and validate the public header layout (see keystone.h for
// the field table) far enough to log sane information and to reject clearly
// malformed input.  Cryptographic verification of the ticket data is out of
// scope — the console keys are not available.
//
// No keystone NIDs exist in the local nid_db or SharpEmu, so the handlers
// are name-only registrations under libkernel.
#include "hle.h"
#include "keystone.h"
#include "../common/log.h"
#include "../memory/memory.h"
#include <cstring>
#include <utility>
#include <vector>

namespace HLE {

namespace {
constexpr u64 ORBIS_GEN2_ERROR_INVALID_ARGUMENT = 0x80020016;
constexpr char KEYSTONE_MAGIC[8] = {'k', 'e', 'y', 's', 't', 'o', 'n', 'e'};

// Per-title keystone loaded from <app0>/.keystone at boot (main.cpp).
std::vector<u8> g_keystone_blob;

u32 ReadLe32(const u8* p) {
    u32 v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

u64 ReadLe64(const u8* p) {
    u64 v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

// (blob, size) -> 0 for a well-formed keystone header, error otherwise.
u64 KeystoneValidateImpl(const GuestArgs& args) {
    const guest_addr_t blob = args.arg1;
    const u64 size = args.arg2;
    if (!blob || size < KEYSTONE_HEADER_SIZE) {
        LOG_WARN(HLE, "KEYSTONE_INVALID reason=bad_args blob=0x%llx size=%llu", blob, size);
        return ORBIS_GEN2_ERROR_INVALID_ARGUMENT;
    }
    std::vector<u8> buf(static_cast<size_t>(size));
    Memory::ReadBuffer(blob, buf.data(), buf.size());
    KeystoneHeader header;
    const KeystoneError err = ParseKeystoneHeader(buf.data(), buf.size(), &header);
    if (err != KeystoneError::kOk) {
        LOG_WARN(HLE, "KEYSTONE_INVALID reason=%s blob=0x%llx size=%llu",
                 KeystoneErrorName(err), blob, size);
        return ORBIS_GEN2_ERROR_INVALID_ARGUMENT;
    }
    LOG_INFO(HLE, "KEYSTONE_OK version=%u.%02u type=%u size=%llu",
             (header.version >> 24) & 0xFF, (header.version >> 16) & 0xFF,
             header.type, size);
    return 0;
}
} // namespace

KeystoneError ParseKeystoneHeader(const u8* data, size_t size, KeystoneHeader* out) {
    if (!data || size < KEYSTONE_HEADER_SIZE) {
        return KeystoneError::kBadArgs;
    }
    if (std::memcmp(data, KEYSTONE_MAGIC, sizeof(KEYSTONE_MAGIC)) != 0) {
        return KeystoneError::kBadMagic;
    }
    KeystoneHeader header;
    header.version = ReadLe32(data + 0x08);
    if (header.version != KEYSTONE_SUPPORTED_VERSION) {
        return KeystoneError::kUnsupportedVersion;
    }
    header.type = ReadLe32(data + 0x0C);
    header.file_size = ReadLe64(data + 0x10);
    header.data_offset = ReadLe32(data + 0x18);
    std::memcpy(header.data_digest, data + 0x20, sizeof(header.data_digest));
    // The header's claimed file size must fit inside the supplied buffer.
    if (header.file_size < KEYSTONE_HEADER_SIZE || header.file_size > size) {
        return KeystoneError::kFileSizeMismatch;
    }
    // The ticket data must start after the public header and inside the blob.
    if (header.data_offset < KEYSTONE_HEADER_SIZE ||
        header.data_offset > header.file_size) {
        return KeystoneError::kBadDataOffset;
    }
    if (out) {
        *out = header;
    }
    return KeystoneError::kOk;
}

const char* KeystoneErrorName(KeystoneError err) {
    switch (err) {
    case KeystoneError::kOk:                 return "ok";
    case KeystoneError::kBadArgs:            return "bad_args";
    case KeystoneError::kBadMagic:           return "bad_magic";
    case KeystoneError::kUnsupportedVersion: return "unsupported_version";
    case KeystoneError::kFileSizeMismatch:   return "size_mismatch";
    case KeystoneError::kBadDataOffset:      return "bad_data_offset";
    }
    return "unknown";
}

void SetKeystoneBlob(std::vector<u8> blob) {
    g_keystone_blob = std::move(blob);
}

const std::vector<u8>& GetKeystoneBlob() {
    return g_keystone_blob;
}

void RegisterLibKeystone() {
    LOG_INFO(HLE, "Registering keystone HLE symbols...");
    RegisterSymbol("libkernel", "sceKernelGetKeystone", KeystoneValidateImpl);
    RegisterSymbol("libkernel", "sceKernelKeystoneValidate", KeystoneValidateImpl);
}

} // namespace HLE
