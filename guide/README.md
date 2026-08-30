# PCSX5 Developer Guide

All engineering documentation in one place. Read `COMPONENT_MAP.md` first; use
`EXTENDING.md` when adding a feature.

## Index

| Doc | When to read |
|---|---|
| [`COMPONENT_MAP.md`](./COMPONENT_MAP.md) | **Start here.** What every subsystem + key class does, who owns it, and where to plug a feature. Prevents touching unrelated code. |
| [`EXTENDING.md`](./EXTENDING.md) | Adding a feature: game modding, resolution/upscaling, native input/audio/graphics backends, config options, guest trace tooling. |
| [`PROJECT_ATOM.md`](./PROJECT_ATOM.md) | The concise, machine-parsable project blueprint + invariant rules + per-feature "touch only these files" table. Load this into session context before any large work. |

## Pointers to existing repo docs

- Architecture deep-dive: `../wiki/architecture.md`
- Developer guide (HLE symbols, tests, porting): `../DEVGUIDE.md`
- Build: `../BUILDING.md` and `../build_release.ps1`
- Roadmap / priorities: `../ROADMAP.md`, `../docs/GOALS.md`

## Generate this folder (authoritative source of truth)

These files are maintained by hand from the real `src/` header inventory. When a new
subsystem or class is added, update `COMPONENT_MAP.md` and `PROJECT_ATOM.md` in the
same change. Do not let them drift.
