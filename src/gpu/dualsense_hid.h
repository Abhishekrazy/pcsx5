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

        static void Publish(const Sample& s) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_sample = s;
        }

        // Open the first present DualSense HID interface, or nullptr when none
        // is attached.  Mirrors WindowsDualSenseReader.OpenDualSense().
        static HANDLE OpenDualSense() {
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
                if (handle == INVALID_HANDLE_VALUE) {
                    handle = CreateFileW(path, GENERIC_READ,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE,
                                         nullptr, OPEN_EXISTING, 0, nullptr);
                }
                if (handle != INVALID_HANDLE_VALUE) {
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
        // button/stick/trigger block and the standard DualSense layout for
        // the motion/touch blocks:
        //   o+0..3  LX LY RX RY
        //   o+4     L2 analog, o+5 R2 analog
        //   o+6     report sequence tag
        //   o+7..9  button bytes 0..2
        //   o+10..13 report timestamp (u32)
        //   o+14..19 gyro pitch/yaw/roll (s16 LE)
        //   o+20..25 accel x/y/z (s16 LE)
        //   o+31..34 touch point 0, o+35..38 touch point 1
        static bool ParseReport(const u8* r, DWORD len, Sample& s) {
            size_t o;
            if (r[0] == 0x01) {
                o = 1;  // USB
            } else if (r[0] == 0x31) {
                o = 2;  // Bluetooth
            } else {
                return false;
            }
            if (len < o + 39 || len < 11) {
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
                const int g_raw = ReadS16LE(r + o + 14 + i * 2);
                const int a_raw = ReadS16LE(r + o + 20 + i * 2);
                s.gyro[i] = static_cast<float>(g_raw) * (2000.0f / 32768.0f) * kDpsToRad;
                s.accel[i] = static_cast<float>(a_raw) / 8192.0f;
            }

            DecodeTouchPoint(r + o + 31, s.touch[0]);
            DecodeTouchPoint(r + o + 35, s.touch[1]);
            s.touch_count = static_cast<u8>((s.touch[0].active ? 1 : 0) +
                                            (s.touch[1].active ? 1 : 0));
            s.connected = true;
            return true;
        }

        static void ReadLoop() {
            bool announced = false;
            for (;;) {
                HANDLE handle = OpenDualSense();
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
                if (!announced) {
                    LOG_INFO(GPU, "DualSense controller connected (HID input reports active).");
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
                        Publish(s);
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

} // namespace DualSense
} // namespace GPU
