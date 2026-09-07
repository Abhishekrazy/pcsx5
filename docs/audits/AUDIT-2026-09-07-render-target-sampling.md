# AUDIT 2026-09-07 - RESOLVED: the post-splash black screen

## Baseline

| | |
|---|---|
| Commit at start | `bffe633`, clean tree |
| Build | `cmake --build build --config Release`, Visual Studio 18 2026 generator, x64 |
| Backend | Vulkan, windowed (`--embed`), GLFW window |
| Title | PPSA02929 (Dreaming Sarah), `Games/PPSA02929-app0/eboot.bin`, 7,806,888 bytes |
| Launch | `python tools/game_runner/session.py run --title PPSA02929 --duration <n>` |
| Stored baseline | `frozen`, first frame 2.02 s - recorded at revision `fd1a56f`, so **not** comparable to this build; re-measured instead |

Re-measured on `bffe633`: `frozen`, first frame 2.02 s, 5 unique frames of 29
over 60 s, longest freeze 38.3 s, process alive throughout.

Every measurement below is from `bffe633` or its direct successor. No figure
is carried across builds - an earlier audit in this series made exactly that
mistake and is corrected in `AUDIT-2026-09-07-submission-rate-collapse.md`
section 7.

## Evidence timeline

```
Splash renders correctly (one draw per frame, one display buffer)
   |
Scene change at ~16 s
   |
Frame composition changes: 1 draw per frame -> 17 draws across 4 targets
   |
Final full-screen pass samples an offscreen surface
   |
That surface reads as empty from guest memory
   |
Black screen painted over the frame
```

## Instrumentation

One structured line per flip, temporary and removed before commit: frame
index, flipped buffer, every render-target base with its draw count, and the
sampled texture with a coverage sample across its full extent. About 500 lines
per run rather than millions.

```
FRAME 6   flipbuf=0 targets=[ 0x2113a0000:1 ]                                  tex=0x215ed0000 1280x720 nz=8/8
FRAME 494 flipbuf=0 targets=[ 0x2113a0000:1 0x21ba00000:1 0x21d2b0000:6 0x21db20000:9 ] tex=0x21ba00000 1280x720 nz=0/8
```

That single pair of lines contains the whole answer: during the splash the
frame is one draw sampling a texture full of data; afterwards it is seventeen
draws across four surfaces, and the one the final pass samples is empty.

## Hypotheses

**H1 - the guest stops submitting work.**
Evidence: the window readout showed `0 draws/s`. Experiment: count command
buffers between kernel heartbeats. Result: 655 in the first 30 s, 43 in the
next; still submitting at the end of the run. Status: `FALSIFIED`.

**H2 - rendering goes to a surface that is never presented.**
Evidence: two display buffers, presentation appeared to use only one.
Experiment: log the presented buffer every frame rather than every 60th.
Result: the buffers alternate evenly and each composite targets the buffer its
own flip names; the earlier reading was a sampling artifact of a probe firing
on an even stride against a strictly alternating sequence. Status: `FALSIFIED`.

**H3 - a fence the guest waits on is never written.**
Evidence: the title called wait, DMA and fence builders. Experiment: implement
a `RELEASE_MEM` consumer and count the packets. Result: zero such packets occur
in this build's stream; the change altered nothing and was reverted rather than
left in as untested code. Status: `FALSIFIED`.

**H4 - the draws are executed but produce nothing visible.**
Evidence: all composites returned success. Experiment: log viewport, scissor,
blend and index count per draw. Result: viewport and scissor fall back to the
full target correctly, geometry is a 6-index quad, blending is off. Status:
`FALSIFIED`.

**H5 - the final pass samples a surface the GPU rendered into, which we upload
from guest memory.**
Evidence: the per-frame trace shows `0x21ba00000` both receiving a draw and
being sampled in the same frame, and reading as zero at all 8 sample points
across 3.6 MB of guest memory, while CPU-supplied textures in the same frame
read as full. Experiment: bind the render target's own image for such a
binding. Result: the scene renders. Status: **`VERIFIED`**.

## Root cause

**Subsystem**: GPU, texture binding (`src/gpu/vk_draw.cpp`).

**Faulty behaviour**: a draw sampling a guest address was always satisfied by
uploading that address from guest memory.

**Why it was wrong**: when the guest renders into a surface, the pixels exist
only in the Vulkan image we drew into. Guest memory for that address is never
written. A later pass sampling the same address therefore received an empty
texture. PPSA02929's post-splash scene is a render-to-texture chain whose final
full-screen pass does exactly this, so it painted black over a frame we had
otherwise drawn correctly.

**Correct behaviour**: a sampled binding naming a live render target binds that
target's image.

**Evidence**: the trace above, plus the result - the black screen is gone and
the scene renders moving content.

## Implementation

- `src/gpu/vk_draw.h` - `VkDrawShouldSampleRenderTarget`, the decision rule as
  a pure predicate so it can be tested without a device.
- `src/gpu/vk_draw.cpp` - the texture loop consults it and binds the render
  target's view; render targets are created and kept in `VK_IMAGE_LAYOUT_GENERAL`,
  which is valid both as a colour attachment and as a sampled source. Vulkan
  does not permit a layout transition inside an active render pass, so a
  single layout valid for both roles is what makes sampling-what-you-drew
  legal within one frame. The cost is some driver-side optimisation.
- `src/gpu/vk_present.cpp` - the present barriers move from
  `COLOR_ATTACHMENT_OPTIMAL` to `GENERAL` to match.

No other behaviour was changed.

## Regression tests

`tests/vk_draw_rt_sampling_tests.cpp` (new), wired into CTest as
`vk_draw_rt_sampling`. Four cases: a sampled binding on a live render target
uses its image; an ordinary texture still uploads from guest memory; a null
address never matches; a storage binding is left to the storage path.

The Vulkan binding path itself requires a device and is covered by runtime
evidence, not by this test. That limitation is stated rather than papered over.

## Validation

| | before | after |
|---|---|---|
| CTest | 52 of 52 | **53 of 53** |
| PPSA02929 status, 60 s | `frozen` | **`progressing`** |
| Unique frames of 29 | 5 | **29** |
| Longest freeze | 38.3 s | **6.1 s** |

Reproduced across four runs (three at 25 s, one at 60 s), all `progressing`
with every sampled frame unique. The runtime baseline was updated deliberately
from run `PPSA02929_20260907_123027`.

## Next boundary

The scene renders but is not yet correct: colours and some geometry are wrong,
and the picture shows banding and stepped edges. The title is still not
playable.

There is also an intermittent crash: an emulated thread-local-storage read
failure at guest RIP `0x800160378`, seen in two of six longer runs and present
before this change (run `PPSA02929_20260907_101044`). It is unrelated to this
fix - both runs that hit it died during the splash, before the new path was
reached - but it now bounds how long the title survives.

Recommended next step: the intermittent TLS failure, because it caps run
length and therefore caps every later measurement. Scene correctness after
that.
