// Input Abstraction Layer (IAL) — abstract interface for controller input.
//
// Every input backend (GLFW keyboard, XInput, SDL GameController, raw HID
// DualSense, null) implements this interface.  The libScePad and the GPU
// event loop access input exclusively through these types.
//
// Multiple backends can be active simultaneously via the InputMultiplexer
// (e.g. keyboard + SDL controller + DualSense HID extras merged into one
// pad state).

#pragma once
#include "../../common/types.h"
#include <cstdint>
#include <string>

// ---------------------------------------------------------------------------
// Controller state — mirrors the PS5 ScePadData layout (0x78 bytes).
// All backends produce this canonical format.
// ---------------------------------------------------------------------------
struct ControllerState {
    // Digital buttons (SCE_PAD_BUTTON bitmask).
    uint32_t buttons = 0;

    // Analog sticks (0..255, 128 = center).
    uint8_t left_x = 128, left_y = 128;
    uint8_t right_x = 128, right_y = 128;

    // Analog triggers (0..255).
    uint8_t l2 = 0, r2 = 0;

    // Touch-pad.
    uint8_t touch_count = 0;  // 0..2 fingers
    uint16_t touch_x[2] = {};
    uint16_t touch_y[2] = {};
    uint8_t  touch_id[2] = {};
    uint8_t  touch_active[2] = {};

    // Motion sensors.
    float accel_x = 0.0f, accel_y = 0.0f, accel_z = 0.0f;  // g
    float gyro_pitch = 0.0f, gyro_yaw = 0.0f, gyro_roll = 0.0f;  // rad/s

    // Connection status.
    bool connected = false;
    bool dualsense_connected = false;  // DualSense HID extras live

    // Timestamp (microseconds, steady clock).
    uint64_t timestamp_us = 0;
};

// ---------------------------------------------------------------------------
// Rumble / haptics
// ---------------------------------------------------------------------------
struct RumbleState {
    uint8_t large_motor = 0;   // 0..255
    uint8_t small_motor = 0;   // 0..255
};

// DualSense trigger effect (mode + 10 parameter bytes).
struct TriggerEffect {
    uint8_t mode = 0;   // 0=off, 1=feedback, 2=weapon, 3=vibration
    uint8_t params[10] = {};
};

// ---------------------------------------------------------------------------
// Backend info
// ---------------------------------------------------------------------------
struct InputCaps {
    std::string backend_name;       // "XInput", "SDL", "GLFW", "DualSense", "Null"
    bool has_rumble      = false;
    bool has_touchpad    = false;
    bool has_motion      = false;
    bool has_haptics     = false;   // adaptive triggers
    int  max_controllers = 1;       // connected simultaneously
};

// ---------------------------------------------------------------------------
// InputBackend interface
// ---------------------------------------------------------------------------
class InputBackend {
public:
    virtual ~InputBackend() = default;

    // ---- lifecycle -------------------------------------------------------
    // `controller_index` selects which controller to poll (0 = first).
    virtual bool Initialize(int controller_index = 0) = 0;
    virtual void Shutdown() = 0;
    virtual bool IsInitialized() const = 0;

    // ---- capabilities ----------------------------------------------------
    virtual InputCaps GetCaps() const = 0;

    // ---- polling ---------------------------------------------------------
    // Poll the device and write the current state into `out`.
    // Returns false if the device is disconnected or an error occurred.
    virtual bool Poll(ControllerState& out) = 0;

    // ---- output (haptics / feedback) -------------------------------------
    // Set rumble motor speeds.  No-op on backends without rumble.
    virtual void SetRumble(const RumbleState& rumble) = 0;

    // Set DualSense adaptive trigger effect.  `left` = L2 (true) or R2 (false).
    virtual void SetTriggerEffect(bool left, const TriggerEffect& effect) = 0;

    // ---- factory ---------------------------------------------------------
    static InputBackend* Create(const std::string& backend_name);
};

// ---------------------------------------------------------------------------
// InputMultiplexer — merges multiple backends into one controller state.
//
// Priority order (first wins for each field):
//   1. DualSense HID (touch, motion, gyro)
//   2. XInput / SDL (buttons, sticks, triggers, rumble)
//   3. GLFW keyboard (buttons only, mapped from keyboard)
// ---------------------------------------------------------------------------
class InputMultiplexer {
public:
    InputMultiplexer();
    ~InputMultiplexer();

    // Add a backend.  Ownership stays with the caller; the multiplexer
    // only stores a pointer.  Backends are polled in add-order.
    void AddBackend(InputBackend* backend);

    // Remove a backend (by pointer identity).
    void RemoveBackend(InputBackend* backend);

    // Poll every registered backend and merge states into `out`.
    // Returns the state of the first connected backend merged with
    // touch/motion from any DualSense backend.
    ControllerState PollMerged();

    // Forward rumble to the best backend (DualSense > XInput/SDL > GLFW).
    void SetRumble(const RumbleState& rumble);

    // Forward trigger effect to the DualSense backend if present.
    void SetTriggerEffect(bool left, const TriggerEffect& effect);

private:
    static constexpr int kMaxBackends = 8;
    InputBackend* m_backends[kMaxBackends] = {};
    int m_count = 0;
};

// ---------------------------------------------------------------------------
// Null backend (always-disconnected placeholder)
// ---------------------------------------------------------------------------
class NullInputBackend : public InputBackend {
public:
    bool Initialize(int) override { return true; }
    void Shutdown() override {}
    bool IsInitialized() const override { return true; }
    InputCaps GetCaps() const override { return InputCaps{"Null"}; }
    bool Poll(ControllerState& out) override { out = ControllerState{}; return false; }
    void SetRumble(const RumbleState&) override {}
    void SetTriggerEffect(bool, const TriggerEffect&) override {}
};
