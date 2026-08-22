# PCSX5 Architecture

## 1. Architectural goals

PCSX5 should be:

- modular
- testable without the UI
- observable
- incrementally extensible
- compatible with multiple platform backends where practical
- safe to refactor
- able to update itself from a trusted release channel
- capable of running old and new compatibility code without turning the core into game-specific spaghetti

## 2. Process model

The preferred runtime process is:

```text
pcsx5.exe
  WPF shell
    |
    +-- Pcsx5.Interop.dll / managed interop
          |
          v
      pcsx5_core.dll
          |
          +-- EmulatorSession
                |
                +-- Scheduler / Clock
                +-- CPU
                +-- Memory / MMU
                +-- Kernel / Syscalls
                +-- HLE Services
                +-- Loader / Modules
                +-- GPU
                +-- Audio
                +-- Input
                +-- Debug / Trace
                +-- Save State
```

A headless `pcsx5_core_tests` target must be able to instantiate the core without WPF.

## 3. Core module boundaries

### Emulator Runtime

Owns lifecycle, session state, pause/resume, timing, shutdown, configuration snapshots, and subsystem wiring.

### CPU

Owns guest CPU state and execution semantics.

It must not own:
- WPF state
- GPU resources
- audio devices
- title-specific compatibility switches

### Memory / MMU

Owns guest virtual/physical memory mapping and access validation.

All subsystems use the memory API rather than arbitrary host pointers.

### Kernel / HLE

Owns guest-facing operating-system behavior.

Kernel services should expose stable internal service interfaces and use explicit guest handles/IDs.

### Loader

Owns executable/module parsing, mapping, relocations, entry points, and module lifecycle.

### GPU

Owns:
- guest GPU command interpretation
- resource tracking
- shader translation/recompilation
- Vulkan execution
- GPU synchronization

Do not mix guest command parsing with Vulkan resource management.

Recommended split:

```text
GPU Guest API
  -> Command Decoder
  -> IR / normalized command representation
  -> Resource Tracker
  -> Shader Translator
  -> Vulkan Backend
```

### Audio

Recommended split:

```text
Guest audio API
  -> audio service / mixing
  -> decoder abstraction
  -> platform output
```

ATRAC9, Bink audio, and other decoders must be wrappers rather than spread through emulator code.

### Input

Recommended split:

```text
Guest input API
  -> logical controller state
  -> device abstraction
  -> Win32 / HID / XInput / DualSense backend
```

### Debug / Trace

Debugging must be a first-class subsystem.

Provide:
- structured logging
- category filters
- trace buffers
- guest PC tracking
- syscall tracing
- memory watchpoints where feasible
- GPU command capture where feasible
- crash context
- deterministic test traces

## 4. Public native ABI

The C# UI must consume a C ABI.

Example shape:

```cpp
extern "C" {
    PCS5_API pcsx5_result pcsx5_create(const pcsx5_config*, pcsx5_handle*);
    PCS5_API pcsx5_result pcsx5_load_game(pcsx5_handle, const char* path);
    PCS5_API pcsx5_result pcsx5_start(pcsx5_handle);
    PCS5_API pcsx5_result pcsx5_pause(pcsx5_handle);
    PCS5_API pcsx5_result pcsx5_stop(pcsx5_handle);
    PCS5_API pcsx5_result pcsx5_destroy(pcsx5_handle);
}
```

The exact ABI must be designed and versioned before implementation.

## 5. Assembly policy

MASM is allowed for the guest dispatcher only if profiling and ABI constraints demonstrate that it provides a material benefit.

Assembly must:
- have a C/C++ callable contract
- document register preservation
- document stack requirements
- have a portable/test fallback if practical
- have focused tests

Do not move arbitrary emulator logic into assembly.

## 6. Configuration

Configuration should be represented by typed internal structures.

JSON is the persistence/serialization format, not the internal configuration model.

Separate:
- global configuration
- per-game configuration
- graphics configuration
- input configuration
- debug configuration
- compatibility/workaround configuration

## 7. Compatibility profiles

Do not scatter checks such as:

```cpp
if (game_id == "...") { ... }
```

through core code.

Instead:

```text
CompatibilityDatabase
  -> GameProfile
      -> known workarounds
      -> required quirks
      -> shader overrides
      -> timing overrides
      -> module/service overrides
```

Every workaround needs evidence and a test.

## 8. Threading

Default to the smallest concurrency model that is correct.

Candidate domains:
- emulation thread
- GPU submission/backend thread if needed
- audio thread
- UI thread

Avoid arbitrary thread pools.

Document synchronization ownership. Never use locks to hide architectural coupling.

## 9. Save states

Save state must be treated as a versioned serialization format.

It must include:
- format version
- emulator build/version
- relevant configuration identity
- CPU state
- memory state
- kernel state
- device state
- GPU state that is required for correctness
- timing state

Never serialize raw C++ object memory as the long-term save-state format.

## 10. Testing architecture

```text
tests/
  unit/
  cpu/
  memory/
  kernel/
  loader/
  gpu/
  audio/
  input/
  integration/
  compatibility/
  regression/
  fixtures/
```

Tests should progress from deterministic unit semantics to end-to-end title boot tests.

## 11. Error handling

Use explicit result/error types internally.

Errors should carry:
- subsystem
- stable error code
- human message
- optional context
- optional guest address / PC
- optional underlying platform error

Never swallow errors.

## 12. Architecture evolution rule

Architecture is allowed to change when evidence shows the current boundary is wrong.

A durable architectural change requires an ADR in `docs/adr/`.

