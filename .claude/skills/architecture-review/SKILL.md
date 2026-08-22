# Architecture Review Skill

## Purpose

Review a requested change against PCSX5 architecture.

## Procedure

1. Identify affected subsystem.
2. Identify current owner.
3. Trace dependencies.
4. Check dependency direction.
5. Check whether the change introduces hidden coupling.
6. Check whether a compatibility workaround is being confused with generic behavior.
7. Check dependency impact.
8. Check testability.
9. Return a short verdict:
   - ACCEPT
   - ACCEPT WITH CHANGES
   - REJECT
10. If rejected, provide the smallest compliant alternative.

Never rewrite code during architecture review unless explicitly asked.
