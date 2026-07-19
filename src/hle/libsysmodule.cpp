// libSceSysmodule HLE — "fake bookkeeping" model (SharpEmu design study).
//
// Module loads are NOT routed to the real PRX loader: sceSysmoduleLoadModule
// records the module id in a registry and returns success.  Known ids map to
// canonical SPRX names; unknown ids get a synthesized "sysmodule_0xXXXX.sprx"
// name.  IsLoaded/UnloadModule answer from the same registry.  This lets
// games proceed past their sysmodule init sequences without us having to
// emulate every system module.
//
// If a real PRX for the module exists in the game's sce_module directory we
// still fake the load (real loading is out of scope) but say so in the log.
#include "hle.h"
#include "../common/log.h"
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace HLE {

namespace {

std::mutex                 g_sysmodule_mutex;
std::unordered_set<u32>    g_loaded_sysmodules;

// Well-known sysmodule ids (SCE_SYSMODULE_* constants), per the SharpEmu
// KernelModuleRegistry table.
const std::unordered_map<u32, const char*>& KnownSysmoduleNames() {
    static const std::unordered_map<u32, const char*> kNames = {
        {0x0006, "libSceFiber.sprx"},
        {0x0021, "libSceRudp.sprx"},
        {0x0095, "libSceIme.sprx"},
        {0x0096, "libSceImeDialog.sprx"},
        {0x00A9, "libSceMouse.sprx"},
    };
    return kNames;
}

std::string SysmoduleName(u32 id) {
    const auto& known = KnownSysmoduleNames();
    auto it = known.find(id);
    if (it != known.end()) {
        return it->second;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "sysmodule_0x%04X.sprx", id);
    return buf;
}

u64 SysmoduleLoadModuleImpl(u32 id) {
    const std::string name = SysmoduleName(id);
    {
        std::lock_guard<std::mutex> lk(g_sysmodule_mutex);
        g_loaded_sysmodules.insert(id);
    }
    // Fake bookkeeping only: we do NOT attempt to load a real PRX even if the
    // game's sce_module directory has one — the HLE surface for the module is
    // expected to cover what the game actually calls.
    LOG_INFO(HLE, "sceSysmoduleLoadModule(id: 0x%04X = %s) -> faked (bookkeeping only)", id, name.c_str());
    return 0;
}

} // namespace

void RegisterLibSysmodule() {
    LOG_INFO(HLE, "Registering libSceSysmodule HLE symbols...");

    // sceSysmoduleLoadModule (g8cM39EUZ6o)
    auto LoadModuleImpl = [](const GuestArgs& args) -> u64 {
        return SysmoduleLoadModuleImpl(static_cast<u32>(args.arg1));
    };
    RegisterSymbol("libSceSysmodule", "sceSysmoduleLoadModule", LoadModuleImpl);
    RegisterSymbol("libSceSysmodule", "g8cM39EUZ6o#T#T", LoadModuleImpl);

    // sceSysmoduleLoadModuleInternalWithArg (hHrGoGoNf+s) — same fake.
    auto LoadModuleInternalImpl = [](const GuestArgs& args) -> u64 {
        return SysmoduleLoadModuleImpl(static_cast<u32>(args.arg1));
    };
    RegisterSymbol("libSceSysmodule", "sceSysmoduleLoadModuleInternalWithArg", LoadModuleInternalImpl);
    RegisterSymbol("libSceSysmodule", "hHrGoGoNf+s#T#T", LoadModuleInternalImpl);

    // sceSysmoduleUnloadModule (eR2bZFAAU0Q)
    auto UnloadModuleImpl = [](const GuestArgs& args) -> u64 {
        const u32 id = static_cast<u32>(args.arg1);
        {
            std::lock_guard<std::mutex> lk(g_sysmodule_mutex);
            g_loaded_sysmodules.erase(id);
        }
        LOG_INFO(HLE, "sceSysmoduleUnloadModule(id: 0x%04X = %s) -> 0", id, SysmoduleName(id).c_str());
        return 0;
    };
    RegisterSymbol("libSceSysmodule", "sceSysmoduleUnloadModule", UnloadModuleImpl);
    RegisterSymbol("libSceSysmodule", "eR2bZFAAU0Q#T#T", UnloadModuleImpl);
    // Games also reach this through a libkernel-scoped NID import; overwrite
    // the legacy return-0 stub registered there (last-registration-win).
    RegisterSymbol("libkernel", "eR2bZFAAU0Q#D#E", UnloadModuleImpl);

    // sceSysmoduleIsLoaded (fMP5NHUOaMk)
    auto IsLoadedImpl = [](const GuestArgs& args) -> u64 {
        const u32 id = static_cast<u32>(args.arg1);
        bool loaded = false;
        {
            std::lock_guard<std::mutex> lk(g_sysmodule_mutex);
            loaded = g_loaded_sysmodules.count(id) != 0;
        }
        if (!loaded) {
            // Not in the registry.  Real hardware returns an error here, but
            // claiming success is the safer lie: games typically gate further
            // use of the module on this call, and our HLE surface fakes the
            // module's exports anyway.
            LOG_INFO(HLE, "sceSysmoduleIsLoaded(id: 0x%04X = %s): not in registry; claiming loaded",
                     id, SysmoduleName(id).c_str());
            return 0;
        }
        LOG_DEBUG(HLE, "sceSysmoduleIsLoaded(id: 0x%04X) -> 0 (loaded)", id);
        return 0;
    };
    RegisterSymbol("libSceSysmodule", "sceSysmoduleIsLoaded", IsLoadedImpl);
    RegisterSymbol("libSceSysmodule", "fMP5NHUOaMk#T#T", IsLoadedImpl);
    RegisterSymbol("libkernel", "fMP5NHUOaMk#D#E", IsLoadedImpl);
}

} // namespace HLE
