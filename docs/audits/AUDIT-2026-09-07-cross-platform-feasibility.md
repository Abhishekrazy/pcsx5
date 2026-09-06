# AUDIT 2026-09-07 - Cross-platform feasibility (Linux / macOS)

Question from the user: how much work is an Avalonia port of the shell, what
does the rest of the emulator need to run on Linux and macOS, what does
rendering look like on macOS (Metal), and is any UI framework a better fit
than Avalonia for all three platforms.

Method: two read-only investigations on the current tree (shell inventory and
core inventory), plus a survey of the candidate UI frameworks. Every count
below is `VERIFIED` by grep on the working tree at commit 7172dcf unless
labelled otherwise; the effort figures are `INFERRED` estimates.

## 1. Where the Windows dependency actually lives

The core is the gate, not the shell. A portable shell on top of a
Windows-only core ships nothing.

| Area | Windows coupling | Label |
|---|---|---|
| Guest memory (`src/memory/`) | `VirtualAlloc`/`VirtualProtect` reserve, commit, protect and query; two vectored exception handlers implement demand-commit and guest faults | VERIFIED |
| Guest threads / TLS (`src/kernel/tls_patch.cpp`, thread.cpp) | Guest threads run on host threads with the TEB `StackBase`/`StackLimit` retargeted; TLS slots are addressed at the hard-coded TEB offset `0x1480` | VERIFIED |
| Syscall / fault dispatch | Win32 `CONTEXT` is the register-file ABI between the fault handler and the kernel; the MASM dispatcher assumes the Windows x64 calling convention | VERIFIED |
| Structured exception handling | 40 `__try` blocks in `src/**/*.cpp` | VERIFIED (grep) |
| Public headers | 8 headers under `src/*/` include `<windows.h>` directly, so it leaks into every consumer | VERIFIED (grep) |
| Periphery | WASAPI audio, XInput and DualSenseWindows HID input, Win32 window, `hid`/`setupapi` linked globally via `link_libraries()` at `CMakeLists.txt:241` | VERIFIED |
| GPU | Vulkan already; no D3D in the core | VERIFIED |

Order of work that keeps the tree buildable on Windows at every step:

1. Header hygiene: remove `<windows.h>` from public headers behind a small
   platform header. Low risk, no behaviour change.
2. Memory through a platform abstraction (`mmap`/`mprotect`/`madvise` on
   POSIX). Needs the memory tests (`tests/memory_*.cpp`) as characterization
   first (Rule 07).
3. Fault handling: `sigaction` + `sigaltstack` replacing the two VEHs; the
   `CONTEXT` ABI must become a PCSX5-owned register struct. This changes a
   subsystem boundary and is a stopping condition (Rule 10): ask first.
4. Threads, dispatcher and TLS. Linux: `arch_prctl(ARCH_SET_FS)` is public.
   macOS: there is no public API to set the fs base for a user thread, and
   the kernel reserves it. This is at least a `SOFT BOUNDARY`; if guest TLS
   cannot be hosted without fs, it is a `HARD BOUNDARY` on macOS
   (Rule 09). `UNKNOWN` until an experiment on a copy establishes it.
5. Periphery: audio (PipeWire/PulseAudio, CoreAudio), input (`hidraw`/
   `evdev`, IOKit HID), window (GLFW already vendored).
6. CMake: replace the global `link_libraries()` with per-target links and
   `if(WIN32)` guards.

Effort, `INFERRED`: Linux is a multi-week L/XL core port; macOS is XL and
carries the fs-base risk above. Apple Silicon is not a target: the guest is
x86-64 and PCSX5 has no recompiler, so only Intel Macs or Rosetta 2 apply, and
Rosetta 2 does not support the fault-driven demand-commit model reliably
(`UNKNOWN`, untested).

## 2. Rendering on macOS

The core targets Vulkan. On macOS that means MoltenVK, which requires
`VK_KHR_portability_enumeration` at instance creation and
`VK_KHR_portability_subset` at device creation, and exposes a subset of
Vulkan features. Anything the GCN-to-SPIR-V translator emits that the subset
does not cover (geometry shaders, some tessellation and image formats, wide
lines) fails at device creation, not at draw time. That is the correct
failure mode for Rule 09: enumerate the required features, record the ones
MoltenVK lacks, classify.

Writing a Metal backend is not justified while MoltenVK exists; it would be a
second GPU backend without a removal plan.

## 3. The shell

The current shell is WPF plus D3D11 through Helix Toolkit (3D pad), NAudio
(DualSense USB audio endpoints), Squirrel (updates), `MediaPlayer` (title
music), `MessageBox`, `RichTextBox`, WPF triggers and `WindowChrome`.

Avalonia port, `INFERRED`: 14-20 engineer-weeks for a Windows-equivalent
shell, plus 4-8 weeks to re-do the 3D pad on OpenGL/Vulkan (Helix is D3D11),
plus 2-4 weeks per additional OS for packaging, window chrome and input
plumbing. Known blockers: no `MessageBox`/`RichTextBox`/property triggers in
Avalonia (styles and selectors instead), Squirrel to Velopack, title music
moved into the core or a cross-platform audio package, NativeControlHost
restrictions on Wayland and macOS for embedding the emulator's window
(frame delivery as a bitmap is the fallback).

## 4. Framework survey (all three desktop platforms)

| Candidate | Fit for PCSX5 | Verdict |
|---|---|---|
| **Avalonia** | XAML-shaped, so the existing XAML, `DynamicResource` tokens and `I18n` port with the least rewriting; Skia renderer; real Linux and macOS desktop support; MIT | Best fit |
| Uno Platform | XAML too, but its desktop targets sit on Skia hosts and its strength is mobile/web; smaller desktop community, heavier toolchain | Second |
| .NET MAUI | No Linux target | Excluded |
| WinUI 3 | Windows only | Excluded |
| Qt (C++) | Excellent desktop support, but a second language for the shell, LGPL/commercial licensing considerations for a GPL-2.0 project, and a full rewrite | Only if the shell were being rewritten from scratch in C++ |
| Dear ImGui | Already vendored; trivially portable; not a product-grade shell (no accessibility tree, no native text, Rule 11 keeps it as debug tooling) | Debug shell only |
| Electron / Tauri | Web stack, large runtime or a Rust build dependency; no path for embedding the emulator window | Excluded |

Recommendation: if the port is ever started, Avalonia is the right shell
choice, and no candidate beats it for a C#/XAML codebase across Windows,
Linux and macOS. But the shell is the last step, not the first. Nothing in
section 3 should start before sections 1.1-1.3 exist and a headless
`pcsx5_cli` runs a title on Linux.

## 5. Defects found on the way (recorded in TASKS.md)

- Two audio output backends: `src/hle/libaudioout.cpp` and `src/hle/audio/`.
- XInput polled in two places: `src/gpu/vulkan_backend.cpp:121` and
  `src/gpu/input/xinput_backend.cpp`.
- `link_libraries(dualsense_windows opus hid)` at `CMakeLists.txt:241` is
  global; every target after it links HID.
- 8 public headers include `<windows.h>`.
