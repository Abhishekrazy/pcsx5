# PCSX5 Technology Policy

## Baseline

| Area | Current choice | Policy |
|---|---|---|
| Emulator core | C++20 | Keep |
| Low-level C libraries | C | Keep where natural |
| Guest dispatcher | MASM x64 | Conditional, measure before keeping |
| Desktop UI | C# / .NET / WPF | Keep outside core |
| Graphics | Vulkan | Keep |
| Windowing | GLFW | Optional; avoid duplication with WPF |
| Debug UI | Dear ImGui | Debug-only/optional |
| Scripts | Lua 5.4 | Keep only for a stable scripting contract |
| JSON | nlohmann/json | Keep |
| Audio decode | LibAtrac9 | Keep if needed |
| Image/audio helpers | stb | Keep isolated |
| Video | FFmpeg | Optional backend |
| Bink2 | vendor DLL | Optional and license-gated |
| UI audio | NAudio | Re-evaluate duplicate native path |
| Native build | CMake | Keep |
| Tests | CTest | Keep |
| Guest ELF tools | clang | Keep in tooling |
| Dev tools | Python | Keep |
| Packaging | WiX / PowerShell | Keep only if release process needs it |

## Key architectural recommendation

The technology list is not the architecture.

A library is allowed to disappear if it becomes redundant.

### MASM

Current role: guest dispatcher and calling-convention bridge.

Keep it if:
- the bridge is difficult/impossible to express correctly in C++
- measured performance matters
- debugger/profiling evidence supports it

Remove it if:
- C++/intrinsics is equally correct
- performance is not materially different
- assembly creates maintenance/portability/debugging cost

### GLFW

WPF already owns the desktop window.

Do not let GLFW become a second UI/windowing stack accidentally.

Use GLFW for:
- a standalone debug window
- a future non-WPF frontend
- tests/tools that actually need it

Otherwise isolate/remove it from the desktop runtime.

### Dear ImGui

Use it for developer tooling:
- CPU state
- memory
- kernel traces
- GPU command inspection
- frame timing
- debug controls

Do not make release emulation depend on debug UI.

### FFmpeg / Bink2

Media decoding is a boundary concern.

Create:

```text
MediaDecoder
  -> FFmpegDecoder
  -> BinkDecoder
```

The emulator core should not know which decoder library is used.

Bink2 distribution must be treated as a legal/licensing decision, not only a technical dependency.

### NAudio

Avoid two competing audio architectures.

Preferred:

```text
Guest Audio
  -> Native Audio Engine
  -> WASAPI
```

Use NAudio only when it provides a concrete UI-side need.

## Dependency retirement rule

For every dependency proposed for removal, create:

`docs/adr/XXXX-remove-<dependency>.md`

The ADR must include:
- reason
- replacement
- migration
- performance impact
- compatibility impact
- removal date/commit
