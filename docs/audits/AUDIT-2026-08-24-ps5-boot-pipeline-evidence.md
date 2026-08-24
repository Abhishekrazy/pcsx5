# AUDIT-2026-08-24: PS5 Boot Pipeline Evidence

## Status
Mandatory project-level architectural audit.

## Executive Summary
This audit rigorously examines the reconstructed PS5 guest boot pipeline to separate the true platform contract from emulator-specific implementations, workarounds, and hacks. The current architecture successfully boots target titles (e.g., PPSA02929) to the GPU wall, but this audit reveals that this success relies heavily on several uncharacterized approximations (silent PRX fallbacks, HLE overrides, heuristic entry-point scanning) that diverge from strict PS5 correctness.

---

## 1. STAGE-BY-STAGE AUDIT

### Stage 1: Emulator Init (`pcsx5_init`)
- **Owner**: `pcsx5_init` / `LuaInit`
- **Entry Point**: `pcsx5_init()`
- **Input**: User configuration, host environment.
- **Output**: Initialized host subsystems, Vulkan device.
- **Invariant**: Subsystems are brought up exactly once in topological order.
- **Implementation**: `src/core_api.cpp` invoking Lua scripts for subsystem sequencing.
- **Test Evidence**: Covered by `pcsx5_ui` launch path.
- **Runtime Evidence**: UI console correctly populates initialization steps.
- **Platform Evidence**: Host-specific; models generic emulator startup.
- **Classification**: **CHARACTERIZED** (Emulator-specific orchestration).
- **Known Approximation**: Lua-driven orchestration is a PCSX5/Kyty specific design, not an OS-level init.
- **Known Hack**: None.
- **Unknowns**: None.

### Stage 2: Title Load & Param Resolution (`pcsx5_load`)
- **Owner**: `pcsx5_load`
- **Entry Point**: `pcsx5_load(path)`
- **Input**: Executable path or Title ID directory.
- **Output**: Auto-detected config, Title ID, keystone setup.
- **Invariant**: Target file must exist and param.json correctly directs config.
- **Implementation**: `param_json.cpp` parser, fallback logic to find `eboot.bin`.
- **Test Evidence**: `param_json_tests.cpp`.
- **Runtime Evidence**: `PPSA02929.json` records correctly loading param.json and keystone.
- **Platform Evidence**: Title ID format matches PSN DB (`PPSAXXXXX`).
- **Classification**: **VERIFIED**
- **Known Approximation**: None.
- **Known Hack**: None.
- **Unknowns**: Retail retail-PKG loading and decryption is bypassed (files extracted manually).

### Stage 3: ELF/SELF Header & Segments (`Loader::Load`)
- **Owner**: `Loader`
- **Entry Point**: `Loader::Load`
- **Input**: `eboot.bin` file path.
- **Output**: Memory maps, ELF segments.
- **Invariant**: Target must be a valid 64-bit ELF/SELF.
- **Implementation**: Extracts structural headers of SELF, falls back to raw ELF if unencrypted.
- **Test Evidence**: `self_header_tests.cpp`, `loader_validation.cpp`.
- **Runtime Evidence**: PPSA02929 ELF successfully loaded and segments parsed.
- **Platform Evidence**: SELF is the standard PS5 signed executable format.
- **Classification**: **APPROXIMATION**
- **Known Approximation**: Lacks cryptographic segment verification and decryption (root keys missing).
- **Known Hack**: Bypassing crypto for fPKG ELFs.
- **Unknowns**: Retail decryption paths.

### Stage 4: Virtual Address Layout (`Memory::Reserve`)
- **Owner**: `Kernel` / `Memory`
- **Entry Point**: `Memory::Reserve`
- **Input**: Segment memory requirements.
- **Output**: Committed guest memory regions.
- **Invariant**: PIE bases must not overlap.
- **Implementation**: Hinted PIE base at `0x800000000`, steps by 256MB on collision.
- **Test Evidence**: `kernel_memory_tests.cpp`.
- **Runtime Evidence**: Traces show consistent module relocation events on collision.
- **Platform Evidence**: PS5 uses PIE with ASLR.
- **Classification**: **APPROXIMATION**
- **Known Approximation**: Uses deterministic `0x10000000` steps instead of true cryptographically secure ASLR.
- **Known Hack**: Unaligned segments expand to `VirtualAlloc` page sizes.
- **Unknowns**: Exact real PS5 ASLR entropy model.

### Stage 5: Module Metadata Parsing (`ParseModuleMetadata`)
- **Owner**: `Loader`
- **Entry Point**: Internal `Loader::Load` loop.
- **Input**: `PT_DYNAMIC` table.
- **Output**: Extracted `DT_NEEDED`, `DT_SYMTAB`, `DT_STRTAB`, etc.
- **Invariant**: Dynamic table must be readable from guest memory.
- **Implementation**: Standard ELF dynamic table walk.
- **Test Evidence**: `elf_metadata_tests.cpp`.
- **Runtime Evidence**: Accurate recovery of `libc.prx`, `libkernel.prx` dependencies.
- **Platform Evidence**: PS5 SDK uses standard ELF dynamic tags.
- **Classification**: **SPECIFIED** (Standard ELF).
- **Known Approximation**: None.
- **Known Hack**: None.
- **Unknowns**: PS5-specific undocumented dynamic tags (if any).

### Stage 6: PRX Loading (`LoadNeededPrxModules`)
- **Owner**: `Kernel`
- **Entry Point**: `LoadNeededPrxModules`
- **Input**: `DT_NEEDED` names.
- **Output**: Recursive loading of PRX files.
- **Invariant**: Cycles must not lock the loader.
- **Implementation**: Directed graph deduplication, cycle breaker.
- **Test Evidence**: `module_graph_tests.cpp`.
- **Runtime Evidence**: Correctly loads up to 20 module deps for PPSA02929.
- **Platform Evidence**: Standard dynamic linker behavior.
- **Classification**: **IMPLEMENTED**
- **Known Approximation**: Missing PRX files silently fall back to HLE instead of aborting the process as a real OS would.
- **Known Hack**: None.
- **Unknowns**: Real Orbis OS module loading depth limits.

### Stage 7: Symbol/NID Resolution (`LinkLoadedPrxModules`)
- **Owner**: `Kernel`
- **Entry Point**: `LinkModule`
- **Input**: Relocation tables and undefined symbols.
- **Output**: Patched PLT and GOT entries.
- **Invariant**: Resolution follows strict precedence (Exact -> HLE -> Base -> Stub).
- **Implementation**: Custom 4-stage resolution pipeline.
- **Test Evidence**: `nid_db_tests.cpp`, `nid_tests.cpp`.
- **Runtime Evidence**: Dreaming Sarah relies heavily on HLE overrides for libc and libkernel.
- **Platform Evidence**: PS5 SDK uses NID hashes for exports.
- **Classification**: **HACK**
- **Known Approximation**: Auto-stubs returning zero for missing imports.
- **Known Hack**: Native C++ implementations forcibly overriding explicit PRX exports to avoid relying on uninitialized guest PRX states.
- **Unknowns**: Whether returning `0` from auto-stubs is safe for untested syscalls.

### Stage 8: Syscall Interception (`PatchSyscalls`)
- **Owner**: `Kernel`
- **Entry Point**: `PatchSyscalls`
- **Input**: Executable memory segments.
- **Output**: `0F 05` patched to `CC 90`.
- **Invariant**: Only executable code is patched.
- **Implementation**: Linear memory scan across `PROT_EXEC` regions.
- **Test Evidence**: `syscall_validation_tests.cpp`.
- **Runtime Evidence**: Syscalls successfully trapped in VEH.
- **Platform Evidence**: PS5 uses ring-0 `syscall` transitions.
- **Classification**: **HACK**
- **Known Approximation**: Emulating ring-0 transitions via Windows VEH exceptions.
- **Known Hack**: Byte-scanning for `0F 05` can easily corrupt data masquerading as code, or miss dynamically generated code (JIT).
- **Unknowns**: How frequently games embed `0F 05` as data in `PROT_EXEC` segments.

### Stage 9: Segment Protection
- **Owner**: `Memory`
- **Entry Point**: `Memory::Protect`
- **Input**: Segment memory bounds and requested permissions.
- **Output**: Merged OS-level page permissions.
- **Invariant**: Pages containing multiple segments receive the union of their permissions.
- **Implementation**: Analyzes overlaps, issues `VirtualProtect` via `Memory::Protect`.
- **Test Evidence**: `memory_query_tests.cpp`.
- **Runtime Evidence**: No execution faults in data sections.
- **Platform Evidence**: Standard OS paging model.
- **Classification**: **CHARACTERIZED**
- **Known Approximation**: None.
- **Known Hack**: Union of overlapping segment protections (required because Windows `VirtualProtect` operates at page granularity, unlike ELF segments which can be unaligned).
- **Unknowns**: PS5 specific W^X enforcement.

### Stage 10: Entry Point Discovery
- **Owner**: `Kernel`
- **Entry Point**: `LoadModule` (end phase)
- **Input**: Symbol table and `entry_point`.
- **Output**: Final guest `main` address.
- **Invariant**: `main` must be found to launch correctly.
- **Implementation**: Exact match -> String scan -> Code scan heuristic (`xk_regs_fpepk` via `0xE8`).
- **Test Evidence**: None explicit for the scanner.
- **Runtime Evidence**: Successfully finds `main` in PPSA02929 via code scan.
- **Platform Evidence**: Undocumented linker behavior/strip effects.
- **Classification**: **HACK**
- **Known Approximation**: None.
- **Known Hack**: Heuristic byte-scanning (`0xE8` relative call to `0x55` or `0x48 0x83 0xEC` prologues) is an inherently fragile architectural debt required because of missing true PS5 linker metadata.
- **Unknowns**: What exact metadata the real OS uses to jump to stripped `main`.

### Stage 11: Guest Thread Creation (`Kernel::Execute`)
- **Owner**: `CpuCore` / `Kernel`
- **Entry Point**: `Execute`
- **Input**: `entry_point`, stack config.
- **Output**: Guest worker thread executing assembly.
- **Invariant**: One dedicated thread for the primary guest context.
- **Implementation**: Spawns host thread, sets TLS, stack guarantee, uses `StartGuestCaptured` (setjmp).
- **Test Evidence**: None explicit for `StartGuest` wrapper logic.
- **Runtime Evidence**: Thread separation allows UI to remain responsive while guest runs.
- **Platform Evidence**: OS spawns primary thread for process.
- **Classification**: **APPROXIMATION**
- **Known Approximation**: `setjmp`/`longjmp` used to fake cooperative guest exit since SEH cannot cleanly unwind across the asm boundary.
- **Known Hack**: None.
- **Unknowns**: Exact stack layout passed by PS5 OS to `main`.

### Stage 12: VEH Handling
- **Owner**: `Kernel`
- **Entry Point**: `VectoredExceptionHandler`
- **Input**: Hardware exceptions (INT3, AV, ILL).
- **Output**: Emulated state or crash.
- **Invariant**: Must safely resolve known traps or terminate cleanly.
- **Implementation**: Handles `INT3` (syscall), `AV` (TLS stub / demand commit), `ILL` (AMD EXTRQ). Fast sentinel recovery skips `-1` or `NULL` accesses in host DLLs.
- **Test Evidence**: `kernel_sync_tests.cpp` (TLS), AMD software emulation tests.
- **Runtime Evidence**: Catching `0xCC` efficiently. Sentinel recovery prevents crashes on null pointers from stubbed HLE functions.
- **Platform Evidence**: None; this is purely host emulation machinery.
- **Classification**: **HACK** (Sentinel Recovery specifically).
- **Known Approximation**: AMD `EXTRQ`/`MONITORX` emulation.
- **Known Hack**: Fast Sentinel Recovery is a high-risk hack that masks true bugs (dereferencing a null pointer returned from a stub) by zeroing `RCX` and skipping the instruction.
- **Unknowns**: If sentinel recovery has incorrectly masked a fatal guest state corruption in PPSA02929.

---

## 2. SELF / ELF AUDIT

- **IsSelfFile()**: IMPLEMENTED. Correctly identifies magic.
- **SELF extraction**: IMPLEMENTED. Extracts inner ELF structurally.
- **Missing Crypto**: HACK. The loader cannot decrypt retail retail ELFs, relying on users to provide fPKG ELFs.
- **ELF parsing**: SPECIFIED. Follows standard generic ELF structures.
- **PT_LOAD, PT_DYNAMIC**: CHARACTERIZED. 
- **PS5-specific behavior**: APPROXIMATION. PS5 SDK modules `0xFE00..0xFEFF` are treated as PIE.
- **Conclusion**: The loader is largely a generic ELF loader. Assuming generic ELF behavior is automatically correct for PS5 is a known risk.

---

## 3. GUEST VA CONTRACT AUDIT

| Region | Owner | Allocator | Lifetime | Protection | Reuse Policy |
| --- | --- | --- | --- | --- | --- |
| Main Module | Loader | `Memory::Reserve` | Process | Per-segment | Unmapped on exit |
| PRX Modules | Kernel | `Memory::Reserve` | Process | Per-segment | Unmapped on exit |
| Guest Stack | Kernel | `Memory::Map` | Process | R/W | Unmapped on exit |
| HLE Heaps | HLE (`liblibc`) | `Memory::Map` | Process | R/W | Internally recycled |
| Direct Memory | `Kernel` / GPU | `VirtualAlloc` (Direct) | Explicit | R/W | Explicit free |

- **Violation Identified**: The direct memory pool (e.g., allocations for the GPU framebuffer or `sceKernelAllocateDirectMemory`) currently relies on raw `VirtualAlloc` or bypasses the `Memory::` subsystem's strict tracking. The audit confirms `VirtualAlloc` placeholders are still used in parts of the memory subsystem rather than a fully unified allocator.

---

## 4. MODULE / PRX AUDIT

- **Dependency Ordering**: CORRECT (Topological via `g_module_graph`).
- **Duplicates**: CORRECT (`g_prx_modules` dictionary deduplication).
- **Cycles**: CORRECT (Breaker tracks `g_prx_loading`).
- **Missing PRX Fallback**: **HACK**. The statement "Missing PRX modules fall back to HLE silently" allows boot progression but fundamentally violates OS linking rules. A real PS5 would fail to launch if a hard dependency is missing.

---

## 5. IMPORT / NID CONTRACT AUDIT

- **Real HLE Precedence over PRX**: **HACK**. This exists because the emulator cannot easily execute the guest's `libc.prx` initialization routines without full OS state, so it overrides them with native C++ HLE implementations.
- **Auto-stub Returning 0**: **APPROXIMATION**. Exists to prevent crash-on-load. When wrong (e.g., a function expected to return a valid pointer, like `sceAgcCreateShader`), it leads directly to the guest dereferencing `0x0` and crashing (masked by Sentinel Recovery).

---

## 6. RELOCATION AUDIT

- **Applied at**: `LinkModule`.
- **Addends**: Checked. Supported via `R_X86_64_64` and `RELATIVE`.
- **Sequence**: Symbol resolution correctly precedes relocation.
- **Delta**: Module relocation delta is applied consistently via `base_address`.
- **Lifecycle**: Parsing -> Resolution -> Relocation -> Protection.

---

## 7. SYSCALL PATCHING AUDIT

- **Safety**: Patching every `0F 05` is **UNSAFE**. 
- **Risks**: Data mistaken for code (if a constant in `.text` is `0x0F05`), dynamically generated JIT code (which will not be patched unless the memory manager intercepts it), and self-modifying code.
- **Classification**: **HACK**. It is an implementation-specific workaround to catch guest-to-kernel transitions.

---

## 8. TLS AUDIT

- **TLS Data Model**: `PT_TLS` parsed and thread TLS blocks allocated correctly (SPECIFIED).
- **Execution Acceleration**: `TlsPatch` (HACK). It rewrites `fs:` segment accesses in guest code into direct pointer arithmetic. Removing it would only change performance (fallback to VEH AV traps for `fs:`), not semantics.

---

## 9. ENTRY POINT AUDIT

- **Heuristic Scanning (`0xE8` patterns)**: **HACK**.
- **Reason**: The loader lacks the correct PS5 linker metadata parser to find `main` when the binary is stripped of standard symbols. This is major architectural debt, as changing compilers or SDK versions will break the pattern matcher.

---

## 10. CPU / ASSEMBLY CONTRACT

```text
HOST (Kernel::Execute)
 ↓ (C++ ABI: entry_point, stack)
MASM bridge (StartGuest)
 ↓ (Registers cleared, RDI/RSI set to args)
GUEST STATE (Guest code executes)
 ↓ (Syscall trap / Hardware exception)
EXCEPTION / RETURN (VEH / longjmp)
 ↓ 
HOST (HandleSyscall / Process exit)
```

---

## 11. VEH AUDIT

- **Fast Sentinel Recovery**: **HACK**. 
- **Activation**: Triggers on `EXCEPTION_ACCESS_VIOLATION` in host DLLs when `Rip` is high and `faddr` is near `NULL` or `-1`.
- **Behavior**: Zeroes `RCX` and skips the instruction.
- **Danger**: High risk. It can incorrectly convert a real guest crash (due to our auto-stubs returning `0` for complex structures) into successful execution, propagating invalid state deeper into the emulator.

---

## 12. REAL TITLE FIRST-DIVERGENCE ANALYSIS

**PPSA02929 (Dreaming Sarah)**
1. **LOAD**: `pcsx5_load` parses param.json.
2. **MAP**: ELF mapped at `0x800000000`.
3. **IMPORT**: Missing `libc.prx` dependencies fall back to HLE (DIVERGENCE 1: Silent HLE fallback).
4. **IMPORT**: NID resolution overrides `malloc` with C++ HLE (DIVERGENCE 2: Native override).
5. **CPU ENTRY**: Heuristic scanner finds `main` (DIVERGENCE 3: Pattern matching).
6. **RUNTIME**: Game calls `sceAgcCreateShader`. NID is auto-stubbed, returns `0`. Game dereferences `0` to read shader handle. VEH Fast Sentinel Recovery catches the NULL deref, zeroes RCX, and skips the instruction. (DIVERGENCE 4: Sentinel Recovery masking fatal state).
7. **CRASH**: The guest dies shortly after at `RIP 0x800029516` due to the cascaded failure.

*(Note: Logs/Manifests for PPSA21564 were not found in the repository. Analysis is restricted to PPSA02929.)*

---

## 13. BOOT CORRECTNESS SCORECARD

| Stage | Implementation | Characterized | Runtime | Platform Evidence | Classification |
| --- | --- | --- | --- | --- | --- |
| 1. Emulator Init | Yes | Yes | Yes | No | CHARACTERIZED |
| 2. Title Load | Yes | Yes | Yes | Yes | VERIFIED |
| 3. ELF Headers | Yes | Yes | Yes | No | APPROXIMATION |
| 4. VA Layout | Yes | Yes | Yes | No | APPROXIMATION |
| 5. Module Metadata| Yes | Yes | Yes | Yes | SPECIFIED |
| 6. PRX Loading | Yes | Yes | Yes | No | IMPLEMENTED |
| 7. Symbol/NID | Yes | Yes | Yes | No | HACK |
| 8. Syscall Patch | Yes | Yes | Yes | No | HACK |
| 9. Protection | Yes | Yes | Yes | Yes | CHARACTERIZED |
| 10. Entry Point | Yes | No | Yes | No | HACK |
| 11. Guest Thread | Yes | No | Yes | No | APPROXIMATION |
| 12. VEH Handling | Yes | Yes | Yes | No | HACK |

---

## 14. ARCHITECTURAL DEBT REGISTER

1. **P0 (Blocks Correct Boot)**: Fast Sentinel Recovery in VEH masking fatal HLE stub errors.
2. **P0 (Blocks Correct Boot)**: Auto-stub returning zero unconditionally.
3. **P1 (Compatibility Risk)**: `0F 05` byte-scanning for syscalls in executable segments.
4. **P1 (Compatibility Risk)**: Heuristic `main` discovery via `0xE8` assembly pattern matching.
5. **P2 (Maintainability)**: Silent HLE fallback for missing PRX hard dependencies.
6. **P2 (Maintainability)**: Native C++ HLE implementations unconditionally overriding explicit PRX exports.

---

## 16. CLAIMS VS REALITY

| Claim | Code | Tests | Runtime | Platform Evidence | Final Classification |
| --- | --- | --- | --- | --- | --- |
| Missing PRX modules fall back to HLE silently | Yes | Yes | Yes | None (PS5 aborts) | **HACK** |
| Auto-stub returns 0 safely | Yes | Yes | Fails (Deref 0) | None | **APPROXIMATION (Dangerous)** |
| 0F 05 byte scanning is safe | Yes | Yes | Yes | None | **HACK** |
| Sentinel Recovery is safe | Yes | No | Fails (Masks bugs)| None | **HACK** |
| Native C++ overrides PRX exports | Yes | Yes | Yes | None | **HACK** |

---

## 17. FINAL DECISION

**BOOT CONTRACT PARTIALLY VERIFIED**

The loader correctly maps ELFs, resolves dependencies topologically, and handles basic execution. However, progression through the boot pipeline currently relies on fragile hacks (sentinel recovery, syscall byte scanning, auto-stub zero returns, heuristic entry points) that diverge significantly from the true PS5 contract.

### Top 5 Architectural Risks
1. **Sentinel Recovery**: Masks fatal guest crashes.
2. **Auto-stub `0` returns**: Creates cascaded null-pointer dereferences.
3. **Syscall Byte Scanning**: Susceptible to data-as-code corruption.
4. **Heuristic `main` Discovery**: Fragile to compiler/SDK changes.
5. **Native HLE Overrides**: Prevents games from running their shipped libc/libkernel PRX environments.

### First Divergence (PPSA02929)
Silent HLE fallback for missing PRX hard dependencies, rapidly followed by the heuristic `main` scanner. The fatal divergence is `sceAgcCreateShader` returning 0 and being masked by Sentinel Recovery.

### First Divergence (PPSA21564)
*Unknown (Insufficient evidence in repository).*

### Recommended Implementation Task
**Remove Fast Sentinel Recovery from the VEH and implement correct struct/handle returns for critical AGC (GPU) stubs like `sceAgcCreateShader`.**
