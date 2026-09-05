# DualSense controller art (vendored)

Used by the shell's Input tab to draw a live DualSense: a body image with the
dynamic parts removed, and per-part sprites painted on top at positions taken
from a VSCView theme.

| | |
|---|---|
| **Sprites** (`sprites/`) | VSCView, `VSCView/themes/dualsense/default/ThemeAssets/` |
| **Layout** (`layout.json`) | VSCView, `themes/dualsense/default/cosmic red/All.json` — canvas 1150x850, every sprite's position and the input that drives it |
| **Body** (`body/`) | Gamepad-Asset-Pack, `Controller Asset Pack/DualSense Controller Image/Default/Templates/White/` |
| VSCView upstream | <https://github.com/Nielk1/VSCView> by Nielk1, pinned `e4ba13f8367fcd5e3bf3090e7e27c969cf2095ff`, **MIT** (`licenses/LICENSE-VSCView`) |
| Gamepad-Asset-Pack upstream | <https://github.com/AL2009man/Gamepad-Asset-Pack> by Al. Lopez (AL2009man), pinned `a6d1113b0b3b2438bbbcb08a33f9a25eb2391d7c`, **MIT** (`licenses/LICENSE-Gamepad-Asset-Pack`) |
| GPL-2.0 compatibility | MIT is compatible. Both licence texts are preserved verbatim. |
| Owner | The shell's Input tab, `src/ui_csharp/` |

## Credit

Both projects require attribution under MIT and the shell's credits screen must
name them:

> Controller artwork by **AL2009man** (Gamepad-Asset-Pack) and **Nielk1**
> (VSCView), used under the MIT licence.

## Why VSCView's copies of the sprites

The Gamepad-Asset-Pack and VSCView ship the same DualSense art: 26 of the 29
sprite files are byte-identical between them and the remaining three differ
only as re-exports. VSCView's set is the one its theme positions, so the
sprites and the layout come from one place and cannot drift apart. The body
templates exist only in the asset pack, so those come from there.

## Layout

`layout.json` is consumed directly rather than transcribed. Children are
positioned relative to their parents; the tab walks the tree accumulating
offsets. Sprites are drawn centred at their resolved position, scaled to the
stated width and height. The `input` and `inputX`/`inputY` expressions
name which controller signal shows, hides or moves each sprite; the tab maps
those names onto the core's pad state.

## A caution recorded honestly

The asset pack's author states its assets are "ripped straight from the official
source or recreated by scratch" and warns that a commercial release might fail a
platform holder's certification. The pack does not say per file which is which,
and neither does VSCView. An MIT licence on the collection does not cure
third-party rights in any ripped element. PCSX5 is a non-commercial GPL-2.0
project, which is the setting the author explicitly permits; this note exists
so a future decision to ship commercially is made with the fact in hand.

## Not vendored

The GIMP working files, the other body colours, the DevMode variants and the
DualShock 4 and prompt packs. Sizes here are small on purpose; the whole
directory is a few hundred kilobytes.
