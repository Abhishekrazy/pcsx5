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

    // sceAppContentInitialize
    auto InitializeImpl = [](const GuestArgs& /*args*/) -> u64 {
        LOG_DEBUG(HLE, "sceAppContentInitialize() -> 0");
        return ORBIS_OK; // ORBIS_OK
    };
    RegisterSymbol("libSceAppContent",     "sceAppContentInitialize",     InitializeImpl);
    RegisterSymbol("libSceAppContent",     "sceAppContentInitialize#T#T", InitializeImpl);


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

    // sceAppContentAppParamGetInt
    auto AppContentAppParamGetInt = [](const GuestArgs& args) -> u64 {
        const guest_addr_t out_ptr = args.arg2;
        if (out_ptr) {
            Memory::Write<s32>(out_ptr, 0); // Give 0 as default parameter
        }
        LOG_INFO(HLE, "sceAppContentAppParamGetInt(id: %llu, out: 0x%llx) -> 0", args.arg1, out_ptr);
        return 0;
    };
    RegisterSymbol("libSceAppContent", "sceAppContentAppParamGetInt", AppContentAppParamGetInt);
    
    // libSceIme 
    auto KeyboardOpen = [](const GuestArgs& args) -> u64 {
        LOG_INFO(HLE, "sceImeKeyboardOpen(uid: 0x%llx, cb: 0x%llx) -> 0", args.arg1, args.arg2);
        return 0; 
    };
    RegisterSymbol("libSceIme", "sceImeKeyboardOpen", KeyboardOpen);
    RegisterSymbol("libSceIme", "eaFXjfJv3xs", KeyboardOpen);

    // libSceMouse
    auto MouseInit = [](const GuestArgs&) -> u64 {
        LOG_INFO(HLE, "sceMouseInit() -> 0");
        return 0;
    };
    RegisterSymbol("libSceMouse", "sceMouseInit", MouseInit);
    RegisterSymbol("libSceMouse", "Qs0wWulgl7U", MouseInit);

    auto MouseOpen = [](const GuestArgs& args) -> u64 {
        LOG_INFO(HLE, "sceMouseOpen(userId: %d, type: %d, index: %d) -> 1 (handle)", args.arg1, args.arg2, args.arg3);
        return 1; // Return a valid looking handle (e.g., 1)
    };
    RegisterSymbol("libSceMouse", "sceMouseOpen", MouseOpen);
    RegisterSymbol("libSceMouse", "RaqxZIf6DvE", MouseOpen);

    // libSceShareUtility
    auto ShareInitialize = [](const GuestArgs&) -> u64 {
        LOG_INFO(HLE, "sceShareInitialize() -> 0");
        return 0;
    };
    RegisterSymbol("libSceShareUtility", "sceShareInitialize", ShareInitialize);
    RegisterSymbol("libSceShareUtility", "nBDD66kiFW8", ShareInitialize);

    // libScePlayGo
    auto PlayGoInitialize = [](const GuestArgs&) -> u64 {
        LOG_INFO(HLE, "scePlayGoInitialize() -> 0");
        return 0;
    };
    RegisterSymbol("libScePlayGo", "scePlayGoInitialize", PlayGoInitialize);
    RegisterSymbol("libScePlayGo", "ts6GlZOKRrE", PlayGoInitialize);

    auto PlayGoOpen = [](const GuestArgs& args) -> u64 {
        if (args.arg1) Memory::Write<s32>(args.arg1, 1); // handle?
        LOG_INFO(HLE, "scePlayGoOpen(out: 0x%llx) -> 0", args.arg1);
        return 0;
    };
    RegisterSymbol("libScePlayGo", "scePlayGoOpen", PlayGoOpen);
    RegisterSymbol("libScePlayGo", "M1Gma1ocrGE", PlayGoOpen);

    auto PlayGoSetInstallSpeed = [](const GuestArgs&) -> u64 {
        LOG_INFO(HLE, "scePlayGoSetInstallSpeed() -> 0");
        return 0;
    };
    RegisterSymbol("libScePlayGo", "scePlayGoSetInstallSpeed", PlayGoSetInstallSpeed);
    RegisterSymbol("libScePlayGo", "4AAcTU9R3XM", PlayGoSetInstallSpeed);

    auto PlayGoGetChunkId = [](const GuestArgs& args) -> u64 {
        if (args.arg2) Memory::Write<u32>(args.arg2, 0); // RSI
        if (args.arg4) Memory::Write<u32>(args.arg4, 0); // RCX
        LOG_INFO(HLE, "scePlayGoGetChunkId() -> 0");
        return 0;
    };
    RegisterSymbol("libScePlayGo", "scePlayGoGetChunkId", PlayGoGetChunkId);
    RegisterSymbol("libScePlayGo", "73fF1MFU8hA", PlayGoGetChunkId);

    auto PlayGoGetToDoList = [](const GuestArgs& args) -> u64 {
        if (args.arg2) Memory::Write<u32>(args.arg2, 0); // RSI
        if (args.arg4) Memory::Write<u32>(args.arg4, 0); // RCX
        LOG_INFO(HLE, "scePlayGoGetToDoList() -> 0");
        return 0;
    };
    RegisterSymbol("libScePlayGo", "scePlayGoGetToDoList", PlayGoGetToDoList);
    RegisterSymbol("libScePlayGo", "Nn7zKwnA5q0", PlayGoGetToDoList);

    auto PlayGoClose = [](const GuestArgs&) -> u64 {
        LOG_INFO(HLE, "scePlayGoClose() -> 0");
        return 0;
    };
    RegisterSymbol("libScePlayGo", "scePlayGoClose", PlayGoClose);
    RegisterSymbol("libScePlayGo", "Uco1I0dlDi8", PlayGoClose);

    auto PlayGoTerminate = [](const GuestArgs&) -> u64 {
        LOG_INFO(HLE, "scePlayGoTerminate() -> 0");
        return 0;
    };
    RegisterSymbol("libScePlayGo", "scePlayGoTerminate", PlayGoTerminate);
    RegisterSymbol("libScePlayGo", "MPe0EeBGM-E", PlayGoTerminate);
}

} // namespace HLE
