# AUDIT 2026-09-07 - PPSA02929's submission rate collapses after the splash

Follow-on from `AUDIT-2026-09-07-black-screen-after-splash.md` section 9. With
render targets binding correctly, the title renders its splash and then goes
black. This audit characterises what the guest does at that point.

## 1. It does not stop drawing; it slows down 15x

The window readout showing `0 draws/s` was misleading - the guest is still
submitting at the end of a 60 s run. Counting command-buffer walks between the
kernel's 30-second heartbeats (run `PPSA02929_20260907_101945`):

| Interval | Command buffers walked |
|---|---|
| 0-30 s | 655 |
| 30-60 s | 43 |

`VERIFIED`. The guest keeps running and keeps submitting, at about a fifteenth
of its earlier rate. That is the signature of something waiting and timing out
rather than something stopped.

## 2. The synchronisation packets are never submitted

The obvious explanation is a fence the guest waits on that we never satisfy.
The title calls the builders for exactly that, 247 times each in a 60 s run:

```
247  sceAgcDcbWaitRegMem
247  sceAgcDcbDmaData
247  sceAgcCbReleaseMem
247  sceAgcCbNop
247  sceAgcWaitRegMemPatchAddress               (unimplemented stub)
247  sceAgcQueueEndOfPipeActionPatchAddress     (unimplemented stub)
247  sceAgcDmaDataPatchSetDstAddressOrOffset    (unimplemented stub)
```

But **none of those packets ever reaches the walker.** An unsampled probe
counting every `WAIT_REG_MEM`, `RELEASE_MEM`, `DMA_DATA` and `WRITE_DATA`
packet, in both their standard and NOP-wrapped forms, found **zero** across a
40 s run (`PPSA02929_20260907_111058`). A census of NOP sub-opcodes over the
same stream shows only:

```
00 marker   04 draw-index-auto   05 draw-reset   11/12/13 sh/cx/uc indirect
14 acquire-mem   17 flip
```

`VERIFIED`. So the guest builds wait, DMA and fence packets that our walker
never sees, and no fence is being missed at execution time because no fence
packet is ever executed.

## 3. It is not a second command buffer we ignore

`sceAgcDriverSubmitDcb` takes a descriptor holding one address at +0 and a
dword count at +8, and the reference implementation reads exactly the same two
fields (`AgcExports.cs:4371-4400`). There is no second buffer in the
descriptor, and the title imports no other submit entry point - only
`sceAgcDriverSubmitDcb`, 427 times, matching its 427 `sceAgcDcbSetFlip` and
427 `sceAgcDcbResetQueue` calls. `VERIFIED`.

## 4. What that leaves

The 247-call family is built into a command buffer that is either never
submitted or reset before submission. `sceAgcDcbResetQueue` is called 427
times, once per submitted frame, and our walker treats the reset sub-opcode as
a full shadow clear. Whether that reset is discarding packets the guest
intends to keep is the open question, and it is the next thing to establish.

`UNKNOWN`: why the builders are called 247 times while their packets never
appear in a submission. Resolving it needs the guest's buffer bookkeeping
followed - which command buffer object those calls target, and what happens to
it - by the same caller analysis that resolved the previous audit.

## 5. Falsified here

- `FALSIFIED`: the guest stops submitting work after the splash. It slows by
  15x and continues to the end of the run.
- `FALSIFIED`: an unwritten `RELEASE_MEM` fence causes the slowdown. A
  consumer for those packets was written and measured; the packets do not
  occur in this title's stream at all, so it changed nothing and was reverted
  rather than left in as untested code.

## 6. Gap recorded, not fixed

`sceAgcCbReleaseMem` emits a `RELEASE_MEM` packet and the walker has no
consumer for it, so the fence value it asks for would never be written. No
title currently exercises the path, so implementing it now would be untested
code; it is recorded in `TASKS.md` instead with the layout already recovered
(control at +8 with the data selection in bits 16-23, destination at +12/+16,
data at +20/+24; selection 1 writes 32 bits, 2 writes 64, 3 writes a clock
sample).
