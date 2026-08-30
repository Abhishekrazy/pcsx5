# PCSX5 Evidence & Repository Artifact Governance Policy

## 1. Purpose & Scope

This policy establishes mandatory repository hygiene standards, artifact classifications, evidence retention rules, and documentation conventions for the PCSX5 engineering program.

The emulator adheres to the principle of **Evidence Before Implementation**. However, raw logs must never compromise repository navigability or displace canonical documentation.

---

## 2. The 10 Core Governance Pillars

### 1. What Belongs in `docs/tasks/`
- Forward-looking task specifications, pre-implementation technical designs, scope boundaries, and acceptance criteria.
- Forward progress reports detailing active investigation objectives, expected contracts, and next-task declarations.
- Every task document must declare its prerequisites, boundary scope, and success criteria.

### 2. What Belongs in `docs/audits/`
- Formal forensic investigations, failure root-cause analyses, reverse-engineering findings, and architectural certifications.
- Every audit must reconcile **Claims vs. Reality** against the Truth Model, cite specific preserved evidence files, document observed guest register states/disassembly, and record falsified assumptions.

### 3. What Belongs in `docs/walkthroughs/`
- End-to-end verification walkthroughs, milestone summaries, user-facing change logs, test execution records, and developer run steps.
- Walkthroughs summarize *how* an implementation was verified and explain the operational impact of changes.

### 4. What Belongs in `docs/evidence/`
- Curated, immutable diagnostic captures (e.g. specific trap traces, golden execution register states, packet captures, minimal reproducible excerpts) that are **explicitly cited** by an audit or task document.
- Raw evidence must be organized chronologically by year-month and topic: `docs/evidence/YYYY-MM/<topic-slug>/<filename>.<ext>`.
- Never commit massive multimegabyte raw dumps; minimize and isolate to the critical diagnostic excerpt.

### 5. What is Temporary Runtime Output
- Ephemeral logs produced during local boot runs (`boot_log*.txt`, `boot_log*.log`, `run_log*.txt`, `guest_trace.log`, `diag_output.txt`, `ppsa*_run*.txt`).
- Ephemeral test audio/video/shader captures (`snd0_decoded.wav`, `baddream_decoded.wav`, `Testing/`, `test_ps5_out/`).
- Runtime shader disk caches (`Cache/`), crash dumps (`pcsx5_crash/`, `*.dmp`, `*.dump`), and intermediate trace files (`*.trace`, `*.tmp`).

### 6. What Can Safely Be Deleted
- Ad-hoc local execution logs not cited in any audit or task document.
- Obsolete patch files (`*.patch`), intermediate memory dumps (`*.bin`, `*.dmp`), and duplicate parser dumps.
- Generated build outputs, compiler intermediates, and .NET build outputs (`build/`, `out/`, `dist/`, `src/ui_csharp/bin/`, `src/ui_csharp/obj/`, `installer/Output/`).
- Transient CTest artifacts in `Testing/Temporary/`.

### 7. What Must Never Be Deleted
- Authoritative source code (`src/**`), unit/integration test suites (`tests/**`), and build scripts (`CMakeLists.txt`, `build_release.ps1`, `build_and_package.ps1`).
- Canonical documentation (`docs/**`, `architecture/**`, `guide/**`, `wiki/**`, `README.md`, `ROADMAP.md`).
- Required test fixtures (`tests/golden/**`, `tests/test_elf/**`, `assets/**`).
- Curated diagnostic evidence in `docs/evidence/**`.
- Developer and reverse-engineering tools (`tools/**`).
- Any file classified as **UNKNOWN** where origin or purpose cannot be definitively determined.

### 8. Naming Conventions
- **Tasks**: `docs/tasks/TASK-YYYY-MM-DD-<slug>.md`
- **Audits**: `docs/audits/AUDIT-YYYY-MM-DD-<slug>.md`
- **Walkthroughs**: `docs/walkthroughs/WALKTHROUGH-YYYY-MM-DD-<slug>.md`
- **Evidence Files**: `docs/evidence/YYYY-MM/<task-or-feature-slug>/<descriptor>.<ext>`
- **Tools**: `tools/<tool_name>.py` or `tools/<tool_name>.cpp`
- **Tests**: `tests/<subsystem>_tests.cpp`

### 9. Retention Policy
- **Authoritative Docs & Tests**: Retained permanently in git history.
- **Curated Evidence**: Retained indefinitely in `docs/evidence/` as long as the referencing audit remains part of active or historical architectural record.
- **Temporary Output**: Retained only during the active debugging session; removed or ignored prior to task completion.
- **Git Hygiene**: No temporary `.txt`, `.log`, `.tmp`, `.dmp`, or test audio dumps may be committed to repository root.

### 10. How Future Tasks Should Preserve Diagnostic Evidence
When an investigation generates diagnostic output that proves a critical architectural fact:
1. Extract the minimal diagnostic excerpt required for proof.
2. Store the file in `docs/evidence/YYYY-MM/<topic-slug>/<filename>.<ext>`.
3. In the audit document (`docs/audits/AUDIT-YYYY-MM-DD-<topic>.md`), cite the relative path `docs/evidence/...` and document:
   - File name & path
   - Purpose & context of the run
   - Relevant guest RIP, thread ID, and function/NID
   - Concrete conclusion proven by the excerpt
4. Delete or ignore all unreferenced temporary logs.
5. Verify `.gitignore` prevents recurrence of loose files.

---

## 3. Truth Model & Claims Hierarchy

All material claims in audits, task reports, and commit messages must use standardized truth classifications:

| Classification | Meaning | Evidence Required |
| :--- | :--- | :--- |
| `SPECIFIED` | Contract is formally documented | PS5 ABI / Architecture specification |
| `IMPLEMENTED` | Code is written and merged | Source reference in core or HLE |
| `VERIFIED` | Unit or subsystem behavior tested | Automated CTest or unit test output |
| `OPERATIONALLY CONFIRMED` | Working in live title environment | Real frame rendering / game execution log |
| `OBSERVED` | Runtime phenomenon recorded | Live log, crash trace, or hardware capture |
| `INFERRED` | Deduction from observed patterns | Technical reasoning backed by evidence |
| `HYPOTHESIS` | Plausible architectural theory | Requires verification before implementation |
| `UNKNOWN` | Unconfirmed or undocumented behavior | Must remain UNKNOWN until evidenced |
| `FALSIFIED` | Disproven hypothesis or broken model | Counterexample or regression test |

> [!IMPORTANT]
> **Raw logs are evidence, not conclusions.** A task is complete only when the code, tests, documentation, and claims are fully reconciled.

---

## 4. Artifact Classification Matrix

Every file in the repository belongs to exactly one category:

| Category | Description | Policy |
| :--- | :--- | :--- |
| **A. Authoritative Source** | Code (`src/**`), tests (`tests/**`), core documentation (`docs/**`, `*.md`) | Permanent. Never delete. |
| **B. Required Test Fixture** | Test data, input dumps, audio/video test streams (`tests/**`, `assets/**`) | Permanent. Referenced by CMake/CTest. |
| **C. Required Reproduction Artifact** | Scripts (`tools/**`), configurations, platform helpers | Permanent. Maintained in `tools/`. |
| **D. Historical Evidence** | Curated logs/traces explicitly cited by audit docs | Preserved in `docs/evidence/`. |
| **E. Temporary Debug Output** | Ad-hoc run logs (`boot_log*.txt`, `out.txt`, `log_*.txt`) | **Disposable.** Cleaned periodically. |
| **F. Generated Build Output** | Binaries, object files (`*.obj`, `*.dll`, `build/`, `out/`) | **Disposable.** Kept in `.gitignore`. |
| **G. Obsolete Scratch Scripts** | One-off exploratory scripts (`patch*.py`, `test_*.py`) | Cleaned or promoted into `tools/`. |
| **H. Unknown** | Unidentified origin or purpose | **DO NOT DELETE.** Requires investigation. |
