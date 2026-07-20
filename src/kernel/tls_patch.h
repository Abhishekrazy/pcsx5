#pragma once

#include "../common/types.h"

namespace Kernel::TlsPatch {

// Patch-once cache for fs-relative guest TLS accesses.
//
// The guest runs natively on the host, so every `mov reg, fs:[disp]` TLS
// access faults (host fs base is not the guest thread pointer) and is
// emulated by the kernel VEH.  Hot sites trap on every execution (~9,300
// first-chance AVs per boot).  This module instead rewrites the faulting
// instruction ONCE into a `call` to a per-site stub that performs the same
// access natively: the stub loads the current thread's guest thread pointer
// from a host TLS slot (read directly from the TEB TlsSlots array via gs:)
// and performs the load/store.  Subsequent executions run at native speed
// with no VEH round-trip.  Reference: SharpEmu's DirectExecutionBackend
// (TryPatchTlsLoadInstruction / CreateTlsHandler).
//
// Only the absolute-disp32 forms are patchable (mod==0, rm==5 or SIB
// base==5; opcodes 0x8B read / 0x89 write / 0xC7 imm store).  Everything
// else keeps the VEH emulation path unchanged.

// Description of one parsed fs-relative access, filled by the VEH parser.
struct AccessInfo {
    u8  opcode;        // 0x8B (read), 0x89 (write), 0xC7 (imm store)
    bool is_64bit;     // REX.W present
    u8  reg;           // full 4-bit register index (REX.R folded in)
    s32 displacement;  // signed fs displacement
    u32 imm32;         // immediate for opcode 0xC7
    u32 instr_len;     // total byte length of the original instruction
};

// Allocates the host TLS slot and the guest-address-space stub region.
// Returns false when patching is unavailable; the VEH emulation path then
// keeps handling every trap (no functional change).
bool Initialize();
void Shutdown();
[[nodiscard]] bool IsInitialized();

// Fallback thread pointer used for threads bound with tp == 0 (mirrors the
// VEH's g_guest_tls fallback).  Called once from Kernel::Initialize.
void SetDefaultThreadPointer(guest_addr_t tp);

// Binds the calling host thread's guest thread pointer into the host TLS
// slot the stubs read.  Must be called by every thread that executes guest
// code (guest worker thread, guest pthread entries).  tp == 0 binds the
// default.
void BindCurrentThread(guest_addr_t tp);

// Called from the VEH AFTER a successful emulation of the access at `rip`.
// Rewrites the guest instruction into a call to a freshly emitted stub.
// Returns true when the site is patched (or already was).
bool TryPatchSite(u64 rip, const AccessInfo& access);

// VEH race/retry helpers:
// A concurrent thread can fault on a site while it is being rewritten.
// ShouldRetry returns true (bounded per site) so the VEH can simply
// re-execute the instruction once the write has landed.
bool ShouldRetry(u64 rip);
// If `stub_rip` lies inside an emitted stub, the patched code faulted
// (typically a thread that reached the site before BindCurrentThread ran,
// i.e. tp == 0).  Restores the original instruction, blacklists the site
// from re-patching, and returns the site RIP via `out_site_rip` so the VEH
// can redirect execution back to the original (emulated) instruction.
bool HandleStubFault(u64 stub_rip, u64* out_site_rip);

// Restores the original bytes of every patched site and frees all stubs.
void InvalidateAll();

// Statistics (heartbeat log + before/after measurement).
u64 TrapCount();
void NoteTrap();
u64 PatchedCount();

} // namespace Kernel::TlsPatch
