# Why Games Don't Boot in PCSX5 — Deep Analysis vs KytyPS5

## Build Command ✅

```powershell
# From the repo root (i:\Personal\Windows\pcsx5):
dotnet build src/ui_csharp/Pcsx5Ui.csproj -c Release

# Output: Build succeeded. 0 Error(s)
# Binary: src/ui_csharp/bin/Release/net9.0-windows/win-x64/pcsx5.dll
```

For the C++ core (pcsx5_core.dll / CLI):
```powershell
cmake -B build -S . -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

---

## What KytyPS5 Does That We Don't (Yet)

KytyPS5 can boot 2D games and UE4/UE5 titles. Their boot pipeline from `emulator.cpp`:

```
1. RuntimeLinker::Load(eboot.bin)
      └─ Parses PS5 SELF/ELF
      └─ Loads PRX deps from sce_module/
      └─ Applies SELF decryption (fake-signed only)
2. FileSystem::Mount → /app0, /download0, /temp0
3. Libs::Init → HLE module stubs (libSceKernel, libSceGnm, etc.)
4. RuntimeLinker::Start → call module init functions
5. Kernel::Execute → call main() → run guest
```

---

## Our Boot Pipeline (core_api.cpp / kernel.cpp)

Our pipeline (also correctly structured):

```
pcsx5_init  → Config, Logging, NID DB, LuaInit::RunDefaultInit()
pcsx5_load  → Kernel::LoadModule() → ELF/SELF parser
pcsx5_run   → std::thread(Kernel::Execute()) + GPU::PumpWindowEvents()
```

The structure is correct. So what's failing?

---

## The 6 Root Causes of Non-Boot

### 🔴 Root Cause 1: Lua Subsystem Registry Is the Gating Item

```cpp
// core_api.cpp:258
if (!LuaInit::RunDefaultInit(&init_error)) {
    LOG_CRITICAL(General, "FATAL: Failed to initialize subsystems: %s", ...);
    return -1;
}
```

If `LuaInit::RunDefaultInit()` fails — and it will fail if `assets/pcsx5_init.lua` is missing,
has a syntax error, or any subsystem `Init()` throws — **pcsx5_init returns -1** and nothing boots.

**KytyPS5** does NOT use Lua. They use a direct C++ `Common::SubsystemsListSingleton`.
Their init never fails silently with a missing script.

> **Action**: Check if `pcsx5_init.lua` exists in the deploy/build output. If not, the game
> never gets past init and `pcsx5_load` is never called.

---

### 🔴 Root Cause 2: SELF Decryption Is Only Implemented for Fake-Signed Titles

Both emulators only support **fake-signed** (fPKG / debug-signed) SELFs.
Retail encrypted SELFs are rejected at `LoadSelf()`:

```cpp
// Our loader/elf.cpp: LoadSelf + ExtractInnerElf
// KytyPS5 same: rejects encrypted retail SELFs
```

If you're testing with retail disc rips or PSN downloads, the binary is encrypted
with per-title keys from Sony's keyset — **neither emulator can decrypt them**.

> **Requirement**: Games must be fake-signed fPKG dumps (jailbroken PS5 scene format).

---

### 🔴 Root Cause 3: HLE Coverage — Missing Critical System Calls

KytyPS5 has implemented many more `libSceKernel` and `libSceGnm` stubs than we have.
The boot sequence calls dozens of syscalls in order. If any is unimplemented and
`strict_imports = true`, boot stops immediately:

```cpp
// core_api.cpp:417
if (g_state.strict_imports && g_state.summary.unresolved_imports > 0) {
    return 3; // boot fails
}
```

Even with `strict_imports = false`, an unimplemented syscall that the game **waits on**
(e.g., a mutex, event flag, or video-output init) will hang the guest thread permanently.

**What KytyPS5 has that our HLE likely doesn't:**
- Full `libSceKernel` thread primitives (pthread, mutex, condvar, event flags)
- `libSceVideoOut` — flip/display queue (without this, no frame ever renders)
- `libSceGnmDriver` — submit/wait GPU command buffers
- `libSceAudioOut` — required for main loop in many titles
- `libSceSystemService` / `libSceNpManager` — required at boot by PS5 SDK

---

### 🔴 Root Cause 4: No param.json / param.sfo → Title ID Missing

```cpp
// core_api.cpp:179
const ConfigService::Config& cfg = ConfigService::EffectiveFor(g_state.title_id);
```

If the game folder has no `sce_sys/param.json` or it's unreadable,
`title_id` is empty → no per-title config → potential path issues later.

KytyPS5 uses `SystemContentParamSfoGetString("TITLE_ID")` with a fallback of `"UNKNOWN"`.
We need the same graceful fallback.

---

### 🔴 Root Cause 5: PRX Module Resolution — sce_module/ Not Found

```cpp
// core_api.cpp:292
Kernel::ConfigureModuleResolver(game_dir, cfg.loader.firmware_modules_dir);
```

PS5 games load multiple `.prx` modules from `sce_module/` inside the game folder.
If `firmware_modules_dir` in config is empty/wrong, dependency loading fails silently
and the main module can't resolve its imports.

**KytyPS5** ships stubs for all required system PRX modules (the HLE library fills them).
We need to either:
- Ship matching stub PRX files, OR
- Have HLE intercept all `dlopen`-equivalent calls and serve stubs

---

### 🟡 Root Cause 6: GPU Vulkan Init Fails Silently

```cpp
// pcsx5_run:374-387
while (!guest_done) {
    GPU::PumpWindowEvents();
    GPU::PollEvents();
    ...
}
```

If the Vulkan device creation fails (no Vulkan 1.3-capable GPU, missing validation layers,
wrong swapchain format), the GPU subsystem may return `HasWindow() == false` immediately.
The guest thread then runs but its first `sceGnmSubmitCommandBuffers` call will fail or hang.

**KytyPS5 requirement**: "A Vulkan 1.3-capable GPU with current drivers" — same as us.

---

## Comparison Table

| Feature | KytyPS5 | PCSX5 | Status |
|---------|---------|-------|--------|
| SELF/ELF loader | ✅ | ✅ | OK |
| Fake-signed fPKG | ✅ | ✅ | OK |
| Retail decryption | ❌ | ❌ | Neither works |
| Subsystem init | C++ direct | Lua-driven | ⚠️ Lua must be present |
| libSceKernel HLE | ✅ Extensive | Partial | 🔴 Gap |
| libSceVideoOut | ✅ | Partial | 🔴 Critical |
| libSceGnmDriver | ✅ | Partial | 🔴 Critical |
| libSceAudioOut | ✅ | Partial | 🟡 |
| PRX resolution | ✅ | ✅ | OK if paths right |
| param.sfo parsing | ✅ | ✅ | OK |
| Vulkan 1.3 | ✅ | ✅ | OK if driver current |

---

## Immediate Diagnostic Steps

### Step 1 — Check if init.lua exists
```powershell
ls src/ui_csharp/bin/Release/net9.0-windows/win-x64/assets/
# Must contain: pcsx5_init.lua, nid_db.txt
```

### Step 2 — Run standalone CLI with a fake-signed fPKG game
```powershell
# After cmake build:
.\build\pcsx5.exe --boot "D:\Games\YourGame\eboot.bin" --log-level=debug 2> boot.log
type boot.log
```
Look for the **last log line before failure**. It will point to:
- `pcsx5_init failed` → Lua missing or subsystem init fail
- `Failed to load target module` → ELF/SELF issue
- Guest hangs silently → missing HLE syscall (check for `UNIMPLEMENTED:` lines)

### Step 3 — Check unresolved imports
```powershell
# In the compat summary JSON / log look for:
grep "UNIMPL\|unresolved\|UNRESOLVED" boot.log
```

### Step 4 — Check Vulkan
```powershell
# Install vulkaninfo from Vulkan SDK:
vulkaninfo --summary | findstr "apiVersion"
# Must show: apiVersion = 1.3.x
```

---

## Summary

KytyPS5 boots games primarily because they have:
1. **No Lua dependency** — C++ init never fails silently
2. **Much broader HLE coverage** — libSceVideoOut, libSceGnmDriver, full kernel threads
3. **Years of PS5 SDK reverse engineering** inherited from the original Kyty + shadPS4 cross-reference

Our architecture is sound. The primary gaps are **HLE completeness** and
**verifying the Lua init chain works at runtime**. The UI + thread isolation work done today
is ready — we just need the C++ core to get further in the boot sequence.
