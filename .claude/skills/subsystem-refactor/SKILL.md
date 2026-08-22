# Subsystem Refactor Skill

## Purpose

Move tangled emulator code into explicit subsystem boundaries while preserving behavior.

## Procedure

1. Inventory current behavior and callers.
2. Add characterization tests/traces.
3. Define the target interface.
4. Add adapters around existing implementation.
5. Migrate one caller at a time.
6. Build and test.
7. Remove old path only after no callers remain.
8. Update architecture docs.

## Success criterion

The refactor changes ownership and dependency structure without silently changing emulator semantics.
