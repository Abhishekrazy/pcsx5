#pragma once
//
// Native DualSense (HID) reader for the emulator core (M4).
//
// Header-only on purpose: the CMake source list is frozen for this task, so
// the implementation lives here and is included by vulkan_backend.cpp (the
// only translation unit that consumes it).  Mirrors the C# launcher's
// src/ui_csharp/WindowsDualSenseReader.cs: enumerate Sony HID interfaces via
// HidD/SetupAPI (VID 0x054C, PID 0x0CE6 DualSense / 0x0DF2 DualSense Edge),
// read input reports on a background thread and parse buttons / sticks /
// triggers per the C# parser, extended with the touch fingers and gyro +
// accel blocks of the standard DualSense report layout.
//
// When no DualSense is attached the reader simply polls once a second and
// publishes a neutral, disconnected sample; callers fall back to XInput.

#include "gpu.h"
#include "../common/log.h"

#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>

#include <mutex>
#include <thread>
#include <vector>
#include <cstring>

#if defined(_MSC_VER)
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")
#endif

namespace GPU {
namespace DualSense {

    // Latest parsed controller sample.  `connected` is false whenever no
    // DualSense report stream is live (no device, or it was unplugged).
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
    };

    namespace detail {

        static constexpr USHORT kSonyVendorId         = 0x054C;
        static constexpr USHORT kDualSenseProductId   = 0x0CE6;
        static constexpr USHORT kDualSenseEdgeProductId = 0x0DF2;

        static std::mutex g_mutex;
        static Sample     g_sample;
        static std::once_flag g_start_once;

        // Last raw input report, kept for diagnostics (dualsense_test hex dump).
        static std::mutex g_raw_mutex;
        static u8    g_last_raw[256] = {};
        static DWORD g_last_raw_len = 0;

        // ── Output report state (Phase 6: haptics + adaptive triggers) ──────
        // The live device handle is owned by the reader thread; senders only
        // use it under g_out_mutex, and the reader clears it under the same
        // mutex BEFORE CloseHandle, so a WriteFile never races a close.
        struct TriggerEffect {
            u8 mode = 0x00;              // 0=off, 1=feedback, 2=weapon, 3=vibration
            u8 params[10] = {};
        };
        static std::mutex g_out_mutex;
        static HANDLE     g_out_handle = nullptr;   // nullptr when not connected
        static bool       g_out_writable = false;   // false when opened read-only
        static bool       g_out_bluetooth = false;  // transport, known after 1st input report
        static bool       g_out_transport_known = false;
        static u8         g_motor_large = 0, g_motor_small = 0;
        static TriggerEffect g_trigger[2];          // [0] = left (L2), [1] = right (R2)
        static u8         g_out_seq = 0;            // BT output sequence tag

        static void Publish(const Sample& s) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_sample = s;
        }

        // Open the first present DualSense HID interface, or nullptr when none
        // is attached.  Mirrors WindowsDualSenseReader.OpenDualSense().
        // `writable` reports whether GENERIC_WRITE was granted (the read-only
        // fallback can still feed input but cannot take output reports).
        static HANDLE OpenDualSense(bool* writable) {
            *writable = false;
            GUID hid_guid;
            HidD_GetHidGuid(&hid_guid);
            HDEVINFO devs = SetupDiGetClassDevsW(&hid_guid, nullptr, nullptr,
                                                 DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
            if (devs == INVALID_HANDLE_VALUE) {
                return nullptr;
            }

            HANDLE result = nullptr;
            for (DWORD index = 0;; ++index) {
                SP_DEVICE_INTERFACE_DATA if_data{};
                if_data.cbSize = sizeof(if_data);
                if (!SetupDiEnumDeviceInterfaces(devs, nullptr, &hid_guid, index, &if_data)) {
                    break;
                }
                DWORD needed = 0;
                SetupDiGetDeviceInterfaceDetailW(devs, &if_data, nullptr, 0, &needed, nullptr);
                if (needed == 0) {
                    continue;
                }
                std::vector<u8> buffer(needed);
                auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
                detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
                if (!SetupDiGetDeviceInterfaceDetailW(devs, &if_data, detail, needed, nullptr, nullptr)) {
                    continue;
                }
                const wchar_t* path = detail->DevicePath;

                // Probe VID/PID with a zero-access handle (same as the C# reader).
                HANDLE probe = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                           nullptr, OPEN_EXISTING, 0, nullptr);
                if (probe == INVALID_HANDLE_VALUE) {
                    continue;
                }
                HIDD_ATTRIBUTES attr{};
                attr.Size = sizeof(attr);
                const bool match = HidD_GetAttributes(probe, &attr) &&
                                   attr.VendorID == kSonyVendorId &&
                                   (attr.ProductID == kDualSenseProductId ||
                                    attr.ProductID == kDualSenseEdgeProductId);
                CloseHandle(probe);
                if (!match) {
                    continue;
                }

                HANDLE handle = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                                            nullptr, OPEN_EXISTING, 0, nullptr);
                bool rw = handle != INVALID_HANDLE_VALUE;
                if (!rw) {
                    handle = CreateFileW(path, GENERIC_READ,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE,
                                         nullptr, OPEN_EXISTING, 0, nullptr);
                }
                if (handle != INVALID_HANDLE_VALUE) {
                    *writable = rw;
                    result = handle;
                    break;
                }
            }

            SetupDiDestroyDeviceInfoList(devs);
            return result;
        }

        static int ReadS16LE(const u8* p) {
            return static_cast<int>(static_cast<short>(
                static_cast<unsigned short>(p[0] | (p[1] << 8))));
        }

        static void DecodeTouchPoint(const u8* p, PadTouchPoint& t) {
            // Per the DualSense report spec, bit 7 of byte 0 is the "not
            // touching" flag and the low 7 bits are the finger id.
            t.active = (p[0] & 0x80) == 0 ? 1 : 0;
            t.id = static_cast<u8>(p[0] & 0x7F);
            t.x = static_cast<u16>(p[1] | ((p[2] & 0x0F) << 8));
            t.y = static_cast<u16>((p[2] >> 4) | (p[3] << 4));
        }

        // Parse one input report.  Layout (o = 1 for USB report id 0x01,
        // o = 2 for Bluetooth report id 0x31), matching the C# parser for the
        // button/stick/trigger block and the Linux hid-playstation
        // dualsense_input_report layout for the motion/touch blocks:
        //   o+0..3  LX LY RX RY
        //   o+4     L2 analog, o+5 R2 analog
        //   o+6     report sequence tag
        //   o+7..10 button bytes 0..3
        //   o+11..14 reserved / timestamp
        //   o+15..20 gyro pitch/yaw/roll (s16 LE)
        //   o+21..26 accel x/y/z (s16 LE)
        //   o+27..30 sensor timestamp, o+31 reserved
        //   o+32..35 touch point 0, o+36..39 touch point 1
        static bool ParseReport(const u8* r, DWORD len, Sample& s) {
            size_t o;
            if (r[0] == 0x01) {
                o = 1;  // USB
            } else if (r[0] == 0x31) {
                o = 2;  // Bluetooth
            } else {
                return false;
            }
            if (len < o + 40 || len < 11) {
                return false;
            }

            s.lx = r[o + 0];
            s.ly = r[o + 1];
            s.rx = r[o + 2];
            s.ry = r[o + 3];
            s.l2 = r[o + 4];
            s.r2 = r[o + 5];
            const u8 b0 = r[o + 7];
            const u8 b1 = r[o + 8];
            const u8 b2 = r[o + 9];

            u32 b = 0;
            if (b0 & 0x10) b |= 0x8000; // Square
            if (b0 & 0x20) b |= 0x4000; // Cross
            if (b0 & 0x40) b |= 0x2000; // Circle
            if (b0 & 0x80) b |= 0x1000; // Triangle
            switch (b0 & 0x0F) { // dpad hat
                case 0: b |= 0x10; break;
                case 1: b |= 0x10 | 0x20; break;
                case 2: b |= 0x20; break;
                case 3: b |= 0x20 | 0x40; break;
                case 4: b |= 0x40; break;
                case 5: b |= 0x40 | 0x80; break;
                case 6: b |= 0x80; break;
                case 7: b |= 0x80 | 0x10; break;
                default: break; // 8 = centered, 9+ invalid
            }
            if (b1 & 0x01) b |= 0x0400; // L1
            if (b1 & 0x02) b |= 0x0800; // R1
            if (b1 & 0x04) b |= 0x0100; // L2 (digital)
            if (b1 & 0x08) b |= 0x0200; // R2 (digital)
            // b1 & 0x10 is Create — the SCE_PAD bitmask used here has no
            // defined Create bit, so it is intentionally not mapped.
            if (b1 & 0x20) b |= 0x0008; // Options
            if (b1 & 0x40) b |= 0x0002; // L3
            if (b1 & 0x80) b |= 0x0004; // R3
            if (b2 & 0x01) b |= 0x00010000; // PS (same bit as XInput Guide)
            if (b2 & 0x02) b |= 0x100000;   // Touch-pad click
            s.buttons = b;

            // Gyro full scale is +/-2000 dps; accel is ~8192 LSB/g.
            static constexpr float kDpsToRad = 3.14159265358979f / 180.0f;
            for (int i = 0; i < 3; ++i) {
                const int g_raw = ReadS16LE(r + o + 15 + i * 2);
                const int a_raw = ReadS16LE(r + o + 21 + i * 2);
                s.gyro[i] = static_cast<float>(g_raw) * (2000.0f / 32768.0f) * kDpsToRad;
                s.accel[i] = static_cast<float>(a_raw) / 8192.0f;
            }

            DecodeTouchPoint(r + o + 32, s.touch[0]);
            DecodeTouchPoint(r + o + 36, s.touch[1]);
            s.touch_count = static_cast<u8>((s.touch[0].active ? 1 : 0) +
                                            (s.touch[1].active ? 1 : 0));
            s.connected = true;
            return true;
        }

        // ── Output reports (Phase 6: rumble + adaptive triggers) ────────────
        // Publicly documented DualSense output layout (same as the C#
        // launcher's BuildOutputReportLocked, extended with the trigger
        // effect blocks):
        //   common payload (47 bytes):
        //     [0]    enable flags 0: 0x01 right motor, 0x02 left motor
        //     [1]    enable flags 1: 0x40 right trigger effect, 0x80 left
        //     [2]    right (small) motor, [3] left (large) motor
        //     [10]   right trigger mode, [11..20] right trigger params
        //     [21]   left trigger mode,  [22..31] left trigger params
        //   USB: 48-byte report { 0x02, common[47] }.
        //   Bluetooth: 78-byte report { 0x31, seq<<4, 0x10, common[47],
        //   crc32le } where crc32 is seeded with 0xA2 over bytes 0..73.
        static u32 Crc32Update(u32 crc, u8 value) {
            crc ^= value;
            for (int bit = 0; bit < 8; ++bit) {
                crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
            }
            return crc;
        }

        static u32 DualSenseCrc32(const u8* data, size_t len) {
            u32 crc = Crc32Update(0xFFFFFFFFu, 0xA2);
            for (size_t i = 0; i < len; ++i) {
                crc = Crc32Update(crc, data[i]);
            }
            return ~crc;
        }

        // Caller must hold g_out_mutex.
        static void SendOutputLocked() {
            if (g_out_handle == nullptr || !g_out_writable || !g_out_transport_known) {
                return;
            }

            u8 common[47] = {};
            common[0] = 0x03;              // enable both rumble motors
            common[1] = 0x40 | 0x80;       // enable right + left trigger effects
            common[2] = g_motor_small;
            common[3] = g_motor_large;
            common[10] = g_trigger[1].mode;  // right
            std::memcpy(common + 11, g_trigger[1].params, 10);
            common[21] = g_trigger[0].mode;  // left
            std::memcpy(common + 22, g_trigger[0].params, 10);

            DWORD written = 0;
            if (!g_out_bluetooth) {
                u8 report[48];
                report[0] = 0x02;
                std::memcpy(report + 1, common, sizeof(common));
                if (WriteFile(g_out_handle, report, sizeof(report), &written, nullptr)) {
                    return;
                }
            } else {
                u8 report[78];
                report[0] = 0x31;
                report[1] = static_cast<u8>((g_out_seq & 0x0F) << 4);
                g_out_seq = static_cast<u8>((g_out_seq + 1) & 0x0F);
                report[2] = 0x10;
                std::memcpy(report + 3, common, sizeof(common));
                const u32 crc = DualSenseCrc32(report, 74);
                report[74] = static_cast<u8>(crc);
                report[75] = static_cast<u8>(crc >> 8);
                report[76] = static_cast<u8>(crc >> 16);
                report[77] = static_cast<u8>(crc >> 24);
                if (WriteFile(g_out_handle, report, sizeof(report), &written, nullptr)) {
                    return;
                }
            }
            // Write failed (device unplugged mid-send): stop using the handle;
            // the reader's blocking ReadFile fails too and re-enumerates.
            LOG_WARN(GPU, "DualSense output report write failed; dropping output until reconnect.");
            g_out_handle = nullptr;
            g_out_transport_known = false;
        }

        static void ReadLoop() {
            bool announced = false;
            for (;;) {
                bool writable = false;
                HANDLE handle = OpenDualSense(&writable);
                if (handle == nullptr) {
                    Sample neutral;
                    Publish(neutral);
                    if (announced) {
                        LOG_INFO(GPU, "DualSense controller disconnected.");
                        announced = false;
                    }
                    Sleep(1000);
                    continue;
                }
                {
                    std::lock_guard<std::mutex> lock(g_out_mutex);
                    g_out_handle = handle;
                    g_out_writable = writable;
                    g_out_transport_known = false; // set on the first input report
                    g_out_bluetooth = false;
                }
                if (!announced) {
                    LOG_INFO(GPU, "DualSense controller connected (HID input reports active, output: %s).",
                             writable ? "rumble + adaptive triggers" : "unavailable (read-only handle)");
                    announced = true;
                }

                u8 buffer[256];
                for (;;) {
                    DWORD bytes = 0;
                    if (!ReadFile(handle, buffer, sizeof(buffer), &bytes, nullptr) || bytes == 0) {
                        break; // device unplugged or error — re-enumerate
                    }
                    Sample s;
                    if (ParseReport(buffer, bytes, s)) {
                        {
                            std::lock_guard<std::mutex> raw_lock(g_raw_mutex);
                            std::memcpy(g_last_raw, buffer, bytes < sizeof(g_last_raw) ? bytes : sizeof(g_last_raw));
                            g_last_raw_len = bytes < sizeof(g_last_raw) ? bytes : sizeof(g_last_raw);
                        }
                        // Report id identifies the transport (0x31 = BT),
                        // which decides the output report framing/CRC.
                        {
                            std::lock_guard<std::mutex> lock(g_out_mutex);
                            if (!g_out_transport_known) {
                                g_out_bluetooth = buffer[0] == 0x31;
                                g_out_transport_known = true;
                            }
                        }
                        Publish(s);
                    }
                }
                // Clear the send handle under the mutex BEFORE closing so a
                // concurrent SendOutputLocked can never race the close.
                {
                    std::lock_guard<std::mutex> lock(g_out_mutex);
                    if (g_out_handle == handle) {
                        g_out_handle = nullptr;
                        g_out_transport_known = false;
                    }
                }
                CloseHandle(handle);

                Sample neutral;
                Publish(neutral);
                if (announced) {
                    LOG_INFO(GPU, "DualSense controller disconnected.");
                    announced = false;
                }
                Sleep(1000);
            }
        }

    } // namespace detail

    // Start the background reader thread (idempotent).  The thread is
    // detached: it blocks in ReadFile/Sleep for the process lifetime.
    inline void EnsureStarted() {
        std::call_once(detail::g_start_once, [] {
            std::thread(detail::ReadLoop).detach();
        });
    }

    // Copy the latest sample.  Returns true when a DualSense is live.
    inline bool GetSample(Sample& out) {
        std::lock_guard<std::mutex> lock(detail::g_mutex);
        out = detail::g_sample;
        return out.connected;
    }

    // Copy the last raw input report for diagnostics.  Returns its length
    // (0 when no report has been received yet).
    inline size_t GetLastRawReport(u8* out, size_t capacity) {
        std::lock_guard<std::mutex> lock(detail::g_raw_mutex);
        const size_t n = detail::g_last_raw_len < capacity
                             ? detail::g_last_raw_len
                             : capacity;
        std::memcpy(out, detail::g_last_raw, n);
        return n;
    }

    // Transport query for diagnostics.  Sets `known` to false until the
    // first input report after connect identifies the framing; returns true
    // for Bluetooth, false for USB.
    inline bool GetTransport(bool& known) {
        std::lock_guard<std::mutex> lock(detail::g_out_mutex);
        known = detail::g_out_transport_known;
        return detail::g_out_bluetooth;
    }

    // Drive the rumble motors (0..255 each, matching ScePadVibrationParam).
    // No-op when no DualSense is connected or the handle is read-only.
    inline void SetRumble(u8 large_motor, u8 small_motor) {
        std::lock_guard<std::mutex> lock(detail::g_out_mutex);
        if (detail::g_motor_large == large_motor && detail::g_motor_small == small_motor) {
            return;
        }
        detail::g_motor_large = large_motor;
        detail::g_motor_small = small_motor;
        detail::SendOutputLocked();
    }

    // Set one adaptive-trigger effect.  `left` selects L2 (true) or R2
    // (false).  Modes per the public DualSense trigger documentation:
    //   0x00 off
    //   0x01 feedback:  params[0] = start position 0..9, params[1] = force 0..8
    //   0x02 weapon:    params[0] = start 2..7, params[1] = end >start..8,
    //                   params[2] = force 0..8
    //   0x03 vibration: params[0] = position 0..9, params[1] = amplitude 0..8,
    //                   params[2] = frequency 0..255
    // Params are clamped defensively; unknown modes are forced to off.
    // No-op when no DualSense is connected or the handle is read-only.
    inline void SetTriggerEffect(bool left, u8 mode, const u8 params[10]) {
        std::lock_guard<std::mutex> lock(detail::g_out_mutex);
        detail::TriggerEffect& t = detail::g_trigger[left ? 0 : 1];
        t.mode = mode <= 0x03 ? mode : static_cast<u8>(0x00);
        for (int i = 0; i < 10; ++i) {
            t.params[i] = params[i];
        }
        auto clamp = [](u8 v, u8 lo, u8 hi) -> u8 {
            return v < lo ? lo : (v > hi ? hi : v);
        };
        switch (t.mode) {
            case 0x01: // feedback
                t.params[0] = clamp(t.params[0], 0, 9);
                t.params[1] = clamp(t.params[1], 0, 8);
                break;
            case 0x02: // weapon
                t.params[0] = clamp(t.params[0], 2, 7);
                t.params[1] = clamp(t.params[1], static_cast<u8>(t.params[0] + 1), 8);
                t.params[2] = clamp(t.params[2], 0, 8);
                break;
            case 0x03: // vibration
                t.params[0] = clamp(t.params[0], 0, 9);
                t.params[1] = clamp(t.params[1], 0, 8);
                break;
            default: // off
                break;
        }
        detail::SendOutputLocked();
    }

} // namespace DualSense
} // namespace GPU
