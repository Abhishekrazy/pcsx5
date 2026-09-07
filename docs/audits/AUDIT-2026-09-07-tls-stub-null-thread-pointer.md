# AUDIT 2026-09-07 - The intermittent TLS crash: a stub with no fallback

## 1. Baseline

| | |
|---|---|
| Commit at start | `9a082ac`, clean tree |
| Build | `cmake --build build --config Release`, Visual Studio 18 2026, x64 |
| Backend | Vulkan, windowed, GLFW |
| Title | PPSA02929, `Games/PPSA02929-app0/eboot.bin` |
| Launch | `session.py run --title PPSA02929 --duration 45` |
| Test suite | 53 of 53 |
| Title behaviour | `progressing`, splash then scene, per the previous phase |

## 2. Crash reproduction

Twelve 45-second runs on the unmodified build. Eleven completed
`progressing`; one crashed (`PPSA02929_20260907_133921`, 12.99 s).

The crashing run's register dump is the whole case:

```
Exception Code: 0xC0000005
Crash Address (RIP): 0x80015ff6d
RAX: 0x0000000000000000
```

Disassembling that address in the retail executable:

```
0x80015ff60  and eax, 0                     ; tail of `mov rax, fs:[0]`
0x80015ff6d  mov r12, qword ptr [rax - 0x15b8]
```

`RAX` is zero immediately after an `fs:[0]` load, and the next instruction
dereferences a null-based address. `VERIFIED`: the guest thread pointer
resolved to zero.

Two earlier crashes in this series (`PPSA02929_20260907_101044`,
`..._122216`) reported `Emulated TLS read failed` instead. Both were also
`fs:[0]` sites, disassembled and confirmed to have displacement zero, so in
those cases the *exception-handler* path had a bad pointer while here the
*patched stub* path returned zero. Same defect class, two manifestations.

## 3. Investigation timeline

```
Crash evidence: RAX = 0 after fs:[0]
   |
Which code produced RAX? -> the patched TLS stub, not the exception handler
   |
Where does the stub get the pointer? -> a host TLS slot, read via gs:
   |
What is in that slot? -> whatever BindCurrentThread stored, or zero
   |
What does the stub do about zero? -> nothing
```

## 4. Hypotheses considered and falsified

**H1 - the thread-registry lookup uses the wrong key.** The fault path calls
`GetCurrentThreadId()` while threads register under a synthetic guest id.
Experiment: resolve the call. Result: unqualified inside `namespace Kernel` it
binds to `Kernel::GetCurrentThreadId()`, which returns the guest id; host-id
uses are explicitly `::`-qualified and commented. `FALSIFIED`.

**H2 - the TLS slot index escapes the inline TEB array.** The stub reads
`TEB+0x1480 + 8*index`, and that array holds only 64 entries. Experiment: read
the initialisation path. Result: it refuses indices at or above 64 and
verifies the computed address against `TlsGetValue` before enabling patching.
`FALSIFIED`.

**H3 - worker TLS blocks are host allocations invisible to guest memory.**
Experiment: read `scePthreadCreate`. Result: the block is registered with
`Memory::AdoptRange`, so it is queryable and readable. `FALSIFIED`.

**H4 - use-after-free of a TLS block at thread exit.** Experiment: check the
observed addresses against every logged allocation. Result: all registered
thread pointers and the shared block are page-aligned; the failing values are
a page base plus `0x9090` and match no allocation. Not supported by evidence,
and superseded by the verified cause. `UNKNOWN`, left recorded.

**H5 - the guest sets its own fs base through a system call.** Experiment:
search for any such entry point. Result: none exists in the emulator.
`FALSIFIED`.

## 5. Root cause

**Subsystem**: kernel, TLS patching (`src/kernel/tls_patch.cpp`).

**Faulty behaviour**: the stub emitted for a patched site loaded the guest
thread pointer with a bare `mov rax, gs:[slot]` and used the result unchecked.

**Why it was wrong**: that slot is populated by `BindCurrentThread`, which is
called on the main thread and on threads created through the emulator's own
thread path. A host thread that reaches guest code by any other route has
never been bound, and a Windows TLS slot starts as zero. The stub then handed
the guest a null thread pointer.

The exception-handler path this stub replaced resolves the identical access
through a three-level chain - per-thread cache, thread registry, shared block -
and refuses to resume the guest if even that yields zero. The two paths
therefore disagreed on exactly this case. `src/cpu/cpu.cpp` states in a comment
that they must agree, because a divergence "silently diverges" and corrupts the
thread. The stub was the half without the fallback.

**Evidence**: the register dump above, plus the emitter, which contained no
test of the loaded value.

## 6. Implementation

`emit_load_tp` now emits:

```
pushfq
mov  rax, gs:[slot]
test rax, rax
jne  done
mov  rax, imm64          ; the shared thread pointer
done:
popfq
```

The `pushfq`/`popfq` pair preserves the stub's EFLAGS guarantee, which the
`test` would otherwise break; every other stub form is flag-transparent. The
pair is balanced inside the helper, so the `[rsp]` offsets the non-`rax` forms
rely on are unaffected.

`EmitStub` additionally refuses to emit when the fallback is zero, leaving the
site unpatched for the exception handler rather than baking in the null
pointer it exists to prevent.

The fallback is the same value the exception-handler path would have used, so
this makes the two agree. It is not a general "turn a bad read into zero"
guard: it supplies a thread pointer, and only when the slot holds a value that
is never valid.

No rendering behaviour was touched.

## 7. Regression test

`tests/tls_patch_stub_tests.cpp` (new), CTest name `tls_patch_stub`. It asserts
the emitted machine code contains the zero test, carries the fallback pointer
it was given, saves and restores EFLAGS, and that a zero fallback refuses to
emit.

**Not covered**: executing the stub, which needs a live process with a real
TLS slot. Only the emitted bytes are asserted. Stated rather than implied.

## 8. Validation

| | before | after |
|---|---|---|
| Test suite | 53 of 53 | **54 of 54** |
| 45 s runs | 11 of 12 `progressing`, 1 null-pointer crash | **12 of 12 `progressing`** |
| Crash lines in run logs | present in the crashing run | **0 across all 12** |
| Render-to-texture fix | passing | passing, and every run still reaches the scene |

The previous phase's fix is intact: its own test still passes and each run
still progresses past the splash, which is only possible through that path.

## 9. Remaining boundary

**The statistics are suggestive, not conclusive.** Against the historical rate
of roughly one crash in four to six runs, twelve clean post-fix runs is
meaningful but not proof - under a one-in-six rate, twelve clean runs would
still happen by chance about one time in eight. The stronger evidence is the mechanism: a register dump showing a
null thread pointer out of `fs:[0]`, and an emitter that demonstrably had no
fallback. Longer soak runs would tighten it.

**The underlying question is unanswered**: *which* host threads reach guest
code without being bound, and why. The fix makes those threads behave as the
exception handler always would have, but a thread that should own private TLS
and instead shares the main block is a latent correctness problem in its own
right. That is recorded as a separate task, not fixed here.

**The `Emulated TLS read failed` variant is unexplained.** The crash this
audit fixes came through the patched stub. The two historical crashes that
printed that message came through the exception handler with a thread pointer
matching no allocation we make, and section 4's H4 remains `UNKNOWN`. It has
not recurred in 12 post-fix runs, but absence is not a diagnosis.

Rather than speculate, the failure branch now records what a recurrence needs:
which of the three resolution sources supplied the pointer, the guest and host
thread ids, the faulting RIP, and what the memory subsystem says about the
address. It costs nothing until the failure happens, because it sits inside
the branch that only runs on failure. Committed alongside this audit.

**Next phase**, per the plan: rendering correctness - colour, geometry,
banding and stepped edges - kept separate so measurements stay attributable.
