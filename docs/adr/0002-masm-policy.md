# ADR 0002: Conditional MASM Use

## Status

Accepted

## Decision

MASM is permitted only for the guest dispatcher/calling-convention bridge where it provides a measurable or correctness-critical benefit.

## Consequences

Assembly remains isolated and reviewable.

If the dispatcher can be replaced with C++/intrinsics without regression, remove MASM.

