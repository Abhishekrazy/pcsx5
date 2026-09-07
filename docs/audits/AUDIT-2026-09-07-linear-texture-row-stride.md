# AUDIT 2026-09-07 - Rendering correctness: linear textures read with the wrong row stride

## 1. Baseline

| | |
|---|---|
| Commit at start | `7c4a8ad`, clean tree |
| Build | `cmake --build build --config Release`, Visual Studio 18 2026, x64 |
| Backend | Vulkan, windowed, GLFW |
| Title | PPSA02929, `Games/PPSA02929-app0/eboot.bin` |
| Launch | `session.py run --title PPSA02929 --duration 45` |
| Test suite | 54 of 54 |

Runtime (`PPSA02929_20260907_171555`): `progressing`, 22 frames, 21 unique,
longest freeze 6.05 s. The scene is reached and animates. The picture shows a
wavy magenta background with a **grey streaky rectangle** where the title
should be.

Category, established rather than assumed: not black output, not stale output,
not a wrong render target, and not a presentation fault - the scene draws and
changes every frame. **Corrupted texture content.**

## 2. Evidence timeline

```
Scene renders, frames change            - the pipeline is live
Render targets: 5, all zero-seeded      - the corruption is not seeded
CPU textures: 4, all linear, tile 0     - no detiling is involved
No draws dropped, no GPU warnings       - nothing is being rejected
Sampling chain is a clean 4-stage pipe  - no self-feedback
Dump the uploaded textures              - the corruption is in the texture
Correlate with width                    - only unaligned widths are corrupt
```

## 3. Hypotheses

**H1 - render targets seeded from uninitialised guest memory.** Experiment: log
each render-target creation with a coverage sample of its seed. Result: 5
targets - two 3840x2160 display buffers and three 1280x720 intermediates - all
seeded from memory that is zero at every sample point. `FALSIFIED`.

**H2 - textures are tiled and detiled incorrectly.** Experiment: log tile mode,
pitch and format for every distinct upload. Result: every texture is `tile=0`
(linear), format 10/0 (8-bit RGBA), and no draw was dropped for an unsupported
swizzle. `FALSIFIED`.

**H3 - a draw samples the surface it is rendering into.** With render targets
now bound as images that would be undefined in Vulkan and could look like
noise. Experiment: log every distinct (target, sampled) pair. Result: a clean
four-stage chain, `0x21da90000 -> 0x21d220000 -> 0x21b980000 -> display`, with
no draw sampling its own target. `FALSIFIED`.

**H4 - the texture content in guest memory is itself wrong.** Experiment: dump
each uploaded texture and view it. Result: the 320x512 background gradient and
the 1280x720 splash are correct art; the 250x250 texture is a set of parallel
diagonals and the 980x347 one is grey streaks. `VERIFIED`.

**H5 - the row stride is wrong.** The two correct textures are 1280 and 320
texels wide, both multiples of 64; the two corrupt ones are 980 and 250, which
are not. Diagonal shear is the signature of a stride that is too small.
Experiment: log the raw descriptor words. Result: **w4 = 0 on every
descriptor**, so the pitch field is absent and the decoder substituted the
width. `VERIFIED`.

## 4. Root cause

**Subsystem**: GPU, image descriptor decode (`src/gpu/gfx10_state.cpp`) and the
linear texture upload (`src/gpu/vk_draw.cpp`).

**Faulty behaviour**: `DecodeImageDescriptor` substituted the surface width for
the pitch whenever the descriptor carried no pitch field.

**Why it was wrong**: an absent pitch does not mean the rows are tight. The
hardware pads each row of a linear surface to a 256-byte boundary. Using the
width asserts zero padding, which is only correct when the width is aligned
already. For a 250-wide 8-bit RGBA surface the real stride is 256 texels, and
reading at 250 shifts every row six texels further left than the last - one
row's worth of shear per row, exactly the diagonal pattern observed.

The upload path itself was never wrong: it already reads row by row when the
pitch exceeds the width. It was handed a pitch that claimed no padding.

**Evidence**: w4 = 0 on all seven descriptors observed; the two aligned widths
correct and the two unaligned ones corrupt; and the decisive artifact - after
the fix the 980x347 texture reads as the title logo, "Dreaming Sarah", where
before it was noise.

## 5. Exact code path

```
libagc.cpp draw walk
  -> Gfx10::DecodeImageDescriptor()      pitch substituted the width here
  -> VkDrawTexture::pitch
  -> vk_draw.cpp UploadTexture(), linear path
       pitch == width -> one contiguous read of width*height*bpe
       (the correct row-by-row branch was never taken)
```

## 6. Fix

Three edits, one logical change.

- `DecodeImageDescriptor` reports **0** for an absent pitch, meaning "not
  specified", instead of inventing the width.
- `Gfx10::LinearPitchTexels(width, bytes_per_element)` derives the padded
  stride: row bytes rounded up to 256, converted back to elements.
- The linear upload uses it when the pitch is unspecified. It is applied where
  the element size is known, which the decoder does not have.

Nothing else was touched. The render-to-texture path, the presentation path and
the patch-snapshot mechanism are unchanged.

## 7. Tests

`tests/gfx10_state_tests.cpp`:

- **Added** `TestLinearPitch`: aligned widths pass through unchanged (1280,
  320); unaligned ones pad (980 to 1024, 250 to 256); the alignment is in bytes
  so it varies with format (250 at 1, 2 and 4 bytes per texel; 200 at 16 bytes
  pads to 208); degenerate inputs pass through rather than yielding a zero
  stride.
- **Changed** one existing assertion. It read "pitch falls back to width",
  which pinned the defect: it asserted that an absent pitch means tight rows.
  It now asserts the pitch decodes as unspecified and that the consumer derives
  an aligned stride. Corrected and strictly more specific, not weakened.

## 8. Results

| | before | after |
|---|---|---|
| Test suite | 54 of 54 | **54 of 54** |
| Render-to-texture test | passing | passing |
| 45 s runs | 1 of 1 `progressing` | **6 of 6 `progressing`** |
| Unique frames per run | 21 of 22 | **22 of 22, every run** |

## 9. Before and after

Before: the title area is a grey streaky rectangle; the 980x347 source texture
dumps as noise and the 250x250 as diagonal lines.

After: the title screen reads "Dreaming Sarah" over the wavy background, and
the same two textures dump as the logo and as a clean surface.

## 10. Remaining limitations

- **The logo is still distorted** on screen - warped and doubled - although its
  source texture is now correct. The title passes through a three-stage
  post-process chain, and this game's aesthetic includes a deliberate dream
  wobble, so it may be intended. `UNKNOWN`; judging it needs a reference
  capture of the real title screen, which we do not have.
- **The 256-byte alignment is `INFERRED`, not `VERIFIED` from documentation.**
  It is the standard GCN linear row alignment and is consistent with all four
  observed textures, but only two of them discriminate between candidate
  alignments. A 128-byte alignment would also explain 250; only the 980 case
  rules it out, and it does so by producing a legible logo rather than by
  citing a specification.
- **No Vulkan validation layer runs in this configuration** - zero validation
  output appears in any run log. All GPU evidence here is behavioural.

## 11. Follow-up

- Judge the residual logo distortion against a reference capture.
- Run once under the Vulkan validation layers and classify anything reported.
- Colour accuracy has not been assessed separately; the palette looks plausible
  but nothing here measures it.
