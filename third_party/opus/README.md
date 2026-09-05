# libopus (vendored)

| | |
|---|---|
| **Upstream** | <https://github.com/xiph/opus> |
| **Author** | Xiph.Org Foundation, Skype Limited, Octasic, Jean-Marc Valin, Timothy B. Terriberry, CSIRO, Gregory Maxwell, Mark Borgerding and others (see `AUTHORS`) |
| **Pinned commit** | `8f39f9725c9546d64c6eb8fff4d8c1b19abba1e6` |
| **Licence** | BSD-3-Clause (`COPYING`), plus the Opus patent grant in `LICENSE_PLEASE_READ.txt` |
| **GPL-2.0 compatibility** | **Compatible.** BSD-3-Clause is GPL-compatible, and Xiph states the royalty-free patent grant is compatible with the GPL, v2 and v3. The only GPLv2-licensed part of the upstream project is the separate `opusinfo` tool, which is not vendored here. |
| **Owner** | The DualSense output path, `src/gpu/dualsense_ds5w.cpp` |

## Why it is here

Exactly one reason: the DualSense Bluetooth **speaker** lane, report `0x35`,
carries a 200-byte Opus frame (48 kHz stereo, 10 ms, 160 kbps CBR). The
controller contains a real Opus decoder, so the bitstream must be bit-exact.

Writing our own encoder was assessed and rejected — see
`architecture/decisions/ADR-002-opus-for-dualsense-speaker.md`. In short, a
conformant CELT-mode encoder needs a bit-exact range coder, Opus's MDCT
windowing, 21-band energy coding, PVQ with exact combinatorial indexing, and the
bit-allocation logic the decoder independently recomputes. There is no partial
credit: 95% correct produces noise, not slightly worse audio.

Haptics over Bluetooth need **none** of this — they are raw signed PCM. Opus is
required only for the speaker.

## Build wiring

Added by the root `CMakeLists.txt` via `add_subdirectory(third_party/opus
EXCLUDE_FROM_ALL)`, built static with shared library, tests, programs, custom
modes and DRED all off.

One local build-system concern, handled in the root file rather than here:
libopus enables its own tests when `BUILD_TESTING` is set, and PCSX5 enables
CTest, so `BUILD_TESTING` is shadowed to `OFF` around the `add_subdirectory` and
restored afterwards. Without that, libopus tries to build test targets whose
sources are not vendored.

## Local modifications

**No source file has been modified.** The tree was trimmed to build inputs only:

| Removed | Size | Why |
|---|---|---|
| `.git/` | 5.2 MB | Nested repository |
| `dnn/torch/` | 14 MB | PyTorch **training** scripts; not build inputs |
| `dnn/training_tf2/` | 253 KB | TensorFlow training scripts; not build inputs |
| `training/` | — | Training tooling |
| `doc/` | 676 KB | Generated documentation |
| `tests/` | 342 KB | Upstream test suite; PCSX5 does not run it |
| `.github/`, `autogen.sh`, `autogen.bat` | — | Upstream CI and autotools bootstrap |

This took the vendored tree from 26 MB to about 5.4 MB. The C sources under
`celt/`, `silk/`, `src/`, `dnn/` and `include/` are untouched and complete.

## Update procedure

1. Clone the new revision upstream to a scratch directory.
2. Remove the directories listed under "Local modifications" above.
3. Replace `third_party/opus/` with the result, keeping this README.
4. Update the pinned commit in the table above and in the root `CMakeLists.txt`
   comment.
5. Reconfigure, build, and re-run `build/Release/dualsense_probe.exe` against
   real hardware. Opus output cannot be verified automatically — it needs a
   person to say whether they heard anything.
