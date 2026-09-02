# WALKTHROUGH-YYYY-MM-DD-<slug>

How a change was verified. The record that lets someone else trust the result
without repeating the work — or repeat it exactly if they do not.

## What changed

One paragraph, and the commit.

## Commands run

The actual commands, in order, with their results. Not a summary of them.

## Runs

Run ids, their status on the three axes (process alive, rendering, progressing),
and the record paths.

## Baseline decision

Whether the baseline was updated, and why. Regenerating a golden is a deliberate
act that must state what changed and why the new output is correct.

## What this does NOT show

The limits of the verification. A passing test proves only the path it
exercises; booting does not prove playability; a rendered menu does not prove
gameplay.

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
