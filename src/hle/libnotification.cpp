// libSceNotification + libSceNpNotification HLE — notification stubs.
//
// PS5 games pop toast notifications for trophy unlocks, friend activity,
// downloads, etc.  We accept these calls, log the message if readable,
// and return 0 so the game doesn't wait on a notification service that
// doesn't exist.
#include "hle.h"
#include "../common/log.h"
#include "../memory/memory.h"
#include <cstring>

namespace HLE {

namespace {
constexpr u64 ORBIS_OK                          = 0;
constexpr u64 ORBIS_NOTIFICATION_ERROR_PARAM    = 0x80B50001;
} // namespace

void RegisterLibNotification() {
    LOG_INFO(HLE, "Registering libSceNotification / libSceNpNotification HLE symbols...");

    // sceNotificationUtilInitialize — boot-time init; return 0.
    auto InitImpl = [](const GuestArgs& args) -> u64 {
        LOG_DEBUG(HLE, "sceNotificationUtilInitialize(0x%llx) -> 0", args.arg1);
        return ORBIS_OK;
    };
    RegisterSymbol("libSceNotification", "sceNotificationUtilInitialize",     InitImpl);
    RegisterSymbol("libSceNotification", "sceNotificationUtilInitialize#T#T", InitImpl);

    // sceNotificationUtilCleanup — shutdown; return 0.
    auto CleanupImpl = [](const GuestArgs&) -> u64 {
        LOG_DEBUG(HLE, "sceNotificationUtilCleanup() -> 0");
        return ORBIS_OK;
    };
    RegisterSymbol("libSceNotification", "sceNotificationUtilCleanup",     CleanupImpl);
    RegisterSymbol("libSceNotification", "sceNotificationUtilCleanup#T#T", CleanupImpl);

    // sceNotificationUtilSendNotification — log and discard.
    // Prototype: int sceNotificationUtilSendNotification(SceNotificationUtilSendParam* param)
    // Param: u32 type, u32 flags, u32 user_id, u32 reserved, char message[512], ...
    auto SendImpl = [](const GuestArgs& args) -> u64 {
        const guest_addr_t param_ptr = args.arg1;
        if (!param_ptr) return ORBIS_NOTIFICATION_ERROR_PARAM;
        // Message starts at offset 0x10 in param struct (after type/flags/userId/reserved)
        LOG_DEBUG(HLE, "sceNotificationUtilSendNotification(param: 0x%llx) -> 0 (discarded)", param_ptr);
        return ORBIS_OK;
    };
    RegisterSymbol("libSceNotification", "sceNotificationUtilSendNotification",     SendImpl);
    RegisterSymbol("libSceNotification", "sceNotificationUtilSendNotification#T#T", SendImpl);

    // sceNotificationUtilProgressBegin / Update / End — progress notifications.
    auto ProgressBeginImpl = [](const GuestArgs& args) -> u64 {
        LOG_DEBUG(HLE, "sceNotificationUtilProgressBegin(0x%llx) -> 0", args.arg1);
        return ORBIS_OK;
    };
    RegisterSymbol("libSceNotification", "sceNotificationUtilProgressBegin",     ProgressBeginImpl);
    RegisterSymbol("libSceNotification", "sceNotificationUtilProgressBegin#T#T", ProgressBeginImpl);

    RegisterSymbol("libSceNotification", "sceNotificationUtilProgressUpdate",
                   [](const GuestArgs&) -> u64 { return ORBIS_OK; });
    RegisterSymbol("libSceNotification", "sceNotificationUtilProgressUpdate#T#T",
                   [](const GuestArgs&) -> u64 { return ORBIS_OK; });

    RegisterSymbol("libSceNotification", "sceNotificationUtilProgressFinish",
                   [](const GuestArgs&) -> u64 { return ORBIS_OK; });
    RegisterSymbol("libSceNotification", "sceNotificationUtilProgressFinish#T#T",
                   [](const GuestArgs&) -> u64 { return ORBIS_OK; });

    // -------- libSceNpNotification variants --------

    // sceNpNotificationRequestSendNotification
    auto NpSendImpl = [](const GuestArgs& args) -> u64 {
        LOG_DEBUG(HLE, "sceNpNotificationRequestSendNotification(0x%llx) -> 0", args.arg1);
        return ORBIS_OK;
    };
    RegisterSymbol("libSceNpNotification", "sceNpNotificationRequestSendNotification",     NpSendImpl);
    RegisterSymbol("libSceNpNotification", "sceNpNotificationRequestSendNotification#T#T", NpSendImpl);

    // sceNpNotificationRequestPopupNotification
    RegisterSymbol("libSceNpNotification", "sceNpNotificationRequestPopupNotification",
                   [](const GuestArgs&) -> u64 { return ORBIS_OK; });
    RegisterSymbol("libSceNpNotification", "sceNpNotificationRequestPopupNotification#T#T",
                   [](const GuestArgs&) -> u64 { return ORBIS_OK; });

    // -------- also import under libSceSystemService (some games use this path) --------
    RegisterSymbol("libSceSystemService", "sceNotificationUtilSendNotification",
                   SendImpl);
    RegisterSymbol("libSceSystemService", "sceNotificationUtilSendNotification#T#T",
                   SendImpl);
}

} // namespace HLE
