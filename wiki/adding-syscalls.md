# Adding syscalls

## Where

Table: `static SyscallHandler g_syscall_table[512]` — src/kernel/syscalls.cpp:64.
Entries are installed in `Kernel::InitializeSyscallTable()` —
src/kernel/syscalls.cpp:106.

## Handler signature

```cpp
using SyscallHandler = s64(*)(CONTEXT*);   // src/kernel/syscalls.h:9
```

The VEH passes the live guest `CONTEXT`. Argument registers follow the
FreeBSD/PS5 convention: number in `RAX`, args in `RDI, RSI, RDX, R10, R8, R9`
(note `R10`, not `RCX`, for arg 4). Return the result in `s64`; it lands in
guest `RAX`. Error returns are negative errno, e.g. `-ENOSYS` (see the
constexpr block at src/kernel/syscalls.cpp:41-57).

## Steps

1. Declare `s64 SysFoo(<args>, CONTEXT* context);` in src/kernel/syscalls.h.
2. Implement it in src/kernel/syscalls.cpp. Use `ReadGuestString` /
   `SafeReadBuffer` / `SafeWriteBuffer` (src/kernel/syscalls.cpp:67-104) for
   guest memory — they are SEH-guarded.
3. Add a table entry in `InitializeSyscallTable()`:

```cpp
g_syscall_table[<num>] = [](CONTEXT* ctx) -> s64 {
    return SysFoo(ctx->Rdi, static_cast<u32>(ctx->Rsi), ctx);
};
```

## Numbering

Numbers follow the FreeBSD/PS5 syscall ABI. Existing examples
(src/kernel/syscalls.cpp:112-168): 1 exit, 3 read, 4 write, 5 open, 6 close,
20 getpid, 54 ioctl, 116 gettimeofday, 188/189 stat/fstat, 240 nanosleep,
370 thr_new, 477 mmap, 478 munmap, 479 mprotect; PS5 extensions 540–542.
Verify against a FreeBSD 9/PS5 syscall table before picking a new number;
numbers ≥ 512 are rejected by `HandleSyscall` (src/kernel/syscalls.cpp:178).

## Runtime registration

External code can install/replace a handler without touching the table via
`CpuCore::RegisterSyscall(num, handler)` (src/cpu/cpu.cpp:435) — it stores a
6-arg `SyscallHandler` (src/cpu/cpu.h:112) and thunks into the kernel table
through `Kernel::RegisterSyscallHandler`. Pass `nullptr` to
`Kernel::RegisterSyscallHandler` to clear an entry.

## Debugging a syscall

Set env var `PCSX5_BREAK_SYSCALL=<num>` to trigger `DebugBreak()` when that
syscall is hit (src/kernel/syscalls.cpp:189-204). Unknown/unimplemented
numbers log a WARN and return `-ENOSYS`.
