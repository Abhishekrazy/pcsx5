//
// dualsense_test — standalone console utility to verify every DualSense
// input and output path in src/gpu/dualsense_hid.h.
//
// Live view (redrawn in place every ~50 ms): all buttons, sticks, L2/R2
// analog values, both touch fingers, gyro + accel, connection/transport.
// Output test keys:
//   1 = rumble pulse (large + small motors)
//   2 = adaptive-trigger feedback on L2 + R2
//   3 = adaptive-trigger weapon mode on L2 + R2
//   0 = all effects off
// Quit with Q or Esc; Ctrl+C also exits cleanly.
//

#include "gpu/dualsense_hid.h"

#include <conio.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <atomic>

namespace {

std::atomic<bool> g_quit{ false };

BOOL WINAPI OnConsoleCtrl(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_quit.store(true);
        return TRUE;
    }
    return FALSE;
}

void EnableVirtualTerminal() {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(out, &mode)) {
        SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
}

struct ButtonName {
    u32 mask;
    const char* name;
};

// Same SCE_PAD encoding produced by DualSense::ParseReport (see
// src/gpu/dualsense_hid.h).  Note: Create has no bit in this encoding
// (intentionally unmapped there), so it is shown separately as "n/a".
constexpr ButtonName kButtons[] = {
    { 0x4000,   "Cross"    },
    { 0x2000,   "Circle"   },
    { 0x8000,   "Square"   },
    { 0x1000,   "Triangle" },
    { 0x0010,   "DPad-Up"  },
    { 0x0040,   "DPad-Down"},
    { 0x0080,   "DPad-Left"},
    { 0x0020,   "DPad-Right"},
    { 0x0400,   "L1"       },
    { 0x0800,   "R1"       },
    { 0x0100,   "L2"       },
    { 0x0200,   "R2"       },
    { 0x0008,   "Options"  },
    { 0x0002,   "L3"       },
    { 0x0004,   "R3"       },
    { 0x10000,  "PS"       },
    { 0x100000, "Touchpad" },
};

const char* OnOff(bool pressed) { return pressed ? "ON " : "off"; }

void AllEffectsOff() {
    GPU::DualSense::SetRumble(0, 0);
    const u8 zero[10] = {};
    GPU::DualSense::SetTriggerEffect(true, 0x00, zero);
    GPU::DualSense::SetTriggerEffect(false, 0x00, zero);
}

void HandleKey(int key) {
    switch (key) {
        case '1': { // rumble pulse on both motors
            GPU::DualSense::SetRumble(200, 160);
            break;
        }
        case '2': { // feedback: resistance from mid-travel, full force
            const u8 params[10] = { 4, 8 };
            GPU::DualSense::SetTriggerEffect(true, 0x01, params);
            GPU::DualSense::SetTriggerEffect(false, 0x01, params);
            break;
        }
        case '3': { // weapon: click point at 1/3 travel
            const u8 params[10] = { 3, 6, 8 };
            GPU::DualSense::SetTriggerEffect(true, 0x02, params);
            GPU::DualSense::SetTriggerEffect(false, 0x02, params);
            break;
        }
        case '0':
            AllEffectsOff();
            break;
        default:
            break;
    }
}

} // namespace

int main(int argc, char** argv) {
    // Optional: auto-quit after N seconds (for scripted smoke runs).
    DWORD auto_quit_ms = 0;
    if (argc > 1) {
        const int secs = std::atoi(argv[1]);
        if (secs > 0) {
            auto_quit_ms = static_cast<DWORD>(secs) * 1000;
        }
    }
    const DWORD start_ms = GetTickCount();

    EnableVirtualTerminal();
    SetConsoleCtrlHandler(OnConsoleCtrl, TRUE);

    std::printf("dualsense_test — verifying src/gpu/dualsense_hid.h input/output paths\n");
    std::printf("Keys: 1 rumble | 2 trigger feedback | 3 trigger weapon | 0 all off | Q/Esc quit\n\n");

    GPU::DualSense::EnsureStarted();

    GPU::DualSense::Sample prev{};
    unsigned long long changed_samples = 0;
    bool have_prev = false;

    while (!g_quit.load()) {
        while (_kbhit()) {
            const int key = _getch();
            if (key == 'q' || key == 'Q' || key == 27) {
                g_quit.store(true);
                break;
            }
            HandleKey(key);
        }
        if (g_quit.load()) {
            break;
        }
        if (auto_quit_ms != 0 && GetTickCount() - start_ms >= auto_quit_ms) {
            break;
        }

        GPU::DualSense::Sample s{};
        const bool connected = GPU::DualSense::GetSample(s);
        if (connected && have_prev && std::memcmp(&s, &prev, sizeof(s)) != 0) {
            ++changed_samples;
        }
        prev = s;
        have_prev = true;

        bool transport_known = false;
        const bool bt = GPU::DualSense::GetTransport(transport_known);
        const char* transport = !transport_known ? "unknown" : (bt ? "Bluetooth" : "USB");

        std::printf("\x1b[H"); // cursor home, redraw in place
        std::printf("dualsense_test — verifying src/gpu/dualsense_hid.h input/output paths\x1b[K\n");
        std::printf("Keys: 1 rumble | 2 trigger feedback | 3 trigger weapon | 0 all off | Q/Esc quit\x1b[K\n\n");

        std::printf("  Connected: %s   Transport: %s   Changed samples: %llu\x1b[K\n",
                    connected ? "YES" : "no", transport, changed_samples);
        std::printf("\x1b[K\n");

        std::printf("  Buttons: ");
        for (const ButtonName& b : kButtons) {
            std::printf("%s:%s ", b.name, OnOff((s.buttons & b.mask) != 0));
        }
        std::printf(" Create:n/a(no bit in encoding)\x1b[K\n");
        std::printf("\x1b[K\n");

        std::printf("  Sticks:  LX=%3u LY=%3u  RX=%3u RY=%3u   L3:%s R3:%s\x1b[K\n",
                    s.lx, s.ly, s.rx, s.ry,
                    OnOff((s.buttons & 0x0002) != 0), OnOff((s.buttons & 0x0004) != 0));
        std::printf("  Triggers: L2=%3u  R2=%3u\x1b[K\n", s.l2, s.r2);
        std::printf("\x1b[K\n");

        std::printf("  Touch (%u fingers):\x1b[K\n", s.touch_count);
        for (int i = 0; i < 2; ++i) {
            const GPU::PadTouchPoint& t = s.touch[i];
            std::printf("    finger %d: contact=%s id=%3u x=%4u y=%4u\x1b[K\n",
                        i, t.active ? "ON " : "off", t.id, t.x, t.y);
        }
        std::printf("\x1b[K\n");

        std::printf("  Accel (g):     x=%+.3f y=%+.3f z=%+.3f\x1b[K\n",
                    s.accel[0], s.accel[1], s.accel[2]);
        std::printf("  Gyro (rad/s):  pitch=%+.3f yaw=%+.3f roll=%+.3f\x1b[K\n",
                    s.gyro[0], s.gyro[1], s.gyro[2]);
        std::printf("\x1b[K\n\x1b[J");
        std::fflush(stdout);

        Sleep(50);
    }

    AllEffectsOff();
    std::printf("\x1b[H\x1b[JExiting cleanly. Changed samples observed: %llu\n", changed_samples);

    u8 raw[256];
    const size_t raw_len = GPU::DualSense::GetLastRawReport(raw, sizeof(raw));
    if (raw_len > 0) {
        std::printf("Last raw report (%zu bytes):", raw_len);
        for (size_t i = 0; i < raw_len; ++i) {
            if (i % 16 == 0) {
                std::printf("\n  %03zu: ", i);
            }
            std::printf("%02x ", raw[i]);
        }
        std::printf("\n");
    }
    return 0;
}
