# DualSense 3D model (Input tab, Testing view)

Split, decimated and exported from the Sketchfab model "Playstation 5 Dualsense"
by AHarmlessPotato, licence CC-BY-4.0. One OBJ per animatable part (body,
each face button, D-pad arm, stick, L1/R1, L2/R2, touchpad, PS and mute
buttons), 47k triangles total, plus the base-colour textures downscaled to
1024 px and the 1011 emissive map (the lightbar). `manifest.json` lists each
part with its bounding box for pivots. Normal maps are omitted (WPF Viewport3D
cannot use them). Coordinates: +X = player's right, -Y = the face, +Z = the
top edge. Regenerate with the headless Blender pipeline noted in TASKS.md.

Credit (required by the licence, keep wherever this is shared):
This work is based on "Playstation 5 Dualsense"
(https://sketchfab.com/3d-models/playstation-5-dualsense-878c1f882808477ab81c2fe86d5a3936)
by AHarmlessPotato (https://sketchfab.com/AHarmlessPotato) licensed under
CC-BY-4.0 (http://creativecommons.org/licenses/by/4.0/)

Local modifications to the upstream texture (`textures/1001_baseColor.png`),
all reproducible from the original by the round-4 texture script noted in
TASKS.md: alpha is opaque everywhere (the low-alpha regions upstream are white
body panels); the printed glyphs on the caps' inner floors are darkened and
thickened; and each face button / D-pad arm has its glyph painted onto its own
top disc, oriented through the disc's model-to-UV mapping, because the model
prints the glyph on a floor 0.03 below a clear cap where WPF's blending cannot
make it legible. The five player-indicator LEDs are not part of the model; the
shell draws them as small quads under the touchpad edge.
