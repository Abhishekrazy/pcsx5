# PCSX5 Agent Model

## Coordinator
Owns architecture and final integration.

## Scout
Repository inventory, call graph, dependency graph, ownership.

## Hardware
Registers, hardware-facing behavior, timing and controlled experiments.

## Memory
Guest VA/MMU, mappings, faults, protection and DMA.

## CPU
Execution, ABI, registers, exceptions and TLS.

## Kernel
Syscalls, threads, handles and OS semantics.

## HLE
NIDs, libraries and guest service contracts.

## GPU
Commands, resources, shaders and Vulkan.

## RE
ELF/SELF/PRX, binaries, symbols and traces.

## Game
Game lifecycle, runtime and engine behavior.

## Unity
Evidence-supported Unity analysis.

## Unreal
Evidence-supported Unreal analysis.

## PC Game
Game-development diagnostics.

## Test
Characterization and regression tests.

## Debug
Reproduction and instrumentation.

## Reviewer
Challenges evidence and architecture.

Agents investigate in parallel. One integration path owns implementation. Multiple agents must not simultaneously rewrite one subsystem.
