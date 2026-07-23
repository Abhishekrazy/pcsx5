// ButtonLayout — shared PlayStation controller button layout component.
//
// Renders a visual DualSense controller with:
//   - Button shapes in correct PlayStation positions
//   - Analog stick 2D position overlays
//   - L2/R2 trigger bars with analog travel
//   - Touchpad with live finger positions
//   - Gyro/Accel 3-axis readout
//   - "Tested" checklist per button
//
// Two renderers:
//   1. ImGuiRender(state) — draw into an ImGui window (dev tools)
//   2. WPF bindable data model (future) — for the launcher UI
//
// The ControllerState input is backend-agnostic, so this layout works
// with any input source (DualSense HID, XInput, SDL, GLFW keyboard).

#pragma once
#include "../common/types.h"
#include <cstdint>
#include <cstring>

// ── Button definitions ──────────────────────────────────────────────────
struct ButtonDef {
    uint32_t sce_pad_mask;  // SCE_PAD bitmask (0 = read from raw report)
    const char* name;        // "Cross", "Circle", ...
    float render_x;          // normalized X position (0..1) on the layout
    float render_y;          // normalized Y position (0..1)
    int   shape;             // 0=circle, 1=square, 2=triangle, 3=stick, 4=bar
};

// The standard PlayStation button layout positions.
constexpr int kButtonLayoutCount = 20;

inline const ButtonDef* GetButtonLayout() {
    static const ButtonDef kLayout[kButtonLayoutCount] = {
        // Action buttons (right cluster)
        { 0x4000,  "Cross",    0.75f, 0.68f, 0 },   // bottom
        { 0x2000,  "Circle",   0.82f, 0.58f, 0 },   // right
        { 0x8000,  "Square",   0.68f, 0.58f, 0 },   // left
        { 0x1000,  "Triangle", 0.75f, 0.48f, 0 },   // top

        // D-pad (left cluster)
        { 0x0010,  "Up",       0.20f, 0.48f, 0 },
        { 0x0040,  "Down",     0.20f, 0.68f, 0 },
        { 0x0080,  "Left",     0.13f, 0.58f, 0 },
        { 0x0020,  "Right",    0.27f, 0.58f, 0 },

        // Shoulder buttons
        { 0x0400,  "L1",       0.20f, 0.18f, 4 },
        { 0x0800,  "R1",       0.75f, 0.18f, 4 },
        { 0x0100,  "L2",       0.08f, 0.10f, 4 },
        { 0x0200,  "R2",       0.88f, 0.10f, 4 },

        // Stick clicks
        { 0x0002,  "L3",       0.33f, 0.58f, 3 },  // left stick press
        { 0x0004,  "R3",       0.62f, 0.58f, 3 },  // right stick press

        // Center buttons
        { 0,       "Create",   0.42f, 0.40f, 0 },
        { 0x0008,  "Options",  0.54f, 0.40f, 0 },
        { 0x10000, "PS",       0.48f, 0.30f, 0 },
        { 0x100000,"TouchPad", 0.48f, 0.38f, 0 },
        { 0,       "Mute",     0.35f, 0.28f, 0 },
        { 0x0400,  "L4",       0.10f, 0.25f, 0 },  // back paddle (if present)
    };
    return kLayout;
}

// ── Input state for rendering ───────────────────────────────────────────
struct ButtonLayoutState {
    // Current controller state (backend-agnostic).
    uint32_t buttons      = 0;         // SCE_PAD bitmask
    uint8_t  left_x       = 128;
    uint8_t  left_y       = 128;
    uint8_t  right_x      = 128;
    uint8_t  right_y      = 128;
    uint8_t  l2           = 0;
    uint8_t  r2           = 0;
    uint8_t  touch_count  = 0;
    uint16_t touch_x[2]   = {};
    uint16_t touch_y[2]   = {};
    float    accel[3]     = {};        // g
    float    gyro[3]      = {};        // rad/s
    bool     connected    = false;
    bool     dualsense_connected = false;

    // Battery info (from DualSense HID or estimated for other controllers).
    uint8_t  battery_level    = 0;    // 0..100 percent (0 = unknown)
    bool     battery_charging = false;
    bool     battery_full     = false;
    bool     headphone_connected = false;

    // Latched "tested" state per button (set true the first time pressed).
    bool tested[kButtonLayoutCount] = {};

    // Raw button read (for buttons without SCE_PAD bits like Create/Mute).
    bool raw_create = false;
    bool raw_mute   = false;

    // Update tested[] from current state.
    void UpdateTested() {
        for (int i = 0; i < kButtonLayoutCount; ++i) {
            if (IsPressed(i)) tested[i] = true;
        }
    }

    bool IsPressed(int idx) const {
        if (idx < 0 || idx >= kButtonLayoutCount) return false;
        const ButtonDef* layout = GetButtonLayout();
        uint32_t mask = layout[idx].sce_pad_mask;
        if (mask == 0) {
            // Raw-report-only buttons.
            switch (idx) {
                case 14: return raw_create;  // Create
                case 18: return raw_mute;    // Mute
                default: return false;
            }
        }
        return (buttons & mask) != 0;
    }
};

// ── ImGui renderer ──────────────────────────────────────────────────────
// Renders a DualSense controller layout in an ImGui window.
// Call from within an ImGui frame (e.g. in the dev tools or tester).
//
// Usage:
//   ButtonLayoutState state;
//   state.buttons = current_controller_state.buttons;
//   // ... fill state ...
//   ButtonLayoutRenderImGui("Controller", &state);

// Forward declaration — requires <imgui.h> to be included before calling.
// If IMGUI_VERSION is not defined, this function is a no-op stub.
#ifdef IMGUI_VERSION
void ButtonLayoutRenderImGui(const char* title, ButtonLayoutState* state);
#endif

// ── WPF data model (future) ─────────────────────────────────────────────
// struct ButtonLayoutViewModel {
//     bool cross_pressed, circle_pressed, ...;
//     float left_stick_x, left_stick_y, ...;
//     // Bindable by the WPF XAML view.
// };
