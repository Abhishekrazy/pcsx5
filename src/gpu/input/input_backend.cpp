// Input backend factory + InputMultiplexer implementation.

#include "input_backend.h"
#include "../../common/log.h"

// Forward declarations for each backend's factory.
InputBackend* CreateGlfwKeyboardBackend();
InputBackend* CreateXInputBackend();
InputBackend* CreateDualSenseInputBackend();
InputBackend* CreateSdlGameControllerBackend();

// ===========================================================================
// InputBackend::Create — factory
// ===========================================================================
InputBackend* InputBackend::Create(const std::string& backend_name) {
    if (backend_name == "keyboard" || backend_name == "glfw") {
        return CreateGlfwKeyboardBackend();
    }
    if (backend_name == "xinput") {
        return CreateXInputBackend();
    }
    if (backend_name == "dualsense" || backend_name == "ds" || backend_name == "hid") {
        return CreateDualSenseInputBackend();
    }
    if (backend_name == "sdl" || backend_name == "sdl_gamecontroller") {
        return CreateSdlGameControllerBackend();
    }
    if (backend_name == "null") {
        return new NullInputBackend();
    }
    // Auto-detect: prefer SDL, then XInput, then keyboard GLFW, then null.
    auto* xi = CreateXInputBackend();
    if (xi && xi->Initialize(0)) return xi;
    delete xi;

    auto* kb = CreateGlfwKeyboardBackend();
    if (kb && kb->Initialize(0)) return kb;
    delete kb;

    LOG_WARN(GPU, "IAL: no input backend available; using Null");
    return new NullInputBackend();
}

// ===========================================================================
// InputMultiplexer
// ===========================================================================
InputMultiplexer::InputMultiplexer() = default;
InputMultiplexer::~InputMultiplexer() = default;

void InputMultiplexer::AddBackend(InputBackend* backend) {
    if (m_count >= kMaxBackends || !backend) return;
    m_backends[m_count++] = backend;
}

void InputMultiplexer::RemoveBackend(InputBackend* backend) {
    for (int i = 0; i < m_count; ++i) {
        if (m_backends[i] == backend) {
            for (int j = i; j < m_count - 1; ++j) {
                m_backends[j] = m_backends[j + 1];
            }
            m_backends[--m_count] = nullptr;
            break;
        }
    }
}

ControllerState InputMultiplexer::PollMerged() {
    ControllerState merged{};

    // Poll each backend; later backends override earlier ones,
    // and DualSense-specific fields (touch, motion) are always
    // taken from the last backend that reports them.
    for (int i = 0; i < m_count; ++i) {
        if (!m_backends[i]) continue;
        ControllerState state{};
        if (!m_backends[i]->Poll(state)) continue;

        // Merge: first connected backend provides buttons/sticks
        if (!merged.connected && state.connected) {
            merged = state;
        } else if (state.connected) {
            // Override with later-connected controllers
            merged.buttons = state.buttons;
            merged.left_x = state.left_x;
            merged.left_y = state.left_y;
            merged.right_x = state.right_x;
            merged.right_y = state.right_y;
            merged.l2 = state.l2;
            merged.r2 = state.r2;
            // DualSense extras: always take touch/motion from whichever
            // backend has live data.
            if (state.dualsense_connected) {
                merged.touch_count = state.touch_count;
                for (int t = 0; t < 2; ++t) {
                    merged.touch_x[t] = state.touch_x[t];
                    merged.touch_y[t] = state.touch_y[t];
                    merged.touch_id[t] = state.touch_id[t];
                    merged.touch_active[t] = state.touch_active[t];
                }
                merged.accel_x = state.accel_x;
                merged.accel_y = state.accel_y;
                merged.accel_z = state.accel_z;
                merged.gyro_pitch = state.gyro_pitch;
                merged.gyro_yaw = state.gyro_yaw;
                merged.gyro_roll = state.gyro_roll;
                merged.dualsense_connected = true;
            }
            merged.connected = true;
            merged.timestamp_us = state.timestamp_us;
        }
    }

    return merged;
}

void InputMultiplexer::SetRumble(const RumbleState& rumble) {
    // Send rumble to the first connected backend that supports it.
    for (int i = 0; i < m_count; ++i) {
        if (!m_backends[i]) continue;
        InputCaps caps = m_backends[i]->GetCaps();
        if (caps.has_rumble) {
            m_backends[i]->SetRumble(rumble);
            break;
        }
    }
}

void InputMultiplexer::SetTriggerEffect(bool left, const TriggerEffect& effect) {
    for (int i = 0; i < m_count; ++i) {
        if (!m_backends[i]) continue;
        InputCaps caps = m_backends[i]->GetCaps();
        if (caps.has_haptics) {
            m_backends[i]->SetTriggerEffect(left, effect);
            break;
        }
    }
}
