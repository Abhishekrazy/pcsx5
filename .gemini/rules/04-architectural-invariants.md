# Permanent Architectural Invariants

The following are intended permanent invariants:

1. Core is independent of WPF.
2. Native ABI is explicit and versionable.
3. Guest VA has one authoritative owner.
4. CPU does not own Kernel/HLE policy.
5. Kernel does not own GPU internals.
6. HLE does not own Vulkan internals.
7. Loader does not become the VM allocator.
8. Platform adapters do not become generic guest semantics.
9. Game-specific compatibility logic remains isolated.
10. Unknown NIDs/imports are observable.
11. Tests cannot bypass the behavior they claim to validate.
12. Debugging infrastructure cannot hide failures.
13. Release artifacts are integrity-verifiable.
14. Optional tooling cannot become a hidden core dependency.
15. No subsystem is retained solely because AI finds it convenient.

