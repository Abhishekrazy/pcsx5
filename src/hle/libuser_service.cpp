#include "hle.h"
#include "../common/log.h"
#include "../config/config.h"
#include "../memory/memory.h"
#include <algorithm>
#include <cstring>

namespace HLE {

    // -------------------------------------------------------------------------
    // libSceUserService — backed by the configured user profiles
    // (ConfigService users section).  Games that statically link the library
    // import these symbols weakly through libkernel, so each is registered
    // under both the canonical module name and the libkernel NID.
    //
    // GetLoginUserIdList reports every configured profile (up to the 4-slot
    // ABI limit) as logged in — the closest offline analogue of a console
    // with several local accounts; the active profile is always slot 0.
    // -------------------------------------------------------------------------
    namespace {
        constexpr u32 kFallbackUserId = 1; // used only when no profile exists

        // Configured profiles in report order: active first, then the rest.
        std::vector<ConfigService::UserProfile> LoginProfiles() {
            std::vector<ConfigService::UserProfile> out;
            const auto& users = ConfigService::Global().users;
            if (const auto* active = ConfigService::ActiveUserProfile()) out.push_back(*active);
            for (const auto& p : users.profiles) {
                if (out.empty() || p.id != out.front().id) out.push_back(p);
            }
            return out;
        }

        void RegisterUserServiceSymbol(const char* name, const char* nid, HleHandler handler) {
            RegisterSymbol("libSceUserService", name, handler);
            RegisterSymbol("libSceUserService", nid, handler);
            RegisterSymbol("libkernel", nid, handler);
        }
    } // namespace

    void RegisterLibUserService() {
        LOG_INFO(HLE, "Registering libSceUserService HLE symbols (config profiles)...");

        // sceUserServiceInitialize(const SceUserServiceInitializeParams*)
        auto Initialize = [](const GuestArgs& args) -> u64 {
            LOG_INFO(HLE, "sceUserServiceInitialize(params: 0x%llx)", args.arg1);
            return 0;
        };
        RegisterUserServiceSymbol("sceUserServiceInitialize", "j3YMu1MVNNo#U#U", Initialize);

        // -----------------------------------------------------------------
        // sceUserServiceGetLoginUserIdList(SceUserServiceLoginUserIdList* out)
        // Struct is { s32 userId[4] }; empty slots are -1.  Returns the count.
        // -----------------------------------------------------------------
        auto GetLoginUserIdList = [](const GuestArgs& args) -> u64 {
            guest_addr_t out = args.arg1;
            LOG_INFO(HLE, "sceUserServiceGetLoginUserIdList(out: 0x%llx)", out);
            const auto profiles = LoginProfiles();
            const size_t count = std::min(profiles.size(), static_cast<size_t>(4));
            if (out) {
                for (size_t i = 0; i < 4; ++i) {
                    const s32 id = (i < count)
                        ? static_cast<s32>(profiles[i].id) : -1;
                    Memory::Write<s32>(out + i * 4, id);
                }
            }
            if (count == 0) {
                if (out) {
                    Memory::Write<s32>(out + 0, static_cast<s32>(kFallbackUserId));
                    Memory::Write<s32>(out + 4, -1);
                    Memory::Write<s32>(out + 8, -1);
                    Memory::Write<s32>(out + 12, -1);
                }
                return 1;
            }
            return static_cast<u64>(count);
        };
        RegisterUserServiceSymbol("sceUserServiceGetLoginUserIdList",
                                  "fPhymKNvK-A#U#U", GetLoginUserIdList);

        // sceUserServiceGetInitialUser(SceUserServiceUserId* out)
        auto GetInitialUser = [](const GuestArgs& args) -> u64 {
            guest_addr_t out = args.arg1;
            LOG_INFO(HLE, "sceUserServiceGetInitialUser(out: 0x%llx)", out);
            if (out) {
                const u32 id = ConfigService::ActiveUserId();
                Memory::Write<s32>(out, static_cast<s32>(id ? id : kFallbackUserId));
            }
            return 0;
        };
        RegisterUserServiceSymbol("sceUserServiceGetInitialUser",
                                  "CdWp0oHWGr0#U#U", GetInitialUser);

        // sceUserServiceGetUserName(userId, char* name, size_t size)
        auto GetUserName = [](const GuestArgs& args) -> u64 {
            s32 user_id        = static_cast<s32>(args.arg1);
            guest_addr_t name  = args.arg2;
            u64 size           = args.arg3;
            LOG_INFO(HLE, "sceUserServiceGetUserName(userId: %d, name: 0x%llx, size: %llu)",
                     user_id, name, size);
            const auto* profile = ConfigService::FindUserProfile(static_cast<u32>(user_id));
            if (!profile) profile = ConfigService::ActiveUserProfile();
            const std::string display = profile ? profile->name : "Player";
            if (name && size > 0) {
                const u64 copy = std::min(static_cast<u64>(display.size() + 1), size);
                Memory::WriteBuffer(name, display.c_str(), copy);
                Memory::Write<u8>(name + copy - 1, 0); // guarantee NUL
            }
            return 0;
        };
        RegisterUserServiceSymbol("sceUserServiceGetUserName",
                                  "1xxcMiGu2fo#U#U", GetUserName);
    }
}
// namespace HLE
