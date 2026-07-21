# Contributing to PCSX5

> [!IMPORTANT]
> The pull request template is mandatory.
>
> Pull requests that do not follow the template or leave the required
> checklist incomplete will be closed without review, even if the
> proposed code is technically correct or beneficial.  Please read
> these contribution guidelines before submitting a pull request.

Contributions are always welcome!

Before opening a pull request, please keep the following in mind:

- Keep PRs small and focused on a single topic.
- Discuss large architectural changes before implementing them
  (open an issue or a draft PR first).
- Follow the project's existing coding style (see
  [BUILDING.md §7](BUILDING.md)).
- Do not submit generated code unless you fully understand and can
  maintain it.
- Do not submit Sony proprietary code, firmware, keys, decrypted
  assets, or other copyrighted PlayStation materials.
- All reverse engineering must be based on publicly available
  information, clean-room techniques, or your own original research.
- Avoid game-specific hacks whenever possible.  Prefer generic
  implementations that improve overall compatibility.
- New features must not break existing behavior.
- Ensure the project **builds and the full CTest suite passes** before
  submitting a PR (see [BUILDING.md §4](BUILDING.md)).

If you're unsure about a design decision, open a discussion or a draft
PR first.

## Legal / Licensing

PCSX5 is licensed under the **GNU General Public License v2.0**
(GPL-2.0-only).  By submitting a pull request you agree to license
your contribution under the same terms.

- New source files should carry a short GPL-2.0 copyright notice
  pointing at the [`LICENSE`](LICENSE) file.
- Do not copy code from projects with incompatible licenses
  (e.g. GPL-3.0-only, Apache-2.0 with patent clauses you cannot
  satisfy, or proprietary sources).

## AI-Assisted Contributions

AI-assisted development is welcome and may be used for research,
reverse engineering, code generation, or documentation.

However, contributors are expected to fully understand every line of
code they submit.  By opening a pull request, you confirm that you are
able to explain, modify, debug, and maintain the submitted code
without relying on the AI that generated it.

When submitting an AI-assisted PR:

- Clearly explain **what the change does**, **why it is needed**, and
  **what problem it solves**, in your own words.
- Describe **how you verified the change**, including the games,
  applications, or test cases used.
- Use logging only when it provides meaningful diagnostic value —
  follow the existing `LOG_INFO(category, ...)` conventions instead of
  adding ad-hoc `printf`/`OutputDebugString` calls.
- Comments should document design decisions or non-obvious
  implementation details in your own words.  Avoid generic
  AI-generated comments that merely restate what the code already
  does.
- Be prepared to answer review questions about the implementation.
  "The AI generated it" is not a sufficient explanation.
- Large AI-generated changes without a clear understanding of the
  implementation are unlikely to be accepted.

The quality, correctness, maintainability, and long-term ownership of
the submitted code remain the responsibility of the contributor.

## Coding Style

PCSX5 is C++20 built with MSVC.  The canonical reference is
[BUILDING.md §7](BUILDING.md); the short version:

- Every subsystem owns its namespace (`HLE`, `Loader`, `Memory`,
  `Kernel`, `GPU`, `ConfigService`, `Reports`, `Diagnostics`).  Do not
  leak internal types into the global scope.
- All C++ files compile with `/W4 /WX` — warnings are errors.  The
  MASM dispatcher is the only exception.
- Include `<windows.h>` only in `.cpp` files, always with
  `#define NOMINMAX` first.
- Every shared structure owns a `std::mutex`; use `mutable` mutexes
  for logically read-only structures.
- Keep naming, formatting, and file organization consistent with the
  surrounding code.  Avoid formatting-only commits unless that is the
  purpose of the PR.
- Prefer small, focused changes over large refactors.

## Testing

Every new component ships with a CTest target — see
[BUILDING.md §9](BUILDING.md) for how to register one.

- New HLE symbols need a positive test in
  `tests/hle_import_report.cpp` (see [BUILDING.md §8](BUILDING.md)).
- Test scratch directories must live under
  `std::filesystem::temp_directory_path() / "pcsx5_<test>_test"` —
  CTest's working directory is the source root, so relative paths
  leak state across runs.
- When relevant, include a `--report=<path>` compatibility summary or
  an updated entry in `compat/compatibility.json` with your PR.

## Recommended Development Environment

- Windows 10/11 (x64), Visual Studio 2022 17.8+ (C++ workload) with
  the Windows SDK — MSVC is required; the HLE ABI bridge is MASM-only.
- CMake 3.20+ and Ninja.
- Optional: LLVM/clang 16+ for the freestanding guest test ELFs.

See [BUILDING.md](BUILDING.md) for the full toolchain table and
troubleshooting guide.

## Releases

Versioning and the alpha/beta/stable release channels are documented
in [RELEASE.md](RELEASE.md).  Contributors do not need to tag
releases; maintainers handle that.
