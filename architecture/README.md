# PCSX5 Architecture Source of Truth

## Current Architecture

This directory contains the current architecture documents and ADRs.

Rules:
- architecture documents describe what the system is
- ADRs explain why durable decisions were made
- compatibility documents record measured title behavior
- test baselines record test/runtime evidence

Do not silently alter an accepted architectural decision.
Supersede it with a new ADR.

## Recommended Structure

architecture/
  README.md
  SYSTEM_ARCHITECTURE.md
  BOUNDARIES.md
  RUNTIME_LIFECYCLE.md
  THREADING_MODEL.md
  MEMORY_MODEL.md
  GPU_ARCHITECTURE.md
  HLE_ARCHITECTURE.md
  COMPATIBILITY_ARCHITECTURE.md
  RELEASE_ARCHITECTURE.md
  decisions/
