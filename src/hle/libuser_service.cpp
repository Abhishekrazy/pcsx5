#include "hle.h"
#include "../common/log.h"
#include "../memory/memory.h"
#include <cstring>

namespace HLE {

    // -------------------------------------------------------------------------
    // libSceUserService — canned offline single-user profile.
    //
    // The emulator has no account system; every title sees exactly one local
    // user (id 1, name "Player1").  Games that statically link the library
    // import these symbols weakly through libkernel, so each is registered
    // under both the canonical module name and the libkernel NID.
    // -------------------------------------------------------------------------
    namespace {
        constexpr s32 kOfflineUserId = 1;

        void RegisterUserServiceSymbol(const char* name, const char* nid, HleHandler handler) {
            RegisterSymbol("libSceUserService", name, handler);
            RegisterSymbol("libSceUserService", nid, handler);
            RegisterSymbol("libkernel", nid, handler);
        }
    } // namespace

    void RegisterLibUserService() {
        LOG_INFO(HLE, "Registering libSceUserService HLE symbols (offline single user)...");

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
            if (out) {
                Memory::Write<s32>(out + 0,  kOfflineUserId);
                Memory::Write<s32>(out + 4,  -1);
                Memory::Write<s32>(out + 8,  -1);
                Memory::Write<s32>(out + 12, -1);
            }
            return 1; // one logged-in user
        };
        RegisterUserServiceSymbol("sceUserServiceGetLoginUserIdList",
                                  "fPhymKNvK-A#U#U", GetLoginUserIdList);

        // sceUserServiceGetInitialUser(SceUserServiceUserId* out)
        auto GetInitialUser = [](const GuestArgs& args) -> u64 {
            guest_addr_t out = args.arg1;
            LOG_INFO(HLE, "sceUserServiceGetInitialUser(out: 0x%llx)", out);
            if (out) {
                Memory::Write<s32>(out, kOfflineUserId);
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
            static const char kName[] = "Player1";
            if (name && size > 0) {
                u64 copy = (sizeof(kName) < size) ? sizeof(kName) : size;
                Memory::WriteBuffer(name, kName, copy);
                Memory::Write<u8>(name + copy - 1, 0); // guarantee NUL
            }
            return 0;
        };
        RegisterUserServiceSymbol("sceUserServiceGetUserName",
                                  "1xxcMiGu2fo#U#U", GetUserName);
    }
}
// namespace HLE
