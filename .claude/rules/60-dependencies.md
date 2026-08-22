# Rule: Dependency Governance

Before adding a dependency, answer:

1. What problem does it solve?
2. Why cannot an existing dependency or small internal implementation solve it?
3. Is it runtime or build-time?
4. License/redistribution implications?
5. Binary size and startup impact?
6. Maintenance activity and update strategy?
7. Security/update process?
8. Can it be isolated behind one wrapper?

Pin versions. Keep vendored dependencies under third_party with metadata.
Do not duplicate libraries that solve the same problem.
