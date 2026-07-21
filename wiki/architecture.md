# Architecture

## Subsystem map

| Subsystem | Path | Role |
|---|---|---|
| Loader | `src/loader/` | ELF/self parsing, module loading, relocation, import linking; PKG/PFS containers, param.json, module resolver, module graph |
| Kernel | `src/kernel/` | VEH setup (`kernel.cpp`), syscall table (`syscalls.cpp`), fd table, `thread.cpp` shim |
| CPU core | `src/cpu/cpu.cpp` | Guest thread registry + lifecycle; syscall adapters |
| HLE | `src/hle/` | High-level emulation of userland libraries (`hle.cpp`, `libkernel.cpp`, ...) |
| GPU | `src/gpu/` | Graphics emulation |
| Memory | `src/memory/` | Guest address space (reserve/commit, read/write helpers) |
| UI | C# WPF (`pcsx5.exe`) | Frontend; hosts the native core in-process via `pcsx5_core.dll` (`CoreBridge` P/Invoke). `pcsx5_cli.exe` is the thin native CLI shim over the same core. |
| Compat | `src/compat/` | Per-title compatibility database |
| Diagnostics | `src/diagnostics/` | Crash handler, minidump bundles |

## Loader: containers and module files

What a dumped game actually contains, top to bottom:

- **PKG** (`src/loader/pkg.h/.cpp`) — Prospero/Orbis PKG container parsing
  and extraction. Fake-signed (fPKG) entries are decrypted via the public
  scene passcode derivation (double-SHA256 key schedule, AES-128-CBC per
  entry); retail NPDRM entries are detected and skipped. Entry ids map to
  conventional file names via `PkgEntryPath` (e.g. `0x1000` →
  `param.sfo`); unknown ids extract as `entries/0x%04X.bin` (PS4-format
  `param.sfo` entries are extracted raw — there is no SFO parser).
- **PFS** (`src/loader/pfs.h/.cpp`) — read-only PFS image parser
  (superblock magic `20130315`, 32-bit inodes, direct + single-indirect
  blocks, dirent walking) with `ExtractFile`/`ExtractAll`.
  Encrypted/64-bit/compressed modes are detected and refused with
  warnings. No guest-visible mount yet.
- **param.json** (`src/loader/param_json.h/.cpp`) — PS5 games ship
  `sce_sys/param.json` (not param.sfo): titleId, contentId, versions,
  category, attributes, localized titles. Feeds the compat DB and UI.

### PRX module resolution

`src/loader/module_resolver.h/.cpp` maps a guest module name to a real
file on disk: guest directories are stripped and `<name>.prx`/`.sprx` are
probed case-insensitively, searching the game's `sce_module/` first, then
the firmware directory given by the `loader.firmware_modules_dir` config
option (`src/config/config.h:81`). The resolver is consulted by
`Kernel::ConfigureModuleResolver` / DT_NEEDED handling in
`src/kernel/kernel.cpp` and by `sceKernelLoadStartModule` in
`src/hle/libkernel.cpp`. Recursive PRX auto-loading is still a TODO
(src/kernel/kernel.cpp:~288).

### Module dependency graph

`src/loader/module_graph.h/.cpp` (`ModuleGraph`) computes a deterministic
topological load order (Kahn's algorithm) from module dependencies,
reports cycles via Tarjan SCC, and reports missing dependencies (those
resolve to HLE). Not yet wired into the Kernel load flow.

### NID database

Friendly names for NIDs come from a built-in table plus a runtime-loaded
text database: `Common::LoadNidDatabase(path)` (src/common/nid.cpp:246)
reads `NID<TAB>module<TAB>name` lines; file entries override built-ins.
The shipped seed is `assets/nid_db.txt` (currently the 8 built-in
entries).

## Native execution model

Guest and host are both x86_64, so there is no JIT: guest code runs natively
(`src/cpu/cpu.h:5-19`). Two interception mechanisms make this work:

### Syscalls via VEH

1. After loading an executable module, the kernel scans executable segments for
   the `syscall` opcode (`0F 05`) and binary-patches each one to
   `INT3; NOP` (`CC 90`) — `src/kernel/kernel.cpp:139-168`.
2. A vectored exception handler is installed at startup with
   `AddVectoredExceptionHandler(1, VectoredExceptionHandler)` —
   `src/kernel/kernel.cpp:76`.
3. On `EXCEPTION_BREAKPOINT`, the VEH checks the bytes at RIP for the
   `CC 90` signature, takes the syscall number from `RAX`, and dispatches via
   `Kernel::HandleSyscall(syscall_number, context)`; the return value is
   written back to `context->Rax` — `src/kernel/kernel.cpp:500-505`.
4. The table itself is a 512-entry `SyscallHandler` array in
   `src/kernel/syscalls.cpp:64`; unimplemented numbers return `-ENOSYS`
   (`src/kernel/syscalls.cpp:177-187`). Args follow the FreeBSD/PS5
   convention: `rdi, rsi, rdx, r10, r8, r9`.

### HLE thunks

Imported library functions never exist as guest code. Instead
`HLE::RegisterSymbol` (src/hle/hle.cpp:309) allocates a 22-byte executable
thunk via `CreateThunk` (src/hle/hle.cpp:280):

```
mov r10, symbol_id
mov rax, HleCommonDispatcher
jmp rax
```

The guest's import table is patched to point at the thunk VA. The common
dispatcher (assembly, `dispatcher.asm`) bridges SysV → Windows ABI and calls
`HleDispatch` (src/hle/hle.h:115), which looks up the handler by symbol id,
records stats, and invokes it. Calling back *into* guest code (callbacks,
thread entry points) goes through `InvokeGuestFunction`
(src/hle/hle.h:124), which saves/restores a per-thread host stack pointer
(`SetHostStackPointer` / `GetHostStackPointer`, `__declspec(thread)`).

## Thread model (consolidated)

`CpuCore` (src/cpu/cpu.cpp) is the single owner of guest thread state. There
is no parallel thread registry.

- Registry: `std::unordered_map<u64, std::unique_ptr<GuestThread>> g_threads`,
  max 1024 threads (src/cpu/cpu.cpp:22-25).
- `CpuCore::CreateThread` (src/cpu/cpu.cpp:190) builds a `GuestThread`
  (entry, guest stack, TLS, `argument` for rdi, wake event), then creates a
  Win32 thread (1 MB host stack) running `GuestThread::ThreadEntrypoint`.
- `GuestThread::ThreadEntrypoint` (src/cpu/cpu.cpp:115) sets the
  thread-local current id, calls `SetHostStackPointer`, then runs the guest
  entry via `InvokeGuestFunction(entry, argument, 0, 0)`. On return,
  `HandleThreadExit` fires `on_exit` and frees detached threads.
- Join/detach/wake: `CpuCore::JoinThread` (waits on the host handle, frees
  guest stack/TLS), `DetachThread` (thread self-cleans on exit),
  `SuspendCurrentThread`/`WakeThread` (per-thread auto-reset event).
- `Kernel::CreateThread`/`CreateThreadEx` in src/kernel/thread.cpp are a
  thin shim over `CpuCore` keeping the historical signatures
  (src/kernel/thread.cpp:34-51).

### scePthreadCreate flow

`libkernel.cpp` scePthreadCreate (src/hle/libkernel.cpp:1123) → allocates a
guest stack → `Kernel::CreateThreadEx` (src/hle/libkernel.cpp:1158) →
`CpuCore::CreateThread` → Win32 `CreateThread` →
`GuestThread::ThreadEntrypoint` → `SetHostStackPointer` →
`InvokeGuestFunction(entry, arg, 0, 0)`.

### Syscall registration path

`CpuCore` maintains no dispatch table of its own. `CpuCore::RegisterSyscall`
(src/cpu/cpu.cpp:435) stores the handler and installs a thunk into the
kernel table via `Kernel::RegisterSyscallHandler`; the thunk unpacks the
`CONTEXT` (number in RAX, args in rdi/rsi/rdx/r10/r8/r9) and calls the
registered `SyscallHandler`. `CpuCore::InvokeSyscall` (src/cpu/cpu.cpp:467)
synthesizes a `CONTEXT` and calls `Kernel::HandleSyscall` directly. The old
dead `SyscallTable` in `src/cpu/syscall.h` was removed.
