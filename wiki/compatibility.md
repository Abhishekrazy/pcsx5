# Compatibility

## Statuses

`Compat::Status` — src/compat/compat.h:35-42. Ordered, lower == worse.
Lowercase string forms from `StatusName` (src/compat/compat.cpp:41-51):

| Status | String | Definition | Tracker label |
|---|---|---|---|
| `Nothing` | `nothing` | does not boot (black screen / immediate crash) | `status-nothing` |
| `Boot` | `boot` | shows something but crashes before intro | `status-boots` |
| `Intro` | `intro` | gets to first logo / warning screen | `status-intro` |
| `Menus` | `menus` | main menu reachable | `status-menus` |
| `Ingame` | `ingame` | enters gameplay with major issues | `status-ingame` |
| `Playable` | `playable` | playable end-to-end with minor/no issues | `status-playable` |

Legacy aliases still parse (`StatusFromName`, src/compat/compat.cpp:53-65):
`untested` → nothing, `menu` → menus, `complete` → playable.

## On-disk layout

```
compat/
  compatibility.json        # index: schema_version + sorted title list
  titles/<TITLE_ID>.json    # one full entry per title
```

Index: `{"schema_version": 1, "description": "...", "titles": [...]}`
(src/compat/compat.cpp:438-448). Currently 6 seeded titles: PPSA01668,
PPSA02929, PPSA07429, PPSA10112, PPSA20591, PPSA23885.

## Per-title JSON schema

Written by `EntryToJson` (src/compat/compat.cpp:457-480):

| Field | Type | Meaning |
|---|---|---|
| `schema_version` | number | always 1 (`kCurrentSchemaVersion`) |
| `title_id` | string | e.g. `PPSA01668` |
| `name` | string | game name |
| `region` | string | free-form, e.g. `US`, `EU`, `JP` |
| `version` | string | free-form, e.g. `01.004` |
| `status` | string | one of the six strings above |
| `notes` | string | free text |
| `curated_at` | string | ISO-8601 UTC, auto-set on first save |
| `workarounds` | string[] | CLI flags or notes |
| `auto.last_tested` | string | ISO-8601 UTC of last automated run |
| `auto.last_run_status` | string | `pass` \| `fail` \| `error` |
| `auto.last_run_git_revision` | string | `""` if unknown |
| `auto.last_run_duration_ms` | number | |
| `auto.last_run_resolved_imports` | number | |
| `auto.last_run_unresolved_imports` | number | |

Curated fields are human-edited; `auto.*` is written by the runtime via
`Compat::UpdateAuto` after a run (src/compat/compat.h:9-18). Unknown fields
are ignored on load; unknown `status` strings are a hard error.

## pcsx5_compat CLI

Source: tests/compat_tool.cpp. Binary: `pcsx5_compat`.

```
pcsx5_compat [--root DIR] <command> [args]     # default root: ./compat

list [--status STATUS]                 # table of all entries
show <TITLE_ID> [--json]               # one entry
add <TITLE_ID> --name "..." [--region R] [--version V] [--status S]
    [--notes "..."] [--workaround "..."]...
update <TITLE_ID> [...same flags...] [--add-workaround T] [--clear-workarounds]
remove <TITLE_ID>
search <QUERY>                         # free-text over name/region/version/notes/status/id
report [--out PATH]                    # markdown table (default compat_report.md)
seed-elf <ELF_PATH> [--name N]         # stub entry from a real ELF
```

Examples:

```
pcsx5_compat --root compat list
pcsx5_compat --root compat update PPSA01668 --status ingame --notes "audio crackles"
pcsx5_compat --root compat report --out wiki/compat.md
```

`pcsx5_compat list` prints TITLE / NAME / REGION / VERSION / STATUS /
LAST TESTED columns (tests/compat_tool.cpp:140-164).
