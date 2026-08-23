# Master Guardrails

trigger: always_on

Before implementation:
1. Read GEMINI.md.
2. Read applicable rules.
3. Load applicable skills.
4. Inspect current architecture.
5. Search repository for reusable capabilities.
6. Inspect relevant ADRs.
7. Establish Claims vs Reality.
8. Define scope and stop conditions.

Never silently expand scope.

Every significant task must have:
- owner
- boundaries
- dependencies
- tests
- rollback/containment strategy
- completion criteria

If these cannot be defined safely, STOP.

