// Frame timing diagnostics — ring buffer of per-frame stage timestamps.
//
// Records wall-clock durations for each pipeline stage every frame:
//   - Wait:    waiting for the GPU to finish the previous frame
//   - Draw:    recording guest draw calls into the command buffer
//   - Upload:  staging buffer uploads (textures, vertex data)
//   - Present: swapchain acquire + blit + present submit
//   - Total:   frame start to present completion
//
// Rendered as an ImGui overlay (Ctrl+F3 toggle) with a timing waterfall
// and a rolling FPS graph.

#pragma once
#include "../common/types.h"
#include <chrono>
#include <cstdint>

namespace Diagnostics {

// Pipeline stage identifiers.
enum class FrameStage : int {
    Wait    = 0,
    Draw    = 1,
    Upload  = 2,
    Present = 3,
    Count   = 4,  // number of stages
};

// One frame's timing record.
struct FrameTiming {
    u64 timestamp_us;        // steady_clock time of frame start
    u64 stage_duration_us[static_cast<int>(FrameStage::Count)]; // per-stage
    u64 total_duration_us;   // frame start to present done
    u64 frame_index;         // monotonic frame counter
};

// Ring buffer capacity (last N frames kept).
constexpr int kTimingRingCapacity = 256;

// Record a stage timestamp for the current frame.  `stage_start_us` is the
// steady_clock time when the stage began (from NowUs()).
void RecordStage(FrameStage stage, u64 stage_start_us);

// Mark the current frame as complete and advance to the next frame.
// Returns the just-completed frame index.
u64 EndFrame();

// Read-only access to the ring buffer (for the ImGui renderer).
// `out_count` receives the number of valid entries (0..kTimingRingCapacity).
const FrameTiming* GetTimingRing(int* out_count);

// Current instantaneous FPS (smoothed over the last 16 frames).
double GetFps();

// Steady clock microsecond helper.
inline u64 NowUs() {
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

// ImGui overlay (call every frame from the main thread).
void RenderTimingOverlay(bool* p_open = nullptr);

} // namespace Diagnostics
