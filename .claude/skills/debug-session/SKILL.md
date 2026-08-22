# Debug Session Skill

## Procedure

1. Establish exact reproduction.
2. Collect bounded logs and crash context.
3. Record guest PC and subsystem.
4. Compare against last known-good revision.
5. Minimize.
6. Instrument.
7. Fix root cause.
8. Add regression test.
9. Remove temporary instrumentation or downgrade it to appropriate logging.
10. Document unresolved hypotheses.

Do not paper over crashes with catch-all exception handling.
