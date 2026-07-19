# Adding HLE symbols

## How registration works

`HLE::RegisterSymbol(module_name, name, handler)` — src/hle/hle.cpp:309.

- Key is `"module::name"` in `g_symbol_registry`.
- Each symbol gets an incrementing id and an executable thunk from
  `CreateThunk` (src/hle/hle.cpp:280): `mov r10, id; mov rax, dispatcher; jmp rax`.
- Handler type: `std::function<u64(const GuestArgs&)>`; `GuestArgs` carries
  the SysV argument registers rdi/rsi/rdx/rcx/r8/r9 (src/hle/hle.h:11-18).
- Resolution order in `HLE::Resolve` (src/hle/hle.cpp:334): exact key match →
  NID base-variant match (e.g. `bzQExy189ZI#j#j` matches a registered
  `bzQExy189ZI#T#T`) → auto-stub. `ResolveAny` (src/hle/hle.cpp:384) searches
  all modules and stubs under module `unknown`.

## NID strings

PS5 imports are named by NID, e.g. `6UgtwV+0zb4#T#T` (scePthreadCreate).
`Common::ParseNidString` / `Common::LookupNidName` map a NID to a friendly
name; the friendly name is stored in stats as `resolved_name`
(src/hle/hle.cpp:184-192). Register the exact NID string the guest requests,
or rely on base-variant matching for `#X#Y` suffix differences.

Names come from a small built-in table plus a runtime text database:
`Common::LoadNidDatabase(path)` (src/common/nid.cpp:246) loads
`NID<TAB>module<TAB>name` lines, and file entries override built-ins. The
shipped seed is `assets/nid_db.txt` — contributors can add friendly names
there instead of touching code.

## Auto-stub behavior

If the guest requests an unregistered symbol (and strict mode is off),
`Resolve`/`ResolveAny` register a stub that returns 0 and logs once:

- First call per `module::name` emits one structured WARN via
  `HLE::LogStubCallOnce` (src/hle/hle.cpp:198):
  `Unimplemented stub called: module::name (nid=... name=...)`.
- The symbol id is recorded via `MarkAutoStubbed` (src/hle/hle.cpp:176);
  `HLE::GetUnresolvedImportCount()` returns the stub count.

## Strict-import mode

`HLE::SetStrictImportMode(true)` (src/hle/hle.cpp:146) — CLI flag
`--strict-imports`. Unresolved NIDs then return 0 (no stub) and the kernel
linker treats the relocation as a hard error. Used by the Phase-0 test suite.

## Adding a new symbol

1. Pick the module file (e.g. `src/hle/libkernel.cpp`) and its registration
   function.
2. Add `HLE::RegisterSymbol("libkernel", "<NID>#T#T", [](const HLE::GuestArgs& args) -> u64 { ... });`
3. Return a guest-ABI value in `rax`; use `Memory::Read/Write` helpers for
   guest pointers. See scePthreadCreate at src/hle/libkernel.cpp:1123 for a
   full example (guest stack alloc + `Kernel::CreateThreadEx`).

## Visibility in stats / JSON

Every dispatched call is recorded (src/hle/hle.cpp:157): module, NID,
resolved name, call count, last caller RIP, auto-stub flag.
`HLE::ExportImportReportJson()` (src/hle/hle.cpp:215) emits an array sorted
by call_count desc:

```json
[{ "module": "...", "nid": "...", "name": "...", "call_count": 1,
   "auto_stubbed": true, "last_caller_rip": "0x..." }]
```

Written to `import_report.json` next to `--report` output (src/main.cpp:69-73).
Unimplemented calls are the entries with `"auto_stubbed": true`.
