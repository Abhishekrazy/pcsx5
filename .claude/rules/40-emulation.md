# Rule: Emulator Correctness

Subsystems must be separable:

CPU -> memory/MMU -> kernel/HLE -> loader -> GPU/audio/input -> runtime orchestration.

Do not let one subsystem reach into another subsystem's private state.

Guest execution must have a defined execution model:
- dispatch
- state
- memory access
- exceptions/faults
- synchronization
- stop/pause/debug behavior

Every new instruction, syscall, GPU feature, or HLE service should have:
- semantic definition
- implementation
- focused test where feasible
- trace/debug visibility
- compatibility notes if title-specific
