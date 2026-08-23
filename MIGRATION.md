# Migrating the AI Harness

The Gemini harness is designed to replace or run alongside the Claude Code harness.

Do not delete the Claude harness automatically.

Recommended sequence:

1. Install `.gemini/`.
2. Add `GEMINI.md`.
3. Keep existing `CLAUDE.md` during transition.
4. Ensure `docs/` remains the shared architectural source of truth.
5. Run an audit-only Gemini session.
6. Compare its findings with the existing PCSX5 audit.
7. Only then allow implementation.
