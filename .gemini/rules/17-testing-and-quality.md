# Testing Governance

Use a test pyramid:

unit
-> subsystem
-> integration
-> headless
-> compatibility
-> real-title

Every behavioral change must have an appropriate test.

Tests must:
- fail for the bug
- pass for the corrected behavior
- exercise the real path where practical
- avoid mocks that bypass the contract under test

Never:
- skip a failing test without explanation
- weaken an assertion
- broaden expected outputs to hide regressions
- change crash classification merely to get green

