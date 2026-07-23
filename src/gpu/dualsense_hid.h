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
#include <atomic>

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
        u8 battery_level = 0;         // 0..100 percent
        bool battery_charging = false;
        bool battery_full = false;
        bool headphone_connected = false;
        u8 trigger_feedback[2] = { 0, 0 }; // raw effect feedback, [0]=left [1]=right
        // Mic audio: 4 channels × 8 samples × 16-bit PCM captured from the
        // directional mic array on DualSense.  Only present when the firmware
        // is actively streaming mic audio (bit 0x04 in the status byte, offset
        // o+53).  Data offset: o+74 for standard DualSense input (USB & BT).
        bool mic_active = false;      // true when mic data is present in report
        u8   mic_channels = 0;        // 1..4 active mic channels
        s16  mic_samples[4][8] = {};  // [channel][sample] PCM16 data
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

        // Report layout detected by ParseReport: 1 = standard DualSense,
        // 2 = DS4-style (DualShock 4 and most DualSense clones/compatibles;
        // such clones report with id 0x01 even over Bluetooth).
        static std::atomic<int> g_last_layout{ 1 };
        // Identity of the currently opened device (for diagnostics).
        static USHORT g_dev_vid = 0, g_dev_pid = 0;
        static wchar_t g_dev_product[128] = {};
        // Consecutive wins for the candidate layout (hysteresis so a single
        // ambiguous report cannot flip the offsets back and forth).
        static int g_layout_streak = 0;
        // Absolute offset of button byte 0 (face buttons + d-pad hat) in the
        // last parsed report (layout-dependent).
        static std::atomic<size_t> g_b0_pos{ 8 };

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
        static u8         g_player_leds = 0;        // 5 player-indicator LED bits
        static bool       g_player_led_fade = false;
        static u8         g_led_brightness = 0x01;  // 0=high 1=medium 2=low
        static bool       g_leds_disabled = false;  // kill switch for all LEDs
        static u8         g_mic_led = 0;            // 0=off 1=on 2=pulse
        static u8         g_lightbar[3] = { 0, 0, 0 }; // RGB
        static u8         g_out_seq = 0;            // BT output sequence tag
        // Device's OutputReportByteLength from the HID caps: Windows HID
        // stacks commonly require WriteFile sizes to match it (the
        // DualSense-Windows library pads BT writes to 547 for this reason).
        static DWORD      g_out_report_len = 0;

        // OutputReportByteLength for an open handle, or 0 when unknown.
        static DWORD QueryOutputReportLength(HANDLE handle) {
            PHIDP_PREPARSED_DATA pp = nullptr;
            if (!HidD_GetPreparsedData(handle, &pp)) {
                return 0;
            }
            HIDP_CAPS caps{};
            const NTSTATUS status = HidP_GetCaps(pp, &caps);
            HidD_FreePreparsedData(pp);
            return status == HIDP_STATUS_SUCCESS
                       ? static_cast<DWORD>(caps.OutputReportByteLength)
                       : 0;
        }

        static void Publish(const Sample& s) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_sample = s;
        }

        // Open the first present DualSense HID interface, or nullptr when none
        // is attached.  Mirrors WindowsDualSenseReader.OpenDualSense() plus
        // the DualSense-Windows capability filter: a DualSense exposes
        // multiple HID interfaces and only the ones whose
        // InputReportByteLength is 64 (USB) or 78 (Bluetooth) carry the
        // controller report stream — opening any other matching interface
        // yields garbage (wrong layout, zeroed motion).  `bluetooth` reports
        // the transport from the caps (78-byte input = BT), which is more
        // reliable than sniffing the report id.
        // `writable` reports whether GENERIC_WRITE was granted (the read-only
        // fallback can still feed input but cannot take output reports).
        static HANDLE OpenDualSense(bool* writable, bool* bluetooth) {
            *writable = false;
            *bluetooth = false;
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
                wchar_t product[128] = {};
                const bool have_attr = HidD_GetAttributes(probe, &attr) != 0;
                HidD_GetProductString(probe, product, sizeof(product));
                const bool match = have_attr &&
                                   attr.VendorID == kSonyVendorId &&
                                   (attr.ProductID == kDualSenseProductId ||
                                    attr.ProductID == kDualSenseEdgeProductId);
                // Capability filter (DualSense-Windows does the same): the
                // real controller interface has a 64-byte (USB) or 78-byte
                // (Bluetooth) input report; skip any other interface the
                // device exposes.
                bool caps_ok = false;
                bool caps_bt = false;
                if (match) {
                    PHIDP_PREPARSED_DATA ppd = nullptr;
                    if (HidD_GetPreparsedData(probe, &ppd)) {
                        HIDP_CAPS caps{};
                        if (HidP_GetCaps(ppd, &caps) == HIDP_STATUS_SUCCESS) {
                            if (caps.InputReportByteLength == 64) {
                                caps_ok = true;
                            } else if (caps.InputReportByteLength == 78) {
                                caps_ok = true;
                                caps_bt = true;
                            }
                        }
                        HidD_FreePreparsedData(ppd);
                    }
                }
                CloseHandle(probe);
                if (!match || !caps_ok) {
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
                    *bluetooth = caps_bt;
                    result = handle;
                    g_dev_vid = attr.VendorID;
                    g_dev_pid = attr.ProductID;
                    wcsncpy_s(g_dev_product, product, _TRUNCATE);
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

        // Parse one input report.  Two layouts are supported and detected per
        // report by scoring three anchors — a valid d-pad hat nibble (<= 8),
        // ~1 g of accel (gravity), and the "not touching" bit of touch
        // finger 0:
        //
        //   Standard DualSense (o = 1 for USB id 0x01, o = 2 for BT id 0x31):
        //     o+0..3   LX LY RX RY
        //     o+4/5    L2/R2 analog
        //     o+7..9   button bytes 0..2
        //     o+15..20 gyro (s16 LE), o+21..26 accel
        //     o+32/36  touch fingers 0/1
        //   DS4-style (DualShock 4 + DualSense clones; id 0x01 even on BT):
        //     o+0..3   LX LY RX RY (same)
        //     o+4..6   button bytes 0..2
        //     o+7/8    L2/R2 analog
        //     o+12..17 gyro, o+18..23 accel
        //     o+34/38  touch fingers 0/1
        //
        // Button bit meanings are identical in both layouts (DS4 Share sits
        // on the DualSense Create bit), only the offsets differ.  Detection
        // deliberately prefers the standard layout on a tie.
        static bool ParseReport(const u8* r, DWORD len, Sample& s) {
            size_t o;
            if (r[0] == 0x01) {
                o = 1;  // USB (or DS4-style clone, any transport)
            } else if (r[0] == 0x31) {
                o = 2;  // Bluetooth
            } else {
                return false;
            }
            if (len < o + 40 || len < 11) {
                return false;
            }

            auto hat_ok = [r, len](size_t p) { return p < len && (r[p] & 0x0F) <= 8; };
            // Strongest anchor: a CENTERED d-pad hat (nibble 8) at the
            // candidate button byte.  The other layout has trigger analog at
            // that offset, which is 0 at rest and rarely exactly 8 — this
            // discriminates even when the accel/touch regions report zeros
            // (common on clones without motion/touch hardware).
            auto hat_centered = [r, len](size_t p) { return p < len && (r[p] & 0x0F) == 8; };
            auto accel_ok = [r, len](size_t p) {
                if (p + 6 > len) {
                    return false;
                }
                float m2 = 0.0f;
                for (int i = 0; i < 3; ++i) {
                    const float a = static_cast<float>(ReadS16LE(r + p + i * 2)) / 8192.0f;
                    m2 += a * a;
                }
                return m2 > 0.25f && m2 < 4.0f; // 0.5..2 g
            };
            // Missing data is not evidence against a layout.
            auto touch_idle_ok = [r, len](size_t p) { return p >= len || (r[p] & 0x80) != 0; };

            int ds5 = 0, ds4 = 0;
            if (hat_centered(o + 7))     { ds5 += 2; }
            else if (hat_ok(o + 7))      { ++ds5; }
            if (hat_centered(o + 4))     { ds4 += 2; }
            else if (hat_ok(o + 4))      { ++ds4; }
            if (accel_ok(o + 21))     { ++ds5; }
            if (accel_ok(o + 18))     { ++ds4; }
            if (touch_idle_ok(o + 32)) { ++ds5; }
            if (touch_idle_ok(o + 34)) { ++ds4; }
            // Hysteresis: only switch layouts after 5 consecutive wins for
            // the other side; offsets must not jitter between reports.
            const bool ds4_winner = ds4 > ds5;
            if (ds4_winner != (g_last_layout.load() == 2)) {
                if (++g_layout_streak >= 5) {
                    g_last_layout.store(ds4_winner ? 2 : 1);
                    g_layout_streak = 0;
                }
            } else {
                g_layout_streak = 0;
            }
            const bool ds4_layout = g_last_layout.load() == 2;

            const size_t b0p    = ds4_layout ? o + 4  : o + 7;
            const size_t l2ap   = ds4_layout ? o + 7  : o + 4;
            const size_t gyrop  = ds4_layout ? o + 12 : o + 15;
            const size_t accelp = ds4_layout ? o + 18 : o + 21;
            const size_t touchp = ds4_layout ? o + 34 : o + 32;
            g_b0_pos.store(b0p);

            s.lx = r[o + 0];
            s.ly = r[o + 1];
            s.rx = r[o + 2];
            s.ry = r[o + 3];
            s.l2 = r[l2ap];
            s.r2 = r[l2ap + 1];
            const u8 b0 = r[b0p];
            const u8 b1 = r[b0p + 1];
            const u8 b2 = r[b0p + 2];

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
                const int g_raw = ReadS16LE(r + gyrop + i * 2);
                const int a_raw = ReadS16LE(r + accelp + i * 2);
                s.gyro[i] = static_cast<float>(g_raw) * (2000.0f / 32768.0f) * kDpsToRad;
                s.accel[i] = static_cast<float>(a_raw) / 8192.0f;
            }

            if (touchp + 8 <= len) {
                DecodeTouchPoint(r + touchp, s.touch[0]);
                DecodeTouchPoint(r + touchp + 4, s.touch[1]);
            } else {
                s.touch[0] = PadTouchPoint{};
                s.touch[1] = PadTouchPoint{};
            }

            // Battery / headphone / adaptive-trigger feedback (standard
            // DualSense layout only; offsets per DualSense-Windows
            // DS5_Input.cpp: level o+52, status o+53 bit0 headphone / bit3
            // charging, o+54 bit5 full, trigger feedback o+41 right / o+42
            // left).
            if (!ds4_layout && len >= o + 55) {
                const u8 level_nib = static_cast<u8>(r[o + 52] & 0x0F);
                s.battery_level = static_cast<u8>(level_nib > 8 ? 100
                                                                : level_nib * 100 / 8);
                s.battery_charging = (r[o + 53] & 0x08) != 0;
                s.battery_full = (r[o + 54] & 0x20) != 0;
                s.headphone_connected = (r[o + 53] & 0x01) != 0;
            }
            if (!ds4_layout && len >= o + 43) {
                s.trigger_feedback[0] = r[o + 42]; // left
                s.trigger_feedback[1] = r[o + 41]; // right
            }
            // Mic audio data: standard DualSense input reports carry 4
            // channels × 8 samples of 16-bit PCM mic data at offset o+74
            // when the mic is actively streaming.
            s.mic_active = false;
            s.mic_channels = 0;
            if (!ds4_layout && len >= o + 74 + 64) {
                // Check mic status: bit 2 (0x04) in the status byte at o+53
                // indicates the mic audio stream is live.
                if (r[o + 53] & 0x04) {
                    s.mic_active = true;
                    s.mic_channels = 4;
                    for (int ch = 0; ch < 4; ++ch) {
                        for (int smp = 0; smp < 8; ++smp) {
                            const int off = o + 74 + (ch * 16) + (smp * 2);
                            if (off + 2 <= static_cast<int>(len)) {
                                s.mic_samples[ch][smp] = static_cast<s16>(
                                    static_cast<s16>(r[off]) |
                                    (static_cast<s16>(r[off + 1]) << 8));
                            }
                        }
                    }
                }
            }
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

        static u32 DualSenseCrc32(const u8* data, size_t len, u8 seed) {
            u32 crc = Crc32Update(0xFFFFFFFFu, seed);
            for (size_t i = 0; i < len; ++i) {
                crc = Crc32Update(crc, data[i]);
            }
            return ~crc;
        }

        // Output wire format: 0 = auto (by layout/transport), 1 = DualSense
        // USB (0x02), 2 = DualSense BT (0x31+CRC 0xA2), 3 = DS4 USB (0x05),
        // 4 = DS4 BT (0x11+CRC 0xA1).  Selectable from tools so unknown
        // clones can be probed.
        static int g_out_format_override = 0;

        // Caller must hold g_out_mutex.
        static void SendOutputLocked() {
            if (g_out_handle == nullptr || !g_out_writable || !g_out_transport_known) {
                return;
            }

            DWORD written = 0;

            // Build into a zero-padded buffer sized to the device's
            // OutputReportByteLength (see g_out_report_len comment); some
            // drivers silently drop shorter writes.
            u8 report[600] = {};
            size_t build_len = 0;

            // Wire format: explicit override wins; otherwise DS4-layout
            // devices get the DS4 USB report, everything else the DualSense
            // report for its transport.
            int fmt = g_out_format_override;
            if (fmt == 0) {
                fmt = g_last_layout.load() == 2 ? 3 : (g_out_bluetooth ? 2 : 1);
            }

            if (fmt == 3) {
                // DS4 USB: 32 bytes, id 0x05, motors at [4]/[5].
                report[0] = 0x05;
                report[1] = 0xFF;
                report[4] = g_motor_small;
                report[5] = g_motor_large;
                build_len = 32;
            } else if (fmt == 4) {
                // DS4 Bluetooth: 78 bytes, id 0x11, motors at [6]/[7],
                // CRC32LE seeded with 0xA1 over bytes 0..73.
                report[0] = 0x11;
                report[1] = static_cast<u8>(0xC0 | (g_out_seq & 0x0F));
                g_out_seq = static_cast<u8>((g_out_seq + 1) & 0x0F);
                report[2] = 0x00;
                report[3] = 0x0F; // enable rumble + LEDs + flash
                report[6] = g_motor_small;
                report[7] = g_motor_large;
                const u32 crc = DualSenseCrc32(report, 74, 0xA1);
                report[74] = static_cast<u8>(crc);
                report[75] = static_cast<u8>(crc >> 8);
                report[76] = static_cast<u8>(crc >> 16);
                report[77] = static_cast<u8>(crc >> 24);
                build_len = 78;
            } else if (fmt == 2 || fmt == 1) {
                u8 common[48] = {};
                common[0] = 0x03;              // valid_flag0: enable both rumble motors
                // valid_flag1 (SDL DS5_OUTPUT_VALID_FLAG1 layout):
                //   bit 0 = mute_led   bit 1 = lightbar   bit 2 = player_ind
                //   bit 3 = fade       bit 4 = mic_led    bit 6 = right_trig
                //   bit 7 = left_trig
                common[1] = 0x40 | 0x80;       // enable right + left trigger effects
                common[2] = g_motor_small;     // motor_left (SDL offset +3)
                common[3] = g_motor_large;     // motor_right (SDL offset +4)
                common[10] = g_trigger[1].mode;  // right
                std::memcpy(common + 11, g_trigger[1].params, 10);
                common[21] = g_trigger[0].mode;  // left
                std::memcpy(common + 22, g_trigger[0].params, 10);
                // LED config: corrected per SDL DS5EffectsState_t layout.
                common[1] |= 0x04;             // valid_flag1 bit 2: enable player LEDs
                common[38] = 0x03;
                common[41] = g_leds_disabled ? 0x01 : 0x02;
                common[42] = g_led_brightness;
                // Corrected byte positions: SDL layout has lightbar at +44..+46
                // and player_leds at +47.  Since common starts at USB+1/BT+3,
                // common[n] → report[1+n] (USB).
                common[43] = g_lightbar[0];    // USB[44] = lightbar R (was player_leds)
                common[44] = g_lightbar[1];    // USB[45] = lightbar G
                common[45] = g_lightbar[2];    // USB[46] = lightbar B
                common[46] = g_player_leds;    // USB[47] = player_leds (was lightbar B)
                if (!g_player_led_fade) {
                    common[46] |= 0x20;        // 0x20 set = instant, clear = fade
                }
                // Mic LED: valid_flag1 bit 4 (0x10) enables the mic_led byte.
                common[1] |= 0x10;             // enable mic LED (corrected from 0x01)
                common[8] = g_mic_led;         // USB[9] = mic_led
                common[1] |= 0x02;             // valid_flag1 bit 1: enable lightbar
                common[47] = 0x00;             // padding for 48-byte alignment

                if (fmt == 1) {
                    report[0] = 0x02;
                    std::memcpy(report + 1, common, sizeof(common));
                    build_len = 49;  // 1 (report id) + 48 (common)
                } else {
                    report[0] = 0x31;
                    report[1] = static_cast<u8>((g_out_seq & 0x0F) << 4);
                    g_out_seq = static_cast<u8>((g_out_seq + 1) & 0x0F);
                    // BT report[2]: 0x00 = default.  0x10 enables speaker
                    // audio data following the common payload — only set
                    // when actually sending haptics+audio (future).
                    report[2] = 0x00;
                    std::memcpy(report + 3, common, sizeof(common));
                    const u32 crc = DualSenseCrc32(report, 74, 0xA2);
                    report[74] = static_cast<u8>(crc);
                    report[75] = static_cast<u8>(crc >> 8);
                    report[76] = static_cast<u8>(crc >> 16);
                    report[77] = static_cast<u8>(crc >> 24);
                    // BT output must be padded to g_out_report_len (typically
                    // 547 bytes for DualSense BT) — the WriteFile below does
                    // this via out_len = max(build_len, g_out_report_len).
                    build_len = 78;
                }
            } else {
                return; // unknown format id
            }

            const DWORD out_len = g_out_report_len > build_len
                                      ? g_out_report_len
                                      : static_cast<DWORD>(build_len);
            if (WriteFile(g_out_handle, report, out_len, &written, nullptr)) {
                return;
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
                bool bluetooth = false;
                HANDLE handle = OpenDualSense(&writable, &bluetooth);
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
                    // Transport is known at open time from the HID caps.
                    g_out_transport_known = true;
                    g_out_bluetooth = bluetooth;
                    g_out_report_len = QueryOutputReportLength(handle);
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
                        g_out_report_len = 0;
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
    // for Bluetooth, false for USB.  Note: DS4-style clones report id 0x01
    // even over Bluetooth, so they read as "USB" here.
    inline bool GetTransport(bool& known) {
        std::lock_guard<std::mutex> lock(detail::g_out_mutex);
        known = detail::g_out_transport_known;
        return detail::g_out_bluetooth;
    }

    // Layout of the last parsed report: 1 = standard DualSense,
    // 2 = DS4-style (DualShock 4 / DualSense clones).
    inline int GetReportLayout() {
        return detail::g_last_layout.load();
    }

    // Absolute offset of button byte 0 (face buttons + d-pad hat) in the
    // last parsed raw report; +1/+2 give button bytes 1/2.
    inline size_t GetButtonBlockPos() {
        return detail::g_b0_pos.load();
    }

    // Device's HID OutputReportByteLength (0 when unknown/disconnected).
    // Output writes are zero-padded up to this size.
    inline DWORD GetOutputReportLength() {
        std::lock_guard<std::mutex> lock(detail::g_out_mutex);
        return detail::g_out_report_len;
    }

    // Identity of the opened device: VID/PID and the HID product string
    // (empty when nothing is connected).
    inline void GetDeviceInfo(u16& vid, u16& pid, wchar_t* product, size_t product_chars) {
        vid = detail::g_dev_vid;
        pid = detail::g_dev_pid;
        wcsncpy_s(product, product_chars, detail::g_dev_product, _TRUNCATE);
    }

    // Override the output wire format (0=auto, 1=DS5 USB, 2=DS5 BT,
    // 3=DS4 USB, 4=DS4 BT).  Re-sends the current output state immediately.
    inline void SetOutputFormatOverride(int format) {
        std::lock_guard<std::mutex> lock(detail::g_out_mutex);
        detail::g_out_format_override = format;
        detail::SendOutputLocked();
    }

    // Set the five player-indicator LEDs (bits 0..4 = RIGHT..LEFT physically:
    // bit 0 is the rightmost LED on the controller, bit 4 the leftmost).
    // `fade` asks for a fade transition.
    inline void SetPlayerLeds(u8 bitmask, bool fade) {
        std::lock_guard<std::mutex> lock(detail::g_out_mutex);
        if (detail::g_player_leds == bitmask && detail::g_player_led_fade == fade) {
            return;
        }
        detail::g_player_leds = static_cast<u8>(bitmask & 0x1F);
        detail::g_player_led_fade = fade;
        detail::SendOutputLocked();
    }

    // LED options: brightness 0=high 1=medium 2=low; `disabled` kills all
    // LEDs on the controller (lightbar + player LEDs).
    inline void SetLedOptions(u8 brightness, bool disabled) {
        std::lock_guard<std::mutex> lock(detail::g_out_mutex);
        detail::g_led_brightness = brightness > 2 ? static_cast<u8>(2) : brightness;
        detail::g_leds_disabled = disabled;
        detail::SendOutputLocked();
    }

    // Microphone LED: 0=off, 1=on, 2=pulse.
    inline void SetMicLed(u8 mode) {
        std::lock_guard<std::mutex> lock(detail::g_out_mutex);
        if (detail::g_mic_led == mode) {
            return;
        }
        detail::g_mic_led = mode > 2 ? static_cast<u8>(0) : mode;
        detail::SendOutputLocked();
    }

    // Lightbar RGB color (full 8-bit per channel).
    inline void SetLightBar(u8 r, u8 g, u8 b) {
        std::lock_guard<std::mutex> lock(detail::g_out_mutex);
        if (detail::g_lightbar[0] == r && detail::g_lightbar[1] == g &&
            detail::g_lightbar[2] == b) {
            return;
        }
        detail::g_lightbar[0] = r;
        detail::g_lightbar[1] = g;
        detail::g_lightbar[2] = b;
        detail::SendOutputLocked();
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
    // (false).  `mode` is the raw DualSense effect byte; known values:
    //   0x00 off        0x01 rigid/feedback   0x02 pulse/section
    //   0x05 rigid B    0x06 pulse B          0x21 rigid A / feedback
    //   0x22 pulse A    0x23 pulse A2         0x25 rigid AB / weapon
    //   0x26 pulse B2 / vibration             0x27 pulse AB
    // Params are mode-specific (the DSX/Steam presets build them); for the
    // basic modes 0x01..0x03 they are clamped defensively:
    //   0x01 feedback:  params[0] = start position 0..9, params[1] = force 0..8
    //   0x02 weapon:    params[0] = start 2..7, params[1] = end >start..8,
    //                   params[2] = force 0..8
    //   0x03 vibration: params[0] = position 0..9, params[1] = amplitude 0..8,
    //                   params[2] = frequency 0..255
    // No-op when no DualSense is connected or the handle is read-only.
    inline void SetTriggerEffect(bool left, u8 mode, const u8 params[10]) {
        std::lock_guard<std::mutex> lock(detail::g_out_mutex);
        detail::TriggerEffect& t = detail::g_trigger[left ? 0 : 1];
        t.mode = mode;
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
