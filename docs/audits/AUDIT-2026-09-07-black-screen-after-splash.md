# AUDIT 2026-09-07 - PPSA02929 goes black after the publisher splash

Follow-on from `AUDIT-2026-09-07-targetless-draw-storage-path.md`. With every
retained targetless draw now composited, the title renders its publisher
splash correctly and then goes black at about 16 s and stays black. This audit
establishes why.

## 1. The splash proves the pipeline works

`artifacts/runtime/PPSA02929_20260907_055927/frames/frame_0007.png` (t=16.13 s)
shows the Ratalaika Games logo rendered correctly: sprite geometry,
texture sampling, compositing and presentation all work end to end.
`VERIFIED` - the frame is a stored artifact.

`frame_0028.png` (t=58.68 s) is entirely black, while the window title readout
in the same capture reads `26 draws/s`. The guest is drawing *more* after the
splash than during it, and producing nothing visible.

## 2. What was eliminated, with evidence

Each of these was probed at runtime and ruled out. All probes were temporary
and are removed.

| Suspect | Finding | Verdict |
|---|---|---|
| Guest waiting for a controller press | The pad is polled 852 times (import report). A replay pressing Cross throughout changed nothing. | Ruled out |
| Composite draws failing | `PROBE composite: ok=400 failed=0`. Every composite returns success. | Ruled out |
| Wrong display buffer presented | Flips alternate evenly, 235 at index 0 and 234 at index 1, and each composite targets the buffer its own flip names. | Ruled out |
| Degenerate viewport or scissor | The registers are zero, but `DecodeViewportScissor` falls back to the full target for exactly this case (`gfx10_state.cpp:530-552`). | Ruled out |
| Render target re-seeded from guest memory each frame | Seeding happens once, at render-target creation only (`vk_draw.cpp:719`). | Ruled out |
| Per-draw clear wiping earlier sprites | The render pass uses `VK_ATTACHMENT_LOAD_OP_LOAD` (`vk_draw.cpp:665`); draws accumulate. | Ruled out |

A correction worth recording: an earlier probe appeared to show that only one
of the two display buffers was ever presented. That was a sampling artifact -
the probe fired every 60th present and the buffers strictly alternate, so it
only ever observed one parity. `FALSIFIED`.

## 3. The cause

The composites sample textures that are **empty in guest memory**. Probing the
first 4 KB of each composite's texture 0:

```
PROBE tex bytes: addr=0x215ed0000 read=4096 nonzero=4096
PROBE tex bytes: addr=0x21bef0000 read=4096 nonzero=4096
PROBE tex bytes: addr=0x21dae0000 read=4096 nonzero=0      <- 1280x720
PROBE tex bytes: addr=0x21c270000 read=4096 nonzero=0      <- 980x347
```

`OBSERVED`, run `PPSA02929_20260907_060719`. Some textures hold real data;
the two that carry the scene - a full-screen 1280x720 surface and a 980x347
one, the shapes of a background and a title graphic - are entirely zero.

Those are textures the guest produced **on the GPU**, not from the CPU. Our
texture upload reads guest memory, so a GPU-produced texture arrives black.

The splash worked because its texture was CPU-supplied (a decoded image).

## 4. Why the guest's render-to-texture work never lands

This is the part that matters architecturally. The guest's draws that *fill*
those textures carry no decodable colour target, so our AGC walker classifies
them as targetless and composites them onto the **display buffer** at the
flip. Their real destination was a texture. The heuristic sends
render-to-texture work to the screen instead of to the texture, and the
texture stays empty.

`INFERRED`, not verified: this follows from the two established facts (the
scene textures are empty, and this guest issues no draw with a decodable
colour target) plus the absence of any render-to-texture path in the walker.
Confirming it needs the destination of an untargeted draw identified.

This connects an existing open item: "Colour-target registers are not decoded
... For PPSA02929 the guest genuinely never binds a target (measured: 54
registers set, none of them colour-target)". That measurement stands. The new
information is that the guest must nevertheless be naming a destination
somewhere, because its own content depends on it.

## 5. Next boundary

Find where an AGC draw names its render destination when the raw
`CB_COLOR0` registers are zero. Candidates, in order of cheapness:

1. Another colour slot (`CB_COLOR1..7`) that the walker does not read.
2. The AGC `RenderTarget` object, bound through an `sceAgc` call rather than
   written as raw context registers. `sceAgcSetCxRegIndirectPatchAddRegisters`
   is called 26,435 times in a 60 s run and is the highest-traffic AGC symbol
   after the register setters - indirect register patching is where a target
   could be arriving without appearing in the shadow.
3. A depth or resolve target standing in for the colour target.

Until one of these is established the black screen must stay `UNKNOWN` rather
than be worked around; substituting a guessed destination would be exactly the
kind of speculative fix that makes later observations uninterpretable.

---

## 6. Candidate 2 investigated: indirect register lists (2026-09-07)

Section 5 named `sceAgcSetCxRegIndirectPatchAddRegisters` as the most likely
place a colour target could arrive without appearing in the register shadow.
It was probed and **does not explain the black screen**.

### What the indirect lists contain

Probing every indirect register packet (`PROBE indirect`, run
`PPSA02929_20260907_092431`) shows two shapes reaching the same opcode:

```
reg=12 count=19  [0]000001B1=00000000 [1]000001C3=00000004 [2]000001C2=00000001 ...
reg=12 count=35  [0]FFFFFFFF=FFFFFFFF [1]00000191=00000000 [2]00000192=00000001 ...
reg=12 count=134 [0]FFFFFFFF=02113A00 [1]FFFFFFFF=FC001FFF [2]FFFFFFFF=EBFAA0AB ...
```

Most lists carry real context-register offsets (`0x1B1`, `0x1C3`, `0x191`…).
One carries `0xFFFFFFFF` in the offset slot of every entry. A raw dword dump
confirms the 8-byte stride is correct and the pattern is a strict alternation
of `FFFFFFFF` and a value, so this is not a stride error:

```
FFFFFFFF 02113A00 FFFFFFFF FC001FFF FFFFFFFF EBFAA0AB FFFFFFFF FFFE0FFF ...
```

The first value, `0x02113A00`, is the display buffer address `0x2113A0000`
shifted right by 8 - the encoding `CB_COLOR0_BASE` uses. That is what made
this list look promising.

### Why it is not the answer

SharpEmu parses this packet identically: 8-byte `{offset, value}` pairs
written straight into the register shadow with no filtering of `0xFFFFFFFF`
(`AgcExports.cs:7878-7893`, including an explicit comment that offset zero is
a real register and not a terminator). Both emulators therefore scatter the
same 134 entries into the same meaningless shadow slot, and SharpEmu still
reaches gameplay. The behaviour is identical on both sides, so it cannot be
the difference. `FALSIFIED` as the cause.

The value matching the display buffer address is most likely a leftover: the
list lives in GPU scratch memory at `0x21139f000` that previously held it.
`INFERRED`; establishing it properly would need the buffer's history.

### A small real defect, recorded not fixed

Entries whose offset is `0xFFFFFFFF` are written into the register shadow at
key `0xFFFFFFFF`. A context register index is under `0x400`, so this is never
a register. It is harmless today because nothing reads that key, but it is
134 junk writes per call across 26,435 calls in a 60 s run. Filtering it is a
separate change and was deliberately not made mid-investigation.

### Where this leaves the boundary

Sections 3 and 4 stand: the scene's textures are empty because they are
GPU-produced and our upload reads guest memory. The remaining candidates from
section 5 are untested - other colour slots (`CB_COLOR1..7`) and a
depth/resolve target standing in for the colour target. The question is still
where an untargeted draw names its destination, and it is still `UNKNOWN`.

---

## 7. The guest does multi-pass render-to-texture (2026-09-07)

### First, a correction to section 3's evidence

Section 3 concluded the scene textures were empty from a 4 KB sample. That
sample is smaller than a single row of a 1280-wide texture, so a surface with
a transparent top edge would have read as empty. The claim was re-tested by
sampling 16 points spread across each texture's full extent:

```
PROBE texscan addr=0x215ed0000 1280x720 bytes=3686400 samples=16 nonzero=16
PROBE texscan addr=0x21b9a0000 1280x720 bytes=3686400 samples=16 nonzero=0
PROBE texscan addr=0x21beb0000  320x512 bytes=655360  samples=16 nonzero=10
PROBE texscan addr=0x21cc60000  250x250 bytes=250000  samples=16 nonzero=16
```

The conclusion survives: one full-screen 3.6 MB surface is zero at every
sample point while the others carry data. `VERIFIED`, run
`PPSA02929_20260907_093317`.

### What the composite batch actually contains

Logging the order and source of every draw in one flush
(`PPSA02929_20260907_093451`):

```
0:0x21bf00000(320x512,nz5/8)   1:0x21daf0000(1280x720,nz0/8)
2:0x21bf00000(320x512,nz5/8)   3:0x21daf0000(1280x720,nz0/8)
4:0x21c280000(980x347,nz5/8)   5:0x21daf0000(1280x720,nz0/8)
6:0x21daf0000(1280x720,nz0/8)  7:0x21c280000(980x347,nz5/8)
8:0x21daf0000(1280x720,nz0/8)  9:0x21ccb0000(250x250,nz8/8)
10:0x21d280000(1280x720,nz0/8) 11:0x21b9f0000(1280x720,nz0/8)
```

The guest alternates a real sprite with a **full-screen quad sampling an
offscreen surface**, and the last two draws of the batch are both full-screen
passes over empty surfaces. That is a multi-pass render-to-texture chain, and
it explains the black screen exactly: we draw the real sprites correctly onto
the display buffer, and then the guest's final full-screen passes paint an
empty surface over the top of them. `VERIFIED`.

Those surfaces sit at different addresses every frame (`0x21daf0000`,
`0x21d280000`, `0x21b9f0000`, `0x21b9a0000`), consistent with ring-allocated
intermediate targets.

### Opcode census of the graphics stream

Every type-3 packet in a 25 s run, by opcode:

| Opcode | Meaning | Count |
|---|---|---|
| `0x10` | NOP (carries the indirect-register and flip sub-opcodes) | 9542 |
| `0x76` | SET_SH_REG | 1996 |
| `0x46` | EVENT_WRITE | 849 |
| `0x13` | INDEX_BUFFER_SIZE | 715 |
| `0x26` | INDEX_BASE | 715 |
| `0x2A` | INDEX_TYPE | 714 |
| `0x35` | DRAW_INDEX_OFFSET_2 | 470 |

Two absences matter:

- **`0x69` SET_CONTEXT_REG never appears.** The guest sets no context register
  through the direct packet at all; every context register it uses arrives
  through the NOP-wrapped indirect lists. This is why the colour-target
  registers are absent from the shadow, and it narrows the search: whatever
  names the destination must be inside those lists.
- **`0x3F` INDIRECT_BUFFER never appears.** `FALSIFIED`: the destination is not
  hidden in a chained command buffer we fail to follow. SharpEmu handles that
  opcode and we do not, which made it a live suspect; this title never emits
  one.

### Also falsified

The AGC patch-list builder was compared against the reference:
`AddIndirectPatchRegisters` there does exactly what ours does - read the count
at `cmd+4`, add, write it back, and nothing else
(`AgcExports.cs:14880-14898`). Neither implementation writes register offsets
into the list. So the `0xFFFFFFFF` offsets are not something the reference
fills in and we miss.

### Where this leaves it

Every mechanism examined so far is identical between the two emulators, yet
the reference renders this scene. The open question is unchanged and now
sharply bounded: **within the indirect register lists, which entry names an
untargeted draw's destination?** The 134-entry list whose offsets are all
`0xFFFFFFFF` is the only candidate left that carries a display-buffer-shaped
value, and its layout is still `UNKNOWN`.

Deliberately not done: guessing a base register for that list and applying it.
Section 6 already shows how a plausible-looking address match can be a
leftover, and a wrong base would silently corrupt the whole context shadow.
