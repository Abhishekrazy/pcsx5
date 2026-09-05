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
        // Raw sensor counts exactly as DualSenseWindows reports them -- NOT g or
        // rad/s, whatever the previous comment said. The scale per count is
        // UNKNOWN here and has not been measured; a consumer that needs physical
        // units must establish it rather than assume one. (The Input tab found
        // this out by graphing them as g: near-zero axes clamped into square
        // waves.)
        float accel[3] = { 0.0f, 0.0f, 0.0f }; // x/y/z, raw counts
        float gyro[3] = { 0.0f, 0.0f, 0.0f };  // pitch/yaw/roll, raw counts
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

        // Connection status, from the input report's status byte (offset 0x35
        // from the evaluator base). DualSenseWindows parses only the headphone
        // bit of it; these are read from the raw buffer alongside.
        //   0x01 headphones   0x02 mic jack   0x04 mic muted
        //   0x08 USB data     0x10 USB power
        // INFERRED from the DualSenseClient reference; the 0x08 bit is the one
        // DualSenseWindows labels "charging", so the two readings agree in
        // practice (USB data present implies charging) and disagree in name.
        bool mic_jack   = false;
        bool mic_muted  = false;
        bool usb_data   = false;
        bool usb_power  = false;
    };

    // Firmware and hardware identity, from feature report 0x20 (64 bytes).
    // Offsets INFERRED from the DualSenseClient reference and not yet confirmed
    // against hardware; the probe prints the raw report for that purpose.
    struct FirmwareInfo {
        bool valid = false;
        char build_date[12] = {};   // ASCII, e.g. "Jan 15 2024"
        char build_time[9]  = {};   // ASCII, e.g. "10:23:45"
        u16  firmware_type = 0;
        u16  software_series = 0;
        u32  hardware_info = 0;     // low 16 bits: model revision; bits 8..15: generation
        u32  main_version = 0;      // render as (v>>24).(v>>16 & 0xFF).(v & 0xFFFF)
        u16  update_version = 0;    // render as X.X in hex nibbles
        u32  sbl_version = 0;       // same rendering as main
        u32  dsp_version = 0;       // render as %04X_%04X
        u32  mcu_dsp_version = 0;   // same rendering as main
    };

    // Read feature report 0x20. Returns false when no device is open or the
    // report is not a valid 0x20 reply. Safe to call from any thread; it is a
    // synchronous HID feature read and takes a few milliseconds.
    bool ReadFirmwareInfo(FirmwareInfo& out);

    // Built-in tests for a shell to trigger without shipping PCM across the
    // ABI: a two-second tone to the speaker, a two-second buzz to the haptics.
    // Both block for their duration and return false if the lane is not
    // available (not on Bluetooth) or a write fails.
    bool PlaySpeakerTestBlocking();
    bool PlayHapticsTestBlocking();

    // ---- multiple controllers -------------------------------------------
    //
    // Up to kMaxPads controllers are streamed at once.  A pad keeps its index
    // for as long as it stays connected: slots are keyed by the HID device
    // path, so unplugging pad 1 does not renumber pad 2.  Every index-less
    // function above is pad 0, kept so existing callers need not change.
    constexpr int kMaxPads = 8;

    // Number of slots currently holding a connected controller.
    int  Count();

    // Per-pad forms.  An index outside [0, kMaxPads) or with no connected pad
    // returns false / does nothing; Sample.connected says which.
    bool GetSample(int index, Sample& out);
    bool IsBluetooth(int index);
    void SetRumble(int index, u8 large_motor, u8 small_motor);
    void SetTriggerEffect(int index, bool left, u8 mode, const u8 params[10]);
    void SetLightBar(int index, u8 r, u8 g, u8 b);
    void SetPlayerLeds(int index, u8 bitmask, bool fade);
    void SetMicLed(int index, u8 mode);
    void SetLedOptions(int index, u8 brightness, bool disabled);
    bool ReadFirmwareInfo(int index, FirmwareInfo& out);
    bool PlayHapticsPcmBlocking(int index, const u8* pcm, size_t bytes);
    bool PlaySpeakerPcmBlocking(int index, const s16* pcm, size_t frames, bool headset);
    bool PlaySpeakerTestBlocking(int index);
    bool PlayHapticsTestBlocking(int index);

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
