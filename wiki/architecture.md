# Architecture

## Subsystem map

| Subsystem | Path | Role |
|---|---|---|
| Loader | `src/loader/` | ELF/self parsing, module loading, relocation, import linking |
| Kernel | `src/kernel/` | VEH setup (`kernel.cpp`), syscall table (`syscalls.cpp`), fd table, `thread.cpp` shim |
| CPU core | `src/cpu/cpu.cpp` | Guest thread registry + lifecycle; syscall adapters |
| HLE | `src/hle/` | High-level emulation of userland libraries (`hle.cpp`, `libkernel.cpp`, ...) |
| GPU | `src/gpu/` | Graphics emulation |
| Memory | `src/memory/` | Guest address space (reserve/commit, read/write helpers) |
| UI | C# WPF (`pcsx5_ui`) | Frontend; drives the native core |
| Compat | `src/compat/` | Per-title compatibility database |
| Diagnostics | `src/diagnostics/` | Crash handler, minidump bundles |

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
