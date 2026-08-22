# Ground-Up Refactoring Plan

The project already launches games. Preserve that achievement.

## Phase 0: Freeze the current behavior

Create:
- reproducible build instructions
- a smoke test
- game boot logs
- a known-good game list
- crash capture
- a compatibility baseline

Do not reorganize the repository yet.

## Phase 1: Inventory

Produce:
- module dependency graph
- executable/DLL ownership map
- global state inventory
- thread inventory
- third-party inventory
- public ABI inventory
- game-specific hack inventory

The inventory is evidence, not a commitment to preserve bad design.

## Phase 2: Characterization

For each major subsystem:
- record current behavior
- add focused tests where possible
- capture traces for hard-to-test behavior
- identify side effects

## Phase 3: Establish core shell

Introduce:
- EmulatorSession
- CoreConfig
- Result/Error
- Logger
- Clock
- subsystem lifecycle

Do not rewrite CPU/GPU yet.

## Phase 4: Establish memory boundary

Create a dedicated Memory/MMU subsystem.

Migrate users gradually.

## Phase 5: Establish CPU boundary

Move CPU implementation behind a clean interface.

Preserve existing semantics.

## Phase 6: Establish kernel/HLE boundary

Separate guest-facing services from internal utilities.

## Phase 7: Loader boundary

Make ELF/module loading independently testable.

## Phase 8: GPU boundary

Split:
- guest command decoding
- normalized GPU IR
- shader translation
- Vulkan backend

## Phase 9: Audio/Input boundaries

Create narrow logical APIs and platform adapters.

## Phase 10: C ABI

Freeze a versioned C ABI for the WPF app.

## Phase 11: UI cleanup

Refactor WPF into:
- shell
- view models
- native interop
- settings
- update service
- logs/crash reporting

## Phase 12: Compatibility database

Move title-specific behavior into explicit profiles.

## Phase 13: Remove obsolete paths

Only after tests prove the replacement.

## Phase 14: Release/update pipeline

Build:
- CI
- release artifacts
- update manifest
- signature verification
- staging/rollback
- migration checks

## Rule

Never perform all phases in one branch/commit/AI session.
