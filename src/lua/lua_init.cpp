// pcsx5 Lua-driven subsystem lifecycle — implementation.
//
// Provides the SubsystemRegistry (topological-sort-based init/teardown),
// the default init chain (mirrors main.cpp), and Lua bindings.

#include "lua_init.h"
#include "../common/log.h"
#include "../config/config.h"
#include "../diagnostics/diagnostics.h"
#include "../memory/memory.h"
#include "../hle/hle.h"
#include "../kernel/kernel.h"
#include "../gpu/gpu.h"

#include <algorithm>
#include <queue>
#include <cstdio>

#ifdef PCSX5_HAS_LUA
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#endif

namespace LuaInit {

// ===========================================================================
// SubsystemRegistry implementation
// ===========================================================================

void SubsystemRegistry::Register(const Subsystem& sub) {
    if (m_subsystems.count(sub.name)) {
        LOG_WARN(General, "SubsystemRegistry: duplicate subsystem '%s', ignoring", sub.name.c_str());
        return;
    }
    m_subsystems[sub.name] = sub;
    m_resolved = false;  // re-resolve on next InitializeAll
}

bool SubsystemRegistry::TopoSort(std::string* error) {
    // Kahn's algorithm for topological sort.
    // Build in-degree map.
    std::unordered_map<std::string, int> in_degree;
    for (auto& [name, _] : m_subsystems) {
        in_degree[name] = 0;
    }

    for (auto& [name, sub] : m_subsystems) {
        for (auto& dep : sub.dependencies) {
            if (!m_subsystems.count(dep)) {
                if (error) *error = "Missing dependency: subsystem '" + sub.name
                                   + "' requires '" + dep + "' (not registered)";
                return false;
            }
            in_degree[name]++;
        }
    }

    // Seed queue with zero in-degree nodes.
    std::queue<std::string> q;
    for (auto& [name, deg] : in_degree) {
        if (deg == 0) q.push(name);
    }

    m_init_order.clear();
    while (!q.empty()) {
        auto current = q.front();
        q.pop();
        m_init_order.push_back(current);

        // Decrease in-degree of dependents.
        for (auto& [name, sub] : m_subsystems) {
            for (auto& dep : sub.dependencies) {
                if (dep == current) {
                    in_degree[name]--;
                    if (in_degree[name] == 0) {
                        q.push(name);
                    }
                }
            }
        }
    }

    if (static_cast<size_t>(m_init_order.size()) != m_subsystems.size()) {
        if (error) *error = "Dependency cycle detected among subsystems";
        return false;
    }

    return true;
}

bool SubsystemRegistry::ResolveOrder(std::string* error) {
    m_resolved = TopoSort(error);
    return m_resolved;
}

bool SubsystemRegistry::InitializeAll(std::string* error) {
    if (!m_resolved) {
        if (!ResolveOrder(error)) return false;
    }

    m_initialized.clear();
    for (auto& name : m_init_order) {
        auto it = m_subsystems.find(name);
        if (it == m_subsystems.end()) continue;

        LOG_INFO(General, "SubsystemRegistry: initializing '%s'...", name.c_str());
        if (!it->second.init()) {
            LOG_ERROR(General, "SubsystemRegistry: init failed for '%s'", name.c_str());
            // Teardown everything initialized so far (reverse order).
            TeardownAll();
            if (error) *error = "Failed to initialize subsystem: " + name;
            return false;
        }
        m_initialized.push_back(name);
        LOG_INFO(General, "SubsystemRegistry: '%s' initialized OK", name.c_str());
    }

    return true;
}

void SubsystemRegistry::TeardownAll() {
    for (auto it = m_initialized.rbegin(); it != m_initialized.rend(); ++it) {
        auto sub_it = m_subsystems.find(*it);
        if (sub_it != m_subsystems.end()) {
            LOG_INFO(General, "SubsystemRegistry: tearing down '%s'", (*it).c_str());
            sub_it->second.teardown();
        }
    }
    m_initialized.clear();
    m_resolved = false;
}

bool SubsystemRegistry::IsInitialized(const std::string& name) const {
    for (auto& n : m_initialized) {
        if (n == name) return true;
    }
    return false;
}

std::vector<std::string> SubsystemRegistry::RegisteredNames() const {
    std::vector<std::string> names;
    names.reserve(m_subsystems.size());
    for (auto& [name, _] : m_subsystems) {
        names.push_back(name);
    }
    return names;
}

SubsystemRegistry& SubsystemRegistry::Instance() {
    static SubsystemRegistry instance;
    return instance;
}

// ===========================================================================
// Default init chain — mirrors the current main.cpp boot sequence
// ===========================================================================

bool RunDefaultInit(std::string* error) {
    auto& reg = SubsystemRegistry::Instance();

    // 1. ConfigService — first, so every other subsystem can read its settings
    reg.Register(Subsystem{
        "ConfigService", {},
        []() {
            // Default config dir; overridden by CLI if ConfigService is already init'd.
            ConfigService::Initialize("pcsx5_config");
            return true;
        },
        []() { /* ConfigService is persistent; no teardown needed */ }
    });

    // 2. Diagnostics — install crash handler early
    reg.Register(Subsystem{
        "Diagnostics", {"ConfigService"},
        [reg]() -> bool {
            auto cfg = ConfigService::EffectiveFor("");
            std::string bundle_dir = cfg.crash.bundle_dir.empty() ? "pcsx5_crash" : cfg.crash.bundle_dir;
            Diagnostics::InstallCrashHandler(bundle_dir);
            return true;
        },
        []() { /* Diagnostics is persistent; no teardown needed */ }
    });

    // 3. Logging — apply config to logging subsystem
    reg.Register(Subsystem{
        "Logging", {"ConfigService"},
        [reg]() -> bool {
            auto cfg = ConfigService::EffectiveFor("");
            if (!cfg.logging.file_path.empty()) {
                LogConfig::SetFileOutput(cfg.logging.file_path, cfg.logging.file_append);
            }
            if (cfg.logging.json_output) {
                LogConfig::SetJsonOutput(true);
            }
            for (int i = 0; i < 6; ++i) {
                LogConfig::SetLevel(static_cast<LogCategory>(i), cfg.logging.min_level);
            }
            return true;
        },
        []() { /* Logging is persistent; no teardown needed */ }
    });

    // 4. Memory — guest virtual memory manager
    reg.Register(Subsystem{
        "Memory", {"ConfigService"},
        []() { return Memory::Initialize(); },
        []() { Memory::Shutdown(); }
    });

    // 5. HLE — high-level emulation (symbol registry, thunk allocator)
    reg.Register(Subsystem{
        "HLE", {"ConfigService", "Memory"},
        []() { return HLE::Initialize(); },
        []() { HLE::Shutdown(); }
    });

    // 6. Kernel — thread management, syscall dispatch, guest execution
    reg.Register(Subsystem{
        "Kernel", {"Memory", "HLE"},
        []() { return Kernel::Initialize(); },
        []() { Kernel::Shutdown(); }
    });

    // 7. GPU — rendering backend (Vulkan primary, OpenGL secondary)
    reg.Register(Subsystem{
        "GPU", {"Memory", "HLE", "Kernel"},
        []() { return GPU::Initialize(); },
        []() { GPU::Shutdown(); }
    });

    // Resolve dependencies and initialize in order.
    return reg.InitializeAll(error);
}

// ===========================================================================
// Lua bindings
// ===========================================================================

#ifdef PCSX5_HAS_LUA

// Helper: get the registry from the Lua state (stored as an upvalue).
static SubsystemRegistry& GetRegistryFromLua(void* lua_state) {
    // The registry pointer is stored at index 0 of the registry's upvalue table.
    // We use a simpler approach: the registry is a singleton, so just call Instance().
    (void)lua_state;  // not needed for singleton access
    return SubsystemRegistry::Instance();
}

// Lua C function: RegisterSubsystem(name, deps_table, init_fn, teardown_fn)
//   name: string — subsystem name
//   deps: table of strings — dependency names
//   init_fn: function — returns boolean (true = success)
//   teardown_fn: function — no arguments, no return value
static int lua_register_subsystem(void* L) {
    // Arguments: name, deps_table, init_fn, teardown_fn
    if (lua_gettop(L) < 4) {
        luaL_error(L, "RegisterSubsystem requires 4 arguments: name, deps, init_fn, teardown_fn");
        return 0;
    }

    const char* name = lua_tostring(L, 1);
    if (!name) {
        luaL_error(L, "RegisterSubsystem: name must be a string");
        return 0;
    }

    Subsystem sub;
    sub.name = name;

    // Parse dependencies table.
    if (!lua_istable(L, 2)) {
        luaL_error(L, "RegisterSubsystem: deps must be a table");
        return 0;
    }
    lua_pushnil(L);
    while (lua_next(L, 2) != 0) {
        const char* dep = lua_tostring(L, -1);
        if (dep) {
            sub.dependencies.push_back(dep);
        }
        lua_pop(L, 1);  // remove value, keep key for next iteration
    }

    // Store init and teardown functions on the Lua stack for later retrieval.
    // We store them in a registry map keyed by subsystem name.
    // For simplicity, we store the function references in a Lua table
    // accessible via the registry.

    auto& reg = SubsystemRegistry::Instance();

    // Capture the Lua state and function indices for later invocation.
    // We store the init function on the stack at index 3 and teardown at index 4.
    // The callbacks will be invoked by popping from the Lua stack.
    sub.init = [L, name]() -> bool {
        // Push the init function and call it.
        // The init function is stored in a per-subsystem Lua table.
        // For now, we use a simpler approach: the init function is
        // already on the Lua stack when this callback is invoked.
        // Actually, we need to store the function reference.
        // Let's use a different approach: store the function in a global table.
        lua_getglobal(L, "__pcsx5_subsystem_init");
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, name);
            if (lua_isfunction(L, -1)) {
                if (lua_pcall(L, 0, 1, 0) != 0) {
                    LOG_ERROR(General, "Lua init failed for '%s': %s", name, lua_tostring(L, -1));
                    lua_pop(L, 1);
                    return false;
                }
                bool result = lua_toboolean(L, -1) ? true : false;
                lua_pop(L, 1);
                lua_pop(L, 1);  // pop __pcsx5_subsystem_init table
                return result;
            }
        }
        lua_pop(L, 1);  // pop __pcsx5_subsystem_init table
        LOG_ERROR(General, "Lua init: no init function registered for '%s'", name);
        return false;
    };

    sub.teardown = [L, name]() {
        lua_getglobal(L, "__pcsx5_subsystem_teardown");
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, name);
            if (lua_isfunction(L, -1)) {
                if (lua_pcall(L, 0, 0, 0) != 0) {
                    LOG_ERROR(General, "Lua teardown failed for '%s': %s", name, lua_tostring(L, -1));
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    };

    reg.Register(sub);
    return 0;
}

// Lua C function: RunInitChain() — initializes all registered subsystems.
static int lua_run_init_chain(void* L) {
    std::string error;
    bool ok = SubsystemRegistry::Instance().InitializeAll(&error);
    lua_pushboolean(L, ok ? 1 : 0);
    if (!ok) {
        lua_pushstring(L, error.c_str());
        return 2;  // boolean false, error message
    }
    return 1;  // boolean true
}

// Lua C function: RunTeardownChain() — tears down all subsystems in reverse order.
static int lua_run_teardown_chain(void* L) {
    SubsystemRegistry::Instance().TeardownAll();
    return 0;
}

// Lua C function: GetInitOrder() — returns the resolved init order as a table.
static int lua_get_init_order(void* L) {
    auto& reg = SubsystemRegistry::Instance();
    auto order = reg.InitOrder();

    lua_createtable(L, static_cast<int>(order.size()), 0);
    for (size_t i = 0; i < order.size(); ++i) {
        lua_pushinteger(L, static_cast<lua_Integer>(i + 1));
        lua_pushstring(L, order[i].c_str());
        lua_settable(L, -3);
    }
    return 1;
}

// Register all Lua C functions on the given state.
void RegisterLuaBindings(void* lua_state) {
    // Create a registry table to store init/teardown function references.
    // This is needed because the C++ callbacks need to call back into Lua.
    (void)lua_state;  // We use the singleton pattern; the registry is global.
    // The actual function registration would use lua_register / lua_pushcfunction.
    // For now, this is a stub — real implementation requires lua_State*.
}

// Run a Lua script that registers subsystems and optionally calls RunInitChain.
bool RunLuaScript(void* lua_state, const char* script_text, const char* script_name, std::string* error) {
    (void)lua_state; (void)script_text; (void)script_name; (void)error;
    // Real implementation requires lua_State*. Stub for now.
    return false;
}

#endif  // PCSX5_HAS_LUA

// ===========================================================================
// Default Lua init — embedded init script (Lua-like DSL)
// ===========================================================================

// The embedded init script is a Lua-like DSL that can be:
// 1. Parsed by a future Lua interpreter when PCSX5_HAS_LUA is defined
// 2. Used as a reference for the C++ registration in RunDefaultInit()
// 3. Written to a file for user customization

static const char* kDefaultInitScript =
    "// pcsx5 default init script (Lua-like DSL)\n"
    "// This script declares the subsystem lifecycle for PCSX5.\n"
    "// When Lua is available, it is executed directly.\n"
    "// Otherwise, the C++ equivalent (RunDefaultInit) is used.\n"
    "\n"
    "// --- Phase 0: Infrastructure ---\n"
    "RegisterSubsystem(\"ConfigService\", {},\n"
    "    function() ConfigService_Init(\"pcsx5_config\") return true end,\n"
    "    function() end)\n"
    "\n"
    "RegisterSubsystem(\"Diagnostics\", {\"ConfigService\"},\n"
    "    function() Diagnostics_Init() return true end,\n"
    "    function() end)\n"
    "\n"
    "// --- Phase 1: Core emulation ---\n"
    "RegisterSubsystem(\"Logging\", {\"ConfigService\"},\n"
    "    function() Logging_Init() return true end,\n"
    "    function() end)\n"
    "\n"
    "RegisterSubsystem(\"Memory\", {\"ConfigService\"},\n"
    "    function() Memory_Init() return true end,\n"
    "    function() Memory_Shutdown() end)\n"
    "\n"
    "RegisterSubsystem(\"HLE\", {\"ConfigService\", \"Memory\"},\n"
    "    function() HLE_Init() return true end,\n"
    "    function() HLE_Shutdown() end)\n"
    "\n"
    "// --- Phase 2: Execution environment ---\n"
    "RegisterSubsystem(\"Kernel\", {\"Memory\", \"HLE\"},\n"
    "    function() Kernel_Init() return true end,\n"
    "    function() Kernel_Shutdown() end)\n"
    "\n"
    "RegisterSubsystem(\"GPU\", {\"Memory\", \"HLE\", \"Kernel\"},\n"
    "    function() GPU_Init() return true end,\n"
    "    function() GPU_Shutdown() end)\n"
    "\n"
    "// Execute the init chain.\n"
    "RunInitChain()\n";

const char* GetDefaultInitScript() {
    return kDefaultInitScript;
}

// Run the default init chain from the embedded Lua script.
// When Lua is available, the script is executed directly.
// Otherwise, it falls back to the C++ registration.
bool RunDefaultLuaInit(std::string* error) {
#ifdef PCSX5_HAS_LUA
    // TODO: Create Lua state, register bindings, run script.
    // For now, fall through to the C++ path.
    LOG_INFO(General, "Lua init available but not yet wired up; using C++ default init");
#endif
    (void)error;
    return RunDefaultInit(error);
}

} // namespace LuaInit
