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

Build output is written to `out/build/`; the legacy `build/` directory is not used.
