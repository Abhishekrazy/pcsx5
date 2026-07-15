#include "ui/theme.h"

#include "imgui.h"
#include "imgui_internal.h"  // ImGui::GetWindowDrawList

#include <cstdio>
#include <cstring>

namespace Ui {

void ApplyModernTheme() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* c = style.Colors;

    // ---- Base palette (Premium Glassmorphism & PS5 Cyan Accent) ----
    c[ImGuiCol_Text]                  = ImVec4(0.95f, 0.96f, 0.98f, 1.00f);
    c[ImGuiCol_TextDisabled]          = ImVec4(0.55f, 0.60f, 0.68f, 1.00f);
    c[ImGuiCol_WindowBg]              = ImVec4(0.04f, 0.06f, 0.10f, 0.35f); // Highly transparent window base
    c[ImGuiCol_ChildBg]               = ImVec4(1.00f, 1.00f, 1.00f, 0.04f); // Frosty glass-like fill
    c[ImGuiCol_PopupBg]               = ImVec4(0.06f, 0.08f, 0.14f, 0.95f);
    c[ImGuiCol_Border]                = ImVec4(1.00f, 1.00f, 1.00f, 0.12f); // Glass border highlight
    c[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_FrameBg]               = ImVec4(1.00f, 1.00f, 1.00f, 0.05f); // Frosted frame bg
    c[ImGuiCol_FrameBgHovered]        = ImVec4(1.00f, 1.00f, 1.00f, 0.12f);
    c[ImGuiCol_FrameBgActive]         = ImVec4(0.00f, 0.66f, 0.99f, 0.25f);
    c[ImGuiCol_TitleBg]               = ImVec4(0.04f, 0.06f, 0.10f, 0.50f);
    c[ImGuiCol_TitleBgActive]         = ImVec4(0.04f, 0.06f, 0.10f, 0.60f);
    c[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.00f, 0.00f, 0.00f, 0.40f);
    c[ImGuiCol_MenuBarBg]             = ImVec4(1.00f, 1.00f, 1.00f, 0.03f);
    c[ImGuiCol_ScrollbarBg]           = ImVec4(0.00f, 0.00f, 0.00f, 0.15f);
    c[ImGuiCol_ScrollbarGrab]         = ImVec4(1.00f, 1.00f, 1.00f, 0.15f);
    c[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(1.00f, 1.00f, 1.00f, 0.25f);
    c[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.00f, 0.66f, 0.99f, 0.50f);
    c[ImGuiCol_CheckMark]             = ImVec4(0.00f, 0.66f, 0.99f, 1.00f);
    c[ImGuiCol_SliderGrab]            = ImVec4(0.00f, 0.66f, 0.99f, 1.00f);
    c[ImGuiCol_SliderGrabActive]      = ImVec4(0.25f, 0.75f, 1.00f, 1.00f);
    c[ImGuiCol_Button]                = ImVec4(1.00f, 1.00f, 1.00f, 0.06f);
    c[ImGuiCol_ButtonHovered]         = ImVec4(0.00f, 0.66f, 0.99f, 0.40f);
    c[ImGuiCol_ButtonActive]          = ImVec4(0.00f, 0.66f, 0.99f, 0.65f);
    c[ImGuiCol_Header]                = ImVec4(1.00f, 1.00f, 1.00f, 0.08f);
    c[ImGuiCol_HeaderHovered]         = ImVec4(0.00f, 0.66f, 0.99f, 0.30f);
    c[ImGuiCol_HeaderActive]          = ImVec4(0.00f, 0.66f, 0.99f, 0.50f);
    c[ImGuiCol_Separator]             = ImVec4(1.00f, 1.00f, 1.00f, 0.10f);
    c[ImGuiCol_SeparatorHovered]      = ImVec4(0.00f, 0.66f, 0.99f, 0.50f);
    c[ImGuiCol_SeparatorActive]       = ImVec4(0.00f, 0.66f, 0.99f, 1.00f);
    c[ImGuiCol_ResizeGrip]            = ImVec4(1.00f, 1.00f, 1.00f, 0.05f);
    c[ImGuiCol_ResizeGripHovered]     = ImVec4(1.00f, 1.00f, 1.00f, 0.15f);
    c[ImGuiCol_ResizeGripActive]      = ImVec4(0.00f, 0.66f, 0.99f, 0.75f);
    c[ImGuiCol_Tab]                   = ImVec4(1.00f, 1.00f, 1.00f, 0.04f);
    c[ImGuiCol_TabHovered]            = ImVec4(0.00f, 0.66f, 0.99f, 0.45f);
    c[ImGuiCol_TabActive]             = ImVec4(1.00f, 1.00f, 1.00f, 0.10f);
    c[ImGuiCol_TabUnfocused]          = ImVec4(1.00f, 1.00f, 1.00f, 0.02f);
    c[ImGuiCol_TabUnfocusedActive]    = ImVec4(1.00f, 1.00f, 1.00f, 0.06f);
    c[ImGuiCol_PlotLines]             = ImVec4(0.60f, 0.65f, 0.70f, 1.00f);
    c[ImGuiCol_PlotLinesHovered]      = ImVec4(1.00f, 0.45f, 0.30f, 1.00f);
    c[ImGuiCol_PlotHistogram]         = ImVec4(0.00f, 0.66f, 0.99f, 1.00f);
    c[ImGuiCol_PlotHistogramHovered]  = ImVec4(0.25f, 0.75f, 1.00f, 1.00f);
    c[ImGuiCol_TableHeaderBg]         = ImVec4(1.00f, 1.00f, 1.00f, 0.06f);
    c[ImGuiCol_TableBorderStrong]     = ImVec4(1.00f, 1.00f, 1.00f, 0.15f);
    c[ImGuiCol_TableBorderLight]      = ImVec4(1.00f, 1.00f, 1.00f, 0.08f);
    c[ImGuiCol_TableRowBg]            = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_TableRowBgAlt]         = ImVec4(1.00f, 1.00f, 1.00f, 0.03f);
    c[ImGuiCol_TextSelectedBg]        = ImVec4(0.00f, 0.66f, 0.99f, 0.35f);
    c[ImGuiCol_DragDropTarget]        = ImVec4(0.00f, 0.66f, 0.99f, 0.90f);
    c[ImGuiCol_NavHighlight]          = ImVec4(0.00f, 0.66f, 0.99f, 0.80f);
    c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.02f, 0.03f, 0.05f, 0.70f);

    // ---- Style metrics: rounded, snug, generous padding ----
    style.WindowPadding     = ImVec2(14, 14);
    style.FramePadding      = ImVec2(10,  6);
    style.CellPadding       = ImVec2(10,  6);
    style.ItemSpacing       = ImVec2(10,  8);
    style.ItemInnerSpacing  = ImVec2( 8,  4);
    style.TouchExtraPadding = ImVec2( 0,  0);
    style.IndentSpacing     = 22;
    style.ScrollbarSize     = 12;
    style.GrabMinSize       = 10;

    style.WindowBorderSize  = 1;
    style.ChildBorderSize   = 1;
    style.PopupBorderSize   = 1;
    style.FrameBorderSize   = 0;
    style.TabBorderSize     = 0;

    // The big one: rounded everything.
    style.WindowRounding    = 10;
    style.ChildRounding     =  8;
    style.FrameRounding     =  6;
    style.PopupRounding     =  8;
    style.ScrollbarRounding =  8;
    style.GrabRounding      =  6;
    style.TabRounding       =  6;

    style.WindowTitleAlign  = ImVec2(0.5f, 0.5f);
    style.ButtonTextAlign   = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign = ImVec2(0.0f, 0.5f);

    // Slight scale bump for the standard font — 1.0 keeps the bundled
    // size but reads cleaner on HiDPI displays at 1280x800.
    // (The font is loaded by ImGui_ImplGlfw; we don't change the size
    // here to avoid surprises with text clipping in widgets.)
}

void DrawAppTitleBar(const char* title, const char* subtitle) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p  = ImGui::GetCursorScreenPos();
    const float  w  = ImGui::GetContentRegionAvail().x;
    const float  h  = ImGui::GetTextLineHeight() + 14;

    // Background gradient: dark blue -> PS5 cyan on the right.
    const ImU32 col_l = IM_COL32(22, 27, 34, 255);
    const ImU32 col_r = IM_COL32(0, 90, 140, 255);
    dl->AddRectFilledMultiColor(p, ImVec2(p.x + w, p.y + h),
                                col_l, col_r, col_r, col_l);

    // Accent bar.
    dl->AddRectFilled(ImVec2(p.x, p.y + h - 2),
                      ImVec2(p.x + w, p.y + h),
                      IM_COL32(0, 168, 252, 255));

    // Title text (slightly larger).
    ImGui::SetCursorScreenPos(ImVec2(p.x + 16, p.y + 6));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();

    if (subtitle && *subtitle) {
        const ImVec2 title_size = ImGui::CalcTextSize(title);
        ImGui::SetCursorScreenPos(ImVec2(p.x + 18 + title_size.x + 12,
                                         p.y + 8));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.78f, 0.85f, 0.95f, 1.0f));
        ImGui::TextUnformatted(subtitle);
        ImGui::PopStyleColor();
    }

    ImGui::Dummy(ImVec2(w, h));
}

void DrawStatusPill(const char* text, ImU32 fill, ImU32 text_col) {
    ImDrawList* dl  = ImGui::GetWindowDrawList();
    const ImVec2 p  = ImGui::GetCursorScreenPos();
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const float  pad_x = 10.0f;
    const float  pad_y = 4.0f;
    const float  w  = ts.x + pad_x * 2.0f;
    const float  h  = ts.y + pad_y * 2.0f;
    const float  r  = h * 0.5f;

    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), fill, r);
    // Subtle highlight at top
    dl->AddRectFilled(ImVec2(p.x + 1, p.y + 1),
                      ImVec2(p.x + w - 1, p.y + 2),
                      IM_COL32(255, 255, 255, 28), r);

    ImU32 tc = text_col;
    // Auto-contrast: if the user didn't override, use white unless the
    // fill is very light (the warn/bad pills look better dark on those).
    if (text_col == IM_COL32(255, 255, 255, 255)) {
        const ImVec4 fv = ImGui::ColorConvertU32ToFloat4(fill);
        const float lum = 0.2126f * fv.x + 0.7152f * fv.y + 0.0722f * fv.z;
        if (lum > 0.65f) tc = IM_COL32(20, 24, 30, 255);
    }
    dl->AddText(ImVec2(p.x + pad_x, p.y + pad_y), tc, text);

    ImGui::Dummy(ImVec2(w, h));
    ImGui::SameLine();
}

void DrawSectionHeader(const char* label) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float  w = ImGui::GetContentRegionAvail().x;
    const float  h = ImGui::GetTextLineHeight() + 2;

    dl->AddText(ImVec2(p.x, p.y + 2),
                IM_COL32(139, 148, 158, 255), label);

    // underline accent
    dl->AddRectFilled(ImVec2(p.x, p.y + h),
                      ImVec2(p.x + 28, p.y + h + 2),
                      IM_COL32(0, 168, 252, 255));

    ImGui::Dummy(ImVec2(w, h + 6));
}

void BeginCard(const char* id, ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.118f, 0.141f, 0.176f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 12));
    ImGui::BeginChild(id, size, true, ImGuiWindowFlags_None);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
}

void EndCard() {
    ImGui::EndChild();
}

}  // namespace Ui
