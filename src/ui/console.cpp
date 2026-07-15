#include "ui/console.h"
#include "common/log.h"

#include "imgui.h"

#include <cctype>
#include <cstring>
#include <mutex>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <regex>
#include <fstream>

namespace Ui {

static std::string GetCurrentTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::stringstream ss;
    struct tm buf;
#ifdef _WIN32
    localtime_s(&buf, &time);
#else
    localtime_r(&time, &buf);
#endif
    ss << std::put_time(&buf, "%H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return ss.str();
}

void LogConsole::Append(const std::string& line) {
    std::lock_guard<std::mutex> lock(mu_);
    LogEntry entry;
    entry.text = line;
    entry.timestamp = GetCurrentTimestamp();
    lines_.push_back(std::move(entry));
    if (lines_.size() > kMaxLines) lines_.pop_front();
}

void LogConsole::Append(const char* data, std::size_t n) {
    if (!data || n == 0) return;
    std::string s(data, n);
    if (!s.empty() && s.back() == '\n') s.pop_back();
    if (!s.empty() && s.back() == '\r') s.pop_back();
    Append(std::move(s));
}

void LogConsole::Clear() {
    std::lock_guard<std::mutex> lock(mu_);
    lines_.clear();
    selected_lines_.clear();
}

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

static bool MatchesFilter(const std::string& s, const char* needle, bool regex_mode) {
    if (!needle || !*needle) return true;
    if (regex_mode) {
        try {
            std::regex re(needle, std::regex_constants::icase);
            return std::regex_search(s, re);
        } catch (...) {
            // fallback
        }
    }
    return ContainsCi(s, needle);
}

bool LogConsole::Draw(const char* title, bool* p_open) {
    bool changed = false;

    if (detached_) {
        if (!ImGui::Begin(title, p_open)) {
            ImGui::End();
            return changed;
        }
    } else {
        // Modern glass-like panel container for inline console
        ImGui::BeginChild(title, ImVec2(0, 0), true);
    }

    // Dynamic level counters
    int cnt_trace = 0, cnt_debug = 0, cnt_info = 0, cnt_warn = 0, cnt_err = 0, cnt_crit = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& entry : lines_) {
            const auto& line = entry.text;
            if (line.find("[Trace]") != std::string::npos) cnt_trace++;
            else if (line.find("[Debug]") != std::string::npos) cnt_debug++;
            else if (line.find("[Info]") != std::string::npos) cnt_info++;
            else if (line.find("[Warn]") != std::string::npos) cnt_warn++;
            else if (line.find("[Error]") != std::string::npos) cnt_err++;
            else if (line.find("[Critical]") != std::string::npos) cnt_crit++;
        }
    }

    // Header (left-aligned)
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.6f, 0.7f, 1.0f));
    ImGui::TextUnformatted("CONSOLE");
    ImGui::PopStyleColor();

    // Controls (right-aligned to match the clean design)
    float controls_width = 460.0f;
    float avail_x = ImGui::GetContentRegionAvail().x;
    if (avail_x > controls_width) {
        ImGui::SameLine(avail_x - controls_width + 12.0f);
    } else {
        ImGui::SameLine();
    }

    // Search bar
    ImGui::SetNextItemWidth(140.0f);
    ImGui::InputTextWithHint("##filter", "Search...", filter_, sizeof(filter_));

    ImGui::SameLine(0, 10);
    ImGui::Checkbox("Auto scroll", &autoscroll_);

    ImGui::SameLine(0, 10);
    if (ImGui::Button("Split")) {
        // Mock split layout toggle
    }

    ImGui::SameLine(0, 6);
    if (ImGui::Button("Copy")) {
        std::string clipboard;
        {
            std::lock_guard<std::mutex> lock(mu_);
            size_t k = 0;
            for (const auto& entry : lines_) {
                if (!MatchesFilter(entry.text, filter_, filter_regex_)) { k++; continue; }
                if (!selected_lines_.empty() && selected_lines_.count(k) == 0) { k++; continue; }
                if (show_timestamps_) clipboard += "[" + entry.timestamp + "] ";
                clipboard += entry.text;
                clipboard += '\n';
                k++;
            }
        }
        if (!clipboard.empty()) ImGui::SetClipboardText(clipboard.c_str());
    }

    ImGui::SameLine(0, 6);
    if (ImGui::Button("Clear")) Clear();

    ImGui::SameLine(0, 6);
    if (ImGui::Button("...")) {
        ImGui::OpenPopup("console_options");
    }
    if (ImGui::BeginPopup("console_options")) {
        ImGui::Checkbox("Timestamps", &show_timestamps_);
        ImGui::Checkbox("Regex Mode", &filter_regex_);
        ImGui::Checkbox("Word-wrap", &word_wrap_);
        
        ImGui::Separator();
        if (ImGui::Button("Save Log File", ImVec2(120, 24))) {
            std::ofstream file("pcsx5_console.log");
            if (file.is_open()) {
                std::lock_guard<std::mutex> lock(mu_);
                for (const auto& entry : lines_) {
                    if (show_timestamps_) file << "[" << entry.timestamp << "] ";
                    file << entry.text << "\n";
                }
                file.close();
            }
            ImGui::CloseCurrentPopup();
        }
        
        ImGui::Separator();
        ImGui::TextDisabled("Log Levels");
        static const char* kCatNames[] = {"Loader", "Memory", "Kernel", "HLE", "GPU", "General"};
        static const char* kLvlNames[] = {"Trace", "Debug", "Info", "Warn", "Error", "Critical"};
        for (int i = 0; i < 6; ++i) {
            int cur = static_cast<int>(LogConfig::GetLevel(static_cast<LogCategory>(i)));
            ImGui::TextUnformatted(kCatNames[i]);
            ImGui::SameLine(80);
            ImGui::SetNextItemWidth(100);
            if (ImGui::Combo(("##lvl" + std::to_string(i)).c_str(), &cur, kLvlNames, 6)) {
                LogConfig::SetLevel(static_cast<LogCategory>(i), static_cast<LogLevel>(cur));
            }
        }
        ImGui::EndPopup();
    }

    // Category counters rendered inline in clean badges
    ImGui::SameLine(0, 10);
    ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 0.6f), "I:%d", cnt_info); ImGui::SameLine(0, 6);
    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.40f, 0.7f), "W:%d", cnt_warn); ImGui::SameLine(0, 6);
    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 0.8f), "E:%d", cnt_err);

    ImGui::Separator();

    // Body: scrollable, filtered lines.
    if (visible_) {
        std::vector<LogEntry> snapshot;
        {
            std::lock_guard<std::mutex> lock(mu_);
            snapshot.assign(lines_.begin(), lines_.end());
        }
        if (ImGui::BeginChild("console.body", ImVec2(0, 0), false,
                              ImGuiWindowFlags_HorizontalScrollbar)) {
            size_t k = 0;
            for (const auto& entry : snapshot) {
                if (!MatchesFilter(entry.text, filter_, filter_regex_)) {
                    k++;
                    continue;
                }

                bool is_selected = (selected_lines_.count(k) > 0);
                
                ImVec4 col = ImVec4(0.85f, 0.85f, 0.90f, 1.0f);
                if (entry.text.find("[Critical]") != std::string::npos)     col = ImVec4(1.0f, 0.30f, 0.60f, 1.0f);
                else if (entry.text.find("[Error]") != std::string::npos)   col = ImVec4(1.0f, 0.45f, 0.45f, 1.0f);
                else if (entry.text.find("[Warn]") != std::string::npos)    col = ImVec4(1.0f, 0.85f, 0.40f, 1.0f);
                else if (entry.text.find("[Info]") != std::string::npos)    col = ImVec4(0.55f, 0.85f, 1.0f, 1.0f);
                else if (entry.text.find("[Debug]") != std::string::npos)   col = ImVec4(0.65f, 0.65f, 0.80f, 1.0f);
                else if (entry.text.find("[Trace]") != std::string::npos)   col = ImVec4(0.50f, 0.50f, 0.55f, 1.0f);
                else if (entry.text.find("crash") != std::string::npos ||
                         entry.text.find("CRASH") != std::string::npos)     col = ImVec4(1.0f, 0.30f, 0.30f, 1.0f);
                
                ImGui::PushStyleColor(ImGuiCol_Text, col);
                ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.00f, 0.66f, 0.99f, 0.35f));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(1.00f, 1.00f, 1.00f, 0.08f));
                
                std::string line_display;
                if (show_timestamps_) {
                    line_display = "[" + entry.timestamp + "] " + entry.text;
                } else {
                    line_display = entry.text;
                }

                char id_label[1024];
                std::snprintf(id_label, sizeof(id_label), "%s##%zu", line_display.c_str(), k);
                
                if (ImGui::Selectable(id_label, is_selected, ImGuiSelectableFlags_SpanAllColumns)) {
                    if (ImGui::GetIO().KeyCtrl) {
                        if (is_selected) selected_lines_.erase(k);
                        else selected_lines_.insert(k);
                    } else if (ImGui::GetIO().KeyShift && !selected_lines_.empty()) {
                        size_t first = *selected_lines_.begin();
                        selected_lines_.clear();
                        size_t min_idx = std::min(first, k);
                        size_t max_idx = std::max(first, k);
                        for (size_t idx = min_idx; idx <= max_idx; ++idx) {
                            selected_lines_.insert(idx);
                        }
                    } else {
                        selected_lines_.clear();
                        selected_lines_.insert(k);
                    }
                }
                
                ImGui::PopStyleColor(3);
                k++;
            }
            if (autoscroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
    }

    if (detached_) {
        ImGui::End();
    } else {
        ImGui::EndChild();
    }
    return changed;
}

}  // namespace Ui
