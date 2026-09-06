# AUDIT 2026-09-07 - Why Dreaming Sarah renders one frame here and plays in SharpEmu

Question from the user: SharpEmu reached in-game on PPSA02929 (Dreaming Sarah)
and reports 60 fps (github.com/sharpemu/sharpemu issue 9). PCSX5's baseline for
the same title is `frozen`. What are they doing that we are not?

Reference use follows the standing rule: read for understanding, implement in
PCSX5's own idiom, no copied code and no attribution.

## 1. What our own run actually shows

Run `artifacts/runtime/PPSA02929_20260906_054720` (60.11 s, status `frozen`).

The guest is **not** hung. `OBSERVED`, all from that run log:

| Observation | Evidence |
|---|---|
| The guest render loop runs continuously | `AGC dcb.graphics: walked 864 dwords - 13 draws ... 1 flips`, repeating |
| It flips ~571 times in 60 s (~9.5 Hz) | 571 `sceVideoOutAddFlipEvent` lines, one per frame |
| Audio runs the whole time | `sceAudioOutOutput #10500: handle=1 bytes=2048` at the tail |
| It composites to the display buffer | `M3: executing deferred composite -> display buffer 0x213380000 2160x1080` |
| **83% of its draws are discarded** | `270 executed, 1302 dropped since start` |
| None of the drops are the "nothing can consume this" kind | 0 occurrences of `no CB_COLOR0 target and no textures` |

So the freeze is not a stall and not a wait on input. The guest draws a frame,
we throw most of it away, and what reaches the screen never changes.

## 2. Where the draws go

`src/hle/libagc.cpp:1398-1435`. A draw takes one of three routes:

1. It names a colour target (`CB_COLOR0`) - executed (`VkDrawExecute`).
2. No target, no sampled textures - dropped, logged. Never happens for this
   title (0 log lines).
3. No target but sampled textures - parked in `st.pending_targetless`, a
   **single slot**, to be composited when the AGC flip names the scanout
   surface.

Route 3 is the whole problem. The slot holds one draw. The title issues 13
draws per frame, so each new one evicts the last and increments
`total_draws_dropped` (`libagc.cpp:1412` and `1429`). Only the final draw of a
frame survives to be composited. `VERIFIED`: the drop counter is incremented
at exactly those two sites and nowhere else, and the third drop site never
fires in this run.

## 3. What SharpEmu does differently

`sharpemu_clone/src/SharpEmu.Libs/Agc/AgcExports.cs:8519-8560`. Their
untargeted-draw handling has **four** branches, not three:

1. Colour target - normal draw.
2. **No colour target, but a texture binding whose `IsStorage` is set - the
   draw is executed immediately against that storage image**
   (`SubmitStorageTranslatedDraw`, sized from the storage target's own
   descriptor).
3. No target, no storage, but sampled textures - one
   `PendingTargetlessDraw`, exactly like ours, held for the flip.
4. Nothing consumable - discarded.

Their pending slot is single too (`AgcExports.cs:1606`), so the difference is
not slot depth. The difference is **branch 2, which PCSX5 does not have**.
A Unity PS5 renderer writes much of its frame into storage images rather than
into a bound colour target; in SharpEmu those draws execute, in PCSX5 they
fall through to the one pending slot and all but one are lost per frame.

`INFERRED`, not verified: that most of our 1302 dropped draws carry a storage
binding. The log does not record the binding kind, so this needs one
instrumentation run to confirm before any code is written (see next step).

## 4. What is already in place on our side

`is_storage` is not missing from PCSX5, only unused for dispatch:

- `VkDrawTexture::is_storage` exists (`src/gpu/vk_draw.h:64`), as does
  `VkDrawCall::texture_is_storage` (`:139`).
- The Vulkan side already honours it: usage bit (`vk_draw.cpp:451`), layout
  `VK_IMAGE_LAYOUT_GENERAL` (`:585`), and `STORAGE_IMAGE` descriptor writes
  (`:999`, `:1529`, `:1682`). A storage-image descriptor pool of 512 is
  allocated (`:1168`).

So the gap is one decision in the AGC draw walker, not new GPU machinery.

## 5. Falsified on the way

- `FALSIFIED`: "the title is waiting for controller input at a title screen".
  SharpEmu's contributor did need pad exports to get past the menu
  (their PR 16 adds keyboard-to-pad mapping), which made this the obvious
  first hypothesis. It does not hold here: PCSX5 already implements
  `scePadReadState` with a real `ScePadData` fill from XInput/keyboard
  (`src/hle/libpad.cpp:77-113`, `:197-239`), and the run shows the guest
  drawing and flipping continuously rather than polling a static screen.

- `FALSIFIED`: "the guest never submits a flip". It flips through the AGC
  command buffer (`AGC dcb.graphics: flip handle=0x4000 index=1 mode=1`), not
  through `sceVideoOutSubmitFlip`, which is why grepping for the latter
  returns nothing.

## 6. Recorded, not yet acted on

`sceVideoOutAddFlipEvent` is called once per frame (571 times) and returns 0
every time (`libvideoout`). Adding the same flip event to the same equeue
repeatedly is not something retail firmware would accept indefinitely; our
unconditional success may be masking an error the guest would otherwise
handle. `UNKNOWN` - resolving it needs the caller's use of the return value
disassembled (Rule 04). It is not the cause of the frozen frame and is
tracked separately.

## Next step

One instrumentation run, no behaviour change: log the storage/sampled binding
kind at the two drop sites, run PPSA02929 for 60 s, and count how many
dropped draws carry a storage binding. If the answer is most of them, the
change is a fourth branch in the AGC draw walker that dispatches such a draw
against its storage target. If it is few, the pending-slot depth is the wrong
suspect and this audit's section 3 is falsified in turn.
