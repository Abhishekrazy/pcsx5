// DualSense controller access.
//
// Declarations only.  The implementation lives in dualsense_ds5w.cpp and is
// built on the vendored DualSenseWindows library (third_party/DualSenseWindows,
// MIT, by Ludwig Fuechsl) rather than on hand-derived HID report offsets.
//
// This header used to carry the whole implementation inline, with ~30
// namespace-scope `static` globals.  `static` at namespace scope has internal
// linkage, so every translation unit that included it received its OWN device
// handle, sample buffer, output state and reader thread.  The three consumers
// (libscepad, vulkan_backend, the input backend) were therefore talking to
// three unrelated copies: a SetRumble/SetLightBar/SetMicLed call from one TU
// wrote to a handle that another TU had opened, which is why controller output
// appeared to do nothing.  Keeping the state in one .cpp fixes that by
// construction.

#pragma once

#include "gpu.h"                 // GPU::PadTouchPoint
#include "../common/types.h"

namespace GPU {
namespace DualSense {

    // Latest parsed controller sample.  `connected` is false whenever no
    // DualSense report stream is live (no device, or it was unplugged).
    //
    // Layout preserved from the previous implementation so existing consumers
    // are unaffected.
    struct Sample {
        bool connected = false;
        u32 buttons = 0;              // SCE_PAD bitmask (same encoding as the XInput mapping)
        u8 lx = 128, ly = 128;        // 0..255, 128 centered
        u8 rx = 128, ry = 128;
        u8 l2 = 0, r2 = 0;            // 0..255
        u8 touch_count = 0;           // 0..2 fingers on the pad
        PadTouchPoint touch[2] = {};
        float accel[3] = { 0.0f, 0.0f, 0.0f }; // x/y/z, in g (approx)
        float gyro[3] = { 0.0f, 0.0f, 0.0f };  // pitch/yaw/roll, rad/s (approx)
        u8 battery_level = 0;         // 0..100 percent
        bool battery_charging = false;
        bool battery_full = false;
        bool headphone_connected = false;
        u8 trigger_feedback[2] = { 0, 0 }; // raw effect feedback, [0]=left [1]=right

        // Microphone audio is not exposed by the DualSenseWindows library, so
        // these stay at their defaults.  They are retained so the struct layout
        // and existing consumers are unchanged.
        bool mic_active = false;
        u8   mic_channels = 0;
        s16  mic_samples[4][8] = {};
    };

    // ---- lifecycle -------------------------------------------------------

    // Start the background reader once.  Safe to call repeatedly and from any
    // thread; enumeration and reconnection are handled internally.
    void EnsureStarted();

    // Stop the reader and release the device.  Safe if never started.
    void Shutdown();

    // Copy the most recent sample.  Returns false when no reader is running.
    // `out.connected` reports whether a device is actually present.
    bool GetSample(Sample& out);

    // Number of DualSense controllers currently enumerated (0 when none).
    // Only controller 0 is currently streamed; see
    // architecture/decisions/ADR-001-input-ownership.md.
    int GetDeviceCount();

    // ---- output ----------------------------------------------------------
    // All output calls are no-ops while no device is connected, and are applied
    // on the reader thread so they cannot race with a report read.

    void SetRumble(u8 large_motor, u8 small_motor);

    // `left` selects L2 (true) or R2 (false).  `mode` follows the previous
    // implementation: 0=off, 1=feedback, 2=weapon, 3=vibration.
    void SetTriggerEffect(bool left, u8 mode, const u8 params[10]);

    void SetLightBar(u8 r, u8 g, u8 b);

    // `bitmask` is the 5 player-indicator LED bits.
    void SetPlayerLeds(u8 bitmask, bool fade);

    // 0 = off, 1 = on, 2 = pulse.
    void SetMicLed(u8 mode);

    // `brightness`: 0 = high, 1 = medium, 2 = low.  `disabled` kills all LEDs.
    void SetLedOptions(u8 brightness, bool disabled);

    // ---- haptics / speaker audio over Bluetooth --------------------------

    // Stream 8-bit unsigned PCM to the controller's haptic actuators.
    //
    // Format is fixed by the protocol, not chosen by us: 3000 Hz, stereo,
    // interleaved (left, right, left, ...).  Blocks for the natural duration of
    // the audio, pacing one 64-byte report every ~10.7 ms, and returns false if
    // the controller is not connected over Bluetooth or a write fails.
    //
    // EXPERIMENTAL.  The report layout is INFERRED from the SAxense research
    // (MPL-2.0) and has not yet been confirmed against hardware; see
    // docs/audits/AUDIT-2026-09-05-dualsense-audio-over-bluetooth.md.
    bool PlayHapticsPcmBlocking(const u8* pcm, size_t bytes);

    // True when the open device is on Bluetooth, where the report above
    // applies.  Over USB the controller exposes real audio endpoints instead.
    // Speaker volume (0-255) and preamp gain for the Bluetooth audio lane,
    // applied by the next init-prime. Defaults are 0x50 and 0x02.
    void SetBluetoothAudioLevels(u8 speaker_volume, u8 preamp_gain);

    bool IsBluetooth();

    // Stream 16-bit signed PCM to the controller's speaker (or its headset jack)
    // over Bluetooth, as Opus in report 0x35.
    //
    // `pcm` is interleaved stereo at 48000 Hz and `frames` counts stereo frames,
    // not bytes. Blocks for the natural duration of the audio. Returns false if
    // the controller is not on Bluetooth, if the encoder cannot be configured,
    // or if a write fails.
    bool PlaySpeakerPcmBlocking(const s16* pcm, size_t frames, bool headset);

} // namespace DualSense
} // namespace GPU
