// DualSense HID input backend — wraps the header-only DualSense HID
// reader (dualsense_hid.h) into the InputBackend interface.
//
// Provides touch-pad, gyro, accelerometer, and haptic feedback
// (adaptive triggers) that keyboard/XInput backends lack.

#include "input_backend.h"
#include "../dualsense_hid.h"
#include "../../common/log.h"

#include <cstring>

class DualSenseInputBackend : public InputBackend {
public:
    DualSenseInputBackend() = default;
    ~DualSenseInputBackend() override { Shutdown(); }

    bool Initialize(int /*controller_index*/) override {
        if (m_initialized) return true;

        // The DualSense reader starts on first GetSample() call,
        // but we pre-warm it here.
        GPU::DualSense::EnsureStarted();

        m_initialized = true;
        return true;
    }

    void Shutdown() override {
        m_initialized = false;
    }

    bool IsInitialized() const override { return m_initialized; }

    InputCaps GetCaps() const override {
        InputCaps caps;
        caps.backend_name = "DualSense HID";
        caps.has_rumble   = true;
        caps.has_touchpad = true;
        caps.has_motion   = true;
        caps.has_haptics  = true;   // adaptive triggers
        caps.max_controllers = 1;
        return caps;
    }

    bool Poll(ControllerState& out) override {
        if (!m_initialized) return false;

        GPU::DualSense::Sample sample;
        if (!GPU::DualSense::GetSample(sample)) {
            out = ControllerState{};
            out.connected = false;
            return false;
        }

        if (!sample.connected) {
            out = ControllerState{};
            out.connected = false;
            return true;   // successfully polled a disconnected state
        }

        out = ControllerState{};
        out.connected = true;
        out.dualsense_connected = true;
        out.buttons = sample.buttons;
        out.left_x  = sample.lx;
        out.left_y  = sample.ly;
        out.right_x = sample.rx;
        out.right_y = sample.ry;
        out.l2      = sample.l2;
        out.r2      = sample.r2;

        // Touch-pad.
        out.touch_count = sample.touch_count;
        for (int i = 0; i < 2 && i < sample.touch_count; ++i) {
            out.touch_x[i]      = sample.touch[i].x;
            out.touch_y[i]      = sample.touch[i].y;
            out.touch_id[i]     = sample.touch[i].id;
            out.touch_active[i] = sample.touch[i].active;
        }

        // Motion sensors.
        out.accel_x = sample.accel[0];
        out.accel_y = sample.accel[1];
        out.accel_z = sample.accel[2];
        out.gyro_pitch = sample.gyro[0];
        out.gyro_yaw   = sample.gyro[1];
        out.gyro_roll  = sample.gyro[2];

        // Timestamp (use steady clock, not the DualSense report timestamp).
        out.timestamp_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());

        return true;
    }

    void SetRumble(const RumbleState& rumble) override {
        if (!m_initialized) return;
        GPU::DualSense::SetRumble(rumble.large_motor, rumble.small_motor);
    }

    void SetTriggerEffect(bool left, const TriggerEffect& effect) override {
        if (!m_initialized) return;
        GPU::DualSense::SetTriggerEffect(left, effect.mode, effect.params);
    }

private:
    bool m_initialized = false;
};

// Factory
InputBackend* CreateDualSenseInputBackend() {
    return new DualSenseInputBackend();
}

// Also register in the factory if not using the Create() dispatch.
// The factory auto-probe in input_backend.cpp checks for XInput first;
// callers can explicitly request "dualsense" to use this backend.
