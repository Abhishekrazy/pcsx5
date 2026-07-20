// NP library family HLE — offline-but-signed-in model (SharpEmu design).
//
// Covers libSceNpManager (sign-in state, online id, callbacks), libSceNpTrophy2
// (context/handle lifecycle), libSceNpGameIntent, libSceNpUniversalDataSystem
// and libSceNpCommon basics.  Everything succeeds with plausible data: the
// console is "signed in" as the active configured user profile (default:
// "Player"), no network activity ever happens, and handle-creating calls
// hand out incrementing ids.
#include "hle.h"
#include "../common/log.h"
#include "../config/config.h"
#include "../memory/memory.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>
#include <vector>

namespace HLE {

namespace {
constexpr u64 ORBIS_GEN2_ERROR_INVALID_ARGUMENT = 0x80020016;

std::atomic<u32> g_np_next_context{1};
std::atomic<u32> g_np_next_handle{1};

// Last title id passed to sceNpSetNpTitleId — included in structured trophy
// logs so compat reports can attribute unlock events to a title.
std::string g_np_title_id;

// ------------------------------------------------------------------
// Trophy unlock persistence.
//
// Unlocked trophies are stored as JSON at <savedata-dir>/trophies.json where
// <savedata-dir> is HLE::GetSaveDataDir() (already <cwd>/pcsx5_savedata/
// <title-id>/).  Schema:
//   {"trophies":[{"id":N,"unlocked_at":"<iso8601>","grade":"<if known>"}]}
// The file is written by us only, so parsing is a small targeted scanner for
// the three fields rather than a general JSON parser (mirrors the hand-rolled
// minimal-JSON approach used by src/config/config.cpp).
// ------------------------------------------------------------------
struct TrophyRecord {
    s32 id = 0;
    std::string unlocked_at;
    std::string grade;
};

std::mutex g_np_trophy_mutex;
std::vector<TrophyRecord> g_np_trophies;
bool g_np_trophies_loaded = false;

// Registered libSceNpTrophy2 unlock callback (sceNpTrophy2RegisterUnlockCallback).
guest_addr_t g_np_unlock_callback = 0;
u64 g_np_unlock_callback_arg = 0;

std::string Iso8601Now() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_s(&tm, &t);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

std::string JsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            default: out += c; break;
        }
    }
    return out;
}

std::string TrophyStorePath() {
    return GetSaveDataDir() + "/trophies.json";
}

// Extracts the string value of "key" appearing after `pos` in `text`.
// Returns empty string when absent.
std::string JsonFindString(const std::string& text, const char* key, size_t pos) {
    const std::string needle = std::string("\"") + key + "\"";
    size_t k = text.find(needle, pos);
    if (k == std::string::npos) return "";
    size_t colon = text.find(':', k + needle.size());
    if (colon == std::string::npos) return "";
    size_t open = text.find('"', colon + 1);
    if (open == std::string::npos) return "";
    size_t close = text.find('"', open + 1);
    if (close == std::string::npos) return "";
    return text.substr(open + 1, close - open - 1);
}

void LoadTrophyStore() {
    if (g_np_trophies_loaded) return;
    g_np_trophies_loaded = true;
    g_np_trophies.clear();
    std::ifstream in(TrophyStorePath());
    if (!in) return;
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    size_t pos = 0;
    while ((pos = text.find("\"id\"", pos)) != std::string::npos) {
        size_t colon = text.find(':', pos + 4);
        if (colon == std::string::npos) break;
        TrophyRecord rec;
        rec.id = static_cast<s32>(std::strtol(text.c_str() + colon + 1, nullptr, 10));
        rec.unlocked_at = JsonFindString(text, "unlocked_at", colon);
        rec.grade = JsonFindString(text, "grade", colon);
        bool dup = false;
        for (const auto& r : g_np_trophies) {
            if (r.id == rec.id) { dup = true; break; }
        }
        if (!dup) g_np_trophies.push_back(rec);
        pos = colon + 1;
    }
    LOG_DEBUG(HLE, "Trophy store: loaded %zu unlocked trophies from %s",
              g_np_trophies.size(), TrophyStorePath().c_str());
}

void SaveTrophyStore() {
    std::ofstream out(TrophyStorePath(), std::ios::trunc);
    if (!out) {
        LOG_WARN(HLE, "Trophy store: failed to write %s", TrophyStorePath().c_str());
        return;
    }
    out << "{\n  \"trophies\": [";
    for (size_t i = 0; i < g_np_trophies.size(); ++i) {
        const TrophyRecord& r = g_np_trophies[i];
        out << (i ? "," : "") << "\n    {\"id\": " << r.id
            << ", \"unlocked_at\": \"" << JsonEscape(r.unlocked_at) << "\"";
        if (!r.grade.empty())
            out << ", \"grade\": \"" << JsonEscape(r.grade) << "\"";
        out << "}";
    }
    out << "\n  ]\n}\n";
}

// Records a trophy unlock (deduped by id) and persists the store.
// Returns true when this is a newly unlocked trophy.
bool RecordTrophyUnlock(s32 trophy_id, const char* grade) {
    std::lock_guard<std::mutex> lock(g_np_trophy_mutex);
    LoadTrophyStore();
    for (const auto& r : g_np_trophies) {
        if (r.id == trophy_id) return false;
    }
    TrophyRecord rec;
    rec.id = trophy_id;
    rec.unlocked_at = Iso8601Now();
    if (grade) rec.grade = grade;
    g_np_trophies.push_back(rec);
    SaveTrophyStore();
    return true;
}
} // namespace

// Trophy-store introspection (declared in hle.h) — lets tests and tooling
// inspect the persisted unlock state without touching libnp internals.
size_t NpTrophyUnlockedCount() {
    std::lock_guard<std::mutex> lock(g_np_trophy_mutex);
    LoadTrophyStore();
    return g_np_trophies.size();
}

bool NpTrophyIsUnlocked(s32 trophy_id) {
    std::lock_guard<std::mutex> lock(g_np_trophy_mutex);
    LoadTrophyStore();
    for (const auto& r : g_np_trophies) {
        if (r.id == trophy_id) return true;
    }
    return false;
}

namespace {

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
        // Source the online id from the active configured user profile
        // (falls back to "Player" when the config has no profiles).
        const auto* profile = ConfigService::ActiveUserProfile();
        const std::string online = profile ? profile->online_id : "Player";
        u8 online_id[20] = {};
        std::memcpy(online_id, online.data(),
                    std::min(online.size(), static_cast<size_t>(16)));
        Memory::WriteBuffer(out_ptr, online_id, sizeof(online_id));
        LOG_DEBUG(HLE, "sceNpGetOnlineId(userId: %d) -> \"%s\"",
                  static_cast<s32>(args.arg1), online.c_str());
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
            // First context creation loads any previously persisted unlocks.
            std::lock_guard<std::mutex> lock(g_np_trophy_mutex);
            LoadTrophyStore();
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
    // unlock events; the callback address + user arg are stored and fired
    // from sceNpTrophyUnlockTrophy (see below) via InvokeGuestFunction.
    auto RegisterUnlockCbImpl = [](const GuestArgs& args) -> u64 {
        g_np_unlock_callback = args.arg2;
        g_np_unlock_callback_arg = args.arg3;
        LOG_INFO(HLE, "TROPHY_UNLOCK_CB title_id=%s context_id=%u callback=0x%llx",
                 g_np_title_id.c_str(), static_cast<u32>(args.arg1), args.arg2);
        return 0;
    };
    RegisterSymbol("libSceNpTrophy2", "sceNpTrophy2RegisterUnlockCallback", RegisterUnlockCbImpl);
    RegisterSymbol("libSceNpTrophy2", "sUXGfNMalIo#T#T", RegisterUnlockCbImpl);
    // libkernel-scoped NID import variant — overwrites the legacy stub.
    RegisterSymbol("libkernel", "sUXGfNMalIo#F#G", RegisterUnlockCbImpl);
    RegisterSymbol("libSceNpTrophy2", "sceNpTrophy2UnregisterUnlockCallback", [](const GuestArgs&) -> u64 {
        g_np_unlock_callback = 0;
        g_np_unlock_callback_arg = 0;
        return 0;
    });
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
    // Records the unlock into the persisted store and fires any registered
    // libSceNpTrophy2 unlock callback.  This handler runs inside HLE dispatch
    // on a guest thread, so the per-thread host stack pointer is already set
    // and InvokeGuestFunction can be called directly (same pattern as
    // scePthreadOnce in libkernel_sync.cpp) — no callback queueing needed.
    RegisterSymbol("libSceNpTrophy", "sceNpTrophyUnlockTrophy", [](const GuestArgs& args) -> u64 {
        const s32 trophy_id = static_cast<s32>(args.arg3);
        LOG_INFO(HLE, "TROPHY_UNLOCK title_id=%s trophy_id=%d context_id=%u handle=%u platinum_id=%d",
                 g_np_title_id.c_str(), trophy_id,
                 static_cast<u32>(args.arg1), static_cast<u32>(args.arg2),
                 static_cast<s32>(args.arg4));
        const bool newly_unlocked = RecordTrophyUnlock(trophy_id, nullptr);
        if (newly_unlocked && g_np_unlock_callback) {
            InvokeGuestFunction(g_np_unlock_callback, args.arg1,
                                static_cast<u64>(static_cast<u32>(trophy_id)),
                                g_np_unlock_callback_arg);
        }
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
