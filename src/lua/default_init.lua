-- pcsx5 default init script (Lua DSL)
--
-- This script declares the full subsystem lifecycle for PCSX5.
-- When Lua is available, it is executed directly; otherwise the C++
-- path (RunDefaultInit) provides the same behaviour.
--
-- API:
--   RegisterSubsystem(name, deps_table, init_fn, teardown_fn)
--       name    : string — unique subsystem identifier
--       deps    : table of strings — must be initialized before this one
--       init_fn : function() -> boolean — returns true on success
--       teardown_fn : function() — no arguments, no return value
--
--   RunInitChain()
--       Resolves dependencies via topological sort, initializes all
--       subsystems in order, tears down on first failure.
--
--   RunTeardownChain()
--       Tears down all subsystems in reverse initialization order.
--
--   GetInitOrder()
--       Returns the resolved init order as a table of strings.

-- ========================================================================
-- Phase 0: Infrastructure
-- ========================================================================

-- ConfigService — first, so every other subsystem can read its settings.
RegisterSubsystem("ConfigService", {},
    function()
        -- Initialize the global configuration service.
        -- The config dir can be overridden by CLI flags before this runs.
        ConfigService_Init("pcsx5_config")
        return true
    end,
    function()
        -- ConfigService is persistent; no teardown needed.
    end
)

-- Diagnostics — install crash handler early so any later crash is captured.
RegisterSubsystem("Diagnostics", {"ConfigService"},
    function()
        local bundle_dir = ConfigService_GetCrashDir()
        if bundle_dir == "" or bundle_dir == nil then
            bundle_dir = "pcsx5_crash"
        end
        Diagnostics_Init(bundle_dir)
        return true
    end,
    function()
        -- Diagnostics is persistent; no teardown needed.
    end
)

-- ========================================================================
-- Phase 1: Core emulation
-- ========================================================================

-- Logging — apply config to the logging subsystem.
RegisterSubsystem("Logging", {"ConfigService"},
    function()
        local log_cfg = ConfigService_GetLoggingConfig()
        if log_cfg.file_path ~= "" and log_cfg.file_path ~= nil then
            LogConfig_SetFileOutput(log_cfg.file_path, log_cfg.file_append)
        end
        if log_cfg.json_output then
            LogConfig_SetJsonOutput(true)
        end
        for i = 0, 5 do
            LogConfig_SetLevel(i, log_cfg.min_level)
        end
        return true
    end,
    function()
        -- Logging is persistent; no teardown needed.
    end
)

-- Memory — guest virtual memory manager.
RegisterSubsystem("Memory", {"ConfigService"},
    function()
        return Memory_Init()
    end,
    function()
        Memory_Shutdown()
    end
)

-- HLE — high-level emulation (symbol registry, thunk allocator).
RegisterSubsystem("HLE", {"ConfigService", "Memory"},
    function()
        return HLE_Init()
    end,
    function()
        HLE_Shutdown()
    end
)

-- ========================================================================
-- Phase 2: Execution environment
-- ========================================================================

-- Kernel — thread management, syscall dispatch, guest execution.
RegisterSubsystem("Kernel", {"Memory", "HLE"},
    function()
        return Kernel_Init()
    end,
    function()
        Kernel_Shutdown()
    end
)

-- GPU — rendering backend (Vulkan primary, OpenGL secondary).
RegisterSubsystem("GPU", {"Memory", "HLE", "Kernel"},
    function()
        return GPU_Init()
    end,
    function()
        GPU_Shutdown()
    end
)

-- ========================================================================
-- Execute the init chain
-- ========================================================================

RunInitChain()
