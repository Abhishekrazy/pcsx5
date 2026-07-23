// GLFW keyboard input backend — maps keyboard keys to PS5 controller
// buttons via GLFW's glfwGetKey().
//
// Static functions: no per-instance state.  GLFW state is process-wide;
// the GLFW window handle is stored separately.  This backend does not
// manage the window; it only polls key state against the existing window.

#include "input_backend.h"
#include "../../common/log.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <cstdint>
#include <chrono>

class GlfwKeyboardBackend : public InputBackend {
public:
    GlfwKeyboardBackend() = default;
    ~GlfwKeyboardBackend() override { Shutdown(); }

    bool Initialize(int /*controller_index*/) override {
        // GLFW must already be initialized by the window/GPU layer.
        m_initialized = true;
        return true;
    }

    void Shutdown() override {
        m_initialized = false;
    }

    bool IsInitialized() const override { return m_initialized; }

    InputCaps GetCaps() const override {
        InputCaps caps;
        caps.backend_name = "GLFW Keyboard";
        caps.has_rumble = false;
        caps.has_touchpad = false;
        caps.has_motion = false;
        caps.has_haptics = false;
        caps.max_controllers = 1;
        return caps;
    }

    bool Poll(ControllerState& out) override {
        if (!m_initialized) return false;

        GLFWwindow* window = glfwGetCurrentContext();
        if (!window) return false;

        out = ControllerState{};
        out.connected = true;
        out.timestamp_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());

        // Digital buttons.
        if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS ||
            glfwGetKey(window, GLFW_KEY_UP) == GLFW_PRESS)
            out.buttons |= 0x00000010; // PAD_UP
        if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS ||
            glfwGetKey(window, GLFW_KEY_DOWN) == GLFW_PRESS)
            out.buttons |= 0x00000040; // PAD_DOWN
        if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS ||
            glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_PRESS)
            out.buttons |= 0x00000080; // PAD_LEFT
        if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS ||
            glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS)
            out.buttons |= 0x00000020; // PAD_RIGHT

        if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS ||
            glfwGetKey(window, GLFW_KEY_Z) == GLFW_PRESS)
            out.buttons |= 0x00004000; // PAD_CROSS
        if (glfwGetKey(window, GLFW_KEY_ENTER) == GLFW_PRESS ||
            glfwGetKey(window, GLFW_KEY_X) == GLFW_PRESS)
            out.buttons |= 0x00002000; // PAD_CIRCLE
        if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
            glfwGetKey(window, GLFW_KEY_V) == GLFW_PRESS)
            out.buttons |= 0x00008000; // PAD_SQUARE
        if (glfwGetKey(window, GLFW_KEY_C) == GLFW_PRESS)
            out.buttons |= 0x00001000; // PAD_TRIANGLE

        if (glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS)
            out.buttons |= 0x00000400; // PAD_L1
        if (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS)
            out.buttons |= 0x00000800; // PAD_R1
        if (glfwGetKey(window, GLFW_KEY_R) == GLFW_PRESS)
            out.buttons |= 0x00000100; // PAD_L2 (digital)
        if (glfwGetKey(window, GLFW_KEY_F) == GLFW_PRESS)
            out.buttons |= 0x00000200; // PAD_R2 (digital)

        if (glfwGetKey(window, GLFW_KEY_TAB) == GLFW_PRESS)
            out.buttons |= 0x00000008; // PAD_OPTIONS
        if (glfwGetKey(window, GLFW_KEY_GRAVE_ACCENT) == GLFW_PRESS)
            out.buttons |= 0x00200000; // PAD_TOUCHPAD

        // Analog sticks via keyboard (wasd-style arrow clusters).
        if (glfwGetKey(window, GLFW_KEY_I) == GLFW_PRESS) out.left_y = 0;
        if (glfwGetKey(window, GLFW_KEY_K) == GLFW_PRESS) out.left_y = 255;
        if (glfwGetKey(window, GLFW_KEY_J) == GLFW_PRESS) out.left_x = 0;
        if (glfwGetKey(window, GLFW_KEY_L) == GLFW_PRESS) out.left_x = 255;

        return true;
    }

    void SetRumble(const RumbleState& /*rumble*/) override {
        // Keyboard has no rumble.
    }

    void SetTriggerEffect(bool /*left*/, const TriggerEffect& /*effect*/) override {
        // No trigger effects on keyboard.
    }

private:
    bool m_initialized = false;
};

// Factory
InputBackend* CreateGlfwKeyboardBackend() {
    return new GlfwKeyboardBackend();
}
