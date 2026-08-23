# Architecture Governance

Architecture is a constraint system, not a suggestion.

Before introducing a subsystem, ask:

1. Who owns the responsibility?
2. Who is allowed to call it?
3. What does it depend on?
4. What depends on it?
5. What state does it own?
6. What is its public contract?
7. What is platform-specific?
8. What is guest-visible?
9. What can be tested independently?
10. What happens during initialization/shutdown?
11. What happens when the subsystem is absent or fails?
12. How can it be removed later?

One responsibility must have one authoritative owner.

Avoid:
- god managers
- hidden singletons
- circular ownership
- duplicated registries
- duplicate allocators
- parallel implementations
- "temporary" abstractions with no removal path

