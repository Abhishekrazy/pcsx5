# Architectural Audit: Memory Subsystem Access & Page-Crossing Safety

**Audit Date**: August 26-27, 2026  
**Auditor**: Gemini Agentic Assistant  
**Classification**: ARCHITECTURAL INTEGRITY & ROBUSTNESS AUDIT  

---

## 1. Executive Summary

This audit evaluated the safety, correctness, and performance of memory access paths across the PCSX5 emulator core following the implementation of page-aware memory access primitives and OS ground-truth commit checking.

Prior to this intervention, sub-range commits resulted in false-positive validity reports from `Memory::Query`, leading to uncommitted memory accesses in `GuardedCopy` and host crashes on non-primary guest worker stacks.

All memory access primitives have been unified into `src/memory/` and verified across 45 automated test targets and empirical title runs.

---

## 2. Invariant & Contract Verification

| Architectural Invariant | Pre-State | Post-State | Status |
| :--- | :--- | :--- | :--- |
| **Commit State Accuracy** | Coarse (Region-wide flag flipped upon any sub-commit) | Fine-grained (VirtualQuery ground-truth check for non-pool allocations) | **VERIFIED** |
| **Page-Crossing Safety** | Single-byte check or 1st-page assumption in string/copy helpers | 4KB host page slicing across entire requested range | **VERIFIED** |
| **Fault Boundary Recovery** | SEH `__try/__except` fallback (skipped on guest worker stacks) | Transparent demand-commit on `MEM_RESERVE`, clean return `false` on `MEM_FREE` without SEH dependency | **VERIFIED** |
| **Pool Concurrency** | Unsynchronized bump/free list operations in `PoolFree` | Thread-safe `PoolAlloc` and `PoolFreeLocked` with free-list recycling under `g_regions_mutex` | **VERIFIED** |
| **String Safety** | `strlen` capped at 4KB; `strcpy`/`strcmp` checked 1 byte only | Full page-aware `GuardedStrlen`, `GuardedStrcpy`, `GuardedStrcmp`, `GuardedMemcmp` | **VERIFIED** |

---

## 3. Test Evidence Summary

- **CTest Target**: `guest_memory_access` (79 checks, 0 failures)
- **CTest Target**: `memory_query` (123 checks, 0 failures)
- **Suite Baseline**: 45/45 tests passed (100%)
- **Concurrency Test**: 1, 2, 4, 8, 16, 32 concurrent worker threads executing 100 allocation/copy/verify/unmap cycles simultaneously with 0 errors.
- **Regression Confirmation**: PPSA02929 sustained execution verified with active AGC graphics submissions and swapchain presentations.
- **Boundary Characterization**: PPSA21564 executes past initial PRX init and direct memory allocation without memory fault.
