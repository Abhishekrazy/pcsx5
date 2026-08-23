# PCSX5 Gemini Harness v2 Installation

Copy this package into the PCSX5 repository root.

Important:
- `GEMINI.md` is the project AI constitution.
- `.gemini/rules/` contains mandatory governance.
- `.gemini/skills/` contains procedural specialist skills.
- `.gemini/commands/` contains bounded workflows.
- `architecture/` is intended as the shared architecture source of truth.
- `docs/` contains goals/reporting/orchestration templates.

If the repository already has a Claude harness, do not delete it automatically.

Recommended transition:
1. Install this harness.
2. Keep existing Claude files during migration.
3. Compare both harnesses against shared architecture/ADRs.
4. Run Gemini in audit/planning mode first.
5. Only then allow implementation.

Do not copy project-specific AZOOG/SharkTravel rules from another project. This harness is rewritten for PCSX5 emulator architecture.
