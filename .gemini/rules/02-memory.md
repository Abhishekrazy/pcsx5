# PCSX5 Rule

Memory/MMU owns guest virtual address space. Kernel, Loader and HLE request memory operations instead of maintaining competing allocators. Keep Reserve, Commit, Map, Unmap, Protect, Query, Translate and fault/access concepts explicit. Guest addresses and host pointers must remain conceptually distinct.
