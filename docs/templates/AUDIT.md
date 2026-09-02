# AUDIT-YYYY-MM-DD-<slug>

Forensics. What was investigated, what was found, and what was disproved.

## Question

The specific question this audit set out to answer.

## Method

How it was investigated — the commands, the instrumentation added, the runs
examined. Enough that someone else can repeat it.

## Findings

Each finding with its evidence and its label. Cite `file:line`, a run id, or the
command that reproduces it.

## Falsified

Hypotheses this audit disproved, and the evidence that killed them. **Keep
these.** A hypothesis silently dropped will be re-tried by a future session; the
project's rules treat a disproved hypothesis as a result worth recording.

## Boundary

If the investigation ended at something the backend cannot represent, classify
it `SOFT BOUNDARY` (solvable with a known technique, bounded work) or
`HARD BOUNDARY` (requires a capability the backend does not have, or PS5
behaviour that is UNKNOWN with no path to evidence), and say what would lift it.

## Next boundary

Where the next investigation should start.

Every material claim carries exactly one evidence label:

| Label | Means |
|---|---|
| `VERIFIED` | Reproducibly observed on this system, with the command that reproduces it |
| `OBSERVED` | Seen at least once at runtime, cited to a specific run |
| `INFERRED` | Deduced from disassembly, call patterns or a reference implementation |
| `UNKNOWN` | Not established; state what evidence would resolve it |
| `FALSIFIED` | A hypothesis disproved; recorded so it is not re-tried |

A label is never promoted without new evidence. Compiling is not evidence, and a
title advancing is not evidence that a change was correct.
