# Debugging

## VEH / crash handling

- A first-chance vectored exception handler is installed at startup
  (`AddVectoredExceptionHandler(1, ...)`, src/kernel/kernel.cpp:76). It owns
  `EXCEPTION_BREAKPOINT` from patched `syscall` sites (`CC 90` at RIP →
  syscall dispatch, src/kernel/kernel.cpp:500-505) and passes everything else
  on.
- A top-level unhandled-exception filter catches host crashes
  (src/kernel/kernel.cpp:82); it also writes a legacy `crash_log.txt`
  (src/kernel/kernel.cpp:706,756).
- `Diagnostics::InstallCrashHandler(crash_dir)` (src/diagnostics/diagnostics.cpp:62)
  is installed early from main (src/main.cpp:170). On a fatal crash it writes
  a bundle (report + MiniDump) into the crash directory.

## Crash dumps

Default dir `pcsx5_crash` (src/main.cpp:112); override with
`--crash-dir=<path>` or config `crash.bundle_dir` (src/main.cpp:165).

## Logging

src/common/log.h. Macros `LOG_TRACE/DEBUG/INFO/WARN/ERROR/CRITICAL(cat, fmt, ...)`
capture file/line/function into a `LogEntry`.

- Categories: `Loader, Memory, Kernel, HLE, GPU, Cpu, General` (log.h:8-16).
- Levels: `Trace < Debug < Info < Warn < Error < Critical` (log.h:18-25).
- Per-category filter: `LogConfig::SetLevel(category, level)` (log.h:53).
- JSON mode (one object per line to stdout): `LogConfig::SetJsonOutput(true)`
  (log.h:44); enabled from config `logging.json_output` (src/main.cpp:161).
- File mirror: `LogConfig::SetFileOutput(path, append)` or `--log-file=<path>`.
- Ring buffer: `GetRecentLogEntries(max_count)` (log.h:65) feeds crash
  reports with the last N messages.

## Reports

- `--report=<path>` writes a JSON compat summary; `import_report.json` is
  written next to it (src/main.cpp:60-74, `PersistSummary`). The import
  report is `HLE::ExportImportReportJson()` — array of
  `{module, nid, name, call_count, auto_stubbed, last_caller_rip}` sorted by
  call count (src/hle/hle.cpp:215-234).
- `--regression-report=<path>` writes a markdown regression comparison
  against stored run history.
- Unimplemented stubs log one WARN per `module::name`
  (`HLE::LogStubCallOnce`, src/hle/hle.cpp:198); totals via
  `HLE::GetUnresolvedImportCount()`.

## Import trace ring

Bounded ring of the most recent guest→host calls:
`HLE::GetImportTrace(max_count = 256)` returns `TraceEntry` records
(timestamp, module, name, symbol id, caller RIP, thunk VA, 6 args) —
src/hle/hle.h:79-94. Included in crash reports; clear between runs with
`ClearImportTrace()`.

## Strict imports

`--strict-imports` turns unresolved-import auto-stubbing into a hard link
error (src/hle/hle.cpp:146,362-365). Use it to find the first missing symbol
instead of running past stubs.

## Misc

- `PCSX5_BREAK_SYSCALL=<num>` env var: `DebugBreak()` on that syscall
  (src/kernel/syscalls.cpp:189-204).
- Debug-level syscall logging prints all six argument registers
  (src/kernel/syscalls.cpp:206).
