# Phase 0 Migration Ledger

**Status:** Draft — populated from source/build reconnaissance; characterization remains pending.

| Current area | Evidence | Rebuild disposition | Phase 1+ destination | Conditions before use |
|---|---|---|---|---|
| CMake root/build scripts | MSVC-only guard, MASM dispatcher, global Windows links | Redesign | `build/`, CMake presets, per-target dependency declarations | Windows + Linux configure in CI with no network-only dependency resolution |
| Common platform layer | `src/common/platform/platform_win32.cpp` | Redesign | `runtime/contracts`, `runtime/windows`, `runtime/posix`, `runtime/apple`, `runtime/android` | Contract tests for paths, timing, memory pages, threads and diagnostics |
| Guest memory and fault system | Win32 virtual-memory calls and VEH/SEH are documented in ADR-005/audit | Characterize then redesign | `core/memory`, `runtime/memory`, `runtime/faults` | Behavioral tests run against old and new implementations |
| Thread/TLS/dispatcher | Windows TEB manipulation, `dispatcher.asm`, Win64 ABI | Characterize then redesign | `core/kernel`, `execution/dispatch`, runtime thread contract | POSIX/macOS feasibility experiments; no code copied blindly |
| Direct execution | Current x86-64 guest execution path; no generic interpreter/JIT | Characterize then contain | `execution/direct-x64` | Trap, signal/fault, state, and cache-invalidation contracts documented |
| Future interpreter | Does not currently exist | New implementation | `execution/interpreter` | Instruction/state differential tests and deterministic trace schema |
| Future ARM64 JIT | Does not currently exist | New implementation | `execution/ir`, `execution/jit-arm64` | Interpreter oracle, W^X code-cache experiments, per-platform ABI validation |
| Vulkan / GCN / SPIR-V | `src/gpu/`, Vulkan backend, shader translator, GPU smoke/golden assets | Retain as reference; extract HAL | `graphics/hal`, `graphics/vulkan`, `graphics/shader` | Render replay/golden tests independent of frontend and windowing |
| Apple graphics | No Metal; MoltenVK not integrated | New integration | `graphics/moltenvk` | Required-feature matrix and replay validation; native Metal only after evidence |
| Input and audio | XInput, DualSenseWindows, WASAPI/XAudio plus SDL components | Reuse behavior only; redesign adapters | `runtime/input`, `runtime/audio` | Capability model; platform backends replace no core code |
| WPF shell | `src/ui_csharp/` includes WPF and Windows integrations | Reference/exclude from initial core | `frontend/desktop` | Select shared UI after headless core works; no frontend chosen by assumption |
| IPC / public core API | `src/core_api*`, IPC server | Characterize then redesign | `api/` | Stable C/C++ ABI and frontend process model decided |
| Tests/replays/golden assets | CTest suite, replay tools, GPU golden files | Reuse as characterization corpus | `tests/conformance`, `tests/regression`, `tools/trace` | Provenance and legal-data review; no proprietary game data committed |
| Third-party libraries | GLFW, Vulkan-Headers, Lua, Opus, ATRAC9, DualSenseWindows | Review individually | `third_party/` or package manifest | License, platform support, pinning, and supply-chain decision recorded |

## Immediate next action

P0.5 begins by inventorying the current memory, TLS, fault and direct-execution test coverage, then documenting the smallest portable contracts required to reproduce that behavior. No implementation migration starts before this characterization gate.
