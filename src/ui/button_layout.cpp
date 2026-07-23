// ButtonLayout — ImGui renderer implementation.
//
// Renders a PlayStation controller button layout with live input state.
// Requires Dear ImGui to be available at compile time.

#include "button_layout.h"

#ifdef IMGUI_VERSION

#include <imgui.h>
#include <cmath>
#include <cstdio>
#include <cstring>

// ── Drawing helpers ─────────────────────────────────────────────────────

static void DrawCircleButton(ImDrawList* dl, ImVec2 center, float radius,
                              bool pressed, bool tested, const char* label,
                              ImU32 color) {
    ImU32 bg = pressed ? color : IM_COL32(80, 80, 80, 200);
    dl->AddCircleFilled(center, radius, bg, 32);
    if (!pressed) {
        dl->AddCircle(center, radius, IM_COL32(160, 160, 160, 100), 32, 2.0f);
    }
    // Tested indicator
    if (tested) {
        dl->AddCircle(center, radius * 0.3f, IM_COL32(0, 255, 0, 180), 12, 2.0f);
    }
    ImVec2 text_size = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(center.x - text_size.x * 0.5f,
                        center.y - text_size.y * 0.5f),
                IM_COL32(255, 255, 255, pressed ? 255 : 160), label);
}

static void DrawStickOverlay(ImDrawList* dl, ImVec2 center, float radius,
                              uint8_t x, uint8_t y, bool pressed,
                              const char* label) {
    // Base circle
    dl->AddCircleFilled(center, radius, IM_COL32(60, 60, 60, 200), 32);
    dl->AddCircle(center, radius, IM_COL32(120, 120, 120, 150), 32, 2.0f);

    // Stick position indicator (normalized: 128,128 = center)
    float nx = (static_cast<float>(x) - 128.0f) / 128.0f;
    float ny = (static_cast<float>(y) - 128.0f) / 128.0f;
    ImVec2 pos = ImVec2(center.x + nx * radius * 0.7f,
                         center.y + ny * radius * 0.7f);
    ImU32 stick_color = pressed ? IM_COL32(255, 80, 80, 255)
                                : IM_COL32(200, 200, 200, 255);
    dl->AddCircleFilled(pos, radius * 0.35f, stick_color, 16);

    // Crosshair
    dl->AddLine(ImVec2(center.x - radius, center.y),
                ImVec2(center.x + radius, center.y),
                IM_COL32(100, 100, 100, 80), 1.0f);
    dl->AddLine(ImVec2(center.x, center.y - radius),
                ImVec2(center.x, center.y + radius),
                IM_COL32(100, 100, 100, 80), 1.0f);

    // Label
    ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(center.x - ts.x * 0.5f, center.y + radius + 4),
                IM_COL32(200, 200, 200, 200), label);
}

static void DrawTriggerBar(ImDrawList* dl, ImVec2 pos, float width,
                            float height, uint8_t value, bool pressed,
                            const char* label) {
    // Background
    dl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height),
                      IM_COL32(50, 50, 50, 200), 4.0f);
    // Value bar
    float fill = (static_cast<float>(value) / 255.0f) * height;
    ImU32 bar_color = pressed ? IM_COL32(255, 100, 50, 255)
                              : IM_COL32(100, 200, 255, 200);
    dl->AddRectFilled(ImVec2(pos.x + 1, pos.y + height - fill),
                      ImVec2(pos.x + width - 1, pos.y + height),
                      bar_color, 2.0f);
    // Label
    ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(pos.x + width * 0.5f - ts.x * 0.5f,
                        pos.y - ts.y - 2),
                IM_COL32(200, 200, 200, 200), label);
    // Value text
    char val[8];
    std::snprintf(val, sizeof(val), "%d", value);
    ImVec2 vs = ImGui::CalcTextSize(val);
    dl->AddText(ImVec2(pos.x + width * 0.5f - vs.x * 0.5f,
                        pos.y + 2),
                IM_COL32(255, 255, 255, 200), val);
}

static void DrawTouchpad(ImDrawList* dl, ImVec2 pos, float width,
                          float height, uint8_t touch_count,
                          const uint16_t touch_x[2],
                          const uint16_t touch_y[2]) {
    // Touchpad surface (aspect ratio ~2:1, matching DualSense touchpad)
    dl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height),
                      IM_COL32(30, 30, 40, 200), 4.0f);
    dl->AddRect(pos, ImVec2(pos.x + width, pos.y + height),
                IM_COL32(100, 100, 120, 100), 4.0f, 0, 1.0f);

    // Normalize touch coordinates (0..1919 → 0..width, 0..941 → 0..height)
    for (int i = 0; i < 2 && i < touch_count; ++i) {
        float tx = pos.x + (static_cast<float>(touch_x[i]) / 1919.0f) * width;
        float ty = pos.y + (static_cast<float>(touch_y[i]) / 941.0f) * height;
        dl->AddCircleFilled(ImVec2(tx, ty), 8.0f,
                            IM_COL32(255, 200, 50, 220), 12);
        dl->AddCircle(ImVec2(tx, ty), 12.0f,
                      IM_COL32(255, 200, 50, 80), 12, 2.0f);
    }

    const char* label = "Touch Pad";
    ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(pos.x + width * 0.5f - ts.x * 0.5f,
                        pos.y + height * 0.5f - ts.y * 0.5f),
                IM_COL32(100, 100, 120, 150), label);
}

static void DrawGyroAccel(ImDrawList* dl, ImVec2 pos, float width,
                           const float accel[3], const float gyro[3]) {
    // Show 3-axis readout for accel and gyro
    char line[128];
    ImGui::PushFont(ImGui::GetFont());

    float y = pos.y;
    const float line_height = ImGui::GetTextLineHeight() + 2;

    // Accel (in g)
    float ax = accel[0], ay = accel[1], az = accel[2];
    float mag = std::sqrt(ax * ax + ay * ay + az * az);
    std::snprintf(line, sizeof(line), "Accel  X: %+.2f  Y: %+.2f  Z: %+.2f  |%.2f g|",
                  ax, ay, az, mag);
    dl->AddText(ImVec2(pos.x, y), IM_COL32(0, 255, 200, 220), line);
    y += line_height;

    // Gyro (deg/s — convert from rad/s)
    float gx = gyro[0] * 57.2958f;
    float gy = gyro[1] * 57.2958f;
    float gz = gyro[2] * 57.2958f;
    std::snprintf(line, sizeof(line), "Gyro   X: %+.0f  Y: %+.0f  Z: %+.0f  deg/s",
                  gx, gy, gz);
    dl->AddText(ImVec2(pos.x, y), IM_COL32(255, 200, 0, 220), line);
    y += line_height;

    // Connection status
    const char* status = "Disconnected";
    ImU32 status_color = IM_COL32(255, 50, 50, 220);
    if (touch_count > 0) {
        // If we have touch data but no accel variation, it's a limited device
        status = "Connected (limited)";
        status_color = IM_COL32(255, 200, 50, 220);
    }
    if (mag > 0.5f && mag < 4.0f) {
        status = "Connected (DualSense)";
        status_color = IM_COL32(50, 255, 50, 220);
    }
    std::snprintf(line, sizeof(line), "Status: %s", status);
    dl->AddText(ImVec2(pos.x, y), status_color, line);
    y += line_height;

    // Battery info placeholder
    std::snprintf(line, sizeof(line), "Touch: %d finger(s)", touch_count);
    dl->AddText(ImVec2(pos.x, y), IM_COL32(200, 200, 200, 180), line);

    ImGui::PopFont();
}

// ── Main render function ─────────────────────────────────────────────────

void ButtonLayoutRenderImGui(const char* title, ButtonLayoutState* state) {
    if (!state) return;

    ImGui::SetNextWindowSize(ImVec2(480, 640), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title, nullptr, ImGuiWindowFlags_NoScrollbar)) {
        ImGui::End();
        return;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float win_w = ImGui::GetContentRegionAvail().x;
    if (win_w < 400) win_w = 400;

    // Scale factor: the layout is designed at 400px width.
    float scale = win_w / 400.0f;
    float layout_w = 400.0f * scale;
    float layout_h = 500.0f * scale;

    // ── Draw controller body outline ────────────────────────────────────
    ImVec2 body_tl = ImVec2(origin.x, origin.y);
    ImVec2 body_br = ImVec2(origin.x + layout_w, origin.y + layout_h);
    dl->AddRectFilled(body_tl, body_br, IM_COL32(40, 40, 50, 230), 12.0f);
    dl->AddRect(body_tl, body_br, IM_COL32(80, 80, 100, 150), 12.0f, 0, 2.0f);

    // ── Map normalized positions to pixel coords ────────────────────────
    auto to_pixel = [&](float nx, float ny) -> ImVec2 {
        return ImVec2(origin.x + nx * layout_w, origin.y + ny * layout_h);
    };

    // ── Render buttons ──────────────────────────────────────────────────
    const ButtonDef* layout = GetButtonLayout();
    for (int i = 0; i < kButtonLayoutCount; ++i) {
        const ButtonDef& b = layout[i];
        ImVec2 center = to_pixel(b.render_x, b.render_y);
        bool pressed = state->IsPressed(i);
        bool tested = state->tested[i];
        float r = 16.0f * scale;

        if (b.shape == 4) {
            // Trigger/L shoulder buttons — draw as small bars
            float tw = 50.0f * scale;
            float th = 10.0f * scale;
            uint8_t val = 0;
            if (std::strcmp(b.name, "L2") == 0) val = state->l2;
            else if (std::strcmp(b.name, "R2") == 0) val = state->r2;
            DrawTriggerBar(dl,
                           ImVec2(center.x - tw * 0.5f, center.y - th * 0.5f),
                           tw, th, val, pressed, b.name);
        } else if (b.shape == 3) {
            // Stick click (L3/R3) — show stick position
            uint8_t sx = 128, sy = 128;
            if (std::strcmp(b.name, "L3") == 0) { sx = state->left_x; sy = state->left_y; }
            else if (std::strcmp(b.name, "R3") == 0) { sx = state->right_x; sy = state->right_y; }
            DrawStickOverlay(dl, center, 28.0f * scale, sx, sy, pressed, b.name);
        } else {
            // Standard face button
            ImU32 color = IM_COL32(100, 200, 255, 220); // default blue
            if (std::strcmp(b.name, "Cross") == 0) color = IM_COL32(100, 200, 255, 220);
            else if (std::strcmp(b.name, "Circle") == 0) color = IM_COL32(255, 150, 100, 220);
            else if (std::strcmp(b.name, "Square") == 0) color = IM_COL32(255, 150, 200, 220);
            else if (std::strcmp(b.name, "Triangle") == 0) color = IM_COL32(150, 255, 150, 220);
            DrawCircleButton(dl, center, r, pressed, tested, b.name, color);
        }
    }

    // ── Touchpad ────────────────────────────────────────────────────────
    ImVec2 tp_pos = to_pixel(0.20f, 0.78f);
    float tp_w = 240.0f * scale;
    float tp_h = 60.0f * scale;
    DrawTouchpad(dl, tp_pos, tp_w, tp_h,
                 state->touch_count, state->touch_x, state->touch_y);

    // ── Gyro/Accel readout below touchpad ───────────────────────────────
    ImVec2 info_pos = to_pixel(0.02f, 0.90f);
    float info_w = layout_w * 0.96f;
    DrawGyroAccel(dl, info_pos, info_w,
                  state->accel, state->gyro);

    ImGui::End();
}

#else // IMGUI_VERSION not defined — stub

void ButtonLayoutRenderImGui(const char*, ButtonLayoutState*) {
    // No-op: ImGui not available.
}

#endif // IMGUI_VERSION
