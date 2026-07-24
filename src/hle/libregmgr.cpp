// libSceRegMgr HLE — registry parameter stubs (offline, read-only).
//
// PS5 games call these to read system registry values (display settings,
// parental controls, network preferences, etc.).  We return sensible
// defaults (0 for integers, empty strings, zeroed binary) so the game
// proceeds without network or hardware access.
#include "hle.h"
#include "../common/log.h"
#include "../memory/memory.h"
#include <cstring>

namespace HLE {

namespace {
constexpr u64 ORBIS_OK                        = 0;
constexpr u64 ORBIS_REGMGR_ERROR_NOT_FOUND    = 0x80900002;
constexpr u64 ORBIS_REGMGR_ERROR_INVALID_ARG  = 0x80900005;
} // namespace

void RegisterLibRegMgr() {
    LOG_INFO(HLE, "Registering libSceRegMgr HLE symbols...");

    // sceRegMgrGetInt — read an integer registry value (return 0).
    auto GetIntImpl = [](const GuestArgs& args) -> u64 {
        const guest_addr_t key_ptr = args.arg1;
        const guest_addr_t val_ptr = args.arg2;
        if (!val_ptr) return ORBIS_REGMGR_ERROR_INVALID_ARG;
        Memory::Write<s32>(val_ptr, 0);
        LOG_DEBUG(HLE, "sceRegMgrGetInt(key: 0x%llx) -> value=0", key_ptr);
        return ORBIS_OK;
    };
    RegisterSymbol("libSceRegMgr", "sceRegMgrGetInt",     GetIntImpl);
    RegisterSymbol("libSceRegMgr", "sceRegMgrGetInt#T#T", GetIntImpl);
    // NID variant
    RegisterSymbol("libSceRegMgr", "AQH+QyZSIUE#T#T", GetIntImpl);

    // sceRegMgrSetInt — write an integer registry value (no-op, return 0).
    auto SetIntImpl = [](const GuestArgs& args) -> u64 {
        LOG_DEBUG(HLE, "sceRegMgrSetInt(key: 0x%llx, val: %d) -> 0 (no-op)",
                  args.arg1, static_cast<s32>(args.arg2));
        return ORBIS_OK;
    };
    RegisterSymbol("libSceRegMgr", "sceRegMgrSetInt",     SetIntImpl);
    RegisterSymbol("libSceRegMgr", "sceRegMgrSetInt#T#T", SetIntImpl);

    // sceRegMgrGetStr — read a string registry value (return empty string).
    // Prototype: int sceRegMgrGetStr(const char* key, char* buf, size_t len)
    auto GetStrImpl = [](const GuestArgs& args) -> u64 {
        const guest_addr_t key_ptr = args.arg1;
        const guest_addr_t buf_ptr = args.arg2;
        const u64          buf_len = args.arg3;
        if (!buf_ptr || buf_len == 0) return ORBIS_REGMGR_ERROR_INVALID_ARG;
        Memory::Write<u8>(buf_ptr, 0); // empty string
        LOG_DEBUG(HLE, "sceRegMgrGetStr(key: 0x%llx, buf: 0x%llx, len: %llu) -> \"\"",
                  key_ptr, buf_ptr, buf_len);
        return ORBIS_OK;
    };
    RegisterSymbol("libSceRegMgr", "sceRegMgrGetStr",     GetStrImpl);
    RegisterSymbol("libSceRegMgr", "sceRegMgrGetStr#T#T", GetStrImpl);

    // sceRegMgrSetStr — no-op write.
    auto SetStrImpl = [](const GuestArgs& args) -> u64 {
        LOG_DEBUG(HLE, "sceRegMgrSetStr(key: 0x%llx) -> 0 (no-op)", args.arg1);
        return ORBIS_OK;
    };
    RegisterSymbol("libSceRegMgr", "sceRegMgrSetStr",     SetStrImpl);
    RegisterSymbol("libSceRegMgr", "sceRegMgrSetStr#T#T", SetStrImpl);

    // sceRegMgrGetBin — read binary registry value (zeroed buffer).
    // Prototype: int sceRegMgrGetBin(const char* key, void* buf, size_t len)
    auto GetBinImpl = [](const GuestArgs& args) -> u64 {
        const guest_addr_t key_ptr = args.arg1;
        const guest_addr_t buf_ptr = args.arg2;
        const u64          buf_len = args.arg3;
        if (!buf_ptr || buf_len == 0) return ORBIS_REGMGR_ERROR_INVALID_ARG;
        for (u64 i = 0; i < buf_len; ++i) Memory::Write<u8>(buf_ptr + i, 0);
        LOG_DEBUG(HLE, "sceRegMgrGetBin(key: 0x%llx, len: %llu) -> zeroed", key_ptr, buf_len);
        return ORBIS_OK;
    };
    RegisterSymbol("libSceRegMgr", "sceRegMgrGetBin",     GetBinImpl);
    RegisterSymbol("libSceRegMgr", "sceRegMgrGetBin#T#T", GetBinImpl);

    // sceRegMgrSetBin — no-op write.
    auto SetBinImpl = [](const GuestArgs& args) -> u64 {
        LOG_DEBUG(HLE, "sceRegMgrSetBin(key: 0x%llx, len: %llu) -> 0 (no-op)", args.arg1, args.arg3);
        return ORBIS_OK;
    };
    RegisterSymbol("libSceRegMgr", "sceRegMgrSetBin",     SetBinImpl);
    RegisterSymbol("libSceRegMgr", "sceRegMgrSetBin#T#T", SetBinImpl);

    // sceRegMgrGetInitVals — return empty/zeroed init values.
    RegisterSymbol("libSceRegMgr", "sceRegMgrGetInitVals",
                   [](const GuestArgs& args) -> u64 {
                       LOG_DEBUG(HLE, "sceRegMgrGetInitVals(0x%llx) -> 0", args.arg1);
                       return ORBIS_OK;
                   });
    RegisterSymbol("libSceRegMgr", "sceRegMgrGetInitVals#T#T",
                   [](const GuestArgs&) -> u64 { return ORBIS_OK; });

    // sceRegMgrNvmFlush / sceRegMgrNvmSync — no hardware; return 0.
    RegisterSymbol("libSceRegMgr", "sceRegMgrNvmFlush",
                   [](const GuestArgs&) -> u64 { return ORBIS_OK; });
    RegisterSymbol("libSceRegMgr", "sceRegMgrNvmFlush#T#T",
                   [](const GuestArgs&) -> u64 { return ORBIS_OK; });
}

} // namespace HLE
