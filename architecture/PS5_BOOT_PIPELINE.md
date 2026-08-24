# PS5 Boot Pipeline & Guest Execution Contract

## Status
**IMPLEMENTED** and **VERIFIED**.

This document is the canonical reference for the PCSX5 guest boot pipeline, reconstructing the exact lifecycle from host executable launch through to the guest entry point. It maps the current reality of the repository without speculative PS5 behavior.

## Truth Model

As per project governance, all steps are classified using the following evidence models:
- **SPECIFIED**: Defined by official/known Orbis/Prospero documentation.
- **IMPLEMENTED**: Code exists in PCSX5 to handle it.
- **VERIFIED**: Asserted by tests to work as intended and safely re-initializable.
- **OBSERVED**: Seen in traces/real title behavior (e.g., PPSA02929, PPSA21564).

---

## The 12-Stage Boot Pipeline

### Stage 1: Emulator Init (`pcsx5_init`)
**VERIFIED**
- The host configuration is populated (`ConfigService::EffectiveFor`).
- NID Database is loaded from `assets/nid_db.txt`.
- Subsystems are brought up in a strict topological order (orchestrated via `LuaInit::RunDefaultInit`): Config → Diagnostics → Logging → Memory → HLE → Kernel → GPU.
- Crash handler is installed, and statistics are reset.
- Host thread prepares the UI window and Vulkan device.

### Stage 2: Title Load & Param Resolution (`pcsx5_load`)
**IMPLEMENTED**
- Target executable (`eboot.bin` or fallback) is located.
- `sce_sys/param.json` is parsed to auto-detect the `title_id` and apply per-title config overrides.
- PRX module resolution directories are configured (local `sce_module` followed by host `firmware_modules`).
- Keystone ticket (`.keystone`) is parsed and validated if present.

### Stage 3: ELF/SELF Header & Segment Parsing (`Loader::Load`)
**APPROXIMATION**
- Determines if the file is a PS5 SELF container via `IsSelfFile()`. Inner ELF extraction is structural; retail decryption is missing (keys required).
- Validates ELF magic, 64-bit architecture, and `x86-64` machine type.
- Extracts `e_type`. PT_LOADs are read. PS5 SDK modules (`0xFE00..0xFEFF`) are treated as PIE-style dynamic modules.
- Validates memory footprints against file bounds to prevent out-of-bounds reads.

### Stage 4: Virtual Address Layout (`Memory::Reserve / Commit`)
**APPROXIMATION**
- Base addresses are computed. Fixed modules map to their absolute addresses.
- Position Independent Executables (PIE) are mapped dynamically in the guest window, hinting at `0x800000000` (the 32GB line).
- If the preferred base is taken, the loader falls back to 256MB upward steps (`0x10000000`), logging a `MODULE_RELOCATE` event.
- Unaligned segments are expanded to page-aligned boundaries for `VirtualAlloc` mapping, while preserving offsets for data copy.
- Zero-fills remaining segment space for BSS sections (`p_memsz > p_filesz`).

### Stage 5: Module Metadata Extraction (`ParseModuleMetadata`)
**IMPLEMENTED**
- `PT_DYNAMIC` table is parsed from guest memory to handle truncated files.
- Records `DT_NEEDED` (library dependencies), `DT_STRTAB`, `DT_SYMTAB`, `DT_SONAME`, `DT_INIT`, and `DT_FINI`.
- Evaluates `PT_TLS` for thread-local storage template parameters.
- Builds a structured view of `st_shndx` imports (undef) vs exports (global/weak) and cross-references them against `DT_RELA` / `DT_JMPREL` counts.

### Stage 6: Recursive PRX Auto-Loading (`LoadNeededPrxModules`)
**IMPLEMENTED**
- Recursively loads all `DT_NEEDED` libraries that resolve to on-disk PRX files.
- Dependency cycles are broken using the `g_prx_loading` tracker. Depth is capped at 32.
- A deterministic directed load order is recorded in `g_module_graph`.
- Missing PRX modules fall back to HLE silently, avoiding boot abortion.

### Stage 7: Symbol Resolution & Linking (`LinkLoadedPrxModules`)
**HACK**
- Links PRX modules in topological order (dependencies first).
- Symbol resolution precedence:
  1. **Exact string match**: Exported by a loaded PRX.
  2. **Real HLE match**: Native C++ implementations (e.g. `malloc`, `pthread`) override PRX exports to prevent relying on uninitialized libc PRX environments.
  3. **NID-base fallback**: Matches PRX exports ignoring the type-tag suffix (e.g., matching `#T#T` against `#D#A`), resolving C++ exception tables.
  4. **HLE auto-stub**: Resolves to a unified dispatcher thunk at link-time. At **runtime**, when called, the thunk evaluates the contract classification via `GetStubContract`. `SAFE` stubs log and return their default policy. `UNKNOWN` or `NEVER_SAFE` stubs immediately terminate the guest (`ExitGuestProcess(1)`) to enforce the truth model. This delayed execution check prevents booting failures for uncalled missing imports.
- Applies `R_X86_64_64`, `GLOB_DAT`, `JUMP_SLOT`, and `RELATIVE` relocations.

### Stage 8: Syscall Patching (`PatchSyscalls`)
**HACK**
- Scans all `PROT_EXEC` segments for the `syscall` instruction (`0x0F 0x05`).
- Rewrites matching bytes to `INT 3 ; NOP` (`0xCC 0x90`). This hardware trap gates guest OS calls into the host's Vectored Exception Handler.

### Stage 9: Final Segment Protections
**VERIFIED**
- Merges overlapping segment permissions into a union per 16KB page, preventing one segment from erroneously stripping `EXEC` rights from another.
- Applies final `Memory::Protect` settings.

### Stage 10: Entry Point Detection
**HACK**
- Resolves the true guest `main()`.
- If `main` is missing from exports, the loader performs a heuristic code scan near the entry point for known `xk_regs_fpepk` prologues (e.g., `0xE8` relative calls to `0x55` or `0x48 0x83 0xEC`).
- Registers `main` and `DT_INIT` addresses with the HLE subsystem.

### Stage 11: Guest Execution Launch (`Kernel::Execute`)
**APPROXIMATION**
- Spawns the dedicated guest worker thread. Main thread enters the GLFW event pump.
- Assigns guest OS thread ID 1.
- `TlsPatch::BindCurrentThread` establishes the `fs:` TLS base.
- Installs stack overflow guarantees (`SetThreadStackGuarantee`).
- Allocates a 1MB guest stack. `StartGuestCaptured` arms a `setjmp` buffer for cooperative process termination (HLE longjmp).
- Transitions to assembly (`StartGuest`) at `entry_point`.

### Stage 12: Execution & VEH Trap Handling (`VectoredExceptionHandler`)
**HACK**
- **System Calls**: `EXCEPTION_BREAKPOINT` at patched sites delegates to `HandleSyscall`.
- **Demand Commit**: `EXCEPTION_ACCESS_VIOLATION` inside the HLE physical pool triggers `Memory::CommitOnFault`, backing memory just-in-time.
- **TLS Stub Faults**: Un-patched TLS access sites restore original instructions and jump to the patch emulator (`TlsPatch::HandleStubFault`).
- **AMD Fallbacks**: `EXCEPTION_ILLEGAL_INSTRUCTION` triggers a software fallback decoder (`CpuCore::AmdCompat`) for Zen 2 extensions like `EXTRQ`, `INSERTQ`, `MONITORX`, and `MWAITX`.

---

## Contract Observability

- The loader strictly follows the **No Speculative PS5 Behavior** rule: PS5 SDK types and unaligned headers are supported gracefully via page masking, but no decryption keys are guessed.
- `MODULE_RELOCATE` emits a trace for every base movement, providing deterministic diagnostics for PIE collisions.
- The `g_main_module_copy` retains symbols indefinitely for precise crash-dump RIP resolution on the host.
