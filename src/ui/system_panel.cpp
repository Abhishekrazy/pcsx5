#include "ui/system_panel.h"
#include "ui/theme.h"
#include "ui/i18n.h"

#include "imgui.h"

#include <cstdio>
#include <string>

namespace Ui {

void SystemPanelState::Refresh() {
    try {
        info = Sys::Snapshot();
        has_info = true;
        last_error.clear();
    } catch (...) {
        last_error = I18n::Tr("system.snapshot_failed");
    }
}

void SystemPanelState::SampleMemory() {
    mem = Sys::Memory();
    has_mem = mem.total_bytes > 0;
}

// ------------------------------------------------------------------------
// Drawing helpers
// ------------------------------------------------------------------------

namespace {

// Render a single labelled row: a dim "key" column on the left and the
// "value" text on the right (wrapped if needed).
void KeyValueRow(const char* key, const std::string& value) {
    ImGui::Columns(2, nullptr, false);
    ImGui::SetColumnWidth(0, 110);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.545f, 0.58f, 0.62f, 1.0f));
    ImGui::TextUnformatted(key);
    ImGui::PopStyleColor();
    ImGui::NextColumn();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.94f, 0.96f, 0.98f, 1.0f));
    ImGui::TextWrapped("%s", value.c_str());
    ImGui::PopStyleColor();
    ImGui::Columns(1);
}

// Render a horizontal progress bar with a label below it.  The bar
// colour is interpolated from green (low) -> amber (mid) -> red (high)
// to mirror the familiar "task manager" look.
void UsageBar(const char* label, double used_fraction,
              const std::string& used_text, const std::string& total_text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.94f, 0.96f, 0.98f, 1.0f));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::SameLine(220);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.545f, 0.58f, 0.62f, 1.0f));
    ImGui::Text("%s / %s (%.0f%%)",
                used_text.c_str(), total_text.c_str(),
                used_fraction * 100.0);
    ImGui::PopStyleColor();

    // Progress bar.
    ImVec2 bar_p   = ImGui::GetCursorScreenPos();
    float  bar_w   = ImGui::GetContentRegionAvail().x;
    float  bar_h   = 14.0f;
    float  used_w  = static_cast<float>(bar_w * (used_fraction < 0.0 ? 0.0
                                       : (used_fraction > 1.0 ? 1.0
                                                          : used_fraction)));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(bar_p, ImVec2(bar_p.x + bar_w, bar_p.y + bar_h),
                      IM_COL32(38, 45, 56, 255), 4.0f);

    // Three-stop gradient: green at the start, amber at 60%, red at 90%.
    auto lerp4 = [](const ImVec4& a, const ImVec4& b, float t) {
        return ImVec4(a.x + (b.x - a.x) * t,
                      a.y + (b.y - a.y) * t,
                      a.z + (b.z - a.z) * t,
                      a.w + (b.w - a.w) * t);
    };
    auto interp = [&](float t) -> ImU32 {
        // 0   -> green
        // 0.6 -> amber
        // 1.0 -> red
        ImU32 c0 = IM_COL32(63, 185, 80, 255);
        ImU32 c1 = IM_COL32(210, 153, 34, 255);
        ImU32 c2 = IM_COL32(248, 81, 73, 255);
        if (t < 0.6f) {
            float k = t / 0.6f;
            return ImGui::GetColorU32(lerp4(
                ImGui::ColorConvertU32ToFloat4(c0),
                ImGui::ColorConvertU32ToFloat4(c1), k));
        }
        if (t < 1.0f) {
            float k = (t - 0.6f) / 0.4f;
            return ImGui::GetColorU32(lerp4(
                ImGui::ColorConvertU32ToFloat4(c1),
                ImGui::ColorConvertU32ToFloat4(c2), k));
        }
        return c2;
    };
    // The bar is filled with a single solid colour at the current
    // fraction.  We sample the gradient at that point so the colour
    // matches the usage.
    ImU32 used_col = interp(static_cast<float>(used_fraction));
    if (used_w > 1.0f) {
        dl->AddRectFilled(bar_p, ImVec2(bar_p.x + used_w, bar_p.y + bar_h),
                          used_col, 4.0f);
    }
    // Thin highlight on top of the bar.
    dl->AddRectFilled(ImVec2(bar_p.x, bar_p.y),
                      ImVec2(bar_p.x + used_w, bar_p.y + 2),
                      IM_COL32(255, 255, 255, 36), 4.0f);

    // Reserve vertical space for the bar.
    ImGui::Dummy(ImVec2(bar_w, bar_h + 4));
}

// Big section card.  Header + body.  All the work happens through
// ImGui's regular widgets, so user copy/paste and Ctrl+click still
// work on the values.
void SectionCard(const char* title, const char* subtitle) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.118f, 0.141f, 0.176f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 14));
    ImGui::BeginChild(title, ImVec2(0, 0), true, ImGuiWindowFlags_None);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();

    // Header.
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    if (subtitle && *subtitle) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.545f, 0.58f, 0.62f, 1.0f));
        ImGui::TextUnformatted(subtitle);
        ImGui::PopStyleColor();
    }
    // Underline accent.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(ImVec2(p.x, p.y),
                      ImVec2(p.x + 32, p.y + 2),
                      IM_COL32(0, 168, 252, 255));
    ImGui::Dummy(ImVec2(0, 6));
}

void EndSectionCard() {
    ImGui::EndChild();
}

}  // namespace

// ------------------------------------------------------------------------
// Public entry point
// ------------------------------------------------------------------------

void DrawSystemPanel(SystemPanelState& s) {
    // Lazy first-time init.
    if (!s.has_info) s.Refresh();
    s.SampleMemory();

    // Header row: title + Refresh button.
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    ImGui::Text("%s", I18n::Tr("system.info"));
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.545f, 0.58f, 0.62f, 1.0f));
    ImGui::Text("%s", I18n::Tr("system.snapshot"));
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, 4));

    // Refresh button (top-right).
    const float btn_w = 100;
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - btn_w);
    if (ImGui::Button(I18n::Tr("system.refresh"), ImVec2(btn_w, 0))) {
        s.Refresh();
        s.SampleMemory();
    }

    ImGui::Dummy(ImVec2(0, 8));

    if (!s.last_error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
        char err_buf[512];
        std::snprintf(err_buf, sizeof(err_buf), I18n::Tr("system.error_prefix"), s.last_error.c_str());
        ImGui::TextWrapped("%s", err_buf);
        ImGui::PopStyleColor();
    }

    // ---- Two-column grid: OS | CPU ----
    ImGui::Columns(2, nullptr, false);
    ImGui::SetColumnWidth(0, ImGui::GetContentRegionAvail().x * 0.5f - 6);

    SectionCard(I18n::Tr("system.os"), s.info.os.version.c_str());
    KeyValueRow(I18n::Tr("system.os_name"),    s.info.os.name);
    KeyValueRow(I18n::Tr("system.os_kernel"),  s.info.os.kernel);
    KeyValueRow(I18n::Tr("system.os_arch"),    s.info.os.arch);
    EndSectionCard();

    ImGui::NextColumn();

    // Build the CPU subtitle in advance so it can be passed in the
    // header.
    char cpu_sub[64] = {0};
    if (s.info.cpu.base_ghz > 0.0) {
        std::snprintf(cpu_sub, sizeof(cpu_sub), "%.2f GHz, %dC/%dT",
                      s.info.cpu.base_ghz,
                      s.info.cpu.physical_cores,
                      s.info.cpu.logical_cores);
    } else {
        std::snprintf(cpu_sub, sizeof(cpu_sub), "%d cores",
                      s.info.cpu.logical_cores);
    }
    SectionCard(I18n::Tr("system.cpu"), cpu_sub);
    KeyValueRow(I18n::Tr("system.cpu_brand"), s.info.cpu.brand);
    char cores[48];
    std::snprintf(cores, sizeof(cores), "%d physical / %d logical",
                  s.info.cpu.physical_cores, s.info.cpu.logical_cores);
    KeyValueRow(I18n::Tr("system.cpu_cores"), cores);
    if (s.info.cpu.base_ghz > 0.0) {
        char ghz[16];
        std::snprintf(ghz, sizeof(ghz), "%.2f GHz", s.info.cpu.base_ghz);
        KeyValueRow(I18n::Tr("system.cpu_base"), ghz);
    }
    EndSectionCard();

    ImGui::Columns(1);
    ImGui::Dummy(ImVec2(0, 12));

    // ---- GPU card (full width) ----
    SectionCard(I18n::Tr("system.gpu"), s.info.gpu.name.c_str());
    KeyValueRow(I18n::Tr("system.gpu_name"),   s.info.gpu.name);
    KeyValueRow(I18n::Tr("system.gpu_vram"),   Sys::FormatBytes(s.info.gpu.vram_bytes));
    if (s.info.gpu.shared_bytes > 0)
        KeyValueRow(I18n::Tr("system.gpu_shared"), Sys::FormatBytes(s.info.gpu.shared_bytes));
    if (!s.info.gpu.driver_version.empty())
        KeyValueRow(I18n::Tr("system.gpu_driver"), s.info.gpu.driver_version);
    EndSectionCard();

    ImGui::Dummy(ImVec2(0, 12));

    // ---- Memory card (with live usage bar) ----
    SectionCard(I18n::Tr("system.memory"), s.has_mem
                            ? Sys::FormatBytes(s.mem.total_bytes).c_str()
                            : I18n::Tr("system.unknown"));
    if (s.has_mem) {
        UsageBar(I18n::Tr("system.ram"),
                 s.mem.used_fraction,
                 Sys::FormatBytes(s.mem.used_bytes),
                 Sys::FormatBytes(s.mem.total_bytes));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.545f, 0.58f, 0.62f, 1.0f));
        ImGui::TextWrapped("%s", I18n::Tr("system.memory_unavailable"));
        ImGui::PopStyleColor();
    }
    EndSectionCard();

    ImGui::Dummy(ImVec2(0, 8));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.40f, 0.43f, 0.50f, 1.0f));
    ImGui::TextWrapped(
        "%s",
        I18n::Tr("system.memory_note"));
    ImGui::PopStyleColor();
}

}  // namespace Ui
