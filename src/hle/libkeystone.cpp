// Keystone HLE — header validation stubs (ROADMAP Phase 7).
//
// A keystone blob is the per-title anti-tamper ticket found inside the PFS
// image (".keystone"); on real hardware the kernel parses and verifies it.
// Here we only validate the public header layout far enough to log sane
// information and to reject clearly malformed input:
//
//   offset 0x00  char[8]  magic "keystone"
//   offset 0x08  u32      version (expected 0x01000000)
//   offset 0x0C  u32      type
//
// No keystone NIDs exist in the local nid_db or SharpEmu, so the handlers
// are name-only registrations under libkernel.
#include "hle.h"
#include "../common/log.h"
#include "../memory/memory.h"
#include <cstring>

namespace HLE {

namespace {
constexpr u64 ORBIS_GEN2_ERROR_INVALID_ARGUMENT = 0x80020016;
constexpr u64 KEYSTONE_MIN_SIZE = 0x20;
constexpr char KEYSTONE_MAGIC[8] = {'k', 'e', 'y', 's', 't', 'o', 'n', 'e'};

// (blob, size) -> 0 for a well-formed keystone header, error otherwise.
u64 KeystoneValidateImpl(const GuestArgs& args) {
    const guest_addr_t blob = args.arg1;
    const u64 size = args.arg2;
    if (!blob || size < KEYSTONE_MIN_SIZE) {
        LOG_WARN(HLE, "KEYSTONE_INVALID reason=bad_args blob=0x%llx size=%llu", blob, size);
        return ORBIS_GEN2_ERROR_INVALID_ARGUMENT;
    }
    u8 header[KEYSTONE_MIN_SIZE];
    Memory::ReadBuffer(blob, header, sizeof(header));
    if (std::memcmp(header, KEYSTONE_MAGIC, sizeof(KEYSTONE_MAGIC)) != 0) {
        LOG_WARN(HLE, "KEYSTONE_INVALID reason=bad_magic blob=0x%llx size=%llu", blob, size);
        return ORBIS_GEN2_ERROR_INVALID_ARGUMENT;
    }
    u32 version = 0, type = 0;
    std::memcpy(&version, header + 0x08, sizeof(version));
    std::memcpy(&type, header + 0x0C, sizeof(type));
    LOG_INFO(HLE, "KEYSTONE_OK version=%u.%02u type=%u size=%llu",
             (version >> 24) & 0xFF, (version >> 16) & 0xFF, type, size);
    return 0;
}
} // namespace

void RegisterLibKeystone() {
    LOG_INFO(HLE, "Registering keystone HLE symbols...");
    RegisterSymbol("libkernel", "sceKernelGetKeystone", KeystoneValidateImpl);
    RegisterSymbol("libkernel", "sceKernelKeystoneValidate", KeystoneValidateImpl);
}

} // namespace HLE
