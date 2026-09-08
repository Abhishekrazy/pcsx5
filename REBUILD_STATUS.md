# PCSX5 Clean Rebuild Status

## Current phase

**Phase 0: characterization COMPLETE within the scope below. Phase 1 — Build and boundary foundation: ACTIVE.**

## Phase 1 task checkpoints

- [x] Make the regular-commit rule explicit in `AGENTS.md`: commit verified task boundaries and buildable increments, review exact staged paths, and report checkpoint hashes. VERIFIED: documentation diff reviewed; no runtime change.
- [x] P1.1 Implement clean Windows/Linux x64 Debug/Release CI and strict headless presets; local checks below passed. Hosted execution is separately gated by P1.5.
- [x] P1.2 Verify fresh-worktree entry points and finish local Codex setup (saved configuration and manual command execution; automatic app-triggered execution remains unobserved).
  - [x] Fresh Windows/Linux Debug/Release configure, build and test entry points.
  - [x] Save and review the Windows-only Codex setup override; leave cleanup empty.
- [x] P1.3 Enforce core include/link boundaries with positive and negative tests.
- [x] P1.4 Extend portable core tests beyond the scaffold smoke test.
- [ ] P1.5 Record actual hosted CI matrix results before phase closure.

Phase 0 checkpoint: `73a20a0`. Phase 1 work is on `codex/phase-1-build-foundation`. The user authorized pushing this branch and verifying hosted CI. Initial hosted execution exposed the script-policy issue below; Phase 1 remains open until the corrected matrix passes.

### P1.1 implementation and local verification

- VERIFIED: `.github/workflows/ci.yml` now defines four isolated Windows 2025/Ubuntu 24.04 Debug/Release jobs using the clean root presets. Repository permissions are read-only; checkout does not persist credentials or fetch submodules/LFS. Only diagnostic logs are uploaded; all legacy packaging, retail PKG targets and release jobs are removed from this workflow.
- VERIFIED: the existing checkout v7, upload-artifact v7 and MSVC setup v1 action references were resolved through their upstream GitHub APIs and pinned to full commit IDs. Their used inputs were checked. No new action or project dependency was added.
- VERIFIED: CMake 4.3.3 accepts the version-6 presets; the dependency-free `pcsx5_build_preset_contract` test checks all four names, build types, host conditions, compilers, output paths and strict test settings. Existing local PyYAML parsed the workflow and checked its matrix/triggers/permissions/action count; no package was installed. This is not a hosted workflow or full expression-linter result.
- VERIFIED: Windows MSVC 19.51 Debug and Release each pass **2/2 CTests**. Both presets reject a deliberately empty selected suite. Normal test runs were repeated afterward to restore passing `ctest.log`/`junit.xml` artifacts.
- VERIFIED: `bootstrap-windows.cmd` accepts either Windows preset, defaults to Debug, works from a different current directory, rejects an invalid preset, and propagates configure/build/test failures including negative exit codes.
- VERIFIED: the existing Ubuntu x86-64 GCC 15.2 compiler builds and runs the clean-core smoke test with `-O0 -g` and `-O2 -DNDEBUG`, using C++23 and the project's warning flags. No warnings were emitted. These manual builds are not Linux CMake/CTest preset runs.
- Historical P1.1 limit: Ubuntu initially lacked CMake/Ninja, so Linux preset execution was UNKNOWN at that checkpoint. P1.2 below supersedes this local limitation. Hosted Windows/Linux workflow results remain UNKNOWN; nothing has been pushed or dispatched.
- VERIFIED: local diff/whitespace review passed. Commit rule checkpoint: `63a7969`. The unfinished `.codex/` environment file remains excluded.

### P1.2 fresh-worktree verification and saved Codex configuration

VERIFIED on 2026-09-08:

- With explicit user approval, installed Ubuntu CMake 4.2.3 and Ninja 1.13.2 plus five required supporting packages. The reviewed installation added seven packages, upgraded none and removed none. No project dependency was added and no unrelated system upgrades were applied.
- Created a detached Windows Git worktree at `out/verification/p1-2 fresh` from `89563d6c12538ef3b4a99678c64163cc2c8cd02d`. Its build output directory did not exist before verification. No submodules, dependency restores or build caches were fetched; the path intentionally contains a space.
- Windows MSVC 19.51.36256.0: `cmd /d /c bootstrap-windows.cmd windows-x64-debug` and the corresponding Release command each configured from scratch, built and passed **2/2 CTests** without compiler warnings. The exact setup command `cmd /d /c bootstrap-windows.cmd` also passed when rerun from this worktree.
- Ubuntu 26.04 x86-64 under WSL, GCC 15.2.0: each Linux preset passed `cmake --preset <preset>`, `cmake --build --preset <preset> --parallel 2`, and `ctest --preset <preset>` with **2/2 tests**, without compiler warnings. These builds used the same fresh Windows-created worktree through `/mnt/i`; this is not a Linux-native Git checkout or the hosted Ubuntu 24.04 runner.
- Both Linux test presets rejected an intentionally absent test selection with CTest status 8. Normal suites were rerun afterward to restore passing logs. Windows empty-suite rejection was already verified in P1.1.
- All four JUnit reports record two tests and zero failures. Compile-command inspection finds only `core/src/version.cpp` and `tests/core_smoke.cpp`; caches point to the fresh source root and their intended compiler/build type. The fresh worktree has no tracked or untracked changes visible to `git status --short`.
- Build logs remain under the ignored `out/verification/p1-2 fresh/out/build/<preset>/` directories. The verification worktree is retained locally; no user source was deleted.

VERIFIED: the user corrected and saved `.codex/environments/environment.toml` through the environment editor. Standard-library TOML parsing confirms an empty default setup, `setup.win32.script` equal to `cmd /d /c bootstrap-windows.cmd`, and no cleanup, actions or secret values. The Windows command was rerun manually from the verification worktree and passed **2/2 CTests**. The reviewed environment configuration is included in this checkpoint; the earlier build-evidence checkpoint is `e44034d`.

Remaining limits: automatic setup triggered by Codex worktree creation remains UNKNOWN; saved configuration and manual execution of its command are verified. Non-Windows automatic setup is intentionally unconfigured; Linux configure/build/test entry points are verified above. No hosted CI or emulator-platform support claim follows from these scaffold tests.

### P1.3 core boundary enforcement

- VERIFIED: the deferred root CMake check inspects actual core target sources, include roots, link/interface properties and selected compiler-injection routes. The core currently admits no link dependencies. The whole core tree is checked at configure and before builds, even with `BUILD_TESTING=OFF` (hook placement reviewed).
- VERIFIED: 38 synthetic boundary fixtures require their specific guard diagnostic; the positive fixture must also compile. Coverage includes OS/UI/graphics/legacy headers, nested and nonliteral includes, external sources/roots, direct/interface/transitive links, generated/opaque sources, PCH, compiler flags and deferred changes. An existing header edited after configure is rejected by the build-time scan.
- Independent review reproduced and then verified fixes for false-valued library names (`OFF`), strings concealing includes during comment removal, tight import syntax, includes of skipped CMake files and child-directory-local compiler flags. These cases now have regressions. The checker remains conservative rather than a full C++ parser or security sandbox; its toolchain and semantic limits are recorded in `ARCHITECTURE.md`.
- VERIFIED: Windows MSVC and WSL Ubuntu GCC Debug/Release configure/build/test runs each passed **40/40 CTests** (38 boundary cases plus the two existing scaffold tests). Linux Debug was run by the implementation agent; the main agent ran Windows Debug/Release and Linux Release. No project dependency, legacy behavior change or remote write was made.
- The portable range API contract is defined for P1.4; its implementation/tests are a separate checkpoint, not part of the P1.3 executable gate.

### P1.4 portable guest-address range

- VERIFIED: newly authored `guest_address_range` implements the documented nonempty, checked 64-bit byte-range contract without OS headers, host pointers, allocations or link dependencies. It does not validate PS5 addresses, own mappings or grant memory access.
- Test-first evidence: the new test initially failed compilation because the contract header was absent. The implementation then passed the independent occupied-byte oracle, explicit edge cases and 288 near-maximum factory cases. Each executable run reports **158,880 checks, zero failures**; checks remain active with `NDEBUG`, and compile-time assertions check construction invariants and maximum-address behavior.
- VERIFIED: the CTest repeatability harness runs the executable twice, requires successful exits and valid success summaries, and compares normalized stdout. CTest timing/JUnit timestamps are not claimed deterministic. Existing no-tests-as-error presets remain enforced by the preset-contract test.
- VERIFIED: integrated Windows MSVC and WSL Ubuntu GCC Debug/Release builds each pass **42/42 CTests**. The range suite also passed GCC AddressSanitizer + UndefinedBehaviorSanitizer with `-DNDEBUG` and strict warnings; no diagnostics were emitted. No sanitizer package or test framework was installed.
- Independent review found no blocking arithmetic, invariant or Release-test issue; its suggested additional near-maximum rejection matrix was included and verified. Tests use only newly authored synthetic values; no legacy or proprietary fixtures were imported.
- P1.3 checkpoint: `b3b0971`. P1.4 is a separate implementation/test checkpoint. ARM64, Apple, Android, full emulator execution and hosted CI remain unverified.

### P1.5 hosted execution and script-policy correction

- VERIFIED: authorized push of `e1aee343e1c263333e301d3e380c862bfb9af5ee` created [run 34260363762](https://github.com/Abhishekrazy/pcsx5/actions/runs/34260363762). Both Windows jobs passed; both Ubuntu jobs failed during the build-time boundary scan before tests ran.
- VERIFIED: hosted Ubuntu CMake 3.31.6 reported unset policy CMP0057 and rejected `IN_LIST` in the standalone `-P` process. Root configure succeeded because its policy version was already declared; that policy does not carry into a separate script process. Local CMake 4 runs had masked the omission.
- Correction: all three clean standalone script entry points now declare the same CMake 3.25 baseline as the root. The existing preset-contract test checks those declarations and was observed failing before the correction. No boundary enforcement was removed and no toolchain dependency was changed. Hosted success remains pending the corrected commit's run.
- VERIFIED after correction: local Windows and WSL Linux Debug builds each passed **42/42 CTests**, including the new policy-baseline assertion and build-time scan. CMake 3 execution will be verified by the hosted rerun, not inferred from these CMake 4 checks.

## Phase 0 closure record

Closure decision, 2026-09-08: the RT-01–RT-08 Windows characterization matrix now exercises the real selected boundaries, including syscall traps, guest-stack exit/fault attempts and cleanup hooks. Failed legacy invariants are recorded as failures, not converted into accepted runtime behavior. Phase 0 establishes evidence for redesign; it does not require preserving defects in the new implementation or prove platform support.

## Preservation and scope

- VERIFIED: legacy checkpoint `f1244ff` exists. The rebuild checkpoint covers completed guidance cleanup, the clean scaffold and Phase 0 characterization; the unfinished local Codex environment configuration is excluded.
- The user explicitly approved testability-only legacy extraction on 2026-09-08. Entry/capture moved to `src/kernel/guest_execution.*`; HLE lifecycle/TLS state moved to `src/hle/guest_lifecycle.*`. Original callers use the extracted functions.
- The real exception handler stays in `src/kernel/kernel.cpp`; `exception_entry.h` exposes its address plus optional diagnostic providers. Defaults call the original providers. Tests supply empty history/timeline data, not substitute trap/exit behavior.
- VERIFIED: independent [extraction review](tests/characterization/extraction-review.json) compared all 18 moved bodies and reset order/scope against the checkpoint. Trap-body differences are diagnostic-provider substitutions only. Source equivalence is not binary/unwind or full-application equivalence.
- The extraction task made no semantic fixes, dependency installations/imports or additional deletions. No legacy code was moved into the clean core. The subsequent user request authorizes regular local commits of completed work, not remote pushes.
- The clean C++23 root excludes `src/`. The standalone C++20 characterization build compiles selected real legacy sources. Any restored full legacy build must include both new implementation files exactly once; the complete legacy emulator was not built.

## Executable characterization

Entry point: `cmd /d /c tools\characterize-windows.cmd`.
Output: ignored `out/build/characterization-windows-x64/`. Tests use isolated working directories and bounded timeouts; fatal lifecycle cases run in hidden child processes with five-second waits. A timeout fails the test.

| Test/group | Evidence and important limit |
|---|---|
| memory_validation, memory_query, guest_memory_access | Memory lifecycle/query/guarded access; legacy regression expectations |
| memory_write_tracker, kernel_memory | Write generations and guest protection mapping |
| tls_context, tls_patch_stub | TLS arithmetic/bounds and emitted byte patterns |
| memory_fault_routes | Actual owned AV recovery, tracked-write precedence, rejection propagation and ownership gaps |
| tls_thread_execution | Patched load across two bound threads, rebinding, fallback, invalidation and red-zone clobber; one RAX 64-bit displacement-zero load form |
| dispatcher_execution | Actual assembly argument/return mapping, GPR/XMM sentinels, alternate stack and StartGuest entry. Injected HLE endpoints and test-only escape isolate assembly behavior |
| register_context | Actual CPUState conversion in both directions, 106 checks; no full x87/extended-state accuracy claim |
| syscall_dispatch | Actual table registration/dispatch and signed return/rejection with synthetic handler; not default syscall-table initialization |
| kernel_trap_* | Nine real-handler syscall/context/breakpoint/AV routes plus cleanup-hook repetition/reentry. Executed guest/host-address code on the host stack; empty diagnostic data supplied |
| guest_lifecycle_execution | Real extracted StartGuestCaptured, original assembly and HLE exit state. Host-stack control, switched-stack/direct-C++ controls, exact guest AV observation, unarmed exit and worker exit with an armed main-thread jump environment |
| guest_lifecycle_kernel | Real entry, VEH, table and HLE together. Synthetic syscall 501 resumes with six arguments/result; syscall 1 wrapper calls real SysExit in both modes; tracked guest-address AV records a real guest crash before fatal recovery. Default table initialization and full Kernel::Execute are not exercised |
| lifecycle_state | 22 focused checks on real HLE stop/crash/reset state and thread-local host-stack/XMM storage; reset scopes and existing XMM block layout retained |
| kernel_sys_exit_process | Real standalone SysExit(42) terminates with status 42 and exactly one flushed hook marker |
| trace_contract, trace_sample | Versioned syntax validation, including 20 malformed-input rejection cases; invented sample is format evidence only |
| *_trace_repeatability, kernel_trace_* | Dispatcher, lifecycle and selected kernel producers emit asserted, schema-valid, byte-identical semantic summaries across two runs; not instruction replay |
| fixture_provenance | 21 reviewed fixture-source hashes and input-origin records; not license clearance |

Abort-only link tripwires isolate unused dependencies and fail if reached. Exercised trap, register conversion, syscall dispatch, crash state and exit behavior are real implementations. Full initialization, graphics, loader and active background-thread teardown are outside this fixture scope.

## Measured lifecycle outcomes

VERIFIED on the local Windows Debug toolchain; these are observed statuses, not diagnoses of every nested fault:

| Route | Observed outcome |
|---|---|
| Host-stack real HLE exit; host-stack in-process SysExit control | Captured guest status 42; host process returns normally |
| Switched-stack HLE exit, both sentinel-wrapper and direct-C++ controls | Process terminates with `0xc0000028`; real exit callback reached once; captured return not reached |
| Synthetic syscall 501 on switched stack, then HLE exit | Handler called once, six arguments match, resumed RAX is `0x1234`; subsequent exit terminates with `0xc0000028` |
| Syscall 1 → real in-process SysExit on switched stack | Real SysExit reached with status 42; process terminates with `0xc0000005` or `0xc0000028`, no captured return or cleanup hook. Both statuses observed during repetition |
| Syscall 1 → real standalone SysExit on switched stack | Process status 42; cleanup hook exactly once, including same-thread reentry |
| Exact fixture AV on switched stack, with and without real kernel VEH | Process terminates with `0xc0000005`; exact fixture AV observed; captured return not reached |
| Tracked guest-address AV with real VEH on switched stack | Original fault reaches post-handler observer with real HLE crash code/RIP recorded; process subsequently terminates with `0xc0000005` |
| Unarmed HLE exit; worker exit while main-thread jump environment is armed | Process terminates with requested status 42 |

Termination prevents post-return host GPR/XMM/RSP/TEB comparisons: restoration is **UNKNOWN/unobserved and its required invariant FAILED**, not verified. Nested diagnostic counts and in-process SysExit status vary; exact values remain in test output. The normalized summary marks that raw status unknown and records failure to return. Its repeatability is not a claim of deterministic runtime behavior. The precise causal chain of nested faults remains UNKNOWN. Cleanup evidence is sequential/reentrant with no active heartbeat, not concurrent exactly-once or full application teardown.

## Legacy risks and rebuild gates

| Finding | Evidence/classification | Required rebuild gate |
|---|---|---|
| Guest calls leak host RDI/RSI and XMM6 | VERIFIED dispatcher_execution | Preserve complete host ABI state at every boundary |
| InvokeGuestOnStack callback alignment/shadow-space defect corrupts saved GPRs | VERIFIED dispatcher_execution | Explicit frame ownership/alignment and host-state sentinels |
| StartGuest host callback stack is misaligned | VERIFIED dispatcher_execution | Test entry preparation as well as guest entry |
| Switched-stack exit/fault paths terminate instead of restoring host execution | VERIFIED outcomes above | Typed execution result and supported recovery with all exit paths tested |
| Untracked host AV reaches guest callback; demand commit accepts untracked reservations | VERIFIED memory_fault_routes | Ownership-filtered routing; unrelated faults remain unhandled |
| In-process kernel filtering uses successful native Query, not guest ownership | VERIFIED kernel_trap untracked breakpoint/host AV fixtures | Separate owned guest regions from queryable native memory |
| TLS patch stub overwrites a guest leaf red-zone slot | VERIFIED tls_thread_execution | Register/flag/stack preservation before reuse |
| CPUState uses FltSave.MxCsr while the other native MxCsr stays inconsistent | VERIFIED register_context; OS effect UNKNOWN | Explicit single-value normalization |
| Incoming XMM block stores first eight contiguous qwords from the spill, not eight low lanes; lifecycle reset leaves TLS values intact | VERIFIED lifecycle_state and source inspection | Define explicit lane layout and per-thread reset lifetime |
| Successful StartGuestCaptured longjmp branch does not disarm the global exit environment | INFERRED risk from retained source; this switched-stack branch did not return in tests | Owned per-execution exit state with explicit lifetime |

These findings disqualify mechanical copying of the legacy runtime. They neither prove a target impossible nor guarantee future platforms need no redesign.

## Phase 0 checklist and invariant disposition

- [x] P0.1 Preserve checkpoint and identify dirty user work.
- [x] P0.2–P0.4 Consolidate inventory, architecture and subsystem migration decisions in `ARCHITECTURE.md`.
- [x] P0.5 Exercise the bounded RT-01–RT-08 Windows matrix with real selected implementations and measured failure outcomes.
- [x] P0.6 Define synthetic-input admission, provenance records and normalized trace v1 summaries. Interpreter/instruction replay remains later work.
- [x] P0.7 Record local dependency/license observations and explicit reuse restrictions.
- [x] P0.8 Complete extraction/fixture evidence review and record the clean-build/CI handoff. Phase 1 tasks are defined, not silently completed.

| Original gate | Characterization result; portable invariant disposition |
|---|---|
| RT-01 reserve/commit/protect/query | VERIFIED local lifecycle/protection behavior; cross-host equivalence UNKNOWN |
| RT-02 first-touch demand commit | VERIFIED behavior including FAILED ownership isolation |
| RT-03 guarded copy | VERIFIED selected generated-input regression cases |
| RT-04 register conversion | VERIFIED modeled fields and MXCSR inconsistency; full native/extended-state accuracy UNKNOWN |
| RT-05 syscall register ABI | VERIFIED real CC90 trap/table, six argument registers, signed return and resume; direct CONTEXT changes only RAX/RIP. Guest PS5 ABI accuracy not established |
| RT-06 guest entry/exit | VERIFIED entry/return, real exit/fault attempts and bounded hook behavior; host-restoration invariant FAILED/unobserved after fatal paths; concurrent/full teardown UNKNOWN |
| RT-07 thread TLS | VERIFIED narrow Windows patched-load isolation plus FAILED red-zone preservation; other forms/flags/non-Windows strategy UNKNOWN |
| RT-08 fault classification | VERIFIED demand-commit, syscall, breakpoint and guest/host AV routes; ownership isolation FAILED, fatal recovery remains unsafe |

## Corpus and dependency decisions

- VERIFIED: newly authored fixtures use synthetic state/code and test-owned memory, with no retail input files. Existing seven memory/TLS fixtures use generated inputs; historical authorship is UNKNOWN.
- Unreviewed replays, graphics goldens, binaries and assets are excluded from the acceptance corpus. No broad legal-data clearance, redistribution approval or clean-room certification was inferred.
- [Dependency review](tests/characterization/dependency-review.json) covers declarations, selected cached revisions/notices, vendors, shell/tool packages, optional loaders and selected attribution. It is not a full SBOM or legal opinion.
- All third-party/legacy imports into the clean core remain unapproved. Unresolved issues include SharpEmu/Kyty-derived attribution, PkgToolBox licensing, inconsistent DualSenseWindows modification notes, and transitive/binary/asset provenance. Existing notices remain intact.

## Environment readiness and next boundary

- VERIFIED: fresh Windows MSVC and WSL Ubuntu GCC Debug/Release builds pass the smoke and preset-contract tests; see P1.2 above.
- VERIFIED: native process launch previously stalled in the sandbox; approved outside-sandbox characterization works. The runner rejects negative and positive failure codes.
- VERIFIED: the editor-generated environment now configures only Windows setup as `cmd /d /c bootstrap-windows.cmd`, with default setup empty and no cleanup command. TOML structure and manual execution passed; see P1.2 above.
- VERIFIED: `.github/workflows/ci.yml` has been replaced locally with clean build-and-test jobs. The remote workflow is unchanged until an authorized push; do not treat remote legacy packaging as rebuild release evidence.
- UNKNOWN: hosted Windows/Linux CI results and automatic Codex worktree setup. New worktrees must include the Phase 1 commits; the local WSL builds do not establish hosted runner success.

Next: verify the corrected P1.5 hosted matrix following the authorized branch push. Local P1.1-P1.4 work is complete, but Phase 1 remains open until the hosted gate passes. After Phase 1 closure, the next phase is narrow runtime contracts and the Windows leaf implementation. Additional dependencies, merges, releases and semantic legacy fixes remain unapproved.

## Phase 0 integrated verification (before Phase 1)

VERIFIED on 2026-09-08:

- `cmd /d /c tools\characterize-windows.cmd`: updated sources compiled/linked without compiler warnings; **36/36 CTests passed**.
- `ctest --test-dir out/build/characterization-windows-x64 --output-on-failure --no-tests=error --repeat until-fail:5`: **all 36 tests passed five repetitions each** (180 CTest executions; 45.57 seconds). Trace tests themselves run each producer twice.
- An earlier repetition failed the single-status in-process SysExit expectation. Both AV and BAD_STACK were then recorded explicitly; the final test accepts only those observed failures with the required path markers. This is characterization of unstable legacy behavior, not a runtime fix.
- `cmd /d /c bootstrap-windows.cmd`: clean scaffold configured/built; **1/1 core smoke test passed**.
- Fixture provenance: **21 records** pass normalized-source hash validation.
- Dependency/extraction review JSON parsed; `git diff --check` reported no whitespace errors (existing LF/CRLF notices remain).
- Checkpoint review additionally checked newly staged files: one trailing-whitespace blank line in `src/hle/guest_lifecycle.cpp` is retained verbatim from the original ExitGuestProcess body, consistent with the extraction hashes. No semantic change was made to remove it.
- Independent extraction and fixture reviews found no blocking source-equivalence or test false-positive issue within their stated scopes.
- Windows Debug only. No Linux/full-emulator execution, instruction replay or supported-platform claim follows from these results.
