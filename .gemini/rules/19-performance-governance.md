# Performance Governance

No optimization before measurement.

Baseline:
- workload
- build/config
- CPU/GPU environment
- median
- p95
- p99
- throughput/frame time where relevant
- memory
- error/regression rate

Measure real emulator paths.

Do not benchmark a bypassed path and call it emulator performance.

Optimization must preserve:
- correctness
- determinism where required
- compatibility
- observability
- architecture

Prefer removing unnecessary work over adding clever machinery.

