// libSceAppContent HLE — fake content availability stubs.
//
// PS5 games call these during boot to check add-on content availability.
// We return "no add-on content" (empty lists, zero flags) with success codes
// so the game proceeds past its DLC init and into its main loop.
#include "hle.h"
#include "../common/log.h"
#include "../memory/memory.h"
#include <cstring>

namespace HLE {

namespace {
constexpr u64 ORBIS_APP_CONTENT_ERROR_PARAMETER = 0x80D90002;
constexpr u64 ORBIS_OK = 0;
} // namespace

void RegisterLibAppContent() {
    LOG_INFO(HLE, "Registering libSceAppContent HLE symbols...");

    // sceAppContentInitParam — must be called before any other AppContent function.
    // We accept and ignore the parameter struct.
    auto InitParamImpl = [](const GuestArgs& args) -> u64 {
        LOG_DEBUG(HLE, "sceAppContentInitParam(param: 0x%llx) -> 0", args.arg1);
        return ORBIS_OK;
    };
    RegisterSymbol("libSceAppContent", "sceAppContentInitParam",             InitParamImpl);
    RegisterSymbol("libSceAppContent", "sceAppContentInitParam#T#T",         InitParamImpl);
    // Also seen imported as libSceAppContentUtil
    RegisterSymbol("libSceAppContentUtil", "sceAppContentInitParam",         InitParamImpl);
    RegisterSymbol("libSceAppContentUtil", "sceAppContentInitParam#T#T",     InitParamImpl);

    // sceAppContentGetAddcontInfoList — returns empty add-on list.
    // Prototype: int sceAppContentGetAddcontInfoList(
    //   SceAppContentAddcontInfo* list, int num, int* hitNum)
    auto GetAddcontInfoListImpl = [](const GuestArgs& args) -> u64 {
        const guest_addr_t hit_num_ptr = args.arg3;
        if (hit_num_ptr) {
            Memory::Write<s32>(hit_num_ptr, 0); // zero add-ons found
        }
        LOG_DEBUG(HLE, "sceAppContentGetAddcontInfoList() -> 0 items");
        return ORBIS_OK;
    };
    RegisterSymbol("libSceAppContent",     "sceAppContentGetAddcontInfoList",     GetAddcontInfoListImpl);
    RegisterSymbol("libSceAppContent",     "sceAppContentGetAddcontInfoList#T#T", GetAddcontInfoListImpl);
    RegisterSymbol("libSceAppContentUtil", "sceAppContentGetAddcontInfoList",     GetAddcontInfoListImpl);

    // sceAppContentGetEntitlementKey — fills 32 zero bytes (no entitlement).
    auto GetEntitlementKeyImpl = [](const GuestArgs& args) -> u64 {
        const guest_addr_t key_ptr = args.arg2;
        if (key_ptr) {
            for (int i = 0; i < 32; ++i) {
                Memory::Write<u8>(key_ptr + i, 0);
            }
        }
        LOG_DEBUG(HLE, "sceAppContentGetEntitlementKey(0x%llx) -> zeroed 32-byte key", key_ptr);
        return ORBIS_OK;
    };
    RegisterSymbol("libSceAppContent",     "sceAppContentGetEntitlementKey",     GetEntitlementKeyImpl);
    RegisterSymbol("libSceAppContent",     "sceAppContentGetEntitlementKey#T#T", GetEntitlementKeyImpl);

    // sceAppContentSmallSize — return 1 to indicate content is present.
    // Games use this to skip "no content" error branches.
    auto SmallSizeImpl = [](const GuestArgs& /*args*/) -> u64 {
        LOG_DEBUG(HLE, "sceAppContentSmallSize() -> 1");
        return 1;
    };
    RegisterSymbol("libSceAppContent",     "sceAppContentSmallSize",     SmallSizeImpl);
    RegisterSymbol("libSceAppContent",     "sceAppContentSmallSize#T#T", SmallSizeImpl);

    // sceAppContentGetPftFlag — no PlayStation Fluid Transitions flag.
    auto GetPftFlagImpl = [](const GuestArgs& args) -> u64 {
        const guest_addr_t flag_ptr = args.arg1;
        if (flag_ptr) Memory::Write<s32>(flag_ptr, 0);
        LOG_DEBUG(HLE, "sceAppContentGetPftFlag(0x%llx) -> flag=0", flag_ptr);
        return ORBIS_OK;
    };
    RegisterSymbol("libSceAppContent",     "sceAppContentGetPftFlag",     GetPftFlagImpl);
    RegisterSymbol("libSceAppContent",     "sceAppContentGetPftFlag#T#T", GetPftFlagImpl);

    // sceAppContentAddcontMount — fake-mount add-on content (no-op, return 0).
    auto AddcontMountImpl = [](const GuestArgs& args) -> u64 {
        LOG_DEBUG(HLE, "sceAppContentAddcontMount(slot: %llu, dir: 0x%llx) -> 0", args.arg1, args.arg2);
        return ORBIS_OK;
    };
    RegisterSymbol("libSceAppContent",     "sceAppContentAddcontMount",     AddcontMountImpl);
    RegisterSymbol("libSceAppContent",     "sceAppContentAddcontMount#T#T", AddcontMountImpl);

    // sceAppContentRequestPatchInstallation — not needed offline, return 0.
    RegisterSymbol("libSceAppContent", "sceAppContentRequestPatchInstallation",
                   [](const GuestArgs& /*args*/) -> u64 { return ORBIS_OK; });
    RegisterSymbol("libSceAppContent", "sceAppContentRequestPatchInstallation#T#T",
                   [](const GuestArgs& /*args*/) -> u64 { return ORBIS_OK; });

    // sceAppContentTemporaryDataMount2 — temporary data directory mount.
    RegisterSymbol("libSceAppContent", "sceAppContentTemporaryDataMount2",
                   [](const GuestArgs& args) -> u64 {
                       LOG_DEBUG(HLE, "sceAppContentTemporaryDataMount2(mode: %llu) -> 0", args.arg1);
                       return ORBIS_OK;
                   });
    RegisterSymbol("libSceAppContent", "sceAppContentTemporaryDataMount2#T#T",
                   [](const GuestArgs& /*args*/) -> u64 {
                       return ORBIS_OK;
                   });

    // sceAppContentDownloadDataMount — download data directory mount.
    RegisterSymbol("libSceAppContent", "sceAppContentDownloadDataMount",
                   [](const GuestArgs& /*args*/) -> u64 {
                       LOG_DEBUG(HLE, "sceAppContentDownloadDataMount() -> 0");
                       return ORBIS_OK;
                   });
    RegisterSymbol("libSceAppContent", "sceAppContentDownloadDataMount#T#T",
                   [](const GuestArgs&) -> u64 { return ORBIS_OK; });
}

} // namespace HLE
