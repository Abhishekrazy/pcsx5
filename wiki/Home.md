# PCSX5 Wiki

PCSX5 is an experimental PlayStation 5 emulator for Windows: a C++20 core (MSVC) that directly executes guest x86_64 code, plus a C# WPF UI.

## Pages

- [Architecture](architecture.md) — subsystem map, native-execution model, VEH syscall interception, HLE thunks, thread model.
- [Adding HLE symbols](adding-hle-symbols.md) — `RegisterSymbol`, NIDs, thunks, auto-stubs, strict-import mode.
- [Adding syscalls](adding-syscalls.md) — kernel syscall table, handler signatures, numbering.
- [Debugging](debugging.md) — crash handling, logging, reports, import trace.
- [Compatibility](compatibility.md) — compat database statuses, JSON schema, `pcsx5_compat` CLI.
- [Retail SELF decryption](self-decryption.md) — why retail SELFs are rejected, plugin point, key/legal model.
