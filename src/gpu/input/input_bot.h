#pragma once
//
// I1: Input Bot Backend — replays pre-recorded controller input sequences.
//
// The bot reads a JSON replay file and feeds synthetic ControllerState
// snapshots into the InputMultiplexer on each Poll() call.  This enables
// automated testing: boot the game, play back a known-good input sequence,
// and detect crashes/hangs without human interaction.
//
// Replay format (I1.1):
//   {
//     "version": 1,
//     "title_id": "PPSA02929",
//     "events": [
//       { "frame": 0,   "buttons": 0,    "lx":128, "ly":128, "rx":128, "ry":128, "l2":0, "r2":0 },
//       { "frame": 120, "buttons": 1,    "lx":128, "ly":128, "rx":128, "ry":128, "l2":0, "r2":0 },
//       { "frame": 240, "buttons": 0x40, "lx":255, "ly":128, "rx":128, "ry":128, "l2":0, "r2":0 }
//     ]
//   }
//
// "frame" is a monotonic frame counter (60 Hz vblank units).  The bot holds
// each event's state until the frame count reaches the next event.
//

#include "input_backend.h"
#include "../../common/types.h"
#include <string>
#include <vector>
#include <fstream>

// ---------------------------------------------------------------------------
// Replay event — one snapshot of the full controller state at a given frame.
// ---------------------------------------------------------------------------
struct InputBotEvent {
    uint64_t  frame = 0;
    uint32_t  buttons = 0;
    uint8_t   lx = 128, ly = 128;
    uint8_t   rx = 128, ry = 128;
    uint8_t   l2 = 0, r2 = 0;
    uint8_t   touch_count = 0;
    uint16_t  touch_x[2] = {};
    uint16_t  touch_y[2] = {};
};

// ---------------------------------------------------------------------------
// Utility: write a ControllerState as a JSON replay event line.
// ---------------------------------------------------------------------------
std::string FormatInputBotEvent(uint64_t frame, const ControllerState& state);

// ---------------------------------------------------------------------------
// Utility: parse a JSON replay file into a vector of events.
// Returns false on parse error.
// ---------------------------------------------------------------------------
bool LoadInputBotReplay(const std::string& path, std::vector<InputBotEvent>& out_events,
                        uint64_t* out_total_frames = nullptr);

// ---------------------------------------------------------------------------
// Recording utility: writes live controller state to a JSON file.
// Appends one JSON object per line (newline-delimited JSON) so the recording
// can be safely truncated on crash and post-processed into a proper array.
// ---------------------------------------------------------------------------
class InputRecorder {
public:
    InputRecorder() = default;
    ~InputRecorder();

    // Open a file for recording.  Returns false on I/O error.
    bool Start(const std::string& path, const std::string& title_id);

    // Record one frame's worth of controller state.  Call once per vblank.
    void RecordFrame(uint64_t frame, const ControllerState& state);

    // Close the recording file and write the JSON trailer.
    void Stop();

    bool IsRecording() const { return m_file.is_open(); }

private:
    std::ofstream m_file;
    uint64_t      m_first_frame = 0;
    uint64_t      m_last_frame  = 0;
    bool          m_has_events  = false;
};

// ---------------------------------------------------------------------------
// InputBotBackend — reads a replay file and synthesizes controller state.
// ---------------------------------------------------------------------------
class InputBotBackend : public InputBackend {
public:
    // `replay_path` — JSON replay file to load.
    explicit InputBotBackend(const std::string& replay_path);
    ~InputBotBackend() override = default;

    bool Initialize(int controller_index = 0) override;
    void Shutdown() override;
    bool IsInitialized() const override { return m_initialized; }
    InputCaps GetCaps() const override;

    // Returns the current event's state and advances the internal frame
    // counter.  The caller (multiplexer or game loop) should call this
    // once per vblank/frame.
    bool Poll(ControllerState& out) override;

    // No-ops — the bot plays back inputs only.
    void SetRumble(const RumbleState&) override {}
    void SetTriggerEffect(bool, const TriggerEffect&) override {}

    // Reset playback to the first event.  Useful for restarting a test.
    void Reset();

    // Returns true when all events have been consumed.
    bool IsFinished() const { return m_current_index >= m_events.size(); }

    // Total replay duration in frames (from the last event's frame).
    uint64_t TotalFrames() const { return m_total_frames; }

    // Current playback frame.
    uint64_t CurrentFrame() const { return m_current_frame; }

private:
    std::string                m_replay_path;
    std::vector<InputBotEvent> m_events;
    uint64_t                   m_current_frame = 0;
    uint64_t                   m_total_frames  = 0;
    size_t                     m_current_index = 0;
    bool                       m_initialized = false;
    int                        m_controller_index = 0;
};
