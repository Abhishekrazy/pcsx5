# PCSX5 — Clean Rebuild

PCSX5 is being rebuilt as a host-independent PS5 emulator architecture.

Read these files in order:

1. [Operating contract](AGENTS.md)
2. [Architecture](ARCHITECTURE.md)
3. [Current rebuild status](REBUILD_STATUS.md)

The existing implementation remains only as a characterization reference during Phase 0. New work follows the target layout and rules in `AGENTS.md`.

## Development environment

- Windows: run `bootstrap-windows.cmd` from a Command Prompt. It locates Visual Studio C++ Build Tools, initializes the x64 developer environment, configures the clean Ninja build, builds it, and runs CTest.
- Linux: `cmake --preset linux-x64-debug`, `cmake --build --preset linux-x64-debug`, then `ctest --preset linux-x64-debug`.

Both hosts also have `*-x64-release` presets with separate output directories.
For Windows Release, run `bootstrap-windows.cmd windows-x64-release`.
Windows presets require an initialized x64 MSVC developer shell; Linux presets
require GCC, CMake 3.25+ and Ninja. No dependencies are downloaded by these builds.
Test presets fail when no tests are selected and write `ctest.log` and `junit.xml`
inside their build directory. CI uses these same presets and uploads diagnostics
only; it does not package or publish the legacy emulator.

Build output is written to `out/build/`; the legacy `build/` directory is not used.
