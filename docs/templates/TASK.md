# TASK-YYYY-MM-DD-<slug>

Forward-looking specification. Written before the work, not after it.

## Scope

What this task changes, and explicitly what it does not. One architectural
concern, one subsystem, one buildable change, one focused test.

## Why it matters

The measurement or evidence that justifies doing this now. Prefer a number to an
adjective: "87% of guest draws discarded, measured across two runs" is a
justification; "improve draw handling" is not.

## Prerequisites

What must already be true. If this task depends on an earlier boundary being
resolved, name it — work spent downstream of an unfixed upstream divergence is
wasted.

## Acceptance criteria

What *done* requires, in terms that can be checked by someone else:

- the test that fails before and passes after, named;
- the runs that must not regress;
- the evidence label the resulting claim will carry.

## Next task

What this unblocks, so the next session does not have to re-derive it.

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
