// Frame timing ring buffer + ImGui overlay.

#include "frame_timing.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>

#ifdef IMGUI_VERSION
#include <imgui.h>
#endif

namespace Diagnostics {

namespace {

// Ring buffer (lock-free single producer: the GPU/main thread).
FrameTiming g_ring[kTimingRingCapacity] = {};
std::atomic<int> g_head{0};  // next slot to write
std::atomic<int> g_count{0}; // valid entries

// Current frame accumulation.
u64   g_frame_start = 0;
u64   g_frame_index = 0;
int   g_accum_stage = 0;
u64   g_stage_starts[static_cast<int>(FrameStage::Count)] = {};

// FPS smoothing.
double g_fps = 0.0;
u64    g_last_fps_time = 0;
int    g_fps_frame_count = 0;

} // anonymous namespace

void RecordStage(FrameStage stage, u64 stage_start_us) {
    const int idx = static_cast<int>(stage);
    if (idx < 0 || idx >= static_cast<int>(FrameStage::Count)) return;

    const u64 now = NowUs();
    const u64 dur = now - stage_start_us;

    if (g_accum_stage == 0) {
        // First stage of the frame.
        g_frame_start = stage_start_us;
    }

    g_stage_starts[idx] = stage_start_us;

    // Write to ring buffer.
    const int head = g_head.load(std::memory_order_relaxed);
    auto& entry = g_ring[head];
    entry.timestamp_us = g_frame_start;
    entry.stage_duration_us[idx] = dur;
    entry.frame_index = g_frame_index;

    g_accum_stage++;
}

u64 EndFrame() {
    const u64 now = NowUs();
    const u64 total = now - g_frame_start;

    // Finalize the current ring entry.
    const int head = g_head.load(std::memory_order_relaxed);
    auto& entry = g_ring[head];
    entry.total_duration_us = total;
    entry.timestamp_us = g_frame_start;
    entry.frame_index = g_frame_index;

    // Advance ring.
    g_head.store((head + 1) % kTimingRingCapacity, std::memory_order_release);
    int c = g_count.load(std::memory_order_relaxed);
    if (c < kTimingRingCapacity) {
        g_count.store(c + 1, std::memory_order_release);
    }

    // FPS tracking.
    g_fps_frame_count++;
    const u64 elapsed = now - g_last_fps_time;
    if (elapsed > 500000) {  // update every 500ms
        g_fps = static_cast<double>(g_fps_frame_count) / (static_cast<double>(elapsed) / 1000000.0);
        g_fps_frame_count = 0;
        g_last_fps_time = now;
    }

    g_accum_stage = 0;
    return g_frame_index++;
}

const FrameTiming* GetTimingRing(int* out_count) {
    if (out_count) *out_count = g_count.load(std::memory_order_acquire);
    return g_ring;
}

double GetFps() {
    return g_fps;
}

void RenderTimingOverlay(bool* p_open) {
#ifdef IMGUI_VERSION
    if (!ImGui::Begin("Frame Timing", p_open,
                      ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }

    // FPS + summary.
    const double fps = GetFps();
    const double frame_ms = fps > 0.0 ? 1000.0 / fps : 0.0;
    ImGui::Text("FPS: %.1f  Frame: %.2f ms", fps, frame_ms);

    // Recent frame timing bar.
    const int count = g_count.load(std::memory_order_acquire);
    if (count > 0) {
        const int head = g_head.load(std::memory_order_acquire);
        // Show last 64 frames as a bar graph.
        const int show = (std::min)(count, 64);
        const float bar_w = (std::max)(2.0f, ImGui::GetContentRegionAvail().x / static_cast<float>(show));

        ImGui::Separator();
        ImGui::Text("Frame Durations (last %d frames, 1 bar = 1 frame)", show);
        ImGui::Spacing();

        // Color legend.
        const ImU32 colors[] = {
            IM_COL32(100, 100, 255, 200),  // Wait
            IM_COL32(100, 255, 100, 200),  // Draw
            IM_COL32(255, 200, 100, 200),  // Upload
            IM_COL32(255, 100, 100, 200),  // Present
        };
        const char* labels[] = {"Wait", "Draw", "Upload", "Present"};

        // Legend row.
        ImGui::Text("Legend: ");
        ImGui::SameLine();
        for (int i = 0; i < static_cast<int>(FrameStage::Count); ++i) {
            ImGui::ColorButton(labels[i], ImVec4(
                (colors[i] >> 24 & 0xFF) / 255.0f,
                (colors[i] >> 16 & 0xFF) / 255.0f,
                (colors[i] >> 8 & 0xFF) / 255.0f,
                (colors[i] >> 0 & 0xFF) / 255.0f),
                ImGuiColorEditFlags_NoTooltip, ImVec2(12, 12));
            ImGui::SameLine();
            ImGui::Text(labels[i]);
            if (i < static_cast<int>(FrameStage::Count) - 1) ImGui::SameLine();
        }

        ImGui::Spacing();

        // Draw stacked bar for each frame.
        const ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
        const float canvas_h = 40.0f;
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Find the max frame duration for scaling.
        u64 max_dur = 16667;  // ~60 FPS in microseconds
        for (int i = 0; i < show; ++i) {
            const int idx = (head - 1 - i + kTimingRingCapacity) % kTimingRingCapacity;
            const auto& e = g_ring[idx];
            if (e.total_duration_us > max_dur) max_dur = e.total_duration_us;
        }

        for (int i = 0; i < show; ++i) {
            const int idx = (head - 1 - i + kTimingRingCapacity) % kTimingRingCapacity;
            const auto& e = g_ring[idx];
            if (e.total_duration_us == 0) continue;

            float x = canvas_pos.x + (show - 1 - i) * bar_w;
            float y = canvas_pos.y + canvas_h;

            // Draw stacked stages.
            u64 accum = 0;
            for (int s = 0; s < static_cast<int>(FrameStage::Count); ++s) {
                const u64 dur = e.stage_duration_us[s];
                if (dur == 0) continue;
                float h = static_cast<float>(dur) / static_cast<float>(max_dur) * canvas_h;
                if (h < 1.0f) h = 1.0f;
                dl->AddRectFilled(
                    ImVec2(x, y - h),
                    ImVec2(x + bar_w, y),
                    colors[s]);
                y -= h;
                accum += dur;
            }
        }

        ImGui::Dummy(ImVec2(static_cast<float>(show) * bar_w, canvas_h + 4));
        ImGui::Spacing();

        // Per-stage breakdown (average of last N frames).
        ImGui::Separator();
        ImGui::Text("Average (last %d frames):", show);
        u64 avg_total = 0;
        u64 avg_stage[static_cast<int>(FrameStage::Count)] = {};
        int avg_n = 0;
        for (int i = 0; i < show; ++i) {
            const int idx = (head - 1 - i + kTimingRingCapacity) % kTimingRingCapacity;
            const auto& e = g_ring[idx];
            if (e.total_duration_us == 0) continue;
            avg_total += e.total_duration_us;
            for (int s = 0; s < static_cast<int>(FrameStage::Count); ++s) {
                avg_stage[s] += e.stage_duration_us[s];
            }
            avg_n++;
        }
        if (avg_n > 0) {
            for (int s = 0; s < static_cast<int>(FrameStage::Count); ++s) {
                ImGui::Text("  %s: %.2f ms", labels[s],
                            static_cast<double>(avg_stage[s] / avg_n) / 1000.0);
            }
            ImGui::Text("  Total: %.2f ms",
                        static_cast<double>(avg_total / avg_n) / 1000.0);
        }
    }

    ImGui::End();
#else
    (void)p_open;
#endif
}

} // namespace Diagnostics
