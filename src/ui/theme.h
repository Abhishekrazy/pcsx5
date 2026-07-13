// Modern dark theme for pcsx5_ui — Fluent/PS5-inspired palette with
// rounded widgets, subtle gradients, and crisp typography.  Applied at
// startup before the first frame and exposes a small set of helpers used
// by the panels (badges, status pills, header bars).

#pragma once

#include "imgui.h"

namespace Ui {

// Apply the theme + tweak global style variables.  Call once after
// ImGui::CreateContext() and before the first NewFrame().
void ApplyModernTheme();

// A short title string for the app bar (e.g. "pcsx5" with a PS5-cyan dot).
void DrawAppTitleBar(const char* title, const char* subtitle);

// Draws a rounded "status pill" with text.  `color` is the fill; text is
// auto-contrasted (white).  Width is fitted to the text.
void DrawStatusPill(const char* text, ImU32 fill, ImU32 text_col = IM_COL32(255, 255, 255, 255));

// Draws a thin separator + small text label, used as a section header.
void DrawSectionHeader(const char* label);

// Push a 1px rounded-border + dark background "card" frame.
void BeginCard(const char* id, ImVec2 size = ImVec2(0, 0));
void EndCard();

// Colours exposed for callers that want to build custom widgets.
namespace Palette {
    inline constexpr ImU32 BgDeep      = IM_COL32(13, 17, 23, 255);   // page bg
    inline constexpr ImU32 BgPanel     = IM_COL32(22, 27, 34, 255);   // panel bg
    inline constexpr ImU32 BgCard      = IM_COL32(30, 36, 45, 255);   // card bg
    inline constexpr ImU32 BgCardHov   = IM_COL32(38, 45, 56, 255);
    inline constexpr ImU32 BgCardSel   = IM_COL32(45, 110, 175, 255);
    inline constexpr ImU32 Border      = IM_COL32(48, 56, 68, 255);
    inline constexpr ImU32 BorderHi    = IM_COL32(80, 90, 110, 255);
    inline constexpr ImU32 Accent      = IM_COL32(0, 168, 252, 255);  // PS5 cyan
    inline constexpr ImU32 AccentDim   = IM_COL32(0, 120, 200, 255);
    inline constexpr ImU32 AccentHov   = IM_COL32(60, 190, 255, 255);
    inline constexpr ImU32 TextPrimary = IM_COL32(230, 237, 243, 255);
    inline constexpr ImU32 TextMuted   = IM_COL32(139, 148, 158, 255);
    inline constexpr ImU32 StatusOk    = IM_COL32(63, 185, 80, 255);
    inline constexpr ImU32 StatusWarn  = IM_COL32(210, 153, 34, 255);
    inline constexpr ImU32 StatusBad   = IM_COL32(248, 81, 73, 255);
    inline constexpr ImU32 StatusIdle  = IM_COL32(110, 118, 129, 255);
    inline constexpr ImU32 StopRed     = IM_COL32(220, 60, 60, 255);
}  // namespace Palette

}  // namespace Ui
