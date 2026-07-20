#pragma once
#include "../common/types.h"

namespace GPU {

    // One touch-pad finger sample (ScePadTouch semantics).
    struct PadTouchPoint {
        u16 x;      // 0..1919
        u16 y;      // 0..941
        u8 id;      // firmware-assigned finger id
        u8 active;  // 1 while the finger contacts the pad
    };

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

        // DualSense HID extras (M4).  All zero when no DualSense feed is live
        // — the keyboard/XInput fallback leaves these fields neutral.
        u8 dualsense_connected; // 1 when a DualSense HID report stream is live
        u8 touch_count;         // 0..2 fingers currently contacting the pad
        u8 pad_ext[2];
        PadTouchPoint touch[2]; // finger slots 0 and 1
        float accel[3];         // accelerometer x/y/z, in g (approx)
        float gyro[3];          // gyroscope pitch/yaw/roll, rad/s (approx)
    };

    bool Initialize();
    void Shutdown();
    void RenderFrame(guest_addr_t framebuffer_addr = 0);
    void SetFramebufferConfig(u32 width, u32 height, u32 format);

    // Query whether the emulator execution window should close
    bool ShouldCloseWindow();

    // True when a presentation window actually exists (false in headless
    // contexts such as unit tests — ShouldCloseWindow() must not be acted on
    // there, since it reports "close" whenever no window exists).
    bool HasWindow();

    // Pump the GLFW event queue.  GLFW event processing is only legal on the
    // thread that created the window (the process main thread), so this is
    // called exclusively from the main thread's window/message loop — never
    // from the guest worker thread.  No-op when no window exists.
    void PumpWindowEvents();

    // Poll keyboard + XInput controller inputs and refresh the pad state
    // returned by GetCurrentPadState().  Keyboard reads (glfwGetKey) are
    // main-thread-only in GLFW, so this is called from the main thread's
    // window/message loop; the guest thread only ever reads the published
    // state via GetCurrentPadState().  Does NOT process GLFW events.
    void PollEvents();

    // Retrieve current keyboard-mapped controller state
    PadButtonState GetCurrentPadState();

    // Drive the primary XInput controller's rumble motors (0..255 each).
    // When a DualSense is connected via the native HID path, its motors are
    // driven instead (HID output report); no-op with no controller.
    void SetPadVibration(u8 large_motor, u8 small_motor);

    // Set one DualSense adaptive-trigger effect via the HID output path.
    // `left` selects L2 (true) or R2 (false); mode/params follow the public
    // DualSense trigger-effect layout (0=off, 1=feedback, 2=weapon,
    // 3=vibration).  No-op when no DualSense is connected.
    void SetPadAdaptiveTrigger(bool left, u8 mode, const u8 params[10]);

    // Boot/loading screen status.  Reports a REAL boot milestone (the stage
    // currently executing) and renders it to the window: dark screen with the
    // stage text plus a determinate progress bar when `total` > 0.  Safe to
    // call from any thread (main thread during subsystem init/module load,
    // guest worker thread afterwards); rendering happens under a mutex via
    // the GDI DIB path, so stages posted while the main thread is blocked in
    // init/load still appear.  No-op once the first guest frame has been
    // presented (IsBootScreenActive() == false) or when no window exists.
    void SetBootStatus(const char* stage, int done = -1, int total = -1);

    // True while the boot screen is still displayed.  Flips to false on the
    // first presented guest frame (the game takes over the window).
    bool IsBootScreenActive();

    // Embedded mode (--embed): the presentation window is created hidden so
    // the launcher UI can reparent it into its own window.  Must be called
    // before Initialize().  The window handle is always printed to stdout as
    // PCSX5_WINDOW_HANDLE=<decimal HWND> right after creation.
    void SetEmbeddedMode(bool enabled);

    // Keep the presentation window alive and responsive until the user closes it.
    // Called from the main thread after the guest thread has finished (it
    // replaces the old guest-exit idle spin).
    void RunIdleLoop();
}
// namespace GPU
