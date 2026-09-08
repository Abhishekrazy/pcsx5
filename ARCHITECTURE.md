# PCSX5 Clean Rebuild Architecture

## Layers

```mermaid
flowchart TB
  UI["Platform frontend"] --> Runtime["Host runtime"]
  Runtime --> Core["Host-independent emulator core"]
  Core --> Execution["Execution subsystem"]
  Execution --> Direct["Direct x86-64"]
  Execution --> Interpreter["Interpreter"]
  Execution --> Arm["x86-64 to ARM64 JIT"]
  Core --> Graphics["Graphics HAL"]
  Graphics --> Vulkan["Vulkan"]
  Graphics --> Apple["MoltenVK, then Metal only if needed"]
```

## Boundaries

- Core owns guest state, scheduling, devices, loader semantics, and emulation contracts.
- Execution owns guest register state and the enter/resume/exit model. Native signal or exception contexts are adapters only.
- Runtime owns memory pages, fault delivery, threads, storage, timing, audio, and input capabilities.
- Graphics HAL owns API-neutral resources and commands. Backend code alone may reference Vulkan or Metal types.
- Frontends own windows, lifecycle, permissions, and user interaction. They never own emulation state.

## Target policy

| Target | Status | Gate |
|---|---|---|
| Windows x64 | Primary | Runtime contracts + Vulkan acceptance corpus |
| Linux x64 | Primary | POSIX memory/fault/runtime acceptance corpus |
| ARM64 desktop/mobile | Planned | ARM64 JIT + W^X code-cache experiments |
| macOS | Planned | Runtime experiment + MoltenVK feature matrix |
| Android | Planned | ARM64 JIT and physical-device validation |
| iOS | Experimental only | Separate JIT/distribution feasibility decision |

## Non-negotiable constraints

- No Windows types in core-facing interfaces.
- No host TEB/TLS layout assumptions in the rebuild.
- No platform-specific `#ifdef` logic in core behavior.
- No title-specific behavior in generic architecture.
- No compatibility claim without reproducible evidence.

## Phase 0 characterization boundary

On 2026-09-08 the user approved behavior-preserving extraction of legacy trap/exit code for independent testing. `src/kernel/guest_execution.*` and `src/hle/guest_lifecycle.*` remain legacy Windows implementation, never clean-core dependencies. Call sites retain the original logic; extraction does not authorize fixes for the known ABI, TLS or exit-state defects. Real lifecycle/trap paths must be exercised, not replaced by success-returning test doubles. Each new translation unit must be included by legacy consumers alongside its original caller.

The original exception handler remains in `src/kernel/kernel.cpp`. `exception_entry.h` exposes that same handler and two optional diagnostic-data providers; null providers retain original behavior. Provider changes are allowed only while execution is stopped and the handler is uninstalled. The independent [extraction review](tests/characterization/extraction-review.json) records source-body equivalence, reset scope/order and its limits; it does not certify identical binaries or the full legacy application build.

The independent project in `tests/characterization/` compiles selected legacy sources for Windows x64 tests only. It is never a dependency of the new core. `tools/characterize-windows.cmd` configures, builds and runs that project with Ninja, using an isolated output tree. Passing these tests characterizes current implementation behavior, including explicitly named defects; it does not establish PS5 hardware accuracy or portability.

Test-only link dependencies that abort if invoked may isolate unused code from a monolithic translation unit. They must have a target-specific compile guard, match declarations in original headers, and never substitute for the behavior under test. Their results cannot establish runtime, syscall-trap, or lifecycle correctness. The dispatcher fixture's injected callbacks similarly establish assembly behavior only, not the real HLE implementation.

## Subsystem migration decisions

This table consolidates the earlier checkpoint inventory without restoring deleted planning files. Dispositions are decisions, not claims that code is reusable or correct. VERIFIED: the cited source areas exist in the legacy tree; the clean root build does not depend on them.

| Legacy boundary / evidence | Disposition | Rebuild destination and gate |
|---|---|---|
| Root build, `bootstrap-windows.cmd`, legacy package scripts | Redesign build; exclude legacy packaging | Per-target CMake, Windows/Linux CI; no network dependency fetch in bootstrap |
| `src/common`, `src/common/platform`, `src/config` | Reference algorithms; redesign host/config adapters | Portable values and injected runtime services; per-file tests and provenance before reuse |
| `src/memory`, `src/kernel/memory.*` | Characterize then redesign | Owned guest regions, explicit page capabilities, checked arithmetic, normalized failures |
| `src/cpu`, `src/hle/dispatcher.asm` | Characterize then contain; no blind assembly port | Owned guest state and enter/resume/exit contract; ABI and lifecycle gates |
| `src/kernel`, `src/hle` | Reference guest semantics; redesign host boundary | Guest-owned kernel/TLS/syscall state, no native context types in core |
| `src/loader`, `src/compat`, `src/system` | Reference only until characterized | Synthetic ELF/module inputs, explicit compatibility policy; no title-specific behavior in generic core |
| `src/gpu` (renderer and shader translation) | Behavioral reference, not automatically reusable | API-neutral graphics HAL and shader model; original synthetic replays before extraction |
| `src/gpu/input`, `src/hle/audio`, `src/media` | Redesign adapters; review codecs individually | Runtime input/audio capabilities, deterministic test sources, approved dependencies only |
| `src/core_api`, `src/ipc` | Characterize and redesign | Stable frontend-facing API after headless lifecycle works |
| `src/ui`, `src/ui_csharp` | UX reference; excluded from clean core | Frontend choice deferred until headless acceptance; no WPF dependency in core |
| `src/lua`, `src/diagnostics`, `src/reports` | Reference; tools are not emulation authority | Injected diagnostics, normalized records; scripting dependency deferred |
| `tests`, `replays`, `assets`, `tools` | Review individually, no blanket import | Only reviewed synthetic fixtures in characterization; unreviewed captures/binaries/assets excluded from acceptance |
| `third_party`, optional DLLs and package declarations | Retain notices; no new-core imports approved | Exact revision, modifications, license/provenance and user approval before reuse |
| Interpreter, IR and ARM64 JIT | New implementation, not present in scaffold | Interpreter oracle before differential JIT tests; per-host code-cache experiments |

Evidence details: [dependency review](tests/characterization/dependency-review.json) and [fixture provenance](tests/characterization/fixture-provenance.json). VERIFIED in a provenance record means the specified observation or newly authored fixture origin, not legal clearance or executed-test status. Historical source authorship and unreviewed assets remain UNKNOWN. This rebuild is not certified as a legal clean-room process.

## Runtime contract decisions from characterization

Design requirements for new implementations (not descriptions of existing correctness):

- Memory reservations carry ownership, bounds, host page size, allocation granularity and allowed protections. Commit-on-fault must check registered guest ownership; unowned host faults continue to the host handler.
- Fault records carry normalized guest location, access kind and cause. Guest invalid access, breakpoint, syscall trap, demand commit and host fault are distinct outcomes. Native `CONTEXT` or signal frames remain inside runtime adapters.
- Execution owns complete modeled guest registers and a typed result (`returned`, `requested_exit`, `guest_fault`, `unsupported`). Host ABI state, stack identity and cleanup must survive every exit path exactly once. The synchronous test-only stack escape is not an implementation of that contract.
- Guest TLS identity belongs to guest-thread state. Host offsets and patched code are adapter details; emitted sequences must preserve guest flags, registers and red-zone storage according to the selected execution contract.
- Register conversion has one authoritative representation per modeled value. Any duplicate native fields must be reconciled intentionally; unsupported extended state is explicit rather than silently assumed preserved.
- Direct x64 is an execution strategy, not a substitute for isolation. A separate x64 JIT remains a measured decision. ARM64 execution is a later x86-64 translation backend, not a compiler switch.
- VERIFIED legacy failures, not acceptable contracts: switched-stack HLE exit terminates with `0xc0000028`; trap-origin in-process `SysExit` varies between `0xc0000005` and `0xc0000028`; invalid guest access terminates with `0xc0000005` in the local synthetic fixtures. A tracked guest fault can be recorded before recovery fails. Host-stack exit controls succeed. The rebuild must establish a supported exit mechanism and full host-state restoration instead of copying this longjmp/TEB strategy. The causal chain of subsequent nested faults remains UNKNOWN.
- Cleanup has explicit ownership and concurrency semantics. Legacy hook tests establish same-thread repetition/reentry and standalone `SysExit` only; concurrent cleanup and active-heartbeat teardown remain unverified. These must be tested before accepting a new execution backend.

## Trace and acceptance-corpus contract

The versioned [trace schema](tests/characterization/trace-v1.schema.json) uses contiguous sequence numbers and normalized scalar/region-relative values. Producers must remove native addresses, wall-clock times and host thread IDs. The dependency-free validator rejects invalid structure/order; it cannot recognize a raw pointer disguised as a scalar.

VERIFIED: dispatcher, selected real kernel routes and isolated/combined lifecycle producers emit **post-assertion semantic summaries** of synthetic executions. Two independent runs must validate and produce identical bytes. Fatal paths encode host restoration as unknown, never as a successful zero-filled comparison. Variable nested-diagnostic counts and in-process SysExit raw statuses remain in diagnostic output; the summary records only its checked failure to return, marking the raw status unknown. Repeatability therefore applies to this abstraction, not runtime determinism. These are not instruction traces, full architectural-state captures, a replay engine or an interpreter oracle. Those remain separate work; do not promote these summaries to accuracy references.

The minimal acceptance corpus is newly authored synthetic allocations, byte patterns, register values and test-only code. Fixtures have reviewed input origin and normalized-source hashes. Existing memory/TLS tests are usable as local legacy regression evidence, but their historical authorship is UNKNOWN. Unreviewed GPU goldens/replays, retail captures and other assets are not accepted inputs. Nothing here approves their redistribution.

## Delivery sequence and Phase 1 handoff

### Phase 6 containment, first increment

P6.2 transport contract: `execution/include/pcsx5/execution/worker_protocol.h`
defines PXQ1/PXR1 little-endian messages. The first worker accepts one 4096-byte
guest RAM image at 0x1000, scalar CPU state, a nonzero caller-unique request ID and
1..256 instruction budget. It returns final RAM/state and bounded PXI1 traces.
Exact sizes, enums, counts, state-chain continuity, fetched bytes and memory-write
effects are checked; stale IDs and malformed/truncated results are rejected.
Structural validation is not authentication or a proof of native ISA correctness.
The initial production helper runs the interpreter; it is not a direct-x64 backend.
Files are an explicit trusted batch transport: caller-owned input/output paths,
no guessed paths, bounded reads and no response accepted after failed child exit.
Tests keep these files in unique build-output directories. Private authenticated
IPC and hostile-worker protection remain separate work; no sandbox claim follows.

Decision: establish owned child-process lifetime before executing guest bytes
natively. `runtime/include/pcsx5/runtime/child_process.h` is the shared contract;
runtime owns Windows/Linux leaves, synthetic helper tests are the first consumer.
The parent does not borrow a guest stack, patch TEB/TLS or recover through longjmp.
One trusted helper is launched by absolute path without a shell. Exits, signal
termination (Linux), deadline termination and cancellation are distinct outcomes;
none imply a valid guest-state snapshot. All paths reap the owned child and close
native handles before return. Cleanup failure is fail-fast rather than silently
abandoning a child, consistent with existing owned runtime resources.

This is process-lifetime containment, NOT a hostile-code sandbox: environment and
working directory are inherited, Linux non-CLOEXEC descriptors may be inherited,
and descendants/filesystem/network access are not confined. Windows starts hidden
without handle inheritance. Callers must not install incompatible child-reaping
policies; no process-global signal disposition is changed by production code.
Timeout starts after native spawn and includes neither a hard scheduling guarantee
nor a bound on OS termination/reaping. The helper must not launch descendants.

First gate: real authored helpers exit, fail, stall and accept exact literal
arguments; parent observes expected outcomes and proves cleanup/repeated use.
Later gates remain guest-state transport with validation, contained native x64
entry/exit/fault snapshots, full modeled-state differential tests and measured
optional x64-JIT decision. Passing this first gate does not complete Phase 6 or
enable direct execution of guest bytes. No legacy code or dependency is imported.
API references: [CreateProcessW](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessw),
[CRT argument parsing](https://learn.microsoft.com/en-us/cpp/c-language/parsing-c-command-line-arguments),
and [posix_spawn](https://man7.org/linux/man-pages/man3/posix_spawn.3.html).

### Phase 5 scalar interpreter contract

Decision: the first reference oracle is a bounded scalar x86-64 long-mode subset,
not a complete ISA or PS5 CPU. `execution/include/pcsx5/execution/interpreter.h`
is the canonical shared type contract. Execution owns the provider; synthetic
conformance/replay tests are the first consumer, with later backends required to
match supported defined state. Contract expansion requires corresponding tests.

Model: 16 64-bit GPRs in architectural encoding order, RIP, RFLAGS and a mask of
known arithmetic flags (CF/PF/AF/ZF/SF/OF). Other state is not modeled. Scalar
32-bit destinations zero-extend; arithmetic wraps at operand width. Logic makes
AF unknown instead of inventing a hardware value. Conditional branches requiring
unknown flags stop. Arithmetic semantics and encodings use the public
[Intel instruction reference](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html),
not the legacy native-context implementation or retail binaries.

Supported encodings: optional single REX; B8-BF MOV immediate; 89/8B MOV with
32/64-bit register or memory operands; register-only 01/03 ADD, 29/2B SUB, 39/3B
CMP, 31/33 XOR, 21/23 AND, 09/0B OR and 85 TEST; register groups 81/83 for those
arithmetic operations; 90 NOP without REX; 50-5F 64-bit PUSH/POP; E8 CALL, C3 RET,
E9/EB JMP, 70-7F and 0F80-8F Jcc. Memory MOV supports 64-bit ModRM/SIB addressing,
signed disp8/disp32 and RIP-relative addressing. No legacy prefixes, byte/word
operands, memory arithmetic, x87/SIMD, atomics, segment overrides or privileged
execution. Other encodings stop as unsupported. CC and 0F05 produce pre-execution
breakpoint/syscall stops, not hardware trap or syscall emulation.

Single-step fetches only needed bytes from guest_memory. Fetch permission is its
existing read policy, not NX emulation. The initial virtual-address policy is
48-bit canonical; no PS5-specific paging or exception-vector accuracy is claimed.
Data accesses must fit one existing mapping. Each step stages register changes;
failed decode/read/write leaves registers unchanged. Native backing write failure
may have partially changed memory and is explicitly marked uncertain; it is not
a rollback guarantee. No native code execution, host pointer exposure or new
dependency is required. Calls are externally serialized.

Run uses a caller-provided instruction budget and trace span; capacity is checked
before executing, never silently dropping records. Versioned fixed-endian trace
records capture instruction bytes, complete modeled before/after state, result,
fault location and the single possible memory write. Traces contain guest values,
never host identities, clocks or pointers. Fresh replay compares every record
against independently asserted synthetic expectations, not just a success summary.
Windows/Linux Debug/Release, malformed input/fault/state-preservation tests,
deterministic traces and sanitizer checks are Phase 5 closure gates. Full ISA,
guest OS behavior, hardware fidelity and ARM64 support remain separate gates.

### Phase 4 offscreen graphics contract

Decision: `graphics/include/pcsx5/graphics/renderer.h` is the canonical typed HAL
for the first headless graphics consumer, the newly authored synthetic replay
corpus. Graphics owns API-neutral extent/color/triangle commands and returned
pixel storage. The Vulkan leaf alone owns native resources and SDK types; core
does not acquire a graphics link dependency. All consumers share the C++ build.
The project owner approves broader capability or dependency changes.

The synchronous renderer accepts a bounded complete frame: clear plus ordered
solid-color triangles. Pixel origin is upper-left; rasterization has no blend,
depth, culling, multisampling or sRGB. Readback is tightly packed RGBA8 UNORM.
Invalid dimensions/vertices/degenerate triangles reject before device work; failure
does not return a successful partial image. Inputs are borrowed for the call only.
The renderer owns native lifetime, requires external serialization and closes
idempotently. Capabilities describe this implementation, not PS5 GPU features.
Validation-required creation must fail if its layer/debug support is unavailable.
It also requires and explicitly enables synchronization validation. Pixel tests
check asymmetric orientation, overwrite ordering and shared-edge single coverage;
they do not prescribe which neighbor owns an exact edge sample. This follows
[Vulkan polygon coverage](https://docs.vulkan.org/spec/latest/chapters/primsrast.html#primsrast-polygons).
Readback uses a transfer-to-host barrier, fence wait and whole-allocation invalidate
as specified by [Khronos](https://docs.vulkan.org/refpages/latest/refpages/source/vkInvalidateMappedMemoryRanges.html).
The private failure seam injects errors before allocation/submission after earlier
resources exist; it does not simulate completed GPU work or prove recovery from
actual hardware loss. A private post-destruction observer verifies zero validation
errors through native device teardown. No callback enters the public HAL.

Phase 4 acceptance includes actual Vulkan draw/readback (not only clear/copy),
independent synthetic pixel expectations, replay on fresh and reused renderers,
invalid inputs, unavailable devices, cleanup and validation-layer error checks,
plus Windows/Linux Debug/Release gates. Cross-vendor floating-point raster edges
are not a bit-identical guest GPU oracle. No legacy shaders or assets are imported.
Guest shader translation, guest command decoding, presentation and full PS5 graphics
semantics require separate contracts; this phase does not claim those capabilities.

Dependency approval: on 2026-09-09 the user explicitly approved using installed
Windows Vulkan SDK 1.4.357.0 and installing Linux Vulkan development/validation and
GLSL/SPIR-V tools with required supporting packages. SDK/loader links remain in
the graphics leaf; no source vendoring, package redistribution or other dependency
is approved by this decision. Bootstrap must not silently download tools.

| Phase | Deliverable / exit gate |
|---|---|
| 0 (characterization complete) | Inventory, provenance dispositions, bounded memory/TLS/register/syscall/trap/exit matrix and evidence review; known failed invariants retained, not accepted |
| 1 | Build and boundary foundation: Windows x64 MSVC and Linux x64 GCC Debug/Release CI, headless CTest, zero-test failure, no legacy linking or release packaging |
| 2 (complete) | Owned data memory, fault observation/forwarding, host workers and monotonic timing; Windows leaf implementations and UI-free contract tests. Closure evidence and exclusions in REBUILD_STATUS.md |
| 3 (complete) | Portable core and Linux runtime; equivalent headless tests on both hosts. Requirement-by-requirement evidence and limits in REBUILD_STATUS.md |
| 4 (complete) | Graphics HAL and Vulkan; isolated synthetic rendering/replay tests. Offscreen acceptance evidence and platform limits in REBUILD_STATUS.md |
| 5 (complete within scalar scope) | Reference interpreter and deterministic state/trace oracle; supported encodings above, acceptance evidence and exclusions in REBUILD_STATUS.md |
| 6 | Direct-x64 containment, complete exit/fault state tests; measured optional x64 JIT decision |
| 7 | ARM64 JIT; interpreter differential tests and W^X/cache experiments |
| 8 | macOS/Apple Silicon runtime plus MoltenVK acceptance |
| 9 | Android ARM64 runtime and physical-device acceptance |
| 10 | Native Metal go/no-go only after measured MoltenVK gaps |
| 11 | Product frontends, packaging and release compatibility gates |

Phase 1 task order (complete; hosted closure evidence and limits in `REBUILD_STATUS.md`):

1. Replace the stale Windows-only legacy CI workflow with clean Windows/Linux Debug/Release jobs. Retain only approved actions, read-only permissions and test logs; no package/release job.
2. Make configure/build/test entry points work from a new worktree without local caches or network restores; wire the Codex setup script through its environment editor.
3. Enforce forbidden OS/UI/graphics includes and legacy link dependencies at the core boundary; test both positive and negative cases.
4. Add useful portable core tests beyond the current architecture-name smoke test. Require no-tests-as-error and reproducible CTest output.
5. Record real CI results before calling the build matrix verified. Runtime/platform support still requires the later acceptance gates.

### Phase 2 memory boundary (first increment)

Decision: host page geometry and reservation-relative page ranges live in the
runtime public headers, not in core. `memory_geometry` accepts nonzero page size
and a nonzero reservation alignment divisible by that size. No power-of-two or
4 KiB assumption is required. A provider must obtain actual host geometry; these
values do not establish guest page size or host support.

`page_range` validates a nonempty, page-aligned offset and count inside a nonempty,
page-aligned reservation byte count. Invalid input returns an empty optional;
validation neither rounds nor allocates. Subtraction-based containment avoids
overflow. This value is NOT an ownership token: a future provider must revalidate
each request against its own live reservation and geometry, then check native
integer representability and current page state. Passing arithmetic checks does
not authorize access to arbitrary host memory. Core has no runtime dependency.

The owned reservation contract is `runtime/include/pcsx5/runtime/memory.h`.
Runtime owns the provider; current consumers are synthetic lifecycle tests,
with future execution adapters requiring a separately reviewed extension.
All consumers share the C++ build; breaking changes require updating this header,
provider and tests together. The project owner approves architectural expansion.
Reservations are uniquely owned and accept offsets, never host addresses.
Data-only permissions are none/read-only/read-write, with no executable capability.
Commit requires reserved pages; protect/decommit require committed pages. Full
validation precedes mutation or copy. Windows queries inspect real host state;
Linux's explicit state model is specified below. Explicit release is idempotent and retains ownership
on failure; destruction attempts release and terminates on failure rather than
silently losing a live mapping. Callers must externally serialize operations and
provide valid copy buffers. This is neither a fault handler nor process isolation.
Windows allocation/protection APIs stay in the leaf implementation. Native failure
does not promise rollback: inspect actual state before retry. No native pointer
escape, fixed-address mapping, executable permissions or concurrency is provided.
References: [VirtualAlloc](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc),
[VirtualFree](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualfree),
[VirtualProtect](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualprotect).
Legacy reserve/commit tests inform lifecycle coverage, not the new ownership API.

P2.3 verification seam: `runtime/src/windows_memory_api.h` is private to the
Windows leaf and its tests. Each owner copies its callback table; the test
context outlives the owner. There is no process-global override or public pointer
escape. Production supplies real Win32 calls through the same implementation.
Tests forward successful operations to Win32, selectively inject native failures,
and inspect captured test-owned addresses without publishing them. Injected
failure evidence proves adapter response, not reproduction of actual OS resource
exhaustion. A subprocess must distinguish the specified destructor termination
path from arbitrary crashes. Public ownership semantics remain unchanged.

### Phase 2 fault observation contract

`fault.h` carries region-relative offset, read/write/execute access and normalized
memory-fault cause. Only a live Windows reservation establishes ownership; native
address conversion stays in its private adapter. Closed/unrelated reservations
are unowned. Unknown exception kinds and malformed native records are unsupported.
The private SEH filter records the observation and always continues host search;
it never commits, resumes, installs process-global handlers, changes native context
or invokes guest code. Callers keep the reservation alive and exclude mutation
during observation. The current consumer is the synthetic Windows test; later
execution adapters must add their own reviewed lifetime and recovery contract.

Breakpoint/syscall recognition, guest instruction location, automatic demand
commit and guest recovery are NOT inferred from a host address or exception code.
They remain execution/guest policy work. Real read/write faults are caught only
by an outer test-owned SEH handler, verifying that the production filter leaves
both owned and unrelated faults unhandled. Synthetic records cover execute and
backing-store errors without running generated code or inducing storage failures.
References: [Windows exception records](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-exception_record)
and [SEH filter dispositions](https://learn.microsoft.com/en-us/cpp/cpp/try-except-statement).

### Phase 2 worker and timing contracts

`worker.h` defines a uniquely owned host worker. Start rejects null callbacks;
join publishes callback completion, distinguishes returned/throwing callbacks,
rejects self-join, and is idempotent. Destruction joins or terminates on failure;
no detach, cancellation, guest TLS or native thread IDs are exposed. Operations
and destruction are externally serialized; callback context outlives completion.
The Windows leaf uses the existing C++ standard library thread facility. Private
per-owner launch/join seams provide deterministic failure tests, not a mock-only
production path. Throwing C++ callbacks are normalized; native faults are not.

`timing.h` defines ticks, checked nonzero frequency and elapsed nanoseconds.
Conversion is exact integer floor with reversed intervals and overflow rejected;
full uint64 frequencies are supported without overflowing intermediate products.
The Windows leaf reads QueryPerformanceCounter/Frequency and rejects failure or
invalid signed values. Stamps must come from the same source/boot; these are host
measurements, not wall time or guest timing accuracy. Synthetic conversion tests
are independent of real clock tests, which require nondecreasing observations
without minimum elapsed-time or resolution assumptions. No native types are public.
References: [C++ thread construction](https://eel.is/c++draft/thread.thread.constr),
[join synchronization](https://eel.is/c++draft/thread.thread.member),
[Windows high-resolution timestamps](https://learn.microsoft.com/en-us/windows/win32/sysinfo/acquiring-high-resolution-time-stamps).

### Phase 3 portable integration contracts

Decision (implemented; acceptance evidence in REBUILD_STATUS.md): core owns a guest-memory mapping table,
using checked guest byte ranges and unique `memory_backing` objects. The backing
contract lives in core and exposes size, offset-based read/write and explicit
release with normalized errors, never host pointers or page geometry. Runtime
implements this interface outside core; dependency direction is runtime to core.
Mapping consumes the caller's backing only on success. Overlap, size mismatch,
invalid permissions and allocation failure leave ownership with the caller.
An access must fit entirely in one mapping; crossing even adjacent mappings is
rejected before copying. Protection changes apply to a complete mapping, not a
host page. Unmap retains the entry on backing release failure so callers can retry.
All operations and teardown are externally serialized. Backing destructors own
their cleanup policy; the runtime adapter preserves release-or-terminate behavior.

Guest permissions are software policy, NOT host protection or a security boundary.
The runtime backing factory allocates and commits data-only read/write host pages;
core enforces guest none/read-only/read-write independently. No execute permission,
guest page size, PS5 address validity, guest ABI or instruction execution is implied.
Factory/provider failures are normalized into core errors; native details remain
private. Synthetic and actual-provider tests must exercise the same guest path.

Linux decisions: anonymous mappings start PROT_NONE; provider-owned metadata
defines the reserved/committed distinction that Linux does not expose as a Win32
commit state. Queries report this contract state, not physical residency. A failed
native protection change conservatively makes affected state uncertain; subsequent
state-dependent operations fail until release. If protection to none succeeds but
discard fails, pages remain committed/none and decommit can be retried. No rollback
or memory-pressure guarantee is claimed. Linux workers preserve the Windows worker
contract through standard-library threads. CLOCK_MONOTONIC stamps use a checked
nanosecond count and frequency 1e9; invalid timestamps and overflow are errors.
Linux fault observation remains owner-scoped and unhandled, with no installed
production signal handler, context mutation, demand paging or guest recovery.
The native region snapshot is captured before handler installation. Its owner
must remain alive/unreleased, with mapping changes excluded until handler removal;
no stale-snapshot or concurrent-unregister protocol is promised. Signal-time
observations use private scalar records rather than standard-library containers.
Linux rejects READ_IMPLIES_EXEC at reserve time; external changes to mappings or
process personality while owners exist are unsupported. CLOCK_MONOTONIC excludes
suspend and may be frequency-adjusted: 1e9 tick units do not imply nanosecond
resolution or identical Windows/Linux suspend semantics.
References: [Linux mprotect](https://man7.org/linux/man-pages/man2/mprotect.2.html),
[discard semantics](https://man7.org/linux/man-pages/man2/madvise.2.html),
[monotonic clock](https://man7.org/linux/man-pages/man2/clock_gettime.2.html),
[signal context](https://man7.org/linux/man-pages/man2/sigaction.2.html),
[C++ signal restrictions](https://eel.is/c++draft/support.signal).

### Phase 1 build/CI contract

- Four Ninja presets isolate Windows x64 MSVC and Linux x64 GCC Debug/Release outputs. Configuration is selected at configure time, not by passing multiple configurations to a single-config build.
- Each test preset fails if no tests are selected, applies a bounded default timeout, and writes CTest/JUnit logs. CI builds only the clean root; no legacy characterization, submodule restore, package creation or release publishing is part of this workflow.
- Every clean standalone CMake `-P` entry point declares the root's 3.25 minimum/policy baseline explicitly; a subprocess does not inherit the root configure policy scope. The preset-contract test checks this to prevent local CMake 4 from masking older supported CMake behavior.
- CI uses read-only repository permissions, does not persist checkout credentials, and retains only the already-used checkout, MSVC initialization and log-upload actions, pinned to verified upstream commit IDs. Runner images are version-labeled but their installed tool versions may change; logs identify the actual tools used.
- Workflow and preset validation is local evidence only. Hosted matrix success requires an actual GitHub Actions run; local GCC compilation without Linux CMake/Ninja is not Linux preset verification.

References checked for this boundary: [CMake 3.25 preset format](https://cmake.org/cmake/help/v3.25/manual/cmake-presets.7.html), [GitHub workflow syntax](https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax), and [hosted runner images](https://github.com/actions/runner-images).

### Phase 1 portable value contract

Design decision: the first useful clean-core value is `guest_address_range`, a nonempty byte range represented by a `uint64_t` guest base and byte count, never a host pointer or `size_t`. Its checked factory rejects zero size and any range whose last byte exceeds `UINT64_MAX`; the single final byte at `UINT64_MAX` is representable. A range spanning all 2^64 bytes is not representable by its count and is intentionally outside this value's domain. Queries provide address/range containment, overlap and an optional offset; adjacent ranges do not overlap. There is no unchecked public constructor or mutation.

These are arithmetic invariants, not PS5 virtual-address validity, mapping ownership, memory permissions, allocation policy or page-size assumptions. Runtime adapters must supply those later. Implementation uses subtraction-based bounds checks to avoid wrapped endpoints. Compile-time edge checks and deterministic runtime enumeration must stay active in Release builds; no proprietary inputs or new test framework are needed.

### Phase 1 core dependency guard

Design decision: the clean core currently has no approved link dependencies. A deferred CMake guard inspects the real core target after subdirectory configuration; additional libraries, interface dependencies, external source/include roots, generated or opaque sources, PCH, and unreviewed compile injection routes must fail configuration. Only existing core-owned files, the core include root and the reviewed warning options are accepted. The approved standard-library headers are explicitly listed in the guard; additions require review and regression coverage. New runtime/execution contracts must extend this policy deliberately, not disable it.

The whole core source/header tree is scanned at configure time and before core builds. Literal local headers must resolve inside core; external OS/UI/graphics/legacy headers and macro includes are rejected. Conservative preprocessing restrictions reject unhandled spellings instead of silently assuming they are portable. Tests must distinguish a guard's diagnostic from an unrelated compiler/configuration failure and include a passing control.

This is an architectural regression guard, not a C++ parser or security sandbox. It trusts the compiler, standard-library installation and build environment; it cannot establish semantic host independence, detect copied legacy algorithms or certify every possible preprocessor/toolchain trick. Platform behavior still requires source review and cross-host acceptance tests.
