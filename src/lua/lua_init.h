// pcsx5 Lua-driven subsystem lifecycle.
//
// Provides a `SubsystemRegistry` that:
//   1. Accepts subsystem registrations (name, dependencies, init, teardown)
//   2. Resolves initialization order via topological sort
//   3. Initializes in dependency order, tearing down on first failure
//   4. Tears down all subsystems in reverse order
//
// Lua bindings expose the registry to a Lua state so the init chain can be
// driven from a script (mirroring Kyty's `Emulator.cpp` + `LuaFunc::Init`
// pattern).  When Lua is not available, the C++ `RunDefaultInit()` path
// provides the same behaviour.
//
// Usage from C++:
//   LuaInit::RunDefaultInit(&error);   // register + init all subsystems
//   ... emulation ...
//   LuaInit::SubsystemRegistry::Instance().TeardownAll();
//
// Usage from Lua (when PCSX5_HAS_LUA is defined):
//   RegisterSubsystem("Memory", {}, function() return Memory_Init() end, function() end)
//   RegisterSubsystem("HLE", {"Memory"}, function() return HLE_Init() end, function() end)
//   RunInitChain()
//   ... emulation ...
//   RunTeardownChain()
//
// The default init chain is:
//   ConfigService → Diagnostics → Logging → Memory → HLE → Kernel → GPU

#pragma once

#include <string>
#include <vector>
#include <functional>
#include <unordered_map>

// Forward-declare Lua types so includers don't need Lua headers.
struct lua_State;

namespace LuaInit {

// ---------------------------------------------------------------------------
// Subsystem — a single emulated subsystem with its lifecycle callbacks.
// ---------------------------------------------------------------------------
struct Subsystem {
    std::string name;
    std::vector<std::string> dependencies;  // must be initialized before this one
    std::function<bool()> init;              // returns true on success, false on failure
    std::function<void()> teardown;          // called in reverse order during shutdown
};

// ---------------------------------------------------------------------------
// SubsystemRegistry — manages registration, dependency resolution, and
// lifecycle of all subsystems.  Thread-safe for registration; init/teardown
// are single-threaded.
// ---------------------------------------------------------------------------
class SubsystemRegistry {
public:
    // Register a subsystem.  Duplicate names are silently ignored (logged).
    void Register(const Subsystem& sub);

    // Resolve the initialization order via topological sort (Kahn's algorithm).
    // Returns false if a cycle or missing dependency is detected.
    bool ResolveOrder(std::string* error = nullptr);

    // Initialize all registered subsystems in dependency order.
    // On first failure, tears down everything that was already initialized.
    bool InitializeAll(std::string* error = nullptr);

    // Tear down all subsystems in reverse initialization order.
    void TeardownAll();

    // Query whether a subsystem has been initialized.
    bool IsInitialized(const std::string& name) const;

    // Get the resolved init order (valid after ResolveOrder).
    const std::vector<std::string>& InitOrder() const { return m_init_order; }

    // Get all registered subsystem names.
    std::vector<std::string> RegisteredNames() const;

    // Singleton access.
    static SubsystemRegistry& Instance();

private:
    std::unordered_map<std::string, Subsystem> m_subsystems;
    std::vector<std::string> m_init_order;
    std::vector<std::string> m_initialized;
    bool m_resolved = false;

    bool TopoSort(std::string* error);
};

// ---------------------------------------------------------------------------
// Default init chain — registers the standard PCSX5 subsystems and
// initializes them.  Mirrors the current main.cpp boot sequence.
// ---------------------------------------------------------------------------
bool RunDefaultInit(std::string* error = nullptr);

// ---------------------------------------------------------------------------
// Lua bindings — expose the registry to a Lua state.
// When Lua is not available (PCSX5_HAS_LUA undefined), these are no-ops.
// ---------------------------------------------------------------------------

// Register the Lua C functions on the given state.  Safe to call multiple
// times; it overwrites any previous registration.
void RegisterLuaBindings(void* lua_state);

// Run the Lua init script stored in `script_text`.  The script should call
// `RegisterSubsystem(name, deps, init_fn, teardown_fn)` for each subsystem,
// then call `RunInitChain()`.
bool RunLuaScript(void* lua_state, const char* script_text, const char* script_name = "init", std::string* error = nullptr);

// Convenience: run the default init chain from a Lua script embedded in
// the binary.  Falls back to the C++ chain if Lua is unavailable.
bool RunDefaultLuaInit(std::string* error = nullptr);

// Get the global Lua state (nullptr if Lua is not initialized).
lua_State* GetLuaState();

// Close the global Lua state.  Safe to call even if Lua was never initialized.
void ShutdownLua();

} // namespace LuaInit
