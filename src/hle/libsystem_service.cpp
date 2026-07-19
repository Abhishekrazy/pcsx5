// libSceSystemService HLE — canned offline parameter set (SharpEmu design).
//
// Games query a handful of system parameters during boot (safe-area, HDCP,
// HDR, suspend state, ...).  We answer with plausible retail values so the
// boot sequence proceeds: everything "off/normal", 180-minute idle timer,
// full display safe area, splash screen hidden.
#include "hle.h"
#include "../common/log.h"
#include "../memory/memory.h"

namespace HLE {

namespace {
constexpr u64 ORBIS_SYSTEM_SERVICE_ERROR_PARAMETER = 0x80A10003;
}

void RegisterLibSystemService() {
    LOG_INFO(HLE, "Registering libSceSystemService HLE symbols...");

    // sceSystemServiceParamGetInt (fZo48un7LK4)
    // Canned map (SharpEmu): {1,2,3,1000 -> 1; 4 -> 180; else 0}.
    auto ParamGetIntImpl = [](const GuestArgs& args) -> u64 {
        const s32 param_id = static_cast<s32>(args.arg1);
        const guest_addr_t value_ptr = args.arg2;
        if (!value_ptr) return ORBIS_SYSTEM_SERVICE_ERROR_PARAMETER;

        s32 value = 0;
        switch (param_id) {
            case 1:    // HDCP
            case 2:
            case 3:
            case 1000:
                value = 1;
                break;
            case 4:    // idle/suspend timer (minutes)
                value = 180;
                break;
            default:
                value = 0;
                break;
        }
        Memory::Write<s32>(value_ptr, value);
        LOG_DEBUG(HLE, "sceSystemServiceParamGetInt(id: %d) -> %d", param_id, value);
        return 0;
    };
    RegisterSymbol("libSceSystemService", "sceSystemServiceParamGetInt", ParamGetIntImpl);
    RegisterSymbol("libSceSystemService", "fZo48un7LK4#T#T", ParamGetIntImpl);

    // sceSystemServiceGetStatus (rPo6tV8D9bM)
    // 0x0C-byte status struct, all zero except byte@6 = 1 ("event service up").
    auto GetStatusImpl = [](const GuestArgs& args) -> u64 {
        const guest_addr_t status_ptr = args.arg1;
        if (!status_ptr) return ORBIS_SYSTEM_SERVICE_ERROR_PARAMETER;
        for (u64 i = 0; i < 0x0C; ++i) {
            Memory::Write<u8>(status_ptr + i, 0);
        }
        Memory::Write<u8>(status_ptr + 0x06, 1);
        LOG_DEBUG(HLE, "sceSystemServiceGetStatus(0x%llx) -> 0", status_ptr);
        return 0;
    };
    RegisterSymbol("libSceSystemService", "sceSystemServiceGetStatus", GetStatusImpl);
    RegisterSymbol("libSceSystemService", "rPo6tV8D9bM#T#T", GetStatusImpl);
    // libkernel-scoped NID import variant — overwrites the legacy stub.
    RegisterSymbol("libkernel", "rPo6tV8D9bM#Q#R", GetStatusImpl);

    // sceSystemServiceGetDisplaySafeAreaInfo (1n37q1Bvc5Y)
    // float ratio (1.0 = full area) followed by 128 zero bytes.
    auto SafeAreaImpl = [](const GuestArgs& args) -> u64 {
        const guest_addr_t info_ptr = args.arg1;
        if (!info_ptr) return ORBIS_SYSTEM_SERVICE_ERROR_PARAMETER;
        Memory::Write<float>(info_ptr, 1.0f);
        for (u64 i = 0; i < 128; ++i) {
            Memory::Write<u8>(info_ptr + sizeof(float) + i, 0);
        }
        LOG_DEBUG(HLE, "sceSystemServiceGetDisplaySafeAreaInfo(0x%llx) -> ratio 1.0", info_ptr);
        return 0;
    };
    RegisterSymbol("libSceSystemService", "sceSystemServiceGetDisplaySafeAreaInfo", SafeAreaImpl);
    RegisterSymbol("libSceSystemService", "1n37q1Bvc5Y#T#T", SafeAreaImpl);

    // sceSystemServiceHideSplashScreen (Vo5V8KAwCmk) — log only; our presenter
    // shows the guest framebuffer as soon as flips arrive.
    auto HideSplashImpl = [](const GuestArgs& /*args*/) -> u64 {
        LOG_INFO(HLE, "sceSystemServiceHideSplashScreen() -> 0");
        return 0;
    };
    RegisterSymbol("libSceSystemService", "sceSystemServiceHideSplashScreen", HideSplashImpl);
    RegisterSymbol("libSceSystemService", "Vo5V8KAwCmk#T#T", HideSplashImpl);
    // libkernel-scoped NID import variant — overwrites the legacy stub.
    RegisterSymbol("libkernel", "Vo5V8KAwCmk#Q#R", HideSplashImpl);

    // sceSystemServiceReportAbnormalTermination (3s8cHiCBKBE) — accept, no-op.
    RegisterSymbol("libSceSystemService", "sceSystemServiceReportAbnormalTermination",
                   [](const GuestArgs& args) -> u64 {
                       LOG_WARN(HLE, "sceSystemServiceReportAbnormalTermination(type: %llu)", args.arg1);
                       return 0;
                   });
    RegisterSymbol("libSceSystemService", "3s8cHiCBKBE#T#T",
                   [](const GuestArgs& args) -> u64 {
                       LOG_WARN(HLE, "sceSystemServiceReportAbnormalTermination(type: %llu)", args.arg1);
                       return 0;
                   });
}

} // namespace HLE
