# Repository Hygiene

What belongs in this repository, and what does not.

## Principle

The repository contains what someone needs to build, understand and contribute
to the emulator. Anything that only makes sense on one developer's machine —
run logs, captures, crash dumps, local clones, scratch notes — stays out of it,
however useful it was at the time.

## What belongs here

| Content | Location |
|---|---|
| Emulator source | `src/**` |
| Tests and fixtures | `tests/**` |
| Architecture reference and ADRs | `architecture/**` |
| Component map, how to extend | `guide/**` |
| Contributor how-tos | `wiki/**` |
| Developer and RE tooling | `tools/**` |
| Vendored dependencies, with licence and pinned version | `third_party/**` |
| Runtime baseline, generated from real runs | `tests/runtime_baseline.json` |

## What does not

Transient output, regardless of how it was produced:

- boot and run logs, guest traces, diagnostic dumps
- crash dumps and minidumps (`*.dmp`, `crash_*`)
- decoded audio, captured frames, shader caches (`Cache/`)
- generated inventories such as the import report
- build outputs (`build/`, `dist/`, `out/`, `bin/`, `obj/`)
- local scratch directories and personal clones of reference projects
- editor and assistant configuration

These belong in an untracked working directory. If a finding depends on a log,
quote the decisive lines in the document that makes the claim rather than
committing the whole file — a multi-hundred-kilobyte log is not evidence anyone
will read.

## What must never be deleted

`src/**`, `tests/**`, `architecture/**`, `tools/**`, `assets/**`, the build
scripts, `LICENSE`, and any vendored `LICENSE` file. A file whose origin or
purpose cannot be determined is left alone until it is understood, not tidied
away.

## Vendored dependencies

Each lives in `third_party/<Name>/` with the upstream licence preserved verbatim
and a `README.md` recording upstream URL, author, pinned commit, licence,
compatibility with this project's GPL-2.0 licence, build wiring, update
procedure and any local modifications.

PCSX5 is GPL-2.0: MIT and BSD are compatible, **Apache-2.0 is not**. Check the
licence before vendoring, never after.

## Before committing

Read `git status`. Nothing from `build/`, `dist/`, `Cache/`, `artifacts/` or a
scratch directory should appear, and no `.log`, `.dmp` or dump file should be
staged. Never `git add -A` without looking at what it picked up.
