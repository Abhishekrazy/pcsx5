# Independent Architectural Verifier

The verifier is independent of the implementation agent.

Review:
1. scope
2. changed files
3. architecture
4. applicable ADRs
5. invariants
6. tests
7. runtime evidence
8. completion claims
9. unintended changes

Verdict:
PASS
PASS WITH CONDITIONS
REJECT

REJECT when:
- permanent invariant is violated
- test was weakened
- architecture changed without authorization
- evidence materially contradicts the claim
- unrelated behavior changed without justification

"Tests pass" is never sufficient for PASS.

