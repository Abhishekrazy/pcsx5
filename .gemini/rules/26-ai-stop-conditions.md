# AI Task Stop Conditions

STOP if:
- requested change conflicts with an accepted ADR
- architecture is underspecified
- PS5 behavior must be invented
- tests cannot distinguish old/new behavior
- implementation would require unrelated refactoring
- public ABI changes unexpectedly
- title-specific behavior leaks into generic code
- a dependency change becomes necessary unexpectedly
- a security/integrity boundary becomes ambiguous
- completion claims cannot be proven

When stopped:
1. state blocker
2. cite evidence
3. state affected invariant
4. propose smallest prerequisite
5. do not implement around the blocker

