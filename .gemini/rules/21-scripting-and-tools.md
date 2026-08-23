# Developer Tooling Governance

Prefer deterministic scripts for repeated work.

Good automation:
- test orchestration
- boot regression
- log parsing
- crash clustering
- trace comparison
- NID inventory
- ELF/SELF/PRX inspection
- shader corpus
- compatibility matrix
- dependency inventory
- architecture reports

Tools must:
- be bounded
- be repeatable
- identify inputs
- identify outputs
- avoid destructive source edits by default
- support machine-readable output where practical

