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
