# ADR-004: The 3D DualSense preview renders through Helix Toolkit (Direct3D 11)

Date: 2026-09-07. Status: accepted.

## Context

The Input tab's controller-testing popup shows a 3D DualSense (vendored
Sketchfab model, CC-BY-4.0) animated from the live pad. The first
implementation used WPF's own `Viewport3D`. That renderer has a fixed
lighting model with no shader stage: no normal maps, no roughness/metallic
maps, per-vertex lighting only. The model's surface detail lives in its normal
map, so the pad looked like smooth plastic whatever the textures said, and the
user asked for the normal map to be used.

## Decision

The shell renders the pad with **HelixToolkit.SharpDX.Core.Wpf 2.27.3** (MIT,
NuGet, pinned in `Pcsx5Ui.csproj`), a Direct3D 11 viewport with a shader
pipeline. Materials use Helix's Phong path with the base-colour and normal
maps and a fixed plastic specular; its PBR path did not sample the albedo map
for this model and was not pursued. The viewport hosts its surface through
WindowsFormsIntegration, so `UseWindowsForms` is enabled (those assemblies
already shipped via NAudio).

The emulator core is untouched: this is a shell-only presentation dependency
(Rule 11). The core's Vulkan renderer exists to execute guest GPU work, not to
draw the shell's furniture, and pulling a preview surface through the native
ABI would have crossed the UI/core boundary for a cosmetic feature.

## Consequences

- Owner: the shell. Update procedure: bump the pin, build, run the Input
  popup capture through the harness, check the pad renders and animates.
- Known behaviour: Helix drifts if a `Transform3D` is mutated in place every
  frame; the visualizer therefore assigns a fresh `MatrixTransform3D` per
  update for the root, every part and the touch-trail dots.
- The packaged exe grows by the Helix and SharpDX managed assemblies (no
  native DLLs beyond the system's D3D11).
- Credit added to the README and the in-app credits string.
