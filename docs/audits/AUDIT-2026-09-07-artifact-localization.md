# AUDIT 2026-09-07 - Localizing the logo warp and the top-region blocks

This phase converts "the final image looks wrong" into a stage and an
operation. It deliberately stops at localization: no shader or post-process
code was changed.

## 1. Baseline

| | |
|---|---|
| Commit | `b3e354c`, clean tree |
| Build | `cmake --build build --config Release`, Visual Studio 18 2026, x64 |
| GPU | NVIDIA GeForce RTX 5070 Ti, driver 616.56 |
| Vulkan | device 1.4.351, instance requesting 1.2 |
| Validation | `VK_LAYER_KHRONOS_validation` forced through the loader, `VK_LAYER_VALIDATE_SYNC=1` |
| Tests | 54 of 54 |
| Validation findings | 0, and 0 throughout this phase |

## 2. Capture methodology

Two pieces of temporary instrumentation, both removed before commit:

1. **Stage identity** - one line per render target at creation, in creation
   order, with base address, dimensions, format and layout.
2. **Stage capture** - `PCSX5_PRESENT_RT_INDEX=<n>` presents the n-th render
   target in place of the display buffer, so the harness screenshots that
   stage instead of the final image.
3. **Pass graph** - one line per distinct (target, vertex shader, pixel shader)
   triple, listing the textures that draw samples.

Captures come from separate runs per stage rather than one frame, because the
present-substitution technique shows one stage per run. The title screen loops,
so several frames per stage were compared rather than one; where a stage was
inspected at more than one frame that is stated below.

The magenta border visible around a 1280x720 stage capture is an artifact of
the capture itself - a 1280x720 image blitted into the 3840x2160 present path
leaves the surrounding swapchain content untouched. It is not game content.

## 3. Stage identity

| Stage | Index | Base | Dimensions | Format | Samples | Layout |
|---|---|---|---|---|---|---|
| Display A | RT[0] | `0x2113a0000` | 3840x2160 | `R8G8B8A8_UNORM` (37) | 1 | GENERAL |
| Display B | RT[1] | `0x213380000` | 3840x2160 | `R8G8B8A8_UNORM` | 1 | GENERAL |
| Intermediate 1 | RT[2] | `0x21db80000` | 1280x720 | `R8G8B8A8_UNORM` | 1 | GENERAL |
| Intermediate 2 | RT[3] | `0x21d310000` | 1280x720 | `R8G8B8A8_UNORM` | 1 | GENERAL |
| Intermediate 3 | RT[4] | `0x21ba80000` | 1280x720 | `R8G8B8A8_UNORM` | 1 | GENERAL |

Usage on every render target: colour attachment, sampled, transfer source,
transfer destination. No multisampling anywhere - every stage is one sample,
which rules multisample resolve out of both artifacts.

Base addresses shift between runs because the guest ring-allocates them; the
creation order is stable and is what the index refers to.

## 4. The pass graph

Recorded per distinct (target, vertex shader, pixel shader):

```
Intermediate 1  <- ps 0x215601300, no source          (procedural background)
Intermediate 1  <- ps 0x215771100, src 320x512 texture (gradient)
Intermediate 2  <- ps 0x215795900, src Intermediate 1  <<< the pass in question
Intermediate 2  <- ps 0x215771100, src 250x250 texture
Intermediate 3  <- ps 0x215771100, src Intermediate 2  (plain textured quad)
Display A/B     <- ps 0x215714300, src Intermediate 3  (final composite, 4 indices)
```

`ps 0x215771100` appears three times and is the plain textured-quad shader.
**`ps 0x215795900` appears exactly once in the entire chain**, and it is the
pass that reads intermediate 1 and writes intermediate 2.

## 5. Stage results

**Source texture: CLEAN.** The 980x347 title texture dumps as a correct,
sharp "Dreaming Sarah" logo. Established in the row-stride phase and unchanged.

**Intermediate 1: CLEAN.** The logo is crisp and correctly proportioned, with
no doubling, no warp and no blocks. Checked at two frames (9 and 13) of run
`PPSA02929_20260907_215533`; the two differ in brightness, which confirms the
capture is live content and not a stale image.

**Intermediate 2: BOTH ARTIFACTS PRESENT.** The logo is warped and doubled with
exactly the character seen in the final image, and the top-region blocks are
visible in the upper band. Checked at two frames (9 and 13) of run
`PPSA02929_20260907_215619`; the warp is present in both, at different
brightness.

**Intermediate 3: unchanged from intermediate 2.** The same warp, carried
forward by a plain textured-quad pass. The top blocks were not visible in the
frame inspected, consistent with their intermittency.

**Final and presented: unchanged.** The same warp reaches the display buffers
and the screen.

## 6. Localization

**Logo doubling first appears at Intermediate 2**, produced by the draw with
pixel shader `0x215795900` sampling Intermediate 1. `VERIFIED` - the stage
before it is clean at multiple frames and the stage itself is warped at
multiple frames.

**Top-region blocks first appear at Intermediate 2**, in the same pass.
`OBSERVED` rather than `VERIFIED`: the blocks are intermittent frame to frame,
so their absence from any single capture proves nothing. They were seen at
intermediate 2 and never at intermediate 1.

Both artifacts are therefore introduced by **one operation**, not by the
texture upload, not by the render-target model, not by the final composite and
not by presentation. The decision tree's earlier branches are all excluded by
evidence rather than by assumption.

## 7. Root cause

**Not established, and deliberately not pursued.** The phase's stopping
condition applies: the first incorrect stage is localized but its cause is not
proven.

What can be said without touching the shader:

- The pass is a single-sample, same-size, same-format read of one full-screen
  image into another. Scaling, format conversion and multisample resolve are
  all excluded by the stage table.
- The shader is unique in the chain. Every other pass in the title screen uses
  a plain textured quad, which produces clean output at intermediates 1 and 3.
- A shader that warps its source is exactly what a deliberate dream-distortion
  effect looks like, so the warp being introduced here is **not** by itself
  evidence of a defect.
- The blocks are harder to attribute to intent. They are blocky, sparse and
  intermittent, and they appear in the same pass. That is suggestive of a
  translation or sampling problem in that shader rather than an artistic
  effect, but it is not proven.

## 8. Classification

**Logo warp: UNKNOWN.** Still no trustworthy reference capture of this title's
PlayStation 5 title screen. Localizing the warp to a dedicated single-use
shader makes an intentional effect *more* plausible, not less. "Different from
expectation" is not "demonstrably incorrect".

**Top-region blocks: UNKNOWN cause, localized stage.** Their intermittency and
appearance are hard to reconcile with a designed effect, but no reference and
no root cause exist yet.

## 9. Regression

| | result |
|---|---|
| Test suite | **54 of 54** |
| Render-to-texture test | passing |
| Row-stride tests | passing |
| Validation findings | **0**, unchanged throughout |
| 45 s run | `progressing`, 22 of 22 unique frames |
| Title texture | correct |
| Patch-snapshot mechanism | untouched |

No production code was changed in this phase. All instrumentation was removed;
the working tree is clean against `b3e354c`.

## 10. Remaining unknowns

- Whether the warp introduced by `ps 0x215795900` is the game's intended
  effect or a translation defect.
- Why the top-region blocks are intermittent.
- Whether the blocks and the warp share one cause inside that pass or are two
  problems that happen to share a stage.

## 11. Next boundary

Examine the translation of pixel shader `0x215795900` specifically: dump its
guest bytecode and the SPIR-V we generate for it, and compare what it does with
what the generated module does. That is a self-contained comparison against the
guest program itself and needs no reference capture, which is what makes it the
right next step for both artifacts.

Do not begin by editing the shader translator. Begin by reading what that one
shader is asked to do.
