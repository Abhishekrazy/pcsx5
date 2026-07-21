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

**PCSX5** is an experimental **PlayStation 5 emulator** for Windows, written in modern C++20. It focuses on High-Level Emulation (HLE) of the PS5's system libraries and kernel, with a Vulkan-based graphics backend.

> ⚠️ **Early Development Stage** — PCSX5 is in active development. Most commercial games will not boot or play correctly. This project is for research, education, and preservation purposes.

### ✨ Key Features

| Feature | Status | Details |
|---------|--------|---------|
| **HLE Kernel** | 🟡 Partial | Thread management, memory, file descriptors, synchronization primitives |
| **HLE Libraries** | 🟡 Partial | libkernel, libpad, libvideoout, libagc, libsnd, libgpu |
| **ELF Loader** | 🟢 Working | PIE/fixed-address, dynamic linking, TLS, segment mapping |
| **Vulkan Backend** | 🟡 Experimental | Command buffer recording, pipeline management, swapchain |
| **Memory Management** | 🟢 Working | Virtual memory, page protection, guest fault handling (VEH) |
| **Diagnostics/Reports** | 🟢 Working | JSON compatibility reports, logging, memory stats |
| **Lua Scripting** | 🟢 Working | Init scripts, automation, testing |

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

Expected: 8+ passing tests covering ELF loading, memory management, TLS, HLE imports, and more.

---

## 🚀 Usage

### Basic Usage

```powershell
# Launch a decrypted PS5 executable (eboot.bin or .elf)
.\build\bin\Release\pcsx5_cli.exe "C:\Path\To\Game\eboot.bin"

# With custom config
.\build\bin\Release\pcsx5_cli.exe --config config.json "C:\Path\To\Game\eboot.bin"
```

### Supported Formats

- **Decrypted ELF** (`.elf`, `eboot.bin`) — Primary format
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
| `Esc` | Exit emulator |

---

## 📁 Project Structure

```
pcsx5/
├── src/
│   ├── common/          # Logging, types, utilities
│   ├── kernel/          # Guest kernel: threads, memory, FD table, syscalls, TLS
│   ├── loader/          # ELF parsing, dynamic linking, segment mapping
│   ├── memory/          # Virtual memory, page protection, VEH fault handling
│   ├── hle/             # High-Level Emulation: libkernel, libpad, libvideoout, etc.
│   ├── gpu/             # Vulkan backend, command buffers, pipelines
│   ├── diagnostics/     # Compatibility reports, logging, statistics
│   ├── config/          # Configuration system (JSON)
│   ├── reports/         # JSON report generation
│   ├── core_api.{h,cpp}  # C API seam (init/load/run/stop/shutdown) shared by
│   │                    # the WPF host and the pcsx5_cli shim
│   ├── ui_csharp/       # WPF GUI (single-file pcsx5.exe, core in-process)
│   └── lua/             # Lua 5.4 scripting engine for init/automation
├── tests/               # Unit & integration tests
├── compat/              # Game-specific compatibility patches
├── assets/              # Shaders, fonts, localization
├── third_party/         # Vendored dependencies (GLFW, etc.)
├── tools/               # Build scripts, automation
├── CMakeLists.txt       # Main build configuration
├── BUILDING.md          # Detailed build guide
└── LICENSE              # GNU General Public License v2.0
```

---

## 🤝 Contributing

We welcome contributions! Please see our [Contributing Guide](CONTRIBUTING.md) for details.

### Areas Needing Help

- 🎮 **HLE Library Stubs** — Implement missing syscalls and library functions
- 🎨 **Vulkan Backend** — Pipeline caching, descriptor management, synchronization
- 🧠 **Kernel Emulation** — Thread scheduling, synchronization primitives, signals
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

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
```

---

## 🙏 Acknowledgments

- **Khronos Group** — Vulkan API specification
- **GLFW** — Window and input management
- **Dear ImGui** — Debug UI
- **nlohmann/json** — JSON parsing
- **Lua** — Scripting engine
- **shadPS4 team** — HLE architecture inspiration
- **RPCS3 team** — Emulation research and documentation
- **All contributors** — Code, testing, reports, and feedback

---

<div align="center">

**Made with ❤️ for PlayStation preservation and emulation research**

[⭐ Star this repo](https://github.com/Abhishekrazy/pcsx5) • [🐛 Report Bug](https://github.com/Abhishekrazy/pcsx5/issues/new) • [💡 Request Feature](https://github.com/Abhishekrazy/pcsx5/issues/new) • [🎮 Check Compatibility](https://github.com/Abhishekrazy/PCSX5-Game-Compatibility)

</div>
