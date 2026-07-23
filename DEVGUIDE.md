# Developer Guide — PCSX5

This guide covers the emulator's architecture, how to extend it with
new backends or HLE modules, how to port to a new platform, and how
to write and run tests.

---

## Architecture Overview

PCSX5 uses a **native x86-64 execution + High-Level Emulation (HLE)**
model. The PS5 guest code runs directly on the host CPU (no dynarec).
System calls and library functions are intercepted and emulated in C++.

```
┌─────────────────────────────────────────────────────────┐
│                    Emulator Frontend                     │
│  pcsx5_cli.exe (CLI) / pcsx5.exe (WPF GUI)              │
├─────────────────────────────────────────────────────────┤
│                      Core API                            │
│  core_api.h: Init / Load / Run / Stop / Shutdown         │
├──────────┬──────────┬──────────┬──────────┬─────────────┤
│  Kernel  │   HLE    │  Loader  │  Memory  │   GPU/VK    │
│ threads  │ libSce*  │ ELF/SELF │ Virtual  │ Vulkan +    │
│ sync     │ stubs    │ PKG/PFS  │ VEH      │ shader xl8  │
│ syscalls │          │ modules  │ large pg │ GAL/VRR/FSR │
├──────────┴──────────┴──────────┴──────────┴─────────────┤
│                Abstraction Layers (pluggable)             │
│  AAL (Audio)  │  IAL (Input)  │  PAL (Platform)          │
│  WASAPI/XA2   │  XInput/DS    │  Win32 VEH/VirtualAlloc  │
│  WaveOut/SDL  │  GLFW/SDL     │  mmap/pthread (future)   │
│  Pacing (null)│  Null         │                           │
├─────────────────────────────────────────────────────────┤
│              Video Decoder (VDAL, pluggable)              │
│  Bink2 DLL │ CRI USM │ FFmpeg │ Null                     │
└─────────────────────────────────────────────────────────┘
```

### Key Concepts

- **Identity-mapped guest memory**: guest_addr == host VA. Guest code
  runs directly with host pointers. No shadow page tables.
- **VEH trapping**: access violations from guest code (TLS fs:0,
  demand-commit pages) are caught by a vectored exception handler.
- **Batch submission**: all draws in one frame are recorded into a
  single Vulkan command buffer and submitted once at present time.

---

## How to Add a New HLE Symbol

PS5 libraries export functions identified by **NID** (a 64-bit hash of
the mangled name).  HLE symbols are registered in `src/hle/`.

### Step 1: Find the NID

```bash
# Search the NID database by name
grep "sceMyFunction" assets/nid_db.txt
# Output: NID64<TAB>module<TAB>name
```

### Step 2: Register the symbol

In the appropriate `lib*.cpp` file (e.g. `libmymodule.cpp`):

```cpp
namespace HLE {

void RegisterLibMyModule() {
    RegisterSymbol("libSceMyModule", "sceMyFunction", [](const GuestArgs& args) -> u64 {
        u32 handle = static_cast<u32>(args.arg1);
        u32 param  = static_cast<u32>(args.arg2);

        LOG_DEBUG(HLE, "sceMyFunction(handle: %u, param: %u)", handle, param);

        // Read guest memory:
        u8 buf[64];
        Memory::ReadBuffer(args.arg3, buf, sizeof(buf));

        // Write guest memory:
        Memory::WriteBuffer(args.arg1, &result, sizeof(result));

        return 0; // SCE_OK
    });

    // Also register by NID for modules that import by hash:
    RegisterSymbol("libSceMyModule", "ABCDEF1234567890", /* same lambda */);
}

} // namespace HLE
```

### Step 3: Call Register from the HLE initializer

In `src/hle/hle.cpp`'s `HLE::Initialize()`:

```cpp
RegisterLibMyModule();  // <-- add this
```

The auto-stub system (`RegisterNidDbStubs`) catches every NID in
the database, so unregistered symbols already log-and-return-0.
You only need to write handlers for symbols a real game calls.

### Calling Convention

Guest arguments arrive in `GuestArgs`:
| Field   | Source          |
|---------|-----------------|
| arg1    | rdi (SysV ABI)  |
| arg2    | rsi             |
| arg3    | rdx             |
| arg4    | rcx             |
| arg5    | r8              |
| arg6    | r9              |
| stack_args | stack (8th+ arg) |

XMM return values (floats/doubles) come via `GetIncomingXmm0()` etc.
Pointers are guest addresses (identity-mapped, so cast to host pointer).

---

## How to Add a New Abstraction-Layer Backend

Each layer has an interface header and a factory.  Adding a new backend
means implementing the interface and registering it in the factory.

### Example: New Audio Backend

**Step 1**: Subclass `AudioDevice` in its own .cpp file:

```cpp
// src/hle/audio/sdl_audio_device.cpp
#include "audio_device.h"

class SdlAudioDevice : public AudioDevice {
public:
    bool Open(const AalFormat& format) override { /* ... */ }
    void Close() override { /* ... */ }
    bool IsOpen() const override { return m_open; }
    AalCaps GetCaps() const override { /* ... */ }
    void Output(const u8* data, uint32_t frame_count) override { /* ... */ }
    void Reset() override { /* ... */ }
    float GetVolume() const override { return m_volume; }
    uint32_t GetLatencyFrames() const override { /* ... */ }

private:
    bool m_open = false;
    float m_volume = 1.0f;
    AalFormat m_format{};
};

AudioDevice* CreateSdlAudioDevice() { return new SdlAudioDevice(); }
```

**Step 2**: Add to the factory in `audio_device.cpp`:

```cpp
// Forward declaration
AudioDevice* CreateSdlAudioDevice();

AudioDevice* AudioDevice::Create(AalBackendType type) {
    case AalBackendType::SDL:
    case AalBackendType::Auto:  // also add to auto-probe chain
        auto* d = CreateSdlAudioDevice();
        if (d) return d;
        // fall through
}
```

**Step 3**: Add the .cpp file to `CMakeLists.txt` under `pcsx5_core`.

**Step 4**: Users select it via config: `audio.backend = 4` (SDL).

### Interface Checklist

Each backend must handle:
- **Init failure**: return false/nullptr; the factory falls through
- **Thread safety**: Output() may be called from any thread
- **Config props**: volume, buffer size, etc. from `AalConfig`

---

## How to Port to a New Platform

The Platform Abstraction Layer (PAL) in `src/common/platform/` is the
only OS-dependent code.  To port to Linux:

### 1. Implement PAL

Create `src/common/platform/platform_linux.cpp`:

| Function | Win32 impl | Linux impl |
|----------|-----------|------------|
| `VirtualAlloc` | `VirtualAlloc` | `mmap(NULL, size, PROT_*, MAP_PRIVATE\|MAP_ANONYMOUS, -1, 0)` |
| `VirtualFree` | `VirtualFree` | `munmap` |
| `VirtualProtect` | `VirtualProtect` | `mprotect` |
| `GetCurrentThreadId` | `GetCurrentThreadId` | `gettid` / `pthread_self` |
| `LoadLibrary` | `LoadLibraryW` | `dlopen` |
| `GetProcAddress` | `GetProcAddress` | `dlsym` |
| `InstallFaultHandler` | `AddVectoredExceptionHandler` | `sigaction(SIGSEGV, ...)` |
| `QueryCpuFeatures` | `__cpuid` | `/proc/cpuinfo` parsing |

### 2. Replace Win32 Headers

Every `#include <windows.h>` in the codebase is behind `#ifdef _WIN32`
guards in the PAL-using files.  New files should use `Platform::*`
functions instead of raw Win32 calls.

### 3. Update CMakeLists.txt

```cmake
if(WIN32)
    set(PLATFORM_SRC src/common/platform/platform_win32.cpp)
elseif(UNIX)
    set(PLATFORM_SRC src/common/platform/platform_linux.cpp)
endif()
```

### 4. Build System

- Replace MASM (`dispatcher.asm`) with equivalent inline asm or
  a pure-C++ calling-convention bridge for Unix x86-64.
- GLFW already cross-platform (handles window creation + input).
- Vulkan backend is already cross-platform (uses volk-style loader).

---

## How to Write and Run Tests

### Unit Tests

Tests live in `tests/` and use the `add_test(NAME ... COMMAND ...)`
pattern in CMakeLists.txt.  Link against `pcsx5_core` or compile
the source files directly.

**Example** — creating a test for the PS5 PKG parser:

```cpp
// tests/pkg_ps5_tests.cpp
#include "loader/pkg_ps5.h"
#include <cassert>
#include <cstdio>

int main() {
    // Synthetic PS5 PKG header (magic 0x7F464948)
    const u8 kMinimalPs5Pkg[] = {
        0x7F, 0x46, 0x49, 0x48,  // magic
        // ... minimal valid header ...
    };
    // Write to temp file
    FILE* f = fopen("test.pkg", "wb");
    fwrite(kMinimalPs5Pkg, 1, sizeof(kMinimalPs5Pkg), f);
    fclose(f);

    Loader::PkgPs5Image image;
    bool ok = Loader::ParsePkgPs5("test.pkg", image);
    assert(ok);
    assert(!image.content_id.empty());
    std::printf("PASS: PS5 PKG parse\n");
    return 0;
}
```

Register in CMakeLists.txt:
```cmake
add_executable(pkg_ps5_tests tests/pkg_ps5_tests.cpp
    src/loader/pkg_ps5.cpp
    src/common/log.cpp
    src/common/crypto.cpp
)
target_include_directories(pkg_ps5_tests PRIVATE src)
add_test(NAME pkg_ps5 COMMAND pkg_ps5_tests)
```

### Running Tests

```powershell
# Build and run all tests
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure

# Run a single test
ctest --test-dir build -C Debug -R pkg_ps5
```

### Guest Test ELFs

Tests in `tests/test_elf/` are compiled with Clang as freestanding
x86-64 ELFs and run through the emulator's full loading + execution
pipeline.  They test syscalls, TLS, and basic HLE.

```powershell
# Build guest ELFs (requires Clang)
cmake --build build --config Debug --target guest_test_elfs

# Run a guest test
./build/bin/Debug/pcsx5_cli.exe --strict-imports build/test_elf/test_guest.elf
```

### Golden Image Tests

PM4 capture/replay tests (`tests/pm4_golden`) record GPU command
streams and compare rendered output against reference PNGs:

```powershell
# Record PM4 stream
set PCSX5_PM4_CAPTURE=capture_dir/
./pcsx5_cli.exe game.elf
# Replay + compare
./build/bin/Debug/pm4_golden_tests
```

---

## Code Style

- **C++20** with modern idioms (RAII, structured bindings, concepts)
- **MSVC** `/W4 /WX` — warnings are errors; suppress via cast not pragma
- **Naming**: `CamelCase` types, `snake_case` functions and variables,
  `kCamelCase` constants, `g_` globals, `m_` members, `t_` thread-locals
- **Headers**: `#pragma once`, minimal includes, forward-declare when possible
- **SEH**: `__try/__except` only in POD-only leaf functions (C2712)
- **No exceptions**: guest exceptions are handled via the VEH, not C++ `throw`
- **No RTTI**: compile with `/GR-`

---

## Debugging Tips

### VEH Recursion

If the emulator crashes silently, check `veh_recursion_depth` in the
heartbeat log.  Values above 8 indicate the VEH is recursing through
TLS emulation or demand-commit and the guard kicked in.

### HLE Tracing

Enable per-call tracing:
```json
{ "hle": { "trace_calls": true, "trace_capacity": 1024 } }
```

### Crash Bundle

On a guest crash, the emulator writes `crash_log.txt` with register
state, stack scan, and the instruction bytes at RIP.  Set
`crash.bundle_dir` to collect full minidumps.

### VK Validation

Set `gpu.debug = true` in config to enable Vulkan validation layers.
Must have the Vulkan SDK installed.
