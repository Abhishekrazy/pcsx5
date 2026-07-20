// NP library family HLE — offline-but-signed-in model (SharpEmu design).
//
// Covers libSceNpManager (sign-in state, online id, callbacks), libSceNpTrophy2
// (context/handle lifecycle), libSceNpGameIntent, libSceNpUniversalDataSystem
// and libSceNpCommon basics.  Everything succeeds with plausible data: the
// console is "signed in" as a local offline user named "Player", no network
// activity ever happens, and handle-creating calls hand out incrementing ids.
#include "hle.h"
#include "../common/log.h"
#include "../memory/memory.h"
#include <atomic>
#include <cstring>

namespace HLE {

namespace {
constexpr u64 ORBIS_GEN2_ERROR_INVALID_ARGUMENT = 0x80020016;

std::atomic<u32> g_np_next_context{1};
std::atomic<u32> g_np_next_handle{1};

// Last title id passed to sceNpSetNpTitleId — included in structured trophy
// logs so compat reports can attribute unlock events to a title.
std::string g_np_title_id;

std::string ReadGuestString(guest_addr_t ptr, u64 max_len) {
    std::string out;
    if (!ptr) return out;
    for (u64 i = 0; i < max_len; ++i) {
        const u8 c = Memory::Read<u8>(ptr + i);
        if (!c) break;
        out += static_cast<char>(c);
    }
    return out;
}

u64 WriteIdAndReturnOk(guest_addr_t out_ptr, std::atomic<u32>& counter, const char* what) {
    if (!out_ptr) return ORBIS_GEN2_ERROR_INVALID_ARGUMENT;
    const u32 id = counter.fetch_add(1);
    Memory::Write<u32>(out_ptr, id);
    LOG_INFO(HLE, "%s -> id %u", what, id);
    return 0;
}

u64 ReturnOkLogged(const char* name) {
    LOG_DEBUG(HLE, "%s -> 0", name);
    return 0;
}
} // namespace

void RegisterLibNp() {
    LOG_INFO(HLE, "Registering NP (NpManager/NpTrophy2/NpGameIntent/NpCommon) HLE symbols...");

    // ------------------------------------------------------------------
    // libSceNpManager — offline but signed in.
    // ------------------------------------------------------------------

    // sceNpGetState (eQH7nWPcAgc) — Gen5 ABI: (userId, state*); state 1 = signed in.
    auto GetStateImpl = [](const GuestArgs& args) -> u64 {
        const guest_addr_t state_ptr = args.arg2;
        if (!state_ptr) return ORBIS_GEN2_ERROR_INVALID_ARGUMENT;
        Memory::Write<u32>(state_ptr, 1);
        LOG_DEBUG(HLE, "sceNpGetState(userId: %d) -> state 1 (signed in, offline)",
                  static_cast<s32>(args.arg1));
        return 0;
    };
    RegisterSymbol("libSceNpManager", "sceNpGetState", GetStateImpl);
    RegisterSymbol("libSceNpManager", "eQH7nWPcAgc#T#T", GetStateImpl);

    // sceNpGetOnlineId (XDncXQIJUSk) — Gen5 ABI: (userId, SceNpOnlineId*).
    // SceNpOnlineId is a 20-byte struct: 16-byte char data + 4 trailing bytes.
    auto GetOnlineIdImpl = [](const GuestArgs& args) -> u64 {
        const guest_addr_t out_ptr = args.arg2;
        if (!out_ptr) return ORBIS_GEN2_ERROR_INVALID_ARGUMENT;
        u8 online_id[20] = {};
        std::memcpy(online_id, "Player", 6);
        Memory::WriteBuffer(out_ptr, online_id, sizeof(online_id));
        LOG_DEBUG(HLE, "sceNpGetOnlineId(userId: %d) -> \"Player\"",
                  static_cast<s32>(args.arg1));
        return 0;
    };
    RegisterSymbol("libSceNpManager", "sceNpGetOnlineId", GetOnlineIdImpl);
    RegisterSymbol("libSceNpManager", "XDncXQIJUSk#T#T", GetOnlineIdImpl);
    // sceNpGetOnlineIdA / sceNpGetAccountIdA (rbknaUjpqWo) — same ABI.
    RegisterSymbol("libSceNpManager", "sceNpGetOnlineIdA", GetOnlineIdImpl);
    RegisterSymbol("libSceNpManager", "sceNpGetAccountIdA", GetOnlineIdImpl);
    RegisterSymbol("libSceNpManager", "rbknaUjpqWo#T#T", GetOnlineIdImpl);

    // sceNpGetNpReachabilityState (e-ZuhGEoeC4) — offline: report success, no network.
    auto ReachabilityImpl = [](const GuestArgs&) -> u64 {
        return ReturnOkLogged("sceNpGetNpReachabilityState");
    };
    RegisterSymbol("libSceNpManager", "sceNpGetNpReachabilityState", ReachabilityImpl);
    RegisterSymbol("libSceNpManager", "e-ZuhGEoeC4#T#T", ReachabilityImpl);

    // State-change callback registrations all succeed; the callback never fires.
    auto RegisterCbImpl = [](const GuestArgs&) -> u64 {
        return ReturnOkLogged("sceNpRegisterStateCallback");
    };
    RegisterSymbol("libSceNpManager", "sceNpRegisterStateCallback", RegisterCbImpl);
    RegisterSymbol("libSceNpManager", "VfRSmPmj8Q8#T#T", RegisterCbImpl);
    RegisterSymbol("libSceNpManager", "sceNpRegisterStateCallbackA", RegisterCbImpl);
    RegisterSymbol("libSceNpManager", "qQJfO8HAiaY#T#T", RegisterCbImpl);
    RegisterSymbol("libSceNpManagerForToolkit", "sceNpRegisterStateCallbackForToolkit", RegisterCbImpl);
    RegisterSymbol("libSceNpManagerForToolkit", "0c7HbXRKUt4#T#T", RegisterCbImpl);

    // Misc bookkeeping calls — succeed trivially.
    auto TrivialOkImpl = [](const GuestArgs&) -> u64 { return 0; };
    RegisterSymbol("libSceNpManager", "sceNpCheckCallback", TrivialOkImpl);
    RegisterSymbol("libSceNpManager", "3Zl8BePTh9Y#T#T", TrivialOkImpl);
    RegisterSymbol("libSceNpManager", "sceNpCheckCallbackForLib", TrivialOkImpl);
    RegisterSymbol("libSceNpManager", "JELHf4xPufo#T#T", TrivialOkImpl);
    RegisterSymbol("libSceNpManager", "sceNpDeleteRequest", TrivialOkImpl);
    RegisterSymbol("libSceNpManager", "S7QTn72PrDw#T#T", TrivialOkImpl);

    // sceNpSetNpTitleId (Ec63y59l9tw) — validate + log.
    auto SetNpTitleIdImpl = [](const GuestArgs& args) -> u64 {
        if (!args.arg1 || !args.arg2) return ORBIS_GEN2_ERROR_INVALID_ARGUMENT;
        const std::string title = ReadGuestString(args.arg1, 16);
        g_np_title_id = title;
        LOG_INFO(HLE, "sceNpSetNpTitleId(title: '%s') -> 0", title.c_str());
        return 0;
    };
    RegisterSymbol("libSceNpManager", "sceNpSetNpTitleId", SetNpTitleIdImpl);
    RegisterSymbol("libSceNpManager", "Ec63y59l9tw#T#T", SetNpTitleIdImpl);

    // ------------------------------------------------------------------
    // libSceNpTrophy2 — context/handle lifecycle with incrementing ids.
    // ------------------------------------------------------------------
    // Structured trophy-event logging (compat-report friendly): every line
    // carries a stable TROPHY_* tag plus title_id so reports can be parsed.
    auto CreateContextImpl = [](const GuestArgs& args) -> u64 {
        const u64 rc = WriteIdAndReturnOk(args.arg1, g_np_next_context, "sceNpTrophy2CreateContext");
        if (rc == 0) {
            LOG_INFO(HLE, "TROPHY_CTX_CREATE title_id=%s context_id=%u",
                     g_np_title_id.c_str(), Memory::Read<u32>(args.arg1));
        }
        return rc;
    };
    RegisterSymbol("libSceNpTrophy2", "sceNpTrophy2CreateContext", CreateContextImpl);
    RegisterSymbol("libSceNpTrophy2", "Bagshr7OQ6Q#T#T", CreateContextImpl);
    // libkernel-scoped NID import variant — overwrites the legacy stub.
    RegisterSymbol("libkernel", "Bagshr7OQ6Q#F#G", CreateContextImpl);

    auto CreateHandleImpl = [](const GuestArgs& args) -> u64 {
        return WriteIdAndReturnOk(args.arg1, g_np_next_handle, "sceNpTrophy2CreateHandle");
    };
    RegisterSymbol("libSceNpTrophy2", "sceNpTrophy2CreateHandle", CreateHandleImpl);
    RegisterSymbol("libSceNpTrophy2", "Gz1rmUZpROM#T#T", CreateHandleImpl);

    auto TrophyOkImpl = [](const GuestArgs&) -> u64 { return 0; };
    RegisterSymbol("libSceNpTrophy2", "sceNpTrophy2DestroyContext", TrophyOkImpl);
    RegisterSymbol("libSceNpTrophy2", "sysY2FHYff4#T#T", TrophyOkImpl);
    RegisterSymbol("libSceNpTrophy2", "sceNpTrophy2DestroyHandle", TrophyOkImpl);
    RegisterSymbol("libSceNpTrophy2", "d8P11CI40KE#T#T", TrophyOkImpl);
    RegisterSymbol("libSceNpTrophy2", "sceNpTrophy2AbortHandle", TrophyOkImpl);
    RegisterSymbol("libSceNpTrophy2", "fYapWA9xVmA#T#T", TrophyOkImpl);

    // RegisterContext(context, handle, options) — trophy set becomes active.
    auto RegisterContextImpl = [](const GuestArgs& args) -> u64 {
        LOG_INFO(HLE, "TROPHY_CTX_REGISTER title_id=%s context_id=%u handle=%u",
                 g_np_title_id.c_str(), static_cast<u32>(args.arg1),
                 static_cast<u32>(args.arg2));
        return 0;
    };
    RegisterSymbol("libSceNpTrophy2", "sceNpTrophy2RegisterContext", RegisterContextImpl);
    RegisterSymbol("libSceNpTrophy2", "bIDov3wBu5Q#T#T", RegisterContextImpl);

    // RegisterUnlockCallback(context, callback, arg) — game subscribes to
    // unlock events; no trophies exist yet, so the callback never fires.
    auto RegisterUnlockCbImpl = [](const GuestArgs& args) -> u64 {
        LOG_INFO(HLE, "TROPHY_UNLOCK_CB title_id=%s context_id=%u callback=0x%llx",
                 g_np_title_id.c_str(), static_cast<u32>(args.arg1), args.arg2);
        return 0;
    };
    RegisterSymbol("libSceNpTrophy2", "sceNpTrophy2RegisterUnlockCallback", RegisterUnlockCbImpl);
    RegisterSymbol("libSceNpTrophy2", "sUXGfNMalIo#T#T", RegisterUnlockCbImpl);
    // libkernel-scoped NID import variant — overwrites the legacy stub.
    RegisterSymbol("libkernel", "sUXGfNMalIo#F#G", RegisterUnlockCbImpl);
    RegisterSymbol("libSceNpTrophy2", "sceNpTrophy2UnregisterUnlockCallback", TrophyOkImpl);
    RegisterSymbol("libSceNpTrophy2", "wVqxM58sIKs#T#T", TrophyOkImpl);
    RegisterSymbol("libSceNpTrophy2", "sceNpTrophy2ShowTrophyList", TrophyOkImpl);
    RegisterSymbol("libSceNpTrophy2", "EHQEDVXZ0TI#T#T", TrophyOkImpl);

    // ------------------------------------------------------------------
    // libSceNpTrophy (legacy PS4-era API) — unlock-relevant subset, log-only.
    // No NID aliases exist in the local nid_db / SharpEmu, so these are
    // name-only registrations; unlocks are recorded as structured INFO lines.
    // ------------------------------------------------------------------
    RegisterSymbol("libSceNpTrophy", "sceNpTrophyCreateContext", CreateContextImpl);
    RegisterSymbol("libSceNpTrophy", "sceNpTrophyRegisterContext", RegisterContextImpl);
    // sceNpTrophyUnlockTrophy(context, handle, trophyId, platinumId).
    RegisterSymbol("libSceNpTrophy", "sceNpTrophyUnlockTrophy", [](const GuestArgs& args) -> u64 {
        LOG_INFO(HLE, "TROPHY_UNLOCK title_id=%s trophy_id=%d context_id=%u handle=%u platinum_id=%d",
                 g_np_title_id.c_str(), static_cast<s32>(args.arg3),
                 static_cast<u32>(args.arg1), static_cast<u32>(args.arg2),
                 static_cast<s32>(args.arg4));
        return 0;
    });
    // sceNpTrophySetTrophyFlag(context, handle, trophyId, flag).
    RegisterSymbol("libSceNpTrophy", "sceNpTrophySetTrophyFlag", [](const GuestArgs& args) -> u64 {
        LOG_INFO(HLE, "TROPHY_FLAG title_id=%s trophy_id=%d flag=%u context_id=%u",
                 g_np_title_id.c_str(), static_cast<s32>(args.arg3),
                 static_cast<u32>(args.arg4), static_cast<u32>(args.arg1));
        return 0;
    });

    // ------------------------------------------------------------------
    // libSceNpGameIntent.
    // ------------------------------------------------------------------
    RegisterSymbol("libSceNpGameIntent", "sceNpGameIntentInitialize", [](const GuestArgs&) -> u64 {
        return ReturnOkLogged("sceNpGameIntentInitialize");
    });
    RegisterSymbol("libSceNpGameIntent", "m87BHxt-H60#T#T", [](const GuestArgs&) -> u64 {
        return ReturnOkLogged("sceNpGameIntentInitialize");
    });

    // ------------------------------------------------------------------
    // libSceNpUniversalDataSystem — event-posting pipeline, all accepted.
    // ------------------------------------------------------------------
    RegisterSymbol("libSceNpUniversalDataSystem", "sceNpUniversalDataSystemInitialize", TrophyOkImpl);
    RegisterSymbol("libSceNpUniversalDataSystem", "sceNpUniversalDataSystemCreateContext",
                   [](const GuestArgs& args) -> u64 {
                       return WriteIdAndReturnOk(args.arg1, g_np_next_context,
                                                 "sceNpUniversalDataSystemCreateContext");
                   });
    RegisterSymbol("libSceNpUniversalDataSystem", "sceNpUniversalDataSystemCreateHandle",
                   [](const GuestArgs& args) -> u64 {
                       return WriteIdAndReturnOk(args.arg1, g_np_next_handle,
                                                 "sceNpUniversalDataSystemCreateHandle");
                   });
    RegisterSymbol("libSceNpUniversalDataSystem", "sceNpUniversalDataSystemCreateEvent",
                   [](const GuestArgs& args) -> u64 {
                       return WriteIdAndReturnOk(args.arg1, g_np_next_handle,
                                                 "sceNpUniversalDataSystemCreateEvent");
                   });
    RegisterSymbol("libSceNpUniversalDataSystem", "sceNpUniversalDataSystemRegisterContext", TrophyOkImpl);
    RegisterSymbol("libSceNpUniversalDataSystem", "sceNpUniversalDataSystemPostEvent", TrophyOkImpl);
    RegisterSymbol("libSceNpUniversalDataSystem", "sceNpUniversalDataSystemDestroyEvent", TrophyOkImpl);
    RegisterSymbol("libSceNpUniversalDataSystem", "sceNpUniversalDataSystemDestroyHandle", TrophyOkImpl);
    RegisterSymbol("libSceNpUniversalDataSystem", "sceNpUniversalDataSystemEventPropertyObjectSetString", TrophyOkImpl);
    RegisterSymbol("libSceNpUniversalDataSystem", "sceNpUniversalDataSystemEventPropertyObjectSetArray", TrophyOkImpl);

    // ------------------------------------------------------------------
    // libSceNpCommon basics — id comparison helpers; 0 = equal/ordered.
    // ------------------------------------------------------------------
    RegisterSymbol("libSceNpCommon", "sceNpCmpNpId", TrophyOkImpl);
    RegisterSymbol("libSceNpCommon", "sceNpCmpNpIdInOrder", TrophyOkImpl);
    RegisterSymbol("libSceNpCommon", "sceNpCmpOnlineId", TrophyOkImpl);
}

} // namespace HLE
