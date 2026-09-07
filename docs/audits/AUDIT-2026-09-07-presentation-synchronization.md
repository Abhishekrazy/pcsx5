# AUDIT 2026-09-07 - Presentation-loop synchronization

## 1. Baseline

| | |
|---|---|
| Commit at start | `85ffd9c`, clean tree |
| Build | `cmake --build build --config Release`, Visual Studio 18 2026, x64 |
| GPU | NVIDIA GeForce RTX 5070 Ti, driver 616.56, device Vulkan 1.4.351 |
| Instance | Vulkan 1.2 requested, 2 extensions |
| Validation | `VK_LAYER_KHRONOS_validation` (SDK 1.4.357.0) forced through the loader, `VK_LAYER_VALIDATE_SYNC=1`; no GPU-assisted validation |
| Title | PPSA02929, `session.py run --title PPSA02929 --duration 45` |
| Tests | 54 of 54 |

Five findings reproduced on this commit, exactly as recorded in the previous
phase: `SYNC-HAZARD-WRITE-AFTER-WRITE` x10, `SYNC-HAZARD-WRITE-AFTER-READ` x10,
`VUID-vkAcquireNextImageKHR-semaphore-01779` x10,
`VUID-vkQueueSubmit-pSignalSemaphores-00067` x2,
`VUID-vkQueueSubmit-fence-00063` x1.

Runs made **with** the layers attached are much slower and sample differently.
They are used only to collect validation output; every progression number comes
from runs without them.

## 2. The lifecycle as it was implemented

Read out of `src/gpu/vk_present.cpp`, not assumed:

```
AcquireNextImageKHR(swapchain, acquire_sem)      one semaphore, shared
WaitForFences(fence)                             one fence, shared
ResetFences(fence)
BeginCommandBuffer(cmd)                          one command buffer, shared
  Barrier src   GENERAL -> TRANSFER_SRC
  Barrier target UNDEFINED -> TRANSFER_DST       srcStage = TOP_OF_PIPE
  ClearColorImage(target)
  BlitImage(src -> target)                       no barrier after the clear
  Barrier target TRANSFER_DST -> PRESENT_SRC
  Barrier src   TRANSFER_SRC -> GENERAL
EndCommandBuffer
QueueSubmit(wait acquire_sem @TRANSFER, signal done_sem, fence)
QueuePresentKHR(wait done_sem)
```

Ownership: one `acquire_sem`, one `done_sem` and one `fence` for the whole
swapchain, and a separate fence in the draw path (`vk_draw.cpp`) reset only
when a batch was already in flight.

## 3. Findings and their dependencies

**F1 - `VUID-vkAcquireNextImageKHR-semaphore-01779` (x10).**
Object: `acquire_sem`. Command: `vkAcquireNextImageKHR`.
Actual: the acquire signalled `acquire_sem` before the fence wait, so the
previous frame's submit could still have a pending wait on that same
semaphore. Expected: no pending signal or wait operations at acquire time.
Root cause: the acquire ran **before** the fence wait, and the submit that
consumes `acquire_sem` is exactly what the fence tracks.

**F2 - `VUID-vkQueueSubmit-pSignalSemaphores-00067` (x2).**
Object: `done_sem`. Command: `vkQueueSubmit`.
Actual: one shared semaphore was signalled again while a previous
`vkQueuePresentKHR` could still be waiting on it. Nothing signals when a
present's wait completes, so the render fence does not cover it.
Root cause: a per-swapchain resource modelled as a per-frame one.

**F3 - `VUID-vkQueueSubmit-fence-00063` (x1).**
Object: a fence, submitted signalled. This one is the **draw** fence, not the
present fence: `BeginBatch` reset it only inside `if (in_flight)`, so the very
first submit of a process handed `vkQueueSubmit` a fence still signalled from
creation. One occurrence per run, which matches "first submit only".

**F4 - `SYNC-HAZARD-WRITE-AFTER-WRITE` (x10).**
Resource: the swapchain image. Commands: `vkCmdClearColorImage` then
`vkCmdBlitImage`, both writing it, with nothing between them.
Root cause: a missing dependency between the letterbox clear and the blit.

**F5 - `SYNC-HAZARD-WRITE-AFTER-READ` (x10).**
Resource: the swapchain image. Commands: `vkAcquireNextImageKHR` reads it, then
`vkCmdPipelineBarrier` writes it via a layout transition. The layer's own hint
names the fix: the barrier must create an execution dependency with the stage
the acquire semaphore is waited at. The barrier used `TOP_OF_PIPE` while the
wait is at `TRANSFER`.

**Are these one defect?** Partly, and the honest answer is no. F1, F2, F4 and
F5 are all the presentation loop and share one theme - synchronization objects
and dependencies modelled per-loop rather than per-resource. F3 turned out to
live in the draw path and is unrelated to the swapchain; it was grouped with
the others only because it is a fence error. That grouping is corrected here.

## 4. The intended model

```
fence            tracks the previous submit; waited BEFORE acquiring, so
                 acquire_sem has no operation pending when it is reused
acquire_sem      one, per frame in flight (there is one frame in flight)
done_sems[i]     one per swapchain image; re-acquiring image i proves the
                 presentation engine released it and its wait has completed
first barrier    srcStageMask covers the acquire semaphore's wait, so the
                 layout transition cannot be ordered ahead of the acquire
clear -> blit    an explicit transfer-write to transfer-write dependency
draw fence       reset unconditionally before every submit
```

## 5. Fix

`src/gpu/vk_present.cpp`:

- `done_sem` becomes `done_sems`, one per swapchain image, created with the
  images and destroyed with them; the submit signals and the present waits on
  `done_sems[index]`.
- `WaitForFences` moves **above** `vkAcquireNextImageKHR`; the reset stays
  after the acquire succeeds, so an early return leaves the fence signalled,
  which is the state the next frame expects.
- A memory barrier between the letterbox clear and the blit.
- The swapchain acquire barrier's `srcStageMask` becomes `ALL_COMMANDS`, in
  all three present paths. Only one path had been changed at first, which is
  why the hazard survived the first attempt - `VkPresentFrame`,
  `VkPresentFromImage` and `VkPresentClearColor` each own a copy.

`src/gpu/vk_draw.cpp`:

- `BeginBatch` resets the batch fence unconditionally rather than only when a
  batch was in flight.

No `vkDeviceWaitIdle`, no added sleeps, no timeout changes, no suppression.

## 6. Validation

| | before | after |
|---|---|---|
| `SYNC-HAZARD-WRITE-AFTER-WRITE` | 10 | **0** |
| `SYNC-HAZARD-WRITE-AFTER-READ` | 10 | **0** |
| `VUID-vkAcquireNextImageKHR-semaphore-01779` | 10 | **0** |
| `VUID-vkQueueSubmit-pSignalSemaphores-00067` | 2 | **0** |
| `VUID-vkQueueSubmit-fence-00063` | 1 | **0** |
| **Total** | **33** | **0** |

No new findings appeared. Confirmed on a 30 s run, a 45 s run and a 60 s run;
the 60 s validated run still reported `progressing` with 29 of 29 unique
frames.

Tests: **54 of 54**, including the render-to-texture and row-stride tests.

## 7. Runtime results, and a correction to the baseline

The first post-fix batch gave 9 of 12 runs `progressing`, against the 12 of 12
recorded in earlier phases. Rather than assume variance, the pre-fix build was
rebuilt from `git stash` and measured in the same session:

| Build, measured back to back | `progressing` |
|---|---|
| Pre-fix | **2 of 6** |
| Post-fix | **9 of 12** |

So the earlier "6 of 6" baselines were taken in a more favourable machine
state, and progression classification on this machine is noisy in a way those
batches did not reveal. Measured against each other, the fix does not regress
progression; it measures better. `OBSERVED`, not `VERIFIED` - this is a rate on
a noisy classifier, not a deterministic result.

## 8. Visual comparison

Same configuration, no layers, same title screen.

- **The logo doubling is unchanged.** Post-fix frames show the same melted
  "Dreaming Sarah" with the same character of warp as the pre-fix capture.
- **The top-region blocky artifacts persist.** One post-fix frame looked clean
  and briefly suggested they were gone; a second frame from the same batch
  shows them plainly. They are intermittent frame to frame, both before and
  after. That first reading was wrong and is corrected here rather than
  reported.

## 9. Classification

**Case B.** Presentation synchronization was not demonstrated to be the cause
of the logo distortion.

Remaining logo distortion: **UNKNOWN**. There is still no trustworthy reference
capture of this title's PlayStation 5 title screen, and the synchronization
work did not change the artifact, which removes one candidate without
identifying another.

**VERIFIED**: the presentation loop was invalidly synchronized in four distinct
ways, and is not any more.
**VERIFIED**: the draw path submitted a signalled fence on its first submit.
**OBSERVED**: post-fix progression measures better than pre-fix in the same
session.
**UNKNOWN**: whether the previous synchronization errors ever affected visible
output. Nothing observable changed when they were fixed.

## 10. Testing limitation

No isolated test was added. The present loop's state machine is a sequence of
Vulkan calls against a live device and swapchain; the parts that could be
extracted as pure functions - "select the semaphore by image index" - are
trivial enough that a test would assert the implementation rather than the
contract. The regression mechanism for this work is a validation run, which is
one command:

```
VK_LOADER_LAYERS_ENABLE=VK_LAYER_KHRONOS_validation VK_LAYER_VALIDATE_SYNC=1 \
  python tools/game_runner/session.py run --title PPSA02929 --duration 45
```

Zero `Validation Error` lines in the run log is the pass condition. This is
stated rather than papered over: this phase's fix rests on validation output
and runtime comparison, not on unit tests.

## 11. Next boundary

Presentation synchronization is correct and the logo distortion is unchanged,
so this phase stops here rather than moving into shaders or post-processing.

The next task is to localize the remaining visual artifacts - the logo warp and
the intermittent top-region blocks - by capturing each stage of the chain:

```
source texture -> intermediate 1 -> intermediate 2 -> intermediate 3 -> presented image
```

and finding the first stage at which each appears. The render-target capture
technique from the row-stride phase does this directly.
