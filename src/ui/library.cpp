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
static ImU32 StatusColor(const std::string& s) {
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

void DrawLibraryPanel(const std::vector<GameEntry>& games,
                      int& selected_index,
                      ThumbnailCache& thumbs) {
    // ---- Filter + view state (per-panel) ----
    static char filter_buf[128] = "";

    // ---- Filter input + clear + count, all on one line ----
    // Caller is expected to have already drawn the page header
    // ("Library  N games"  + search box).  We just draw the grid.

    // ---- Filter + sort ----
    std::string needle = filter_buf;
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
        return;
    }
    if (idx.empty()) {
        ImGui::Dummy(ImVec2(0, 24));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.545f, 0.58f, 0.62f, 1.0f));
        ImGui::Text("No titles match \"%s\".", filter_buf);
        ImGui::PopStyleColor();
        return;
    }

    // ---- Card grid ----
    // Manual absolute-positioned grid (NO ImGui::SameLine).  The
    // previous version used SameLine + SetCursorScreenPos, which
    // desynchronised ImGui's internal column tracking and produced a
    // zigzag layout.  Here we set the cursor position explicitly for
    // every card using ImGui::SetCursorPos (relative coordinates
    // inside the current window), which always lines up correctly.
    const float avail_w = ImGui::GetContentRegionAvail().x;
    const float card_min_w = 168.0f;        // min comfortable width
    int   cols = std::max(1, static_cast<int>(avail_w / card_min_w));
    if (cols > 7) cols = 7;
    const float gap      = 14.0f;
    const float card_w   = (avail_w - (cols - 1) * gap) /
                           static_cast<float>(cols);
    const float cover_h  = card_w;          // square cover
    const float meta_h   = 52.0f;
    const float card_h   = cover_h + meta_h;
    const float row_gap  = gap;
    const float col_gap  = gap;

    // Absolute content-region origin (parent-window coordinates) so
    // the first card lines up flush with the panel's left edge.
    const ImVec2 origin = ImGui::GetCursorPos();
    ImDrawList* dl_grid = ImGui::GetWindowDrawList();

    for (size_t k = 0; k < idx.size(); ++k) {
        const int  row = static_cast<int>(k) / cols;
        const int  col = static_cast<int>(k) % cols;
        const int  i   = idx[k];
        const auto& g  = games[i];
        Compat::Entry e;
        std::string status = g.status_text;
        std::string name   = g.display_name;
        if (Compat::Load(g.title_id, e, nullptr)) {
            status = Compat::StatusName(e.status);
            if (!e.name.empty()) name = e.name;
        }

        const bool sel = (selected_index == i);
        ImGui::PushID(i);

        // ---- Absolute position of this card (relative to the
        //      current window's content area).  Setting the cursor
        //      here is what makes the grid a true grid: each card
        //      lives at (col, row) with a fixed stride, regardless
        //      of what the previous card was doing.
        const ImVec2 card_pos(origin.x + col * (card_w + col_gap),
                              origin.y + row * (card_h + row_gap));
        ImGui::SetCursorPos(card_pos);

        // ---- Card frame ----
        const ImVec2 p  = ImGui::GetCursorScreenPos();
        const ImVec2 sz(card_w, card_h);
        const bool hovered = ImGui::IsMouseHoveringRect(
            p, ImVec2(p.x + sz.x, p.y + sz.y));
        // Background fill: subtle darken for hover/select.
        const ImU32 fill = sel ? IM_COL32(0, 70, 120, 180)
                               : (hovered ? IM_COL32(255, 255, 255, 8)
                                          : IM_COL32(0, 0, 0, 0));
        const float r = 10.0f;
        if (fill != 0)
            dl_grid->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y),
                                   fill, r);

        // ---- Cover art area (top portion) ----
        const ImVec2 cover_p(p.x, p.y);
        const ImVec2 cover_sz(card_w, cover_h);
        // Prefer the game's own sce_sys/icon0.png (SharpEmu-style:
        // pull straight from the game files).  If that is missing
        // (older / unusual dumps), fall back to a global covers dir
        // lookup by title id.
        Thumbnail thumb;
        if (!g.cover_path.empty()) {
            thumb = thumbs.GetFromPath(g.title_id, g.cover_path);
        }
        if (!thumb.valid()) {
            thumb = thumbs.Get(g.title_id);
        }
        if (thumb.valid()) {
            // Draw the cover texture in absolute coordinates, clipped
            // to the cover area so the rounded-card look is preserved.
            dl_grid->PushClipRect(cover_p,
                                  ImVec2(cover_p.x + cover_sz.x,
                                         cover_p.y + cover_sz.y),
                                  true);
            dl_grid->AddImage(reinterpret_cast<ImTextureID>(thumb.texture),
                              cover_p,
                              ImVec2(cover_p.x + cover_sz.x,
                                     cover_p.y + cover_sz.y),
                              ImVec2(0, 0), ImVec2(1, 1),
                              IM_COL32(255, 255, 255, 255));
            dl_grid->PopClipRect();
        } else {
            DrawPlaceholder(dl_grid, cover_p, cover_sz, g.title_id);
        }

        // Selection / hover ring (drawn above the cover, inside the
        // card area, so the title strip is not hidden by it).
        if (sel) {
            dl_grid->AddRect(p, ImVec2(p.x + sz.x, p.y + sz.y),
                             IM_COL32(0, 168, 252, 255), r, 0, 2.5f);
        } else if (hovered) {
            dl_grid->AddRect(p, ImVec2(p.x + sz.x, p.y + sz.y),
                             IM_COL32(255, 255, 255, 60), r, 0, 1.0f);
        }

        // ---- Meta strip (title + title id + size) ----
        const ImVec2 meta_p(p.x, p.y + cover_h);
        const ImVec2 meta_sz(card_w, meta_h);

        // Subtle separator between cover and meta.
        dl_grid->AddLine(ImVec2(p.x, p.y + cover_h - 0.5f),
                         ImVec2(p.x + sz.x, p.y + cover_h - 0.5f),
                         IM_COL32(255, 255, 255, 18));

        // Status dot.
        const float dot_r = 3.5f;
        const float dot_x = meta_p.x + 10.0f;
        const float dot_y = meta_p.y + 12.0f;
        dl_grid->AddCircleFilled(ImVec2(dot_x, dot_y), dot_r,
                                 StatusColor(status), 8);

        // Game name (truncated to fit, with pixel-width measurement).
        const float text_x = meta_p.x + 20.0f;
        const float text_w = meta_sz.x - 24.0f;
        std::string trimmed = name;
        while (!trimmed.empty() &&
               ImGui::CalcTextSize(trimmed.c_str()).x > text_w) {
            trimmed.pop_back();
        }
        if (trimmed.size() + 3 < name.size() && trimmed.size() > 3) {
            // We had to drop at least 4 characters -> ellipsise.
            trimmed.resize(trimmed.size() - 1);
            trimmed += "...";
        }
        dl_grid->AddText(ImVec2(text_x, meta_p.y + 6),
                         IM_COL32(232, 238, 244, 255), trimmed.c_str());

        // Title id + size (muted).  Append a small "♪ <file>" hint
        // when the game ships a default-music track so the user can
        // see at a glance which games have a snd0.at9 / .ogg / .wav
        // available for the home-screen preview.
        char meta_line[128];
        if (!g.music_path.empty()) {
            const std::string base =
                std::filesystem::path(g.music_path).filename().generic_string();
            std::snprintf(meta_line, sizeof(meta_line), "%s  -  %s  -  %s",
                          g.title_id.c_str(),
                          FormatSize(g.size_bytes).c_str(),
                          base.c_str());
        } else {
            std::snprintf(meta_line, sizeof(meta_line), "%s  -  %s",
                          g.title_id.c_str(),
                          FormatSize(g.size_bytes).c_str());
        }
        std::string meta_str = meta_line;
        while (!meta_str.empty() &&
               ImGui::CalcTextSize(meta_str.c_str()).x > text_w) {
            meta_str.pop_back();
        }
        if (meta_str.size() > 3) {
            meta_str[meta_str.size() - 1] = '.';
            meta_str[meta_str.size() - 2] = '.';
            meta_str[meta_str.size() - 3] = '.';
        }
        dl_grid->AddText(ImVec2(text_x, meta_p.y + 26),
                         IM_COL32(125, 135, 148, 255), meta_str.c_str());

        // ---- Clickable hot zone ----
        // Set the cursor to the card's screen position and emit a
        // transparent button the size of the card.  This makes the
        // entire card a single hit target for selection / tooltip.
        ImGui::SetCursorScreenPos(p);
        if (ImGui::InvisibleButton("##card", sz)) {
            selected_index = i;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s\n%s\nSize: %s",
                              g.title_id.c_str(),
                              g.dir_path.c_str(),
                              FormatSize(g.size_bytes).c_str());
        }
        ImGui::PopID();
    }

    // Reserve enough vertical space inside the parent for the whole
    // grid (rows * card_h + (rows-1) * gap), so any scroll container
    // sees the correct total content size.  We place a single Dummy
    // at the end rather than per-card, so ImGui's column tracking is
    // not affected.
    const int total_rows = static_cast<int>(
        (idx.size() + cols - 1) / static_cast<size_t>(cols));
    const float total_h = static_cast<float>(total_rows) * card_h +
                          static_cast<float>(total_rows > 0 ? total_rows - 1 : 0) *
                              row_gap;
    ImGui::SetCursorPos(ImVec2(origin.x, origin.y + total_h));
    ImGui::Dummy(ImVec2(avail_w, 8.0f));
}

}  // namespace Ui
