# Rule: Evidence, Scope, and Anti-Imagination

Before implementing emulator behavior, separate:

- OBSERVED: directly verified from code, tests, traces, hardware observations, public documentation, or a reproducible artifact.
- INFERRED: a reasoned conclusion supported by evidence.
- HYPOTHESIS: plausible but unverified.
- UNKNOWN: not currently understood.

Never turn HYPOTHESIS or UNKNOWN into silent production behavior.

Every compatibility workaround must state:
- affected title/build if known
- symptom
- evidence
- root cause confidence
- workaround
- test coverage
- removal condition

Do not use "probably", "should be", or "PS5 likely does X" as a substitute for evidence.
