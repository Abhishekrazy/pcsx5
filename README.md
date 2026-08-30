# PCSX5

[![Platform](https://img.shields.io/badge/platform-Windows%20x64-blue)](#building)
[![Language](https://img.shields.io/badge/C%2B%2B-20-blue)](#building)
[![UI](https://img.shields.io/badge/UI-.NET%209%20WPF-512BD4)](#building)
[![Graphics](https://img.shields.io/badge/graphics-Vulkan%201.3-red)](#building)
[![License](https://img.shields.io/badge/license-GPL--2.0-green)](LICENSE)

An experimental PlayStation 5 emulator for Windows x64, written in C++20 with a
Vulkan graphics backend and a .NET 9 WPF desktop shell.

---

## Important notice

PCSX5 is **not affiliated with Sony Interactive Entertainment Inc.** in any way.
"PlayStation", "PS5" and "DualSense" are trademarks of Sony Interactive
Entertainment Inc.

PCSX5 does not include, distribute or enable the download of any PlayStation
firmware, system libraries or games. You may only use software you legally own.

---

## Current status

**Early development. No game is playable.**

PCSX5 can load unmodified retail PS5 executables, run them through the boot
pipeline, spawn guest threads, translate shaders and present frames — but every
title tested so far terminates within about a minute. Expect crashes, missing
rendering and no sound in most cases.

What currently works:

- SELF/ELF loading, including PS5 `PT_SCE*` segments, relocations and PIE mapping
- Dynamic module loading, import resolution and NID linking
- Guest thread creation and teardown with correct TEB/TLS handling
- A large HLE surface: libkernel, libc, libScePad, libSceAudioOut, libSceAgc,
  libSceVideoOut and others
- GCN→SPIR-V shader translation and a Vulkan 1.3 renderer
- ATRAC9 audio decoding
- DualSense support over USB and Bluetooth (input, rumble, adaptive triggers,
  lightbar, player LEDs)
- A desktop shell with a game library, controller configuration, settings and a
  boot/memory analyzer

What does not work yet:

- No title runs to a playable state
- Multiple controllers — the kernel currently accepts a single pad
- DualSense speaker and microphone audio (USB-only on PC; see
  [Controller support](#controller-support))

## Compatibility

Tracked in [`tests/runtime_baseline.json`](tests/runtime_baseline.json), which is
generated from real runs rather than maintained by hand.

| Title | ID | Status | Sustained | Reached |
|---|---|---|---|---|
| Dreaming Sarah | PPSA02929 | Crashes | ~24 s | content load, menus, guest threads |
| ASTRO BOT | PPSA21564 | Crashes | ~35 s | guest threads |

Both terminate with `STATUS_ACCESS_VIOLATION`. The current investigation targets
are recorded in [`docs/`](docs/).

## Screenshots

The desktop shell — game library, controller setup and the boot analyzer:

> Screenshots live in the project wiki. The shell runs fullscreen, is fully
> operable from a DualSense or the keyboard, and switches its on-screen prompts
> to match whichever you last touched.

## Building

### Prerequisites

- Windows 10/11 x64
- Visual Studio 2022 or later with the C++ desktop workload
- CMake 3.20+
- .NET 9 SDK (for the WPF shell; the native core builds without it)
- Vulkan-capable GPU and drivers

### Native core and CLI

```bash
cmake -B build -S .
cmake --build build --config Release
```

Outputs:

| Artifact | Path |
|---|---|
| Emulator core | `build/bin/Release/pcsx5_core.dll` |
| Headless CLI | `build/bin/Release/pcsx5_cli.exe` |
| Boot analyzer | `build/Release/pcsx5_boot_parser.exe` |

### Desktop shell

```bash
dotnet build src/ui_csharp/Pcsx5Ui.csproj -c Release -r win-x64
```

### Tests

```bash
ctest --test-dir build -C Release --output-on-failure
```

## Running

The shell is the normal entry point. For automation and debugging, the CLI runs
a title headlessly:

```bash
build/bin/Release/pcsx5_cli.exe --headless path/to/eboot.bin
```

Useful options:

| Option | Effect |
|---|---|
| `--headless` | No window; log output only |
| `--title-id=<ID>` | Tag the session with a title ID |
| `--log-file=<path>` | Write the log to a file |
| `--log-level=<level>` | Set log verbosity |
| `--report=<path>` | Write an import/stub inventory as JSON |
| `--crash-dir=<dir>` | Where crash dumps are written |
| `--play-input=<file>` | Replay a recorded controller session |
| `--record-input=<file>` | Record a controller session |
| `--strict-imports` | Fail on unresolved imports instead of stubbing |
| `--extract-pkg <pkg> <dir>` | Extract a fake-signed PS4/PS5 PKG |

## Controller support

DualSense is supported over both USB and Bluetooth for input, rumble, adaptive
triggers, the lightbar and the player LEDs. XInput pads and the keyboard also
work, and the shell can be driven entirely from either.

The DualSense **speaker and microphone are USB-only on PC**: the controller
exposes them as a USB Audio Class device, which Windows does not enumerate when
the controller is connected over Bluetooth. Connect over USB-C to use them.

## Project layout

```
src/            emulator core — cpu, memory, kernel, hle, loader, gpu, media
src/ui_csharp/  .NET 9 WPF desktop shell
tests/          unit, integration and regression tests (CTest)
tools/          developer and reverse-engineering tooling
third_party/    vendored dependencies
docs/           architecture, task and audit records
```

## Contributing

Bug reports and pull requests are welcome. See
[CONTRIBUTING.md](CONTRIBUTING.md) and [DEVGUIDE.md](DEVGUIDE.md).

A compatibility report is far more useful with the title ID, the emulator
revision, and the log from the run.

## License

PCSX5 is released under the **GNU General Public License v2.0**. See
[LICENSE](LICENSE).

## Credits

- **[DualSense-Windows](https://github.com/Ohjurot/DualSense-Windows)** by
  Ludwig Füchsl (MIT) — DualSense HID input and output report handling. PCSX5's
  controller support is built on this work rather than re-deriving the report
  layouts. Vendored in
  [`third_party/DualSenseWindows`](third_party/DualSenseWindows).
- **[LibAtrac9](https://github.com/Thealexbarney/LibAtrac9)** by Alex Barney
  (MIT) — ATRAC9 audio decoding.
- **stb** by Sean Barrett (public domain) — image and audio decoding.
- The wider PS5 emulation and reverse-engineering community, whose public
  research made much of this possible.
