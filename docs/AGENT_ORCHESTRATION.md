# Agent Orchestration

## Coordinator

Owns scope, architecture and final integration.

## Specialist Agents

Scout
Architecture
CPU
Memory
Kernel
HLE
Loader
GPU
Shader/RE
Audio
Input
Hardware
Game
Unity
Unreal
PC Game
Debugger
Test
Performance
Release
Reviewer

## Investigation Protocol

Parallel investigation is allowed when questions are independent.

Each agent must return:

Question
Scope
Files Inspected
Commands/Experiments
Evidence
Conclusion
Confidence
Unknowns
Recommendation

## Implementation Protocol

Only one coordinator/integrator should normally modify a given subsystem.

Specialists propose patches only when explicitly assigned implementation.

No two agents may independently rewrite the same architectural boundary.

## Verifier

The reviewer/verifier runs after implementation and independently inspects the diff.
