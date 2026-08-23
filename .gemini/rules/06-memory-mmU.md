# Memory / MMU Governance

Memory/MMU owns guest virtual address space.

Required conceptual distinction:

Reserve
Commit
Map
Unmap
Protect
Query
Translate
Fault
Backing

Do not collapse these concepts merely because Windows exposes convenient APIs.

Guest pointer, host pointer, guest VA and backing allocation must be distinguishable in interfaces.

Every direct host allocation representing guest-visible memory must either:
- be created through Memory/MMU, or
- be explicitly adopted/tracked.

No competing guest VA allocator may exist.

Memory lifecycle must cover:
- initialization
- allocation
- mapping
- protection
- fault
- release
- shutdown
- reinitialization

Page-zero/null behavior must remain explicit.

