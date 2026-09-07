# AUDIT 2026-09-07 - Vulkan validation, and the residual title distortion

Two questions were open after the row-stride fix: whether the remaining logo
distortion is a defect, and what the Vulkan validation layers say. This audit
answers the second and explains why the first cannot yet be answered.

## 1. Verification baseline

| | |
|---|---|
| Commit at start | `7b386ce`, clean tree |
| Build | `cmake --build build --config Release`, Visual Studio 18 2026, x64 |
| Backend | Vulkan, windowed, GLFW |
| Title | PPSA02929 |
| Test suite | 54 of 54 |
| Runtime | 6 of 6 `progressing`, 22 of 22 unique frames each |
| Validation layer | `VK_LAYER_KHRONOS_validation`, Vulkan SDK 1.4.357.0, present in the explicit-layer registry |

The layer was force-enabled through the loader (`VK_LOADER_LAYERS_ENABLE`) with
synchronization validation (`VK_LAYER_VALIDATE_SYNC=1`). The build and the game
configuration were otherwise unchanged. GPU-assisted validation was not used.

Note: runs made **with** the layers attached are slower and sample differently,
so they are used only to collect validation output. Every pass/fail number
below comes from runs without the layers.

## 2. Question 1 - the residual logo distortion

**Is there a trustworthy reference?** No. There is no reference capture of this
title's PlayStation 5 title screen in the repository, and none can be produced
here. A screenshot of another platform's build taken from the web would not be
a sound basis for judging a post-processed frame at pixel level, so none was
used.

**Status: UNCLASSIFIED.** No change was made to the rendering pipeline on
account of it.

What can be said from internal evidence, and what it does not establish:

- The source texture is now provably correct: the 980x347 upload dumps as a
  clean "Dreaming Sarah" logo.
- On screen the words are **legible** and the warp is smooth and continuous
  rather than randomly broken. It animates between frames.
- The title passes through a three-stage post-process chain, and this game's
  aesthetic is explicitly dreamlike.

A coherent animated warp is *consistent with* an intentional effect. It is not
proof of one, and no attempt was made to "fix" it on the strength of it looking
unusual. Distinguishing the two needs a reference capture.

**A separate observation, not the same thing**: frames show a narrow strip of
blocky artifacts along the **top edge** of the image
(`PPSA02929_20260907_180248`, frame 18, roughly the first 45 rows). That does
not look like a stylistic effect and is recorded as its own item rather than
folded into the distortion question.

## 3. Question 2 - validation results

### Findings, before any change

| Finding | Count | Path |
|---|---|---|
| `SYNC-HAZARD-READ-AFTER-WRITE` | 5 | render pass, title screen |
| `VUID-RuntimeSpirv-NonWritable-06340` | 5 | fragment shader, every draw |
| `VUID-RuntimeSpirv-NonWritable-06341` | 5 | vertex shader, every draw |
| `VUID-VkShaderModuleCreateInfo-pCode-08740` | 8 | every shader module |
| `VUID-VkShaderModuleCreateInfo-pCode-08737` | 8 | every shader module |
| `SYNC-HAZARD-WRITE-AFTER-WRITE` | 10 | swapchain present |
| `SYNC-HAZARD-WRITE-AFTER-READ` | 10 | swapchain present |
| `VUID-vkAcquireNextImageKHR-semaphore-01779` | 10 | swapchain present |
| `VUID-vkQueueSubmit-pSignalSemaphores-00067` | 2 | swapchain present |
| `VUID-vkQueueSubmit-fence-00063` | 1 | swapchain present |

### Fixed, each demonstrated relevant to the render path

**1. Read-after-write hazard at `vkCmdBeginRenderPass`.** The layer reported
that a render pass beginning with `loadOp LOAD` reads the colour attachment
after a layout transition wrote it, with the barrier allowing only shader
access. This was self-inflicted: when render targets moved to
`VK_IMAGE_LAYOUT_GENERAL` so a guest pass could sample the surface it drew
into, the seeding barrier kept naming only the shader stages. GENERAL covers
both roles here, so the barrier must too. `src/gpu/vk_draw.cpp` now includes
colour-attachment access and the colour-attachment-output stage. **Finding
cleared: 5 to 0.**

**2. Storage resources without `NonWritable`, in both stages.** Guest shaders
bind guest memory as storage buffers in the vertex and fragment stages. Without
`fragmentStoresAndAtomics` and `vertexPipelineStoresAndAtomics` the spec
requires a `NonWritable` decoration our generated SPIR-V does not carry, so
every pipeline was invalid. The features are now requested, each only when the
device advertises it, with a warning otherwise. Enabling them is the honest
reading: these shaders really can write. **Findings cleared: 10 to 0.**

**3. SPIR-V version above the target environment.** The translator emits
SPIR-V 1.5 while the instance requested Vulkan 1.1, which accepts at most
SPIR-V 1.3 - so every shader module was invalid, in the layer's words
"Invalid SPIR-V binary version 1.5 for target environment SPIR-V 1.3". The
instance now requests Vulkan 1.2, the version that accepts what we already
emit, and falls back to 1.1 if the loader cannot provide it. Both devices on
this machine report 1.3 or 1.4. **Findings cleared: 16 to 0.**

All three were invalid by specification, which means undefined behaviour that
drivers happened to tolerate. That is sufficient relevance to fix; none of them
is claimed to explain the distortion.

### Remaining, and why they were not fixed here

| Finding | Count |
|---|---|
| `SYNC-HAZARD-WRITE-AFTER-WRITE` | 10 |
| `SYNC-HAZARD-WRITE-AFTER-READ` | 10 |
| `VUID-vkAcquireNextImageKHR-semaphore-01779` | 10 |
| `VUID-vkQueueSubmit-pSignalSemaphores-00067` | 2 |
| `VUID-vkQueueSubmit-fence-00063` | 1 |

These are one coherent cluster in the **presentation loop**: a blit writing a
swapchain image previously written by a clear, a barrier writing an image
previously read by `vkAcquireNextImageKHR`, and semaphores and a fence reused
before the prior work completed. All name swapchain objects, none names a
render target or a guest draw.

They are **not demonstrated unrelated** to the on-screen result - a present-loop
race can produce tearing, ghosting or a stale frame, and "ghosting" is not
obviously distinct from the doubling seen around the logo. They are also not
demonstrated to cause it. They are a different subsystem from the three fixed
above, so they are recorded as a task with this evidence rather than fixed in
the same phase, which would make the measurements unattributable.

## 4. Regression results

| | before | after |
|---|---|---|
| Test suite | 54 of 54 | **54 of 54** |
| Render-to-texture test | passing | passing |
| Row-stride tests | passing | passing |
| 45 s runs | 6 of 6 `progressing` | **6 of 6 `progressing`** |
| Unique frames per run | 22 of 22 | **22 of 22, every run** |
| Title texture | correct | correct - the logo is legible on screen |
| Patch-snapshot fix | untouched | untouched; only three GPU files changed |

## 5. Remaining limitations

- The logo distortion is **unclassified**, for want of a reference capture.
- The **top-edge artifact strip** is newly observed and uninvestigated.
- The presentation-loop synchronization cluster is **known and unfixed**.
- The 256-byte row alignment remains `INFERRED`; nothing in this phase
  independently confirmed it from documentation.
- Validation was run with the standard layer and synchronization validation
  only. GPU-assisted validation was not exercised.

## 6. Recommendation for the next phase

Fix the presentation-loop synchronization cluster. It is the only remaining
validation finding, it is a single coherent defect rather than five, and it is
the one candidate that could plausibly explain visible artifacts around the
logo. Doing it alone keeps the measurement attributable, and if the doubling
changes afterwards that is itself evidence about the distortion question.

The top-edge artifact strip is the natural second item, and is cheap to
localise with the render-target capture technique used in the previous phase.
