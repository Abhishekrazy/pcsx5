# ADR-002: Opus for the DualSense Bluetooth speaker

- Status: Accepted (2026-09-05, by the repository owner)
- Date: 2026-09-05
- Supersedes: None
- Superseded By: None

## Context

DualSense haptics over Bluetooth now work (report `0x32`, raw signed PCM, no
codec). The **speaker** is a separate lane: report `0x35`, 334 bytes, carrying a
**200-byte Opus frame** — 48 kHz stereo, 10 ms, 160 kbps CBR. The arithmetic is
exact: 160 kbps x 10 ms = 1600 bits = 200 bytes, which is why the payload is a
fixed size.

There is no PCM speaker lane over Bluetooth. Opus is not a choice we are making;
it is what the device decodes.

## Current Evidence

- Haptics: `VERIFIED` working without any codec.
- Speaker lane format: `INFERRED` from the DualSenseClient implementation and
  the DS5Dongle/vDS references.
- **Over USB the speaker is an ordinary Windows audio endpoint and needs no
  codec at all.** This ADR concerns Bluetooth only.

## Decision

**Vendor libopus** (BSD-3-Clause) under `third_party/opus/`, following the
existing `LibAtrac9` pattern, and use it only to encode the fixed
48 kHz / stereo / 10 ms / 160 kbps CBR frame the protocol requires.

## Why

### Writing our own encoder is not viable

This was assessed seriously rather than dismissed. A conformant encoder for this
configuration is CELT-mode Opus, and the controller contains a real Opus
decoder, so our bitstream must be **bit-exact** or it produces noise. That
requires all of:

- a bit-exact range (arithmetic) coder;
- MDCT with Opus's specific windowing and overlap;
- 21-band energy analysis with coarse (Laplace-coded, inter/intra predicted),
  fine, and final priority bits;
- **the bit allocation logic** — the static table, interpolation, trim and boost
  — which the decoder independently recomputes, so any divergence desynchronises
  it entirely;
- PVQ with exact V(N,K) combinatorial index enumeration;
- stereo coupling, spreading/rotation, transient and TF analysis, anti-collapse,
  folding.

libopus is roughly 50,000 lines written by codec specialists over years. There is
no partial credit here: an encoder that is 95% right produces noise, not
slightly-worse audio. This is the same category as writing our own H.264
encoder, and the project's own rule against speculative reimplementation applies.

### The licence is clean

libopus is **BSD-3-Clause**, compatible with PCSX5's GPL-2.0. The Opus patent
grant is royalty-free and explicitly stated by Xiph to be compatible with the
GPL. Only the separate `opusinfo` tool is GPLv2, and we do not need it.

### It matches existing practice

`third_party/LibAtrac9` is already vendored for `sceAtrac9`, and `stb_*` headers
for images. A permissive C codec with no external dependencies is exactly the
shape of dependency this project already accepts.

## Alternatives Considered

**Reuse the existing FFmpeg integration.** Rejected. `src/media/ffmpeg_decoder.cpp`
loads FFmpeg DLLs dynamically with hand-declared struct layouts frozen to the
n7.0 ABI, and it is video decode only. Using it would mean mirroring more
encoder ABI by hand, and would make pad audio depend on DLLs the user may not
have. FFmpeg's good Opus encoder is a libopus wrapper regardless, so this adds
fragility to reach the same library indirectly.

**Write our own.** Rejected above.

**Do nothing; support the speaker over USB only.** Put to the repository owner
explicitly, since USB gives speaker and microphone with no codec at all. They
asked for audio over Bluetooth *and* USB, and for the microphone to work, so the
dependency is wanted and this alternative is declined.

## Consequences

- One new vendored dependency, encoder path only.
- Build time and binary size grow modestly; libopus is self-contained C.
- Bluetooth speaker audio becomes possible; haptics are unaffected either way.

## Compatibility Impact

None to the public ABI. The encoder is internal to the DualSense output path.

## Runtime Impact

One Opus encode per 10 ms while pad speaker audio is streaming, and only then.

## Testing Impact

Cannot be verified automatically: it needs hardware and a person to say whether
they heard sound, like the haptics work before it. `tools/dualsense_probe.cpp`
is the place for it, and it must stay out of CTest for the same reason.

## Tooling / AI Impact

`third_party/opus/README.md` must record upstream URL, pinned commit, licence,
GPL-2.0 compatibility, owner, build wiring, update procedure and any local
modifications — as the vendoring policy requires.

**Noted while checking:** `third_party/LibAtrac9` has **no README.md**, so the
existing vendored codec does not satisfy that policy. Recorded in TASKS.md.

## Migration

None. Additive.

## Rollback / Containment

The encoder sits behind the DualSense speaker path alone. Removing it disables
Bluetooth speaker audio and nothing else.
