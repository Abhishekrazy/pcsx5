# Phase 0 — Current-System Audit and Clean-Room Ledger

**Status:** In progress  
**Purpose:** Establish what the existing PCSX5 codebase proves, what is portable, and what the rebuild must deliberately leave behind.

## Task list

- [x] P0.1 Capture repository state and identify uncommitted work to preserve.
- [x] P0.2 Map current build/toolchain, execution, graphics, runtime, and frontend coupling.
- [x] P0.3 Record the architectural corrections required for x86-64 guest execution.
- [x] P0.4 Build the initial subsystem migration ledger: `reference`, `reuse as test/tool`, `redesign`, or `exclude`.
- [ ] P0.5 Characterize the memory, TLS, fault, and dispatcher behaviours with portable tests/traces; do not modify implementation yet.
- [ ] P0.6 Define a legal, minimal acceptance corpus and deterministic trace format.
- [ ] P0.7 Capture third-party provenance, licenses, and replacement/retention decisions.
- [ ] P0.8 Review Phase 0 evidence and approve Phase 1 repository bootstrap.

## Initial evidence (2026-09-08)

| Area | Verified current state | Rebuild disposition |
|---|---|---|
| Build | CMake requires MSVC, enables MASM, and fails non-MSVC configuration | Redesign |
| Host runtime | `platform_win32.cpp`, Win32 APIs, SEH/VEH, TEB/TLS assumptions | Redesign behind contracts |
| Execution | x86-64 guest code currently executes directly; no JIT or interpreter exists | Characterize, then redesign |
| Graphics | Vulkan renderer, GCN-to-SPIR-V translator, GLFW usage; no Metal backend | Retain as behavioral reference; extract HAL |
| Frontend | WPF/C# shell, Windows-specific integrations | Keep only as UX/reference material |
| Audio/input | Windows backends plus SDL audio/controller components | Retain portable adapters only after contracts exist |
| Tests | Broad CTest suite plus replay/golden assets and GPU smoke tests | Reuse as characterization inputs after licensing/data review |

## Preservation boundary

The working tree already contains user changes, including architecture decisions, audits, rendering work, and test changes. Phase 0 will not modify, delete, reset, or migrate those files. New rebuild artifacts live under `plans/` until a separately approved repository/bootstrap step.

## P0.1 evidence

The current tree contains substantial Windows/x64-specific code and uncommitted user work. Notable existing architectural evidence includes:

- `architecture/decisions/ADR-005-cross-platform-constraints.md`
- `architecture/decisions/ADR-006-target-execution-architecture.md`
- `docs/audits/AUDIT-2026-09-07-cross-platform-feasibility.md`

These documents correctly identify the primary blockers: Windows memory/fault infrastructure, Windows TLS/dispatcher assumptions, and the absence of an ARM64 execution backend.

## P0.2 findings

1. The architecture direction is correct, but the rebuild cannot assume a generic x64 JIT improves x86-64-host execution. Direct execution is the present baseline.
2. Linux and Windows can be first-class targets once the host-runtime contracts exist. macOS requires specific experiments around thread/TLS/fault design and MoltenVK support.
3. Apple Silicon and Android are gated by the ARM64 JIT, not merely by frontend or graphics work.
4. A native Metal backend is not the first Apple milestone. MoltenVK validates the renderer before a native backend is justified.
5. iOS is a separate product decision because code-generation and distribution constraints may rule out the needed JIT model.

## P0.3 acceptance criteria

Phase 0 is complete only when:

- Each current subsystem has a documented rebuild disposition.
- Existing tests/traces are identified as usable, unsafe, or missing provenance.
- The supported-platform matrix lists capability gates rather than promises based on compile success.
- Phase 1 has an agreed directory layout, build matrix, CI baseline, and no dependency on current Windows-only sources.

## P0.4 output

The initial rebuild disposition for each major subsystem is recorded in [phase-0-migration-ledger.md](phase-0-migration-ledger.md). It intentionally marks memory, TLS, fault handling, and direct execution as **characterize before redesign**: these are the pieces most likely to cause a superficially portable rewrite to fail later.

## P0.5 progress

The source-level runtime contract and the required characterization-test matrix are now captured in [phase-0-portable-runtime-contract.md](phase-0-portable-runtime-contract.md). The current Debug build passes all five memory tests plus TLS-context and syscall-validation tests; the registered TLS patch-stub target is missing from that build output and must be rebuilt. P0.5 remains open until the full matrix and traces run against the current implementation.
