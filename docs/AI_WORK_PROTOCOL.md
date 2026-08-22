# AI Work Protocol

## Task classification

Before editing, classify the task:

- BUGFIX
- REFACTOR
- FEATURE
- COMPATIBILITY
- PERFORMANCE
- TOOLING
- RELEASE
- DEPENDENCY

## Required output before large changes

```text
Intent:
Scope:
Evidence:
Behavior to preserve:
Files/subsystems:
Tests:
Risks:
Rollback:
```

## Stop conditions

Stop and ask the user when:
- the requested behavior is not supported by available evidence
- a change requires deleting a working subsystem
- a public ABI must break
- a dependency with unclear licensing is required
- a major architecture decision is ambiguous
- correctness and compatibility conflict without a known policy
- the task would create a second competing architecture

## Do not

- refactor unrelated code "while here"
- rename hundreds of symbols without a migration need
- introduce framework layers just to make code look clean
- generate placeholder implementations for hardware behavior
- mark tests as passed without actually running them
- claim a game is compatible from compilation or launch alone
