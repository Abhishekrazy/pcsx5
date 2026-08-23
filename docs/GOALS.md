# PCSX5 Engineering Goals

## Long-Term Goals

### Emulator
- Clean subsystem ownership.
- Stable versioned native ABI.
- Headless deterministic runtime where practical.
- Correct CPU, Memory/MMU, Kernel, HLE, Loader, GPU, Audio and Input boundaries.
- Observable execution and fault diagnostics.
- Strong compatibility regression infrastructure.
- Repeatable title boot/playability evidence.
- Maintainable shader translation pipeline.
- Reliable save state architecture.
- Safe and reproducible release/update system.

### Engineering Platform
- Specialized AI agents for investigation.
- Independent architectural verification.
- Automated crash clustering.
- Trace comparison.
- Binary/NID inventory.
- Shader corpus regression.
- Compatibility matrix generation.
- Reproducible developer environments.

### Distribution
- Reproducible builds.
- Signed releases.
- Stable/preview channels.
- Automatic update checks.
- Staged updates.
- Health verification.
- Rollback.

## Short-Term Goals

1. Preserve current working behavior.
2. Finish Memory/MMU contract and lifecycle cleanup.
3. Establish correct TLS/config lifecycle ownership.
4. Reduce Kernel/HLE dependency cycles.
5. Isolate AGC from GPU internals.
6. Consolidate GPU public front door.
7. Clean build/source duplication.
8. Reduce harmful global mutable state.
9. Make unknown NIDs/stubs observable.
10. Strengthen title compatibility records.
11. Build automated boot/regression tooling.
12. Establish secure release/update architecture.

## Non-Goals

- No speculative JIT.
- No C++26 upgrade merely for novelty.
- No wholesale rewrite.
- No framework accumulation.
- No title-specific hacks without evidence.
- No optimization without measurement.
