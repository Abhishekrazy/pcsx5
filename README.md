# PCSX5: PlayStation 5 Translation Layer & Emulator

PCSX5 is an experimental high-performance PlayStation 5 compatibility layer and emulator for Windows x64. It translates PS5 ELF/SELF binaries into native Windows system structures using a High-Level Emulation (HLE) approach.

By bridging CPU execution conventions natively, PCSX5 avoids traditional heavy virtualization overhead, allowing direct guest execution on your x86-64 CPU while emulating PS5 system calls, graphics rendering APIs, and OS features.

---

## ⚡ Key Architectural Features

- **Direct Thread Execution**: Executes PS5 user-mode code directly on host cores, eliminating slow instruction translation.
- **Assembly-Level ABI Bridge**: Translates FreeBSD/Linux System V ABI conventions (`rdi`, `rsi`, `rdx`...) to Windows x64 ABI (`rcx`, `rdx`, `r8`...) dynamically inside [dispatcher.asm](src/hle/dispatcher.asm).
- **Vulkan & Win32 Presentation**: Emulates the PS5's modern graphics engine and outputs using high-speed Vulkan GDI blitting (`vulkan_backend.cpp`). Includes an animated legacy-style boot animation.
- **Vectored Exception Handling (VEH)**: Captures guest kernel traps (`syscall` instructions) and translates them into host stub calls transparently.
- **Dynamic SCE relocation scanner**: Resolves complex SCE custom relocations (type 1, type 8) and extracts internal offsets (like the game's actual entry point) dynamically.

---

## 🏗️ Project Structure

The project code is divided into modular subsystems under `src/`:

```
pcsx5/
├── src/
│   ├── common/         # Common types, macro alignments, and thread-safe logging
│   ├── gpu/            # Vulkan rendering backend, boot screens, and frame presenters
│   ├── hle/            # HLE library stubs (libkernel, libvideoout, libagc, libpad)
│   ├── kernel/         # Guest thread context, VEH exception dispatchers, and loaders
│   ├── loader/         # ELF segment mapping, strtab resolvers, and ELF dynamic linking
│   └── memory/         # Virtual Memory management and page protection stubs
├── CMakeLists.txt      # Cross-platform project build file
└── README.md           # Project documentation
```

---

## 🚀 Building from Source

For full host requirements, supported toolchains, and a contributor build
guide see [`BUILDING.md`](BUILDING.md).  The quick version:

### Prerequisites
- **Visual Studio 2022 17.8+ (or 2026 18.x)** with the C++ Desktop development workload
- **CMake 3.20+**
- **Windows SDK 10.0.19041+**
- **Vulkan SDK 1.3+** (optional; falls back to GDI)

### Steps

1. Configure:
   ```bash
   cmake -B build -G "Visual Studio 17 2022" -A x64
   ```
2. Build:
   ```bash
   cmake --build build --config Debug
   ```
3. Test (optional but recommended):
   ```bash
   ctest --test-dir build -C Debug --output-on-failure
   ```

The compiled executable is generated at `./build/bin/Debug/pcsx5.exe`.

---

## 🎮 Usage

Launch a decrypted PS5 executable (`eboot.bin` or `.elf` format):
```bash
./build/bin/Release/pcsx5.exe "C:\Path\To\PPSA02929-app0\eboot.bin"
```

### Controls & Presenter
PCSX5 starts with a boot animation showing color bars, active memory allocations, and registers initialization status. Once the boot loader identifies the entry point, it executes the guest program, presenting frame output to the Vulkan window container.

---

## 🤝 Contributing

Contributions to HLE syscall implementations, threading primitives, and graphics emulation are welcome!
Please review the codebase style:
- Use C++17 conventions.
- Keep system handlers locked using thread-safe structures (`std::mutex`).
- Document all added HLE NIDs according to their official hashes.

## ⚖️ License
This project is licensed under the [MIT License](LICENSE).
