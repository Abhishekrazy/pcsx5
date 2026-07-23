# PCSX5

<div align="center">

![PCSX5 Logo](https://img.shields.io/badge/PCSX5-PS5%20Emulator-00D9FF?style=for-the-badge&logo=playstation&logoColor=white)

[![Build Status](https://img.shields.io/github/actions/workflow/status/Abhishekrazy/pcsx5/build.yml?branch=main&style=flat-square&logo=github-actions&logoColor=white)](https://github.com/Abhishekrazy/pcsx5/actions)
[![License: GPL v2](https://img.shields.io/badge/License-GPL_v2-blue.svg?style=flat-square&logo=gnu&logoColor=white)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg?style=flat-square&logo=c%2B%2B&logoColor=white)](https://isocpp.org/std/the-standard)
[![Platform: Windows](https://img.shields.io/badge/Platform-Windows%2010%2F11-0078D6?style=flat-square&logo=windows&logoColor=white)](https://www.microsoft.com/windows)
[![Vulkan](https://img.shields.io/badge/Graphics-Vulkan%201.3-red?style=flat-square&logo=vulkan&logoColor=white)](https://www.vulkan.org/)
[![MSVC](https://img.shields.io/badge/Compiler-MSVC%202022%2F2026-purple?style=flat-square&logo=visual-studio&logoColor=white)](https://visualstudio.microsoft.com/)

[![Discord](https://img.shields.io/discord/1527049564983656518?label=Discord&logo=discord&logoColor=white&color=5865F2&style=flat-square)](https://discord.gg/Ks8rZTbzx)
[![GitHub Stars](https://img.shields.io/github/stars/Abhishekrazy/pcsx5?style=flat-square&logo=github&logoColor=white)](https://github.com/Abhishekrazy/pcsx5/stargazers)
[![GitHub Issues](https://img.shields.io/github/issues/Abhishekrazy/pcsx5?style=flat-square&logo=github&logoColor=white)](https://github.com/Abhishekrazy/pcsx5/issues)
[![GitHub Forks](https://img.shields.io/github/forks/Abhishekrazy/pcsx5?style=flat-square&logo=github&logoColor=white)](https://github.com/Abhishekrazy/pcsx5/network/members)

</div>

---

## 🎮 About PCSX5

**PCSX5** is an experimental **PlayStation 5 emulator** for Windows, written
in modern C++20. It focuses on High-Level Emulation (HLE) of the PS5's system
libraries and kernel, with a Vulkan-based graphics backend and a pluggable
hardware abstraction layer design.

> ⚠️ **Early Development Stage** — PCSX5 is in active development. Most
> commercial games will not boot or play correctly. This project is for
> research, education, and preservation purposes.

### ✨ Key Features

| Feature | Status | Details |
|---------|--------|---------|
| **HLE Kernel** | 🟢 Working | Thread management, memory, FD table, synchronization primitives (`sceKernelSyncOnAddressWait/Wake`, POSIX semaphores `sem_t`) |
| **HLE Libraries** | 🟢 Working | libkernel, libpad, libvideoout, libagc, libsnd, libgpu, liblibc (string/sprintf/RNG/system exports) |
| **ELF Loader** | 🟢 Working | PIE/fixed-address, dynamic linking, TLS, segment mapping |
| **CPU Emulation** | 🟢 Working | SysV variadic float ABI (XMM0-XMM7), BMI1/BMI2/ABM instruction software fallback, VEH recovery |
| **Vulkan Backend** | 🟢 Working | Graphics & compute pipelines, storage images, mipmapped samplers, 3-way scissor clipping, swapchain, VRR support (FreeSync/G-SYNC via FIFO_RELAXED) |
| **Memory Management** | 🟢 Working | Virtual memory, page protection, guest fault handling (VEH), large page support (2 MB), partial overlapping fixed mapping |
| **Audio (AAL)** | 🟢 Working | Abstracted audio layer with WASAPI / XAudio2 / waveOut / Pacing backends, auto-probe priority, per-backend volume |
| **Input (IAL)** | 🟢 Working | Abstracted input layer with GLFW keyboard / XInput / DualSense HID backends, InputMultiplexer for merging, full touch/motion/gyro |
| **VRR / Adaptive Sync** | 🟢 Working | Variable Refresh Rate support via `VK_PRESENT_MODE_FIFO_RELAXED_KHR` and `VK_PRESENT_MODE_IMMEDIATE_KHR` |
| **FSR Upscaling** | 🟡 Scaffolding | FSR 2.x/3.x integration via `ffx_fsr2_api_vk.dll` with quality presets and RCAS sharpening (full implementation requires AMD FidelityFX SDK) |
| **PS5 PKG Extraction** | 🟢 Working | PS5 PKG parser (magic 0x7F464948), fPKG AES-128-CBC decryption, PFS image mounting, `eboot.bin` extraction |
| **Diagnostics/Reports** | 🟢 Working | JSON compatibility reports, logging, memory stats |
| **Lua Scripting** | 🟢 Working | Init scripts, automation, testing |
| **VEH/TLS Fast Path** | 🟢 Optimized | Thread-local TLS cache eliminates mutex contention on every VEH trap |

### Abstraction Layers

PCSX5 uses a clean layered architecture so every subsystem can work with
any OS-supported component:

| Layer | Interface | Backends |
|-------|-----------|----------|
| **GAL** (Graphics) | `GpuDevice` | Vulkan (primary), GDI (fallback), Null (headless) |
| **AAL** (Audio) | `AudioDevice` | WASAPI, XAudio2, waveOut, SDL (future), Pacing (null) |
| **IAL** (Input) | `InputBackend` | GLFW keyboard, XInput, DualSense HID, Null |
| **PAL** (Platform) | `Platform::*` | Win32 (VirtualAlloc, threads, VEH, CPU features) |
| **VDAL** (Video) | `VideoDecoder` | Bink2 (.bik2), CRI USM (.usm), FFmpeg (H.264/H.265/VP9/AV1) |

Backends are selected at runtime via config (`gpu.backend`, `audio.backend`,
`input.backend`) and can be swapped without modifying HLE code.

---

## 📊 Game Compatibility

Track game compatibility and contribute reports at our dedicated repository:

[![Compatibility Repo](https://img.shields.io/badge/🎮_Game_Compatibility-PCSX5_Game_Compatibility-00D9FF?style=for-the-badge&logo=github&logoColor=white)](https://github.com/Abhishekrazy/PCSX5-Game-Compatibility)

- **Auto-updating statistics** via GitHub Actions
- **Structured issue templates** for compatibility reports
- **Status labels**: `status-nothing`, `status-boots`, `status-menus`, `status-ingame`, `status-playable`
- **Community-driven** testing and verification

---

## 🏗️ Building from Source

### Prerequisites

| Tool | Version | Purpose |
|------|---------|---------|
| **Visual Studio** | 2022 17.8+ / 2026 18.x | MSVC compiler, MASM assembler |
| **Windows SDK** | 10.0.19041+ | Win32 APIs, VEH/SEH, dbghelp |
| **CMake** | 3.20+ | Build configuration |
| **Vulkan SDK** | 1.3+ (optional) | GPU backend; falls back to GDI |
| **Ninja** | 1.10+ (optional) | Faster incremental builds |
| **LLVM/Clang** | 16+ (optional) | Guest test ELF compilation |

### Quick Start (Visual Studio Generator)

```powershell
# Configure
cmake -B build -G "Visual Studio 17 2022" -A x64

# Build Debug
cmake --build build --config Debug

# Build Release (recommended for testing)
cmake --build build --config Release
```

### Quick Start (Ninja - Faster Iteration)

```powershell
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Output Locations

```
build/bin/Release/pcsx5_cli.exe   # CLI frontend (dev) + pcsx5_core.dll
build/publish/pcsx5.exe           # Single-file self-contained GUI app
                                  # (emulator core hosted in-process)
```

### Running Tests

```powershell
# Build tests (enabled by default)
cmake --build build --config Debug

# Run test suite
ctest --test-dir build -C Debug --output-on-failure
```

Expected: 30+ passing tests covering ELF loading, memory management, TLS,
HLE imports, audio/pad state machines, AGC PM4, and more.

---

## 🚀 Usage

### Basic Usage

```powershell
# Launch a decrypted PS5 executable (eboot.bin or .elf)
.\build\bin\Release\pcsx5_cli.exe "C:\Path\To\Game\eboot.bin"

# With custom config
.\build\bin\Release\pcsx5_cli.exe --config config.json "C:\Path\To\Game\eboot.bin"
```

### Extract a PS5 PKG

```powershell
# Extract eboot.bin + all assets from a fake-signed PKG
.\build\bin\Release\pcsx5_cli.exe --extract-pkg "game.pkg" output_dir/
# Auto-detects PS4 vs PS5 PKG format by magic byte.
```

### Supported Formats

- **Decrypted ELF** (`.elf`, `eboot.bin`) — Primary format
- **PS5 fPKG** (`.pkg`, magic `0x7F464948`) — PFS image + eboot.bin extraction
- **PIE and fixed-address executables**
- **Dynamic linking** with HLE library stubs

### Controls & Debugging

| Key | Action |
|-----|--------|
| `F1` | Toggle debug overlay |
| `F2` | Toggle ImGui demo window |
| `F3` | Dump memory map |
| `F4` | Dump thread list |
| `F5` | Reload Lua init script |
| `F11` | Toggle fullscreen |
| `Esc` | Exit emulator |

### DualSense Controller

| Feature | USB | Bluetooth | Notes |
|---------|-----|-----------|-------|
| Buttons/Sticks/Triggers | ✅ | ✅ | Full mapping with deadzones |
| Rumble | ✅ | ✅ | Both motors via HID output |
| Adaptive Triggers | ✅ | ✅ | L2/R2 feedback/weapon/vibration |
| Touchpad | ✅ | ✅ | Up to 2 fingers |
| Gyro/Accel | ✅ | ✅ | 6-axis motion sensing |
| Lightbar RGB | ✅ | ✅ | Configurable via scePadSetLightBar |
| Player LEDs | ✅ | ✅ | Wired to user profile index |
| Mic/Speaker | ✅ | 🟡 BT fixed (audio data path incomplete) |

---

## 📁 Project Structure

```
pcsx5/
├── src/
│   ├── common/          # Logging, types, utilities, crypto
│   │   └── platform/    # Platform Abstraction Layer (PAL)
│   ├── kernel/          # Guest kernel: threads, memory, FD table, syscalls, TLS
│   ├── loader/          # ELF/SELF parsing, PKG/PS4+PS5, PFS, dynamic linking
│   ├── memory/          # Virtual memory, page protection, VEH fault handling
│   ├── hle/             # High-Level Emulation: libkernel, libpad, libvideoout, etc.
│   │   └── audio/       # Audio Abstraction Layer (AAL) backends
│   ├── gpu/             # GAL interface, Vulkan backend, shader compiler
│   │   ├── input/       # Input Abstraction Layer (IAL) backends
│   │   ├── shader/      # RDNA2→SPIR-V translator
│   │   └── vulkan/      # VulkanDevice (GAL adapter)
│   ├── media/           # Video Decoder Abstraction Layer (VDAL)
│   ├── ui/              # ImGui overlay, button layout component
│   ├── diagnostics/     # Compatibility reports, logging, statistics
│   ├── config/          # Configuration system (JSON)
│   ├── reports/         # JSON report generation
│   ├── core_api.{h,cpp} # C API seam (init/load/run/stop/shutdown) shared by
│   │                    # the WPF host and the pcsx5_cli shim
│   └── ui_csharp/       # WPF GUI (single-file pcsx5.exe, core in-process)
├── tests/               # Unit & integration tests
├── compat/              # Game-specific compatibility patches
├── assets/              # Shaders, fonts, localization
├── third_party/         # Vendored dependencies (GLFW, etc.)
├── tools/               # Build scripts, DualSense test tools, PkgToolBox
├── CMakeLists.txt       # Main build configuration
├── BUILDING.md          # Detailed build guide
├── PENDING.md           # Prioritized work queue
├── PROGRESS.md          # Development status and milestones
└── LICENSE              # GNU General Public License v2.0
```

---

## 🤝 Contributing

We welcome contributions! Please see our [Contributing Guide](CONTRIBUTING.md) for details.

### Areas Needing Help

- 🎮 **HLE Library Stubs** — Implement missing syscalls and library functions
- 🎨 **Vulkan Backend** — Pipeline caching, descriptor management, full GAL port
- 🧠 **Kernel Emulation** — Thread scheduling, synchronization primitives, signals
- 📹 **Video Decoders** — Bink 2 / CRI USM / FFmpeg integration for cutscenes
- 🎬 **VRR & FSR** — Variable refresh rate, FSR 2.x/3.x upscaling, HDR support
- 🧪 **Testing** — Expand test corpus, add regression tests
- 📚 **Documentation** — API docs, architecture guides, tutorials
- 🐛 **Bug Reports** — Test games, file detailed issues with logs

### Development Workflow

1. Fork the repository
2. Create a feature branch: `git checkout -b feature/amazing-feature`
3. Make your changes with tests
4. Run the test suite: `ctest --test-dir build -C Debug`
5. Submit a Pull Request

### Code Style

- **C++20** with modern idioms (RAII, structured bindings, concepts)
- **MSVC** `/W4 /WX` clean builds
- **clang-format** (config in `.clang-format`)
- **Doxygen** comments for public APIs

---

## 🔗 Related Projects

| Project | Description |
|---------|-------------|
| [PCSX5-Game-Compatibility](https://github.com/Abhishekrazy/PCSX5-Game-Compatibility) | Community game compatibility database |
| [shadPS4](https://github.com/shadPS4/shadPS4) | PS4 emulator (reference for HLE approach) |
| [RPCS3](https://github.com/RPCS3/rpcs3) | PS3 emulator (architecture inspiration) |
| [Vulkan](https://www.vulkan.org/) | Graphics API used by PCSX5 |

---

## 📄 License

PCSX5 is licensed under the **GNU General Public License v2.0** — see [LICENSE](LICENSE) for details.

```
PCSX5 - a PlayStation 5 emulator
Copyright (C) 2026 PCSX5 Team

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.
```

---

## 🙏 Acknowledgments

- **Khronos Group** — Vulkan API specification
- **GLFW** — Window and input management
- **Dear ImGui** — Debug UI
- **nlohmann/json** — JSON parsing
- **Lua** — Scripting engine
- **AMD** — FidelityFX Super Resolution (FSR)
- **RAD Game Tools** — Bink 2 video format
- **CRIWARE** — Sofdec2 movie format
- **shadPS4 team** — HLE architecture inspiration
- **RPCS3 team** — Emulation research and documentation
- **All contributors** — Code, testing, reports, and feedback

---

<div align="center">

**Made with ❤️ for PlayStation preservation and emulation research**

[⭐ Star this repo](https://github.com/Abhishekrazy/pcsx5) • [🐛 Report Bug](https://github.com/Abhishekrazy/pcsx5/issues/new) • [💡 Request Feature](https://github.com/Abhishekrazy/pcsx5/issues/new) • [🎮 Check Compatibility](https://github.com/Abhishekrazy/PCSX5-Game-Compatibility)

</div>
