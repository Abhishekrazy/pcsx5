#include "input_bot.h"
#include "../../common/log.h"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <fstream>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Format / parse helpers
// ---------------------------------------------------------------------------
std::string FormatInputBotEvent(uint64_t frame, const ControllerState& state) {
    char buf[512];
    std::snprintf(buf, sizeof(buf),
        R"({"frame":%llu,"buttons":%u,"lx":%u,"ly":%u,"rx":%u,"ry":%u,)"
        R"("l2":%u,"r2":%u,"touch":%u})",
        (unsigned long long)frame,
        (unsigned)state.buttons,
        (unsigned)state.left_x, (unsigned)state.left_y,
        (unsigned)state.right_x, (unsigned)state.right_y,
        (unsigned)state.l2, (unsigned)state.r2,
        (unsigned)state.touch_count);
    return std::string(buf);
}

bool LoadInputBotReplay(const std::string& path,
                         std::vector<InputBotEvent>& out_events,
                         uint64_t* out_total_frames) {
    out_events.clear();
    if (out_total_frames) *out_total_frames = 0;

    std::ifstream file(path);
    if (!file.is_open()) {
        LOG_ERROR(General, "InputBot: cannot open replay file: %s", path.c_str());
        return false;
    }

    json j;
    try {
        file >> j;
    } catch (const std::exception& e) {
        LOG_ERROR(General, "InputBot: JSON parse error: %s", e.what());
        return false;
    }

    // Validate version.
    int version = j.value("version", 0);
    if (version != 1) {
        LOG_ERROR(General, "InputBot: unsupported replay version %d (expected 1)", version);
        return false;
    }

    // Parse events.
    auto it = j.find("events");
    if (it == j.end() || !it->is_array()) {
        LOG_ERROR(General, "InputBot: no 'events' array in replay file");
        return false;
    }

    for (const auto& e : *it) {
        InputBotEvent ev{};
        ev.frame   = e.value("frame", 0ULL);
        ev.buttons = e.value("buttons", 0U);
        ev.lx      = static_cast<uint8_t>(e.value("lx", 128));
        ev.ly      = static_cast<uint8_t>(e.value("ly", 128));
        ev.rx      = static_cast<uint8_t>(e.value("rx", 128));
        ev.ry      = static_cast<uint8_t>(e.value("ry", 128));
        ev.l2      = static_cast<uint8_t>(e.value("l2", 0));
        ev.r2      = static_cast<uint8_t>(e.value("r2", 0));
        ev.touch_count = static_cast<uint8_t>(e.value("touch", 0));
        out_events.push_back(ev);
    }

    if (out_events.empty()) {
        LOG_WARN(General, "InputBot: replay file has no events");
        return false;
    }

    uint64_t last_frame = out_events.back().frame;
    if (out_total_frames) *out_total_frames = last_frame;
    LOG_INFO(General, "InputBot: loaded %zu events (%llu frames) from %s",
             out_events.size(), (unsigned long long)last_frame, path.c_str());
    return true;
}

// ---------------------------------------------------------------------------
// InputRecorder — writes live controller state to newline-delimited JSON.
// ---------------------------------------------------------------------------
InputRecorder::~InputRecorder() {
    if (m_file.is_open()) Stop();
}

bool InputRecorder::Start(const std::string& path, const std::string& title_id) {
    m_file.open(path);
    if (!m_file.is_open()) {
        LOG_ERROR(General, "InputRecorder: cannot open %s", path.c_str());
        return false;
    }
    // Write the JSON preamble (open array).
    m_file << "{\n  \"version\": 1";
    if (!title_id.empty())
        m_file << ",\n  \"title_id\": \"" << title_id << "\"";
    m_file << ",\n  \"events\": [\n";
    m_first_frame = 0;
    m_last_frame = 0;
    m_has_events = false;
    LOG_INFO(General, "InputRecorder: recording to %s", path.c_str());
    return true;
}

void InputRecorder::RecordFrame(uint64_t frame, const ControllerState& state) {
    if (!m_file.is_open()) return;

    // Comma separator between events.
    if (m_has_events) m_file << ",\n";
    m_has_events = true;

    m_file << "    "
           << FormatInputBotEvent(frame, state);
    m_last_frame = frame;
    if (m_first_frame == 0) m_first_frame = frame;
}

void InputRecorder::Stop() {
    if (!m_file.is_open()) return;
    m_file << "\n  ],\n";
    m_file << "  \"total_frames\": " << (m_last_frame - m_first_frame + 1) << "\n}\n";
    m_file.close();
    LOG_INFO(General, "InputRecorder: stopped (%llu frames, %llu..%llu)",
             (unsigned long long)(m_last_frame - m_first_frame + 1),
             (unsigned long long)m_first_frame,
             (unsigned long long)m_last_frame);
}

// ---------------------------------------------------------------------------
// InputBotBackend
// ---------------------------------------------------------------------------
InputBotBackend::InputBotBackend(const std::string& replay_path)
    : m_replay_path(replay_path) {}

bool InputBotBackend::Initialize(int controller_index) {
    m_controller_index = controller_index;
    if (!LoadInputBotReplay(m_replay_path, m_events, &m_total_frames)) {
        LOG_ERROR(General, "InputBot: failed to load replay: %s", m_replay_path.c_str());
        return false;
    }
    m_current_frame = 0;
    m_current_index = 0;
    m_initialized = true;
    LOG_INFO(General, "InputBot: initialized (controller %d, %zu events, %llu frames)",
             controller_index, m_events.size(), (unsigned long long)m_total_frames);
    return true;
}

void InputBotBackend::Shutdown() {
    m_initialized = false;
    m_events.clear();
    m_current_index = 0;
    m_current_frame = 0;
    LOG_INFO(General, "InputBot: shut down after %llu frames (replay: %s)",
             (unsigned long long)m_current_frame, m_replay_path.c_str());
}

InputCaps InputBotBackend::GetCaps() const {
    InputCaps caps;
    caps.backend_name = "InputBot";
    caps.max_controllers = 1;
    // The bot can synthesize any state the replay contains.
    caps.has_rumble   = false;   // output not supported
    caps.has_touchpad = true;    // replay may include touch
    caps.has_motion   = false;   // gyro/accel not recorded
    caps.has_haptics  = false;
    return caps;
}

bool InputBotBackend::Poll(ControllerState& out) {
    if (!m_initialized || m_events.empty()) {
        out = ControllerState{};
        return false;
    }

    // Find the event that covers the current frame.
    // Events are ordered by frame; advance until we find the right one.
    while (m_current_index + 1 < m_events.size() &&
           m_events[m_current_index + 1].frame <= m_current_frame) {
        m_current_index++;
    }

    const InputBotEvent& ev = m_events[m_current_index];

    // Synthesise ControllerState from the current event.
    out.buttons     = ev.buttons;
    out.left_x      = ev.lx;
    out.left_y      = ev.ly;
    out.right_x     = ev.rx;
    out.right_y     = ev.ry;
    out.l2          = ev.l2;
    out.r2          = ev.r2;
    out.touch_count = ev.touch_count;
    for (int i = 0; i < 2 && i < ev.touch_count; i++) {
        out.touch_x[i] = ev.touch_x[i];
        out.touch_y[i] = ev.touch_y[i];
    }
    out.connected = true;
    out.timestamp_us = 0;  // not meaningful for synthetic data

    m_current_frame++;
    return true;
}

void InputBotBackend::Reset() {
    m_current_frame = 0;
    m_current_index = 0;
    LOG_INFO(General, "InputBot: reset playback");
}
