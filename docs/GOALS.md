# PCSX5 Goals

## Long-term

- Clean layered emulator architecture.
- Stable native ABI and thin UI.
- Deterministic headless core.
- Strong CPU, memory/MMU, kernel/HLE, loader, GPU, audio and input boundaries.
- High-quality observability and crash diagnostics.
- Evidence-based compatibility database.
- Automated regression, trace comparison and crash clustering.
- Specialized AI agents that investigate without inventing behavior.
- Reproducible builds.
- Signed releases.
- Automatic updates with rollback.
- Human- and AI-maintainable codebase.

## Short-term

1. Preserve current working behavior.
2. Complete Memory/MMU ownership migration.
3. Verify real-title regressions.
4. Clean TLS ownership.
5. Reduce Kernel/HLE coupling.
6. Isolate AGC from GPU internals.
7. Consolidate GPU front door.
8. Remove duplicated CMake sources.
9. Reduce harmful global state.
10. Make stubbed imports observable.
11. Strengthen compatibility regression infrastructure.
12. Build safe release/update infrastructure.

## Explicit non-goals

- No speculative JIT.
- No C++26 upgrade without a concrete need.
- No framework accumulation.
- No aesthetic rewrite.
- No title-specific hacks without evidence.
