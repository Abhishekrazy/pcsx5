# Walkthrough: Direct Memory Contract Implementation

## Overview
This walkthrough traces the resolution of the Direct Memory mapping dependency encountered by the game engine after the `libc.prx` boot phase completes.

## 1. Verifying libc.prx Completion
We first verified that the solution from the previous task was robust. By ensuring that `PT_SCE_PROC_PARAM` was parsed correctly by the ELF loader and mapped properly via `KernelGetProcParam`, `libc.prx` successfully brought up the entire C/C++ environment natively. Execution smoothly transitioned from the PRX queue to the `eboot.bin` entry point, fully validating our previous completion claims.

## 2. Characterizing `sceKernelAllocateDirectMemory`
Once the game engine started, it immediately attempted to allocate a massive 1.9 GB memory arena. By tracing the invocation of `sceKernelAllocateDirectMemory`, we identified that the kernel strictly returns a 0-based **physical memory offset** (in this case, `0x200000`), rather than a virtual address or a memory handle.

## 3. Implementing `sceKernelMapNamedDirectMemory`
The game subsequently called an unknown stub: `sceKernelMapNamedDirectMemory`. 
By analyzing the crash registers, we proved its calling convention matches `sceKernelMapDirectMemory` exactly, with the addition of a `name_ptr` on the stack (`[RSP+8]`). The physical offset (`0x200000`) passed in `R8` matched the allocation offset perfectly.

We implemented the stub, extracting the name (`"orbis_user_malloc"`) and piping the core mapping logic into our existing `MapDirectMemoryCore`. The host commits the underlying physical pool memory and returns the identity-mapped Virtual Address back to the guest.

## Conclusion
The game successfully used the mapped direct memory pool. The emulator securely tracked ownership and released the physical pool on shutdown. With Direct Memory operational, the game progressed to the next subsystem check: `sceKernelIsAddressSanitizerEnabled`.
