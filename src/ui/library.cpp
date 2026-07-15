// Game library panel — SharpEmu-style grid.
//
// Compact square (1:1) cards laid out in a responsive grid (4-7 per
// row, depending on window width).  Each card is a clean rounded
// rectangle with the cover art on top and a small title / title-id
// strip below.  When no cover is available a dark grey tile with the
// first letter of the title id is shown in its place (matching the
// SharpEmu placeholder look).

#include "ui/library.h"
#include "ui/theme.h"
#include "ui/thumbnail.h"
#include "compat/compat.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace Ui {

// colour coding for Compat::Status values.  We only use a small palette
// (green/amber/grey) — SharpEmu's approach is to keep the status visual
// quiet so the cover art can dominate.
[[maybe_unused]] static ImU32 StatusColor(const std::string& s) {
    if (s == "complete") return IM_COL32(80, 200, 100, 255);
    if (s == "playable") return IM_COL32(80, 200, 100, 255);
    if (s == "menu")     return IM_COL32(220, 165, 60, 255);
    if (s == "intro")    return IM_COL32(220, 130, 60, 255);
    if (s == "untested") return IM_COL32(110, 120, 135, 255);
    return IM_COL32(200, 80, 80, 255);
}

static const char* StatusLabel(const std::string& s) {
    if (s == "complete") return "Complete";
    if (s == "playable") return "Playable";
    if (s == "menu")     return "Menu";
    if (s == "intro")    return "Intro";
    if (s == "untested") return "Untested";
    if (s == "missing")  return "Missing";
    return "Unknown";
}
// Suppress C4505 ("unreferenced function with internal linkage has been
// removed") under /WX — the function is intentionally available for
// future use but the current UI shows the status only as a colour dot.
[[maybe_unused]] static auto kKeepStatusLabelAlive = &StatusLabel;

static std::string FormatSize(std::uint64_t bytes) {
    if (bytes == 0) return "-";
    char buf[32];
    constexpr double KB = 1024.0;
    constexpr double MB = KB * 1024.0;
    constexpr double GB = MB * 1024.0;
    if (bytes >= static_cast<std::uint64_t>(GB))
        std::snprintf(buf, sizeof(buf), "%.1f GiB",
                      static_cast<double>(bytes) / GB);
    else if (bytes >= static_cast<std::uint64_t>(MB))
        std::snprintf(buf, sizeof(buf), "%.1f MiB",
                      static_cast<double>(bytes) / MB);
    else if (bytes >= static_cast<std::uint64_t>(KB))
        std::snprintf(buf, sizeof(buf), "%.0f KiB",
                      static_cast<double>(bytes) / KB);
    else
        std::snprintf(buf, sizeof(buf), "%llu B",
                      static_cast<unsigned long long>(bytes));
    return std::string(buf);
}

// Renders a clean PS5-themed placeholder tile for games whose cover
// art is missing.  Layout (top to bottom):
//   1. subtle dark navy gradient
//   2. soft cyan "halo" near the top
//   3. a stylised concentric-ring motif (matches the auto-generated
//      cover art in ./Covers/ so missing and real covers feel like
//      part of the same family)
//   4. a tiny cyan dot at the centre
//   5. the full title id in clean white text near the bottom, with a
//      graceful fallback to the first letter if the id does not fit.
//
// The first-letter fallback uses ImGui's native text rendering, so it
// is always crisp regardless of the cover's pixel size.
static void DrawPlaceholder(ImDrawList* dl, const ImVec2& p,
                            const ImVec2& sz,
                            const std::string& title_id) {
    // 1) Subtle two-stop gradient.  We use top=mid for both corners
    //    (a slightly bluer tone) so it does not look like a hard top
    //    band when the cards are small.
    const ImU32 top = IM_COL32(40, 50, 64, 255);
    const ImU32 bot = IM_COL32(20, 26, 36, 255);
    dl->AddRectFilledMultiColor(p, ImVec2(p.x + sz.x, p.y + sz.y),
                                top, top, bot, bot);

    // 2) Cyan halo (matches generated covers).
    const float halo_r = std::min(sz.x, sz.y) * 0.65f;
    const float halo_y = p.y + sz.y * 0.18f;
    const ImU32 halo_col = IM_COL32(0, 168, 252, 36);
    dl->AddCircleFilled(ImVec2(p.x + sz.x * 0.5f, halo_y), halo_r, halo_col, 32);

    // 3) Concentric rings (the disc motif from the cover art).
    const float cx = p.x + sz.x * 0.5f;
    const float cy = p.y + sz.y * 0.40f;
    const float min_dim = std::min(sz.x, sz.y);
    const float r_outer = min_dim * 0.28f;
    dl->AddCircle(ImVec2(cx, cy), r_outer,
                  IM_COL32(0, 168, 252, 220), 32, 2.0f);
    dl->AddCircle(ImVec2(cx, cy), r_outer * 0.78f,
                  IM_COL32(0, 168, 252, 110), 32, 1.2f);
    dl->AddCircleFilled(ImVec2(cx, cy), 3.0f, IM_COL32(255, 255, 255, 230));

    // 4) Title id text.  Prefer to show the full id; fall back to the
    //    first letter if the cover is too small to fit it.
    const float pad = 8.0f;
    const float max_text_w = sz.x - pad * 2.0f;
    const std::string& id  = title_id;
    const std::string  one = id.empty() ? std::string("?")
                                        : std::string(1, id[0]);
    ImVec2 id_ts  = ImGui::CalcTextSize(id.c_str());
    ImVec2 one_ts = ImGui::CalcTextSize(one.c_str());
    if (id_ts.x <= max_text_w) {
        const ImVec2 pos(cx - id_ts.x * 0.5f,
                         p.y + sz.y - id_ts.y - 12.0f);
        dl->AddText(pos, IM_COL32(232, 238, 244, 235), id.c_str());
    } else if (one_ts.x * id.size() <= max_text_w) {
        // Spaced-out letters: P P S A 2 3 8 8 5
        const float char_w = one_ts.x + 2.0f;
        const float total_w = char_w * static_cast<float>(id.size()) - 2.0f;
        float x = cx - total_w * 0.5f;
        for (size_t k = 0; k < id.size(); ++k) {
            char buf[2] = { id[k], '\0' };
            ImVec2 ts = ImGui::CalcTextSize(buf);
            dl->AddText(ImVec2(x, p.y + sz.y - one_ts.y - 12.0f),
                        IM_COL32(232, 238, 244, 200), buf);
            x += char_w;
        }
    } else {
        // Last resort: single big letter, like the previous design.
        const ImVec2 pos(cx - one_ts.x * 0.5f,
                         p.y + sz.y * 0.78f - one_ts.y * 0.5f);
        dl->AddText(pos, IM_COL32(220, 230, 240, 220), one.c_str());
    }

    // 5) Soft border to match the cover's outer ring.
    dl->AddRect(p, ImVec2(p.x + sz.x, p.y + sz.y),
                IM_COL32(56, 64, 76, 255), 8.0f, 0, 1.0f);
}

bool DrawLibraryPanel(const std::vector<GameEntry>& games,
                      int& selected_index,
                      ThumbnailCache& thumbs,
                      float card_min_w,
                      const char* filter_text) {
    (void)card_min_w;
    bool boot_requested = false;

    // ---- Filter + sort ----
    std::string needle = filter_text ? filter_text : "";
    auto tolower_c = [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    };
    auto matches = [&](const GameEntry& g) {
        if (needle.empty()) return true;
        std::string lo_title = g.title_id, lo_disp = g.display_name, lo_need = needle;
        for (auto& c : lo_title) c = tolower_c(c);
        for (auto& c : lo_disp)  c = tolower_c(c);
        for (auto& c : lo_need)  c = tolower_c(c);
        return lo_title.find(lo_need) != std::string::npos ||
               lo_disp.find(lo_need)  != std::string::npos;
    };

    std::vector<int> idx;
    idx.reserve(games.size());
    for (int i = 0; i < static_cast<int>(games.size()); ++i) {
        if (matches(games[i])) idx.push_back(i);
    }
    std::sort(idx.begin(), idx.end(),
              [&](int a, int b) { return games[a].title_id < games[b].title_id; });

    if (games.empty()) {
        ImGui::Dummy(ImVec2(0, 24));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.545f, 0.58f, 0.62f, 1.0f));
        ImGui::TextWrapped(
            "No games found.\n\nPlace a PS5 PKG extract under\n"
            "./Games/<TITLE_ID>-app0/eboot.bin(.esbak)");
        ImGui::PopStyleColor();
        return false;
    }
    if (idx.empty()) {
        ImGui::Dummy(ImVec2(0, 24));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.545f, 0.58f, 0.62f, 1.0f));
        ImGui::Text("No titles match \"%s\".", needle.c_str());
        ImGui::PopStyleColor();
        return false;
    }

    // ---- Card shelf (horizontal scrolling list) ----
    const float card_w   = 130.0f;
    const float cover_h  = 160.0f;          // vertical cover art
    const float text_h   = 40.0f;           // space for title text below
    const float gap      = 18.0f;

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    if (ImGui::BeginChild("library_shelf", ImVec2(0, cover_h + text_h + 24.0f), false, ImGuiWindowFlags_HorizontalScrollbar)) {
        ImDrawList* dl_grid = ImGui::GetWindowDrawList();

        for (size_t k = 0; k < idx.size(); ++k) {
            const int  i   = idx[k];
            const auto& g  = games[i];
            std::string name   = g.display_name;
            if (name.empty()) name = g.title_id;

            const bool sel = (selected_index == i);
            ImGui::PushID(i);

            if (k > 0) {
                ImGui::SameLine(0, gap);
            }

            ImGui::BeginGroup();
            
            const ImVec2 p  = ImGui::GetCursorScreenPos();
            const ImVec2 sz(card_w, cover_h); // Only the cover art card gets hovered/selected effects
            const bool hovered = ImGui::IsMouseHoveringRect(p, ImVec2(p.x + sz.x, p.y + sz.y));
            
            if (sel && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
                ImGui::SetScrollHereX(0.5f);
            }

            // Draw card background
            const ImU32 fill = sel ? IM_COL32(0, 168, 252, 30)
                                   : (hovered ? IM_COL32(255, 255, 255, 10)
                                              : IM_COL32(0, 0, 0, 60));
            const float r = 10.0f;
            dl_grid->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), fill, r);

            // ---- Cover art area ----
            Thumbnail thumb;
            if (!g.cover_path.empty()) {
                thumb = thumbs.GetFromPath(g.title_id, g.cover_path);
            }
            if (!thumb.valid()) {
                thumb = thumbs.Get(g.title_id);
            }
            if (thumb.valid()) {
                dl_grid->PushClipRect(p, ImVec2(p.x + sz.x, p.y + sz.y), true);
                dl_grid->AddImage(reinterpret_cast<ImTextureID>(thumb.texture),
                                  p, ImVec2(p.x + sz.x, p.y + sz.y),
                                  ImVec2(0, 0), ImVec2(1, 1),
                                  IM_COL32(255, 255, 255, 255));
                dl_grid->PopClipRect();
            } else {
                DrawPlaceholder(dl_grid, p, sz, g.title_id);
            }

            // Glow border on selected/hovered card
            if (sel) {
                dl_grid->AddRect(p, ImVec2(p.x + sz.x, p.y + sz.y),
                                 IM_COL32(0, 168, 252, 255), r, 0, 2.5f);
            } else if (hovered) {
                dl_grid->AddRect(p, ImVec2(p.x + sz.x, p.y + sz.y),
                                 IM_COL32(255, 255, 255, 100), r, 0, 1.0f);
            }

            // ---- Title text (directly below the card) ----
            // We draw a dummy layout spacer for the cover art
            ImGui::Dummy(sz);
            
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6.0f);
            
            // Render text
            std::string trimmed = name;
            float text_w = card_w;
            auto fits = [&](const std::string& s) {
                return ImGui::CalcTextSize(s.c_str()).x <= text_w;
            };
            if (!fits(trimmed)) {
                while (trimmed.size() > 4 && !fits(trimmed)) {
                    trimmed.pop_back();
                }
                if (trimmed.size() > 3) {
                    trimmed.resize(trimmed.size() - 1);
                    trimmed += "...";
                }
            }
            
            ImGui::PushStyleColor(ImGuiCol_Text, sel ? ImVec4(0.0f, 0.66f, 1.0f, 1.0f) : ImVec4(0.9f, 0.93f, 0.96f, 1.0f));
            ImGui::Text("%s", trimmed.c_str());
            ImGui::PopStyleColor();

            // ---- Clickable hot zone on the card ----
            ImGui::SetCursorScreenPos(p);
            if (ImGui::InvisibleButton("##card", sz)) {
                selected_index = i;
            }
            if (ImGui::IsItemHovered()) {
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    selected_index = i;
                    boot_requested = true;
                }
                ImGui::SetTooltip("%s\nSize: %s", g.title_id.c_str(), FormatSize(g.size_bytes).c_str());
            }

            ImGui::EndGroup();
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();

    return boot_requested;
}

}  // namespace Ui
