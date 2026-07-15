#pragma once
#include "../common/types.h"

namespace GPU {

    // Structure matching PlayStation's controller button state reporting format
    struct PadButtonState {
        u32 buttons;      // Bitmask of digital buttons pressed
        u8 left_analog_x; // 0 - 255 (128 is centered)
        u8 left_analog_y;
        u8 right_analog_x;
        u8 right_analog_y;
        u8 l2_trigger;    // 0 - 255
        u8 r2_trigger;
        u8 padding[2];
    };

    bool Initialize();
    void Shutdown();
    void RenderFrame(guest_addr_t framebuffer_addr = 0);
    void SetFramebufferConfig(u32 width, u32 height, u32 format);

    // Query whether the emulator execution window should close
    bool ShouldCloseWindow();

    // Poll keyboard inputs and update event states
    void PollEvents();

    // Retrieve current keyboard-mapped controller state
    PadButtonState GetCurrentPadState();

    // Keep the presentation window alive and responsive until the user closes it.
    // Used after a guest application exits cleanly or crashes.
    void RunIdleLoop();
}
// namespace GPU
