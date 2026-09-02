# TASK documents

Forward-looking specifications: scope, prerequisites, acceptance criteria and the next task.

Naming: `TASK-YYYY-MM-DD-<slug>.md`

These directories are referenced by the project's engineering rules — a rule
that says "record it in the owning audit" has nowhere to discharge to if
`docs/audits/` does not exist. They were missing entirely, so every such rule
was unenforceable in practice. `tools/check_doc_links.py` now fails if a
documented path stops resolving.

Transient runtime output does not belong here. Logs, traces, frames, crash
dumps and shader caches live in `artifacts/runtime/` and `.work/`, both
untracked (see `docs/EVIDENCE_POLICY.md`).
