# Change Control

Classify changes:

LEVEL 1: local bug/test correction.
LEVEL 2: subsystem-local reusable capability.
LEVEL 3: cross-subsystem boundary change.
LEVEL 4: architectural/platform change.
LEVEL 5: public ABI, persistence, compatibility contract, or destructive change.

Levels 3-5 require a plan. Levels 4-5 normally require ADR review.

Before significant changes answer:

- What exists already?
- What can be reused?
- Which subsystem owns this?
- Does an existing contract cover it?
- Does an ADR cover it?
- What behavior changes?
- What tests prove it?
- What real-title behavior could change?
- Can it be rolled back?
- What is explicitly out of scope?

