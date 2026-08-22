# Rule: Safe Ground-Up Refactoring

The existing emulator is a valuable behavioral reference even if its architecture is wrong.

Never start by deleting it.

Refactor in phases:
1. freeze current behavior with characterization tests and traces
2. introduce boundaries
3. move implementation behind boundaries
4. compare old/new behavior
5. delete obsolete paths only after equivalence is demonstrated
6. keep each migration buildable

If a rewrite is proposed, provide:
- reason incremental refactoring cannot achieve the goal
- affected subsystems
- behavior to preserve
- tests/benchmarks
- rollback plan
