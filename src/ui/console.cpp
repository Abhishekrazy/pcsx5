#include "ui/console.h"

#include "imgui.h"

#include <cctype>
#include <cstring>
#include <mutex>

namespace Ui {

void LogConsole::Append(const std::string& line) {
    std::lock_guard<std::mutex> lock(mu_);
    lines_.push_back(line);
    if (lines_.size() > kMaxLines) lines_.pop_front();
}

void LogConsole::Append(const char* data, std::size_t n) {
    if (!data || n == 0) return;
    std::string s(data, n);
    // Strip a trailing newline so the ImGui TextWrapped line counter
    // matches what a terminal would render.
    if (!s.empty() && s.back() == '\n') s.pop_back();
    if (!s.empty() && s.back() == '\r') s.pop_back();
    Append(std::move(s));
}

void LogConsole::Clear() {
    std::lock_guard<std::mutex> lock(mu_);
    lines_.clear();
}

// Case-insensitive substring search.
static bool ContainsCi(const std::string& s, const char* needle) {
    if (!needle || !*needle) return true;
    auto tolower_c = [](unsigned char c) { return std::tolower(c); };
    const std::size_t nl = std::strlen(needle);
    if (nl > s.size()) return false;
    for (std::size_t i = 0; i + nl <= s.size(); ++i) {
        std::size_t j = 0;
        while (j < nl && tolower_c(static_cast<unsigned char>(s[i + j])) ==
                            tolower_c(static_cast<unsigned char>(needle[j]))) {
            ++j;
        }
        if (j == nl) return true;
    }
    return false;
}

bool LogConsole::Draw(const char* title, bool* p_open) {
    bool changed = false;

    if (!ImGui::Begin(title, p_open)) {
        ImGui::End();
        return changed;
    }

    // Toolbar.
    if (ImGui::Button("Clear")) Clear();
    ImGui::SameLine();
    ImGui::Checkbox("Autoscroll", &autoscroll_);
    ImGui::SameLine();
    ImGui::Checkbox("Word-wrap", &word_wrap_);
    ImGui::SameLine();
    bool was_detached = detached_;
    if (ImGui::Checkbox("Detach as window", &detached_)) {
        changed = (detached_ != was_detached);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180);
    ImGui::InputTextWithHint("##filter", "filter...", filter_, sizeof(filter_));

    ImGui::Separator();

    // Body: scrollable, filtered lines.
    std::vector<std::string> snapshot;
    {
        std::lock_guard<std::mutex> lock(mu_);
        snapshot.assign(lines_.begin(), lines_.end());
    }
    if (ImGui::BeginChild("console.body", ImVec2(0, 0), false,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        for (const auto& line : snapshot) {
            if (!ContainsCi(line, filter_)) continue;
            // Colour-level pseudo-tagging: lines starting with [.*] get a
            // small coloured badge.  This is heuristic and good enough for
            // most emulator log lines.
            ImVec4 col = ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
            if (line.find("[Error]") != std::string::npos)        col = ImVec4(1.0f, 0.45f, 0.45f, 1.0f);
            else if (line.find("[Warn]") != std::string::npos)    col = ImVec4(1.0f, 0.85f, 0.40f, 1.0f);
            else if (line.find("[Info]") != std::string::npos)    col = ImVec4(0.55f, 0.85f, 1.0f, 1.0f);
            else if (line.find("[Debug]") != std::string::npos)   col = ImVec4(0.65f, 0.65f, 0.80f, 1.0f);
            else if (line.find("crash") != std::string::npos ||
                     line.find("CRASH") != std::string::npos)     col = ImVec4(1.0f, 0.30f, 0.30f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            if (word_wrap_) ImGui::TextWrapped("%s", line.c_str());
            else            ImGui::TextUnformatted(line.c_str());
            ImGui::PopStyleColor();
        }
        if (autoscroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();

    ImGui::End();
    return changed;
}

}  // namespace Ui
