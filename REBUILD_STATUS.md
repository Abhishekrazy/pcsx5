# PCSX5 Clean Rebuild Status

## Current phase

**Phase 0: characterization COMPLETE within the scope below. Phase 1 — Build and boundary foundation: COMPLETE. Phase 2 — Narrow runtime contracts and Windows leaf implementation: COMPLETE within the acceptance scope below. Phase 3 — Portable core integration and Linux runtime: COMPLETE. Phase 4 — Graphics HAL and Vulkan: IN PROGRESS.**

## Phase 4 acceptance tasks

Phase 4 is IN PROGRESS; no graphics gate is complete yet.

- [x] P4.1 Approve/verify Vulkan tools and record exact dependency use.
- [x] P4.2 API-neutral offscreen HAL and validation contract with unit tests.
- [ ] P4.3 Real Vulkan draw/readback, bounded resources, errors and cleanup.
- [ ] P4.4 Synthetic replay with independent pixel expectations and validation.
- [ ] P4.5 Windows/Linux Debug/Release acceptance, repeatability, review and commits.

P4.1 environment evidence (2026-09-09): user approved the Vulkan dependency set.
VERIFIED: installed five Linux packages with no upgrades/removals: libvulkan-dev
1.4.341.0-1, vulkan-tools 1.4.341.0+dfsg1-1, vulkan-validationlayers 1.4.341.0-1,
glslang-tools 16.2.0-2 and required spirv-tools 2026.1-1. Existing Linux loader is
1.4.341.0-1 and Mesa Vulkan driver is 26.0.3-1ubuntu1. Windows uses the pre-existing
SDK 1.4.357.0, glslang 16.4.0 and SPIRV-Tools v2026.3.rc1.

VERIFIED: vulkaninfo reports Linux llvmpipe (software Vulkan, LLVM 21.1.8), Windows
NVIDIA RTX 5070 Ti (616.56) and Intel Graphics (101.5869). Khronos validation layer
is present on both hosts. Enumeration is not rendering acceptance. Windows loader
also reports installed overlay hooks and ReShade load errors; acceptance processes
will disable implicit layers only, not alter installations or disable explicit
Khronos validation. Linux loader emits display-extension warnings; no surface is
required by this offscreen implementation.

Header notice observation: installed Windows vulkan.h/vk_platform.h/vulkan_core.h
declare Apache-2.0 OR MIT; Linux vulkan.h declares Apache-2.0. No headers or SDK
binaries are copied into the repository. This is build-time dependency evidence,
not a combined-work redistribution/license compatibility certification. Packaging
and distribution obligations require a later review; the root license stays intact.

### Phase 4 implementation checkpoint

- VERIFIED: HAL unit tests observed missing `validate_frame` linkage before
  implementation, then pass 750 checks including exhaustive small-coordinate
  winding/degeneracy cases and signed-coordinate limits. Core remains unchanged.
- VERIFIED: four opt-in `*-x64-graphics-*` presets require installed SDK/tools;
  the four original presets retain no Vulkan dependency. Shader sources are newly
  authored GLSL, built and validated as Vulkan 1.1 SPIR-V and embedded privately.
  No shader binary or legacy asset was imported. The preset-policy test covers
  all eight configurations and standalone CMake policy baselines.
- VERIFIED: Windows and WSL Linux graphics Debug each pass 62/62 CTests. Actual
  draw/readback, default factory, synchronization validation, missing-layer failure,
  replay on fresh/reused owners, synthetic resource failures, retry and implicit
  destructor observation all pass. Base Debug builds passed 58/58 on both hosts.
- The first native run exposed a Vulkan 1.0 instance versus 1.1 shader mismatch;
  instance and device requirements were corrected to 1.1. Adding private failure
  tests exposed directory-scoped imported CMake targets; tests now discover their
  private SDK target explicitly. The replay matcher was updated alongside the
  stronger asserted transcript; no failed test was skipped to obtain a pass.
- Independent review prompted asymmetric geometry to detect vertical flips,
  contrasting shared edges to detect holes/double coverage, a real default factory
  check, explicit synchronization validation and observable destructor cleanup.
  Native lifetime/synchronization review found no remaining blocking defect.

Remaining before closure: Release graphics matrix, repeated/cross-device replay,
final diff and acceptance audit. Injected errors occur before native operations;
actual hardware loss, memory exhaustion and all-vendor support remain UNKNOWN.

## Phase 3 acceptance tasks

Phase 3 is COMPLETE. Its original deliverable is portable core plus Linux
runtime with equivalent headless tests on Windows and Linux. Implementation must
establish integration, not merely compile Linux factory declarations.

- [x] P3.1 Linux owned-memory provider under the existing lifecycle contract;
  shared Windows/Linux lifecycle tests plus Linux native failures and cleanup.
- [x] P3.2 Linux workers and monotonic timing; shared behavioral corpus and
  platform-specific failure checks, with no host types in public interfaces.
- [x] P3.3 Linux owned fault observation, including real synthetic signal delivery,
  owner isolation, unsupported cases and handler disposition/lifetime evidence.
- [x] P3.4 Portable core guest-memory ownership/mapping and runtime-backed storage
  integration. Test guest bounds, overlap, permissions, cleanup and error mapping
  against real providers on both hosts; retain the core dependency guard.
- [x] P3.5 Equivalent deterministic headless integration output on both hosts,
  four Debug/Release preset gates, independent review, evidence/remaining risks.

No guest instruction execution, graphics, firmware, guest ABI implementation or
platform support is inferred from this foundation; those have later phase gates.

### Phase 3 implementation checkpoint

VERIFIED on 2026-09-09 (final acceptance audit follows):

- New Linux memory provider uses anonymous data mappings, explicit contract-state
  metadata and conservative uncertain-state errors after native protection failure.
  The same lifecycle source runs on Windows and Linux. Linux native failure tests
  cover partial protection changes, discard failure/retry/zero-fill, release retry,
  independent owners and mincore evidence that successful unmap removed the mapping.
- Linux standard-library workers run the same lifecycle/failure test source as
  Windows. CLOCK_MONOTONIC has native failure/malformed/overflow tests and 4096
  nondecreasing samples; portable elapsed conversion remains shared.
- Linux owner capture occurs before signal installation; signal-time observation
  uses scalar records and always leaves the fault unhandled. Four real synthetic
  SIGSEGV child cases cover read/write and owned/other-owner forwarding, requiring
  exact exit 73 through the prior test handler. Synthetic cases cover unsupported
  contexts and execute/backing-store records; no recovery or global handler added.
- Portable guest-memory mappings own core-defined backing objects and enforce
  checked bounds, non-overlap and guest permissions. Runtime implements the backing
  outside core. Tests cover retained ownership on map/unmap failures, final-address
  arithmetic, full-range rejection before copy, error normalization and cleanup.
  A forwarding wrapper observes successful real-provider release on guest teardown.
- Test-first evidence: new core test failed for missing guest-memory header before
  implementation; Linux agents observed missing-provider link failures before
  implementation. Focused GCC Debug/Release tests passed. Core mapping also passed
  AddressSanitizer + UndefinedBehaviorSanitizer locally, without new packages.
- Independent review identified optional operations in the signal path and a
  premature destructor-failure marker. Both were corrected and focused tests rerun:
  signal path is scalar-only, and the marker now originates in the failing native
  unmap callback. Review also prompted bridge operation-failure and real cleanup
  assertions rather than treating mere destructor execution as proof.

Implementation checkpoint: `27a51d1` on `codex/phase-3-linux-core`. Heap allocation
exhaustion itself is not injected; exception-to-error paths have source review.
No new project dependency, legacy emulator modification or remote push occurred.

### Phase 3 acceptance audit and closure

VERIFIED for implementation commit `27a51d1`:

| Requirement | Executed evidence |
|---|---|
| P3.1 Linux memory | Shared `pcsx5_runtime_memory`; Linux native failure and exact destructor-termination tests; native unmap observations |
| P3.2 Workers and timing | Shared worker lifecycle/failure/termination corpus; Linux clock malformed/overflow/native sampling; shared portable tick conversion |
| P3.3 Fault ownership and forwarding | Linux x64 synthetic records and four actual SIGSEGV child cases per run; exact prior-handler exit; unchanged parent dispositions and native records |
| P3.4 Portable core integration | `pcsx5_guest_memory`, backing-failure tests and real-provider cleanup test on both hosts; all 38 core boundary fixtures pass without guard exceptions |
| P3.5 Equivalent headless behavior | Same guest-memory integration source runs real storage + host worker + join + monotonic clock on both hosts; common expected semantic summary is checked twice by CTest and actual outputs from all four builds were directly compared equal |

| Local preset | Result |
|---|---|
| Windows x64 MSVC Debug | 57/57 passed |
| Windows x64 MSVC Release | 57/57 passed |
| WSL Ubuntu x64 GCC Debug | 57/57 passed |
| WSL Ubuntu x64 GCC Release | 57/57 passed |

All 14 runtime-labeled Debug tests on each host additionally passed five repetitions
per test (140 executions across hosts). Checks remain active in Release. Core ASan
and UBSan passed; worker/timing sanitizer checks were separately run by their agent.
No claim is made that intentional native fault tests are sanitizer-compatible.

A later direct Windows CTest invocation outside the Visual Studio environment
failed compiler initialization in the boundary fixtures. Re-running the documented
`bootstrap-windows.cmd windows-x64-debug` entry point passed 57/57 without source
changes. The final four JUnit reports each contain 57 tests, zero failures and zero
disabled tests; Linux Debug was also restored after repeat-only verification.

The cross-host summary is a post-assertion semantic result, not an instruction
trace, replay oracle, timing-equivalence measurement or PS5 accuracy claim. Linux
execution was on the installed WSL Ubuntu environment, not a newly observed hosted
runner. Hosted Phase 3 CI is UNKNOWN: the new branch was not pushed, in accordance
with the explicit no-push rule. The unchanged CI workflow will run the same presets
when a push is authorized; no hosted result is inferred from local tests.

Phase 3's portable-core/runtime integration gate is met. Later phase gates remain:
graphics, interpreter, guest scheduling/ABI/devices, direct execution, ARM64 JIT,
Apple/Android and actual emulator compatibility. These tests establish the declared
Windows/Linux x64 runtime foundation, not a supported game-running emulator.

## Phase 1 task checkpoints

- [x] Make the regular-commit rule explicit in `AGENTS.md`: commit verified task boundaries and buildable increments, review exact staged paths, and report checkpoint hashes. VERIFIED: documentation diff reviewed; no runtime change.
- [x] P1.1 Implement clean Windows/Linux x64 Debug/Release CI and strict headless presets; local checks below passed. Hosted execution is separately gated by P1.5.
- [x] P1.2 Verify fresh-worktree entry points and finish local Codex setup (saved configuration and manual command execution; automatic app-triggered execution remains unobserved).
  - [x] Fresh Windows/Linux Debug/Release configure, build and test entry points.
  - [x] Save and review the Windows-only Codex setup override; leave cleanup empty.
- [x] P1.3 Enforce core include/link boundaries with positive and negative tests.
- [x] P1.4 Extend portable core tests beyond the scaffold smoke test.
- [x] P1.5 Record actual hosted CI matrix results before phase closure.

Phase 0 checkpoint: `73a20a0`. Phase 1 work is on `codex/phase-1-build-foundation`. The user authorized pushing this branch and verifying hosted CI. The corrected matrix passed at `550e0e9`; the exact run and closure limits are recorded below. No merge or release was performed.

### P1.1 implementation and local verification

- VERIFIED: `.github/workflows/ci.yml` now defines four isolated Windows 2025/Ubuntu 24.04 Debug/Release jobs using the clean root presets. Repository permissions are read-only; checkout does not persist credentials or fetch submodules/LFS. Only diagnostic logs are uploaded; all legacy packaging, retail PKG targets and release jobs are removed from this workflow.
- VERIFIED: the existing checkout v7, upload-artifact v7 and MSVC setup v1 action references were resolved through their upstream GitHub APIs and pinned to full commit IDs. Their used inputs were checked. No new action or project dependency was added.
- VERIFIED: CMake 4.3.3 accepts the version-6 presets; the dependency-free `pcsx5_build_preset_contract` test checks all four names, build types, host conditions, compilers, output paths and strict test settings. Existing local PyYAML parsed the workflow and checked its matrix/triggers/permissions/action count; no package was installed. This is not a hosted workflow or full expression-linter result.
- VERIFIED: Windows MSVC 19.51 Debug and Release each pass **2/2 CTests**. Both presets reject a deliberately empty selected suite. Normal test runs were repeated afterward to restore passing `ctest.log`/`junit.xml` artifacts.
- VERIFIED: `bootstrap-windows.cmd` accepts either Windows preset, defaults to Debug, works from a different current directory, rejects an invalid preset, and propagates configure/build/test failures including negative exit codes.
- VERIFIED: the existing Ubuntu x86-64 GCC 15.2 compiler builds and runs the clean-core smoke test with `-O0 -g` and `-O2 -DNDEBUG`, using C++23 and the project's warning flags. No warnings were emitted. These manual builds are not Linux CMake/CTest preset runs.
- Historical P1.1 limit: Ubuntu initially lacked CMake/Ninja, so Linux preset execution was UNKNOWN at that checkpoint. P1.2 supersedes this local limitation; P1.5 supplies later hosted results. Nothing had been pushed or dispatched at P1.1.
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
- P1.3 checkpoint: `b3b0971`; P1.4 checkpoint: `e1aee34`. ARM64, Apple, Android and full emulator execution remain unverified. Hosted CI was unverified at P1.4 and is recorded separately below.

### P1.5 hosted execution and script-policy correction

- VERIFIED: authorized push of `e1aee343e1c263333e301d3e380c862bfb9af5ee` created [run 34260363762](https://github.com/Abhishekrazy/pcsx5/actions/runs/34260363762). Both Windows jobs passed; both Ubuntu jobs failed during the build-time boundary scan before tests ran.
- VERIFIED: hosted Ubuntu CMake 3.31.6 reported unset policy CMP0057 and rejected `IN_LIST` in the standalone `-P` process. Root configure succeeded because its policy version was already declared; that policy does not carry into a separate script process. Local CMake 4 runs had masked the omission.
- Correction in `550e0e9`: all three clean standalone script entry points now declare the same CMake 3.25 baseline as the root. The existing preset-contract test checks those declarations and was observed failing before the correction. No boundary enforcement was removed and no toolchain dependency was changed.
- VERIFIED after correction: local Windows and WSL Linux Debug builds each passed **42/42 CTests**, including the new policy-baseline assertion and build-time scan. CMake 3 execution is verified by the hosted rerun below, not inferred from these CMake 4 checks.

### Phase 1 hosted gate and closure

VERIFIED: [run 34260730535](https://github.com/Abhishekrazy/pcsx5/actions/runs/34260730535), attempt 1, completed successfully on 2026-09-08 for exact commit `550e0e99882ee8b2a653cb88e747e563d4fe52f9`. All four configure/build/test and diagnostic-upload steps succeeded. Job logs explicitly report 42 tests passing per job (168 CTest executions total).

| Hosted preset | Runner | Observed toolchain | Result |
|---|---|---|---|
| windows-x64-debug | windows-2025 | MSVC 19.51.36256.0, CMake 4.4.2, Ninja 1.13.2 | 42/42 passed |
| windows-x64-release | windows-2025 | MSVC 19.51.36256.0, CMake 4.4.2, Ninja 1.13.2 | 42/42 passed |
| linux-x64-debug | ubuntu-24.04 | GCC 13.3.0, CMake 3.31.6, Ninja 1.13.2 | 42/42 passed |
| linux-x64-release | ubuntu-24.04 | GCC 13.3.0, CMake 3.31.6, Ninja 1.13.2 | 42/42 passed |

Closure is limited to the clean build/boundary foundation and synthetic portable value tests. It is not emulator compatibility or platform-runtime support. Exact CMake 3.25 execution, automatic Codex worktree setup, ARM64/Apple/Android, full legacy application builds and runtime acceptance remain unverified. GitHub reported a non-blocking Node 20-to-24 runtime migration warning for the pinned MSVC setup action; the action succeeded, and updating its pin remains reviewed maintenance rather than a reason to weaken this gate.

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
- VERIFIED: the clean workflow is published on `codex/phase-1-build-foundation` and its corrected hosted matrix passed. This branch has not been merged into `main`; no package or release was published.
- UNKNOWN: automatic Codex worktree setup. New worktrees must include the Phase 1 commits. Hosted runner evidence is recorded separately from local WSL results above.

Phase 2 has started on `codex/phase-2-memory-runtime`. Additional dependencies,
merges, releases and semantic legacy fixes remain unapproved.

### Phase 2 memory milestone tasks

Remaining phase acceptance tasks (defined before implementation):

- [x] P2.4 Portable normalized fault records and Windows ownership-filtered
  observation. Real synthetic Windows faults must be observed without swallowing
  unrelated faults; unsupported causes must remain explicit. This is observation,
  not guest execution recovery (Phase 6).
- [x] P2.5 Owned host worker threads: start/join, completion publication, failure
  handling and deterministic lifecycle tests, without host TLS layout assumptions.
- [x] P2.6 Monotonic host timing: checked tick conversion, Windows counter source,
  synthetic arithmetic and real source tests; no guest timing accuracy claim.
- [x] P2.7 Integrated Windows/Linux Debug/Release checks and independent review;
  evidence-backed phase closure with remaining later-phase work explicit.

P2.4 VERIFIED locally: Windows Debug real owned read/write and unrelated read
faults reach the outer test-owned SEH handler; synthetic records cover malformed
input, execute, in-page errors, unsupported breakpoint and released/out-of-range
ownership. Portable normalization enumerates 25,600 inputs. Windows Debug passes
48/48 tests and Linux Debug 44/44. Release/integrated closure is tracked in P2.7.
The test first failed to link before adding the private observation adapter.
No production exception is consumed and no recovery/guest-address claim is made.

P2.5 VERIFIED locally: Windows Debug and Release each pass 50/50 tests after
worker integration. Tests cover successful/destructor/idempotent join, published
writes, throwing callbacks, two live workers synchronized by latches, self-join,
injected start failure and join failure/retry. Destructor failure subprocess
requires both injected-join and termination markers with exact exit status 73.
No sleeps, detach, forced cancellation or guest TLS assumptions were introduced.
Initial focused test failed to link before provider implementation, then passed.
Failure seams model pre-mutation errors; arbitrary native faults remain excluded.

P2.6 VERIFIED locally: exact tick conversion passes 67,086 synthetic checks,
including near-uint64 limits, reversed samples and overflow. Windows QPC/QPF
tests cover 4,096 sequential samples, stable frequency, and injected failed/null/
negative/zero native query outcomes. Windows Debug/Release each pass 52/52 tests;
all ten Windows Debug runtime tests additionally pass five repetitions each.
Independent arithmetic and worker-lifetime reviews found no blocking issue.
Main review replaced unsafe test-only expected accesses with guarded checks.
No clock accuracy, guest-time or cross-thread synchronization guarantee follows.

- [x] P2.1 Portable host geometry and reservation-relative page-range validation;
  exhaustive synthetic arithmetic tests and four local preset checks.
- [x] P2.2 Specify owned reservation lifetime, capability, state-transition and
  failure contracts; implement a Windows leaf with real reserve/commit/protect/
  decommit/release tests. Revalidate ranges against owned state on every operation.
- [x] P2.3 Exercise failure cleanup and ownership isolation, including requests
  outside owned reservations. Record hosted CI evidence before closing milestone.
  - [x] P2.3a Local injected native failure, cleanup and isolation tests.
  - [x] P2.3b Hosted CI evidence (user authorized the branch push).

VERIFIED on 2026-09-08: all four local Windows MSVC/Linux WSL GCC Debug/Release
presets build and pass 43/43 tests each. The new runtime test enumerates 443,784
small-domain inputs, with additional overflow and compile-time geometry checks.
Independent source review found no blocking issue. At P2.1, hosted Phase 2 CI
was UNKNOWN and the branch was not pushed; P2.3b below supersedes that limit.
No legacy source or dependency was changed.

### P2.2 owned Windows data memory

VERIFIED on 2026-09-09:

- The shared `memory.h` contract contains only portable types. A separate
  `pcsx5_runtime_windows` target implements anonymous data reservations using
  Windows APIs; core has no new include or link dependency.
- The lifecycle test first compiled but failed to link on the two missing
  provider functions, then passed with the real provider connected. No success
  stub or new testing dependency was introduced.
- Real native queries verify reserve/commit/protect/decommit state and normalized
  permissions. Synthetic tests cover zero initialization, cross-page copy,
  no partial copy on denied access, mixed-state rejection without mutation,
  invalid enums/ranges, overflow-sized reservation rejection, zero-on-recommit,
  independent owners, explicit repeated release and closed-object rejection.
- Windows MSVC Debug and Release each build and pass **44/44 CTests**; Linux WSL
  GCC Debug and Release each build the shared interface and pass **43/43 CTests**.
  Linux does not link or execute a memory provider. No compiler warnings observed.
- Windows Debug lifecycle test additionally passes ten consecutive repetitions.
  Independent read-only implementation/test review found no blocking issue.
- No legacy implementation or project dependency changed. Diff whitespace checks
  pass. This checkpoint is local only; no push, merge or release was performed.

Historical P2.2 limits (local native checks superseded by P2.3a below): injected native allocation/protection/release failures,
direct native observation of automatic cleanup, destructor failure subprocess
coverage and hosted CI. The current destructor release path is exercised but
its native cleanup effect is not independently observed. The API requires external
serialization and valid copy buffers; it does not catch host faults or isolate
untrusted execution. No emulator-supported-platform claim follows.

### P2.3a local failure cleanup and ownership evidence

VERIFIED on 2026-09-09:

- Private per-owner Win32 callback tables exercise the same reservation class as
  production. Public runtime headers and core dependencies are unchanged.
- Synthetic reserve failures cover all three mapped out-of-memory codes and a
  generic native error. Commit/protect/decommit failures preserve the observed
  state when the injected callback deliberately performs no native mutation.
- Failed/foreign-allocation queries prevent mutation and copying; out-of-range
  requests do not even reach native query. Simultaneously live owners with
  separate contexts retain independent failure and release behavior.
- Failed explicit release retains usable memory; destructor retries successfully.
  Successful automatic and explicit cleanup are independently observed as native
  `MEM_FREE`, with release counts proving no duplicate release after closure.
- A child-process test requires the injected-release marker, termination-handler
  marker and exact status 73. Normal return, unrelated crash or timeout fails.
  It proves the destructor invokes termination, not default CRT crash behavior.
- Windows Debug/Release each pass **46/46 CTests**; Linux Debug/Release each pass
  **43/43** portable tests. Both new Windows Debug tests pass ten repetitions each.
  No compiler warnings observed. Independent review prompted simultaneous-owner
  and release-route checks; both are included in final passing tests.
- No dependencies or legacy changes. These are injected-failure response tests,
  not OS exhaustion experiments. C++ owner-allocation `bad_alloc`, concurrency,
  partial native mutation and arbitrary host faults remain untested.

### P2.3b hosted closure: memory milestone COMPLETE

VERIFIED on 2026-09-09 (local date): the user authorized pushing
`codex/phase-2-memory-runtime`. [Hosted run 34266380223](https://github.com/Abhishekrazy/pcsx5/actions/runs/34266380223)
passed all four jobs at implementation commit
`8ce78a9cffd3c14fe9572055e209781d917d9ff3`:

- Windows x64 Debug and Release: **46/46 tests each**.
- Linux x64 Debug and Release: **43/43 tests each**.

No CI fix or rerun was needed. P2.1-P2.3 are complete within the documented
data-memory scope and limitations. No merge, release or platform-support claim
was made. Linux still validates portable contracts only, not a memory provider.

### P2.7 phase closure

VERIFIED on 2026-09-09: [hosted run 34268583215](https://github.com/Abhishekrazy/pcsx5/actions/runs/34268583215)
passed all four jobs at `43a29cdcea3fc6c8519a4653feeaa04987970483`.
Windows Debug/Release each pass **52/52** tests; Linux Debug/Release each pass
**45/45**. The same totals passed locally. Fault, worker and timing source reviews
found no blocking issue within their documented preconditions. New implementation
checkpoints are `b6098ce` (fault observation), `b5c2152` (worker lifetime) and
`43a29cd` (timing). No dependencies, legacy changes, merge or release were made.

Phase 2 acceptance is complete for owned data memory, ownership-filtered fault
observation, owned host workers and monotonic host timing. This is not a complete
emulator runtime: no executable memory, automatic demand commit, guest syscall/
breakpoint handling, guest scheduler/TLS, stack recovery, storage/audio/input or
frontend integration is claimed. Faults are observed and forwarded, never consumed.
Windows-only implementation coverage is separate from portable Linux contracts.
The known legacy failed invariants remain recorded, not reclassified as fixed.

Next: Phase 3 portable-core integration and Linux runtime, beginning with a Linux
owned-memory provider using the same contracts and reusable lifecycle corpus.
Later execution recovery must not copy the unsafe legacy stack/longjmp strategy.

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
