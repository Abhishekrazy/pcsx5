# DualSenseWindows (vendored)

Windows USB-HID / Bluetooth driver library for the Sony DualSense controller,
vendored into PCSX5 as source.

## Provenance

| | |
|---|---|
| Upstream | <https://github.com/Ohjurot/DualSense-Windows> |
| Author | Ludwig Füchsl (`Ohjurot`) |
| Pinned commit | `a78fbab1fbc021115f37d297f4eb3bf393d633d6` (2025-03-10) |
| Licence | MIT — see `LICENSE`, © 2020 Ludwig Füchsl |
| Vendored | 2026-08-30 |
| Owner in PCSX5 | Input / HLE pad subsystem |

## Credit

PCSX5's DualSense support is built on **DualSense-Windows** by Ludwig Füchsl.
The library solved the DualSense HID report layouts — input reports, the
Bluetooth CRC32 requirement, and the output report format for rumble, adaptive
triggers, lightbar, player LEDs and the microphone LED — and PCSX5 uses that work
rather than re-deriving it. Thank you.

The upstream MIT licence text is preserved verbatim in `LICENSE` and must remain
with any redistribution of PCSX5.

## Licence compatibility

PCSX5 is **GPL-2.0** (see the repository root `LICENSE`). MIT is a permissive
licence compatible with GPL-2.0, so MIT-licensed source may be incorporated into
a GPL-2.0 work provided the MIT notice is retained. It is retained here.

This was checked before vendoring rather than assumed: an Apache-2.0 dependency,
for example, would **not** have been compatible with GPL-2.0 and could not have
been vendored.

## Trademarks

`TRADEMARKS.md` is upstream's notice and is preserved. "PlayStation",
"DualSense" and related marks belong to Sony Interactive Entertainment Inc.
Neither the upstream author nor PCSX5 is affiliated with Sony.

## What is vendored

Only the library itself — the public headers and its implementation:

```
include/DualSenseWindows/   DSW_Api.h  Device.h  DS5State.h  Helpers.h  IO.h
src/DualSenseWindows/       IO.cpp  DS5_Input.cpp  DS5_Output.cpp
                            DS_CRC32.cpp  Helpers.cpp  (+ private headers)
```

Upstream's Visual Studio solution, test application, LaTeX documentation and
image assets are **not** vendored — PCSX5 builds with CMake and does not need
them. The upstream layout (`include/` + `src/`) is preserved so a future diff
against upstream stays readable.

## Build

Built as a static library target `dualsense_windows` from the root
`CMakeLists.txt`, compiled with `DS5W_BUILD_LIB` so `DS5W_API` resolves to
nothing (no dllexport/dllimport). It links `hid` and `setupapi`.

Consumers define `DS5W_USE_LIB` before including `<DualSenseWindows/IO.h>`.

## Dependencies

Windows only: `Windows.h`, HID (`hid.lib`) and SetupAPI (`setupapi.lib`). No
third-party dependencies of its own.

## Update procedure

1. `git clone --depth 1 https://github.com/Ohjurot/DualSense-Windows` into a
   scratch directory and note the new commit hash.
2. Diff `include/DualSenseWindows/` and `src/DualSenseWindows/` against this
   directory.
3. Copy the changed files, keeping `LICENSE` and `TRADEMARKS.md` current.
4. Update the pinned commit and date in the table above.
5. Rebuild and re-run the controller verification before accepting the update.

Local modifications, if any ever become necessary, must be listed in the section
below so an update does not silently discard them.

## Local modifications

None. The vendored source is upstream verbatim at the pinned commit.
