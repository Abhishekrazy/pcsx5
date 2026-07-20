# Building pcsx5 — host requirements & contributor guide

This document is the canonical reference for building pcsx5 from source on a
supported host and for running the full test suite.  If something here is
out of date with the code, please open an issue or update this file in the
same PR.

---

## 1. Supported host platforms

| Platform | Status | Notes |
| -------- | ------ | ----- |
| Windows 10 21H2 (x64) | Supported | Primary development host. |
| Windows 11 (x64)      | Supported | Primary development host. |
| Windows Server 2022   | Supported | Headless build + test farm host. |
| Linux / macOS          | Not supported | The HLE ABI bridge ([`src/hle/dispatcher.asm`](src/hle/dispatcher.asm)) is MASM-only and there is no GCC/Clang port yet.  CMake refuses to configure on non-MSVC toolchains. |

The rest of this guide assumes Windows.

---

## 2. Required toolchain

| Tool | Version | Required for | Notes |
| ---- | ------- | ------------ | ----- |
| **MSVC** (Visual Studio 2022 17.x or 2026 18.x) with the *Desktop development with C++* workload | 2022 17.8 or newer | Build, HLE dispatcher, Vulkan loader. | The `ASM_MASM` language is enabled in `CMakeLists.txt`. |
| **Windows SDK** | 10.0.19041 or newer | Win32 headers, VEH/SEH, `dbghelp`. | Installed alongside MSVC. |
| **CMake** | 3.20 or newer | Configure + build. | The project sets `cmake_minimum_required(VERSION 3.20)`. |
| **Ninja** (optional) | 1.10 or newer | Faster incremental builds. | Default generator is the Visual Studio generator. |
| **Vulkan SDK** (optional) | 1.3 or newer | Vulkan runtime on the host for GPU smoke. | The GPU backend gracefully degrades to GDI when the SDK is absent. |

### Optional toolchain

| Tool | Used for | Notes |
| ---- | -------- | ----- |
| **LLVM / clang** (16 or newer, with `lld-link`) | Building the freestanding guest test ELFs in `tests/test_elf/`. | The CMake configure step auto-detects `clang`/`clang.exe` in well-known install paths.  If it is missing, the guest smoke tests are skipped gracefully with a clear log line. |
| **Git for Windows** | Source control, CI scripts. | Any 2.30+ release. |
| **PowerShell 5+** | Running the contributor scripts in `tools/`. | Comes with Windows. |

---

## 3. Fetching the source

```bash
git clone https://example/pcsx5.git
cd pcsx5
# Optional: pull submodules if/when they are introduced.
git submodule update --init --recursive
```

The repository has no submodules today, but the `FetchContent` step in
`CMakeLists.txt` will pull GLFW 3.3.9 and Vulkan-Headers v1.3.275 on the
first configure.

---

## 4. Configure & build

### Visual Studio generator (default)

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
```

> Replace `Visual Studio 17 2022` with `Visual Studio 18 2026` if you are
> on the newer toolchain.  The build is otherwise identical.

The resulting executable is at:

```
build/bin/Debug/pcsx5.exe          (Debug)
build/bin/Release/pcsx5.exe        (Release)
```

### Ninja (faster incremental)

```powershell
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

### Enabling the test suite

Tests are gated by `BUILD_TESTING` (default ON when `CTest` is available).
No extra flag is required:

```bash
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

The expected output is thirty-one passing tests:

```
 1/31 loader_validation
 2/31 loader_corpus
 3/31 memory_validation
 4/31 memory_query
 5/31 kernel_memory
 6/31 elf_metadata
 7/31 nid
 8/31 self_header
 9/31 tls_context
10/31 hle_import_report
11/31 reports
12/31 config
13/31 diagnostics
14/31 dump_imports
15/31 dump_dt
16/31 compat
17/31 pcsx5_compat
18/31 snd_player_test
19/31 crypto
20/31 param_json
21/31 pkg
22/31 pfs
23/31 nid_db
24/31 module_graph
25/31 syscall_validation
26/31 kernel_sync
27/31 hle_phase3
28/31 hle_agc
29/31 guest_printf
30/31 shader
31/31 gfx10_state
```

`dump_imports` and `snd_player_test` depend on real game files under
`Games/`; when those are absent (e.g. on a CI runner) they skip
gracefully and still report success.

If `clang` is on `PATH` the two extra guest smoke tests
(`guest_syscall_smoke`, `guest_tls_smoke`) are also registered and should
print `Hello Guest` and `TLS OK` respectively.

---

## 5. Command-line flags

```text
pcsx5.exe [options] <path-to-eboot-or-elf>

  --strict-imports             Fail (return non-zero) on unresolved imports.
  --report=<path>              Write a JSON compatibility summary to <path>.
  --regression-report=<path>   Write an aggregated markdown regression report.
  --log-file=<path>            Mirror log output to <path>.
  --crash-dir=<path>           Directory for crash-report bundles (default: pcsx5_crash).
  --config-dir=<path>          Directory holding global.json + per-title overrides.
  --title-id=<id>              PS5 title id (CUSAxxxxx) for per-title overrides and history.
  -h, --help                   Print this message and exit.
```

A typical contributor smoke run:

```powershell
.\build\bin\Debug\pcsx5.exe `
    --config-dir=.\pcsx5_config `
    --title-id=CUSA00001 `
    --report=.\pcsx5_config\CUSA00001.json `
    --regression-report=.\pcsx5_config\regression.md `
    --strict-imports `
    .\tests\test_elf\test_guest.elf
```

---

## 6. Project layout

```
pcsx5/
├── src/
│   ├── common/         # types.h, log.{h,cpp}  (structured logging, ring buffer)
│   ├── config/         # ConfigService: schema-versioned JSON config
│   ├── diagnostics/    # SEH crash filter, MiniDump bundle writer
│   ├── gpu/            # GPU facade + Vulkan backend (boots a window)
│   ├── hle/            # HLE dispatch (libkernel/libpad/libvideoout/libagc)
│   │                   # dispatcher.asm bridges SysV -> Windows x64 ABI
│   ├── kernel/         # Process + thread context, VEH trap, ELF loader
│   ├── loader/         # ELF64 parser + ElfBuilder (test fixture)
│   ├── memory/         # 16KB page VirtualAlloc-backed memory manager
│   └── reports/        # CompatSummary, jsonl history, regression verdicts
├── tests/              # CTest targets (one .cpp per test executable)
│   └── test_elf/       # Freestanding guest ELFs (built by clang)
├── tools/              # Python helpers (NID extraction, SCE parsing)
└── CMakeLists.txt
```

---

## 7. Coding conventions

* **Language**: C++20.  All `target_compile_options` use `/W4 /WX` for
  C++ files; the MASM dispatcher is excluded from `/WX` via a generator
  expression.
* **Namespaces**: every subsystem owns its namespace (`HLE`, `Loader`,
  `Memory`, `Kernel`, `GPU`, `ConfigService`, `Reports`, `Diagnostics`).
  Avoid leaking internal types into the global scope.
* **Logging**: use the `LOG_INFO(category, fmt, ...)` macros.  Categories
  are `Loader | Memory | Kernel | HLE | GPU | General`.  The macros pass
  `__FILE__ / __LINE__ / __FUNCTION__` so the log ring buffer always
  carries diagnostic context.
* **Windows headers**: include `<windows.h>` only inside `.cpp` files and
  always with `#define NOMINMAX` first to prevent macro collisions with
  `std::min` / `std::max`.
* **Threading**: every shared structure owns a `std::mutex`.  If the
  structure is logically read-only, the mutex is `mutable` so `const`
  member functions can lock it (see `Snapshot()` in
  [src/common/log.cpp](src/common/log.cpp) and [src/hle/hle.cpp](src/hle/hle.cpp)).
* **Testing**: every new component ships with a CTest target.  Test
  scratch directories must live under
  `std::filesystem::temp_directory_path() / "pcsx5_<test>_test"` —
  CTest's working directory is the source root, so relative paths leak
  state across runs.

---

## 8. Adding a new HLE symbol

1. Compute the official NID (see [`tools/extract_nids.py`](tools/extract_nids.py))
   and add a `static constexpr` declaration in the relevant
   `src/hle/lib*.cpp` file.
2. Register it via `HLE::Register(module, nid, &Handler, name)` inside
   the module's `Register*Symbols()` function.
3. Add a positive test in `tests/hle_import_report.cpp` that calls
   `HLE::Resolve` and asserts a non-zero thunk is produced.
4. Add a regression entry under `tests/test_elf/` if the symbol is
   exercised by the freestanding guest ELF.

---

## 9. Adding a new CTest target

```cmake
add_executable(<name>_tests
    tests/<name>_tests.cpp
    # Add every translation unit the test links against.
    src/common/log.cpp
    src/<other>/<file>.cpp
)
target_include_directories(<name>_tests PRIVATE src)
target_compile_options(<name>_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX>)
target_link_libraries(<name>_tests PRIVATE glfw Vulkan::Headers)  # only if the test uses them
add_test(NAME <name> COMMAND <name>_tests)
```

Tests that depend on the HLE ABI bridge must also include
`${DISPATCHER_SRC}` in their source list.

---

## 10. Troubleshooting

### `error C2598: linkage specification must be at file scope`
An `extern "C"` function is being declared inside another function.
Move the declaration to file scope.

### `error C1104: fatal error LNK1104: cannot open 'pcsx5.exe'`
Another `pcsx5.exe` is still running.  Stop the previous build's process
and retry.

### `error MSB6006: "cmd.exe" exited with code 1` during the linker step
A previous binary is locked (most often by an open shell in the
`build/bin/Debug` directory).  Close any open handles and re-run.

### `MiniDumpWriteDump fails with error 0x80070005` (Access Denied)
The crash bundle directory is on a read-only share or a network drive
that disallows file creation.  Point `--crash-dir=` at a local path.

### Guest smoke tests are skipped with "clang not found on PATH"
This is intentional.  Install LLVM 16+ and add `C:\Program Files\LLVM\bin`
to `PATH`, then delete the build directory and re-configure.

### `pcsx5_config/` is filled with stale defaults
Delete the directory; the service will recreate it with fresh defaults
on the next launch.  The schema version is stamped on every file so
older files are migrated (or passed through with a warning) on load.

### `MaskingExceptionCode: 'exception_code'` from MSVC
Do not name a `CrashContext` field `exception_code` — MSVC's SEH context
rewrites the identifier.  Use `exc_code` (or any other suffix) instead.

---

## 11. Building the installer (Inno Setup)

Release builds can be wrapped in a Windows installer produced with
[Inno Setup 6](https://jrsoftware.org/isinfo.php).  The script lives at
[`installer/pcsx5.iss`](installer/pcsx5.iss) and packages whatever
`build_and_package.ps1` placed in `dist/`.

The installer wizard shows:

1. an **introduction page** (`installer/INTRO.txt`) with requirements and
   a note about the games folder,
2. the **license / terms page** (the repo `LICENSE`),
3. the standard **installation path** picker (default
   `%ProgramFiles%\PCSX5`),
4. a **games folder page** whose value is written to
   `[Paths] GameFolders` in `config.ini`,
5. a **"Launch PCSX5"** checkbox on the finish page.

To build it locally, install Inno Setup 6 and run:

```powershell
.\build_and_package.ps1 -CreateInstaller -Version 1.0.0
```

The script auto-detects `ISCC.exe` from the registry or
`%ProgramFiles(x86)%\Inno Setup 6`; set the `INNO_SETUP` environment
variable to the full path of `ISCC.exe` to override.  The result is
`installer\Output\PCSX5-<version>-win64-Setup.exe`.

CI builds both the portable ZIP and the installer on every push, and
attaches them to the GitHub Release when a `v*` tag is pushed
(see [`.github/workflows/ci.yml`](.github/workflows/ci.yml)).

---

## 12. CI

The repository ships an out-of-the-box CTest configuration; a minimal CI
script looks like:

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

A typical CI runner image needs:

* Visual Studio 2022 Build Tools (C++ workload + Windows SDK)
* CMake 3.20+
* Vulkan SDK 1.3 (for the GPU smoke check; optional)

---

## 13. License

This project is licensed under the [MIT License](LICENSE).  By submitting
a pull request you agree to license your contribution under the same
terms.
