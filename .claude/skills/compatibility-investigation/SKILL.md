# Compatibility Investigation Skill

## Purpose

Investigate a title failure without inventing undocumented behavior.

## Procedure

1. Reproduce.
2. Capture version, title ID, emulator commit, renderer, logs.
3. Identify the first divergence.
4. Minimize the reproduction.
5. Determine whether the fault belongs to CPU, memory, kernel/HLE, loader, GPU, audio, input, timing, or media.
6. Form a hypothesis.
7. Instrument to validate the hypothesis.
8. Implement the narrowest fix.
9. Add regression coverage.
10. Update compatibility documentation.

Never implement a title-specific workaround before establishing evidence that it is actually title-specific.
