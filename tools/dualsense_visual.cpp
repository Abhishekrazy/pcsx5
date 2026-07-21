//
// dualsense_visual — windowed (Dear ImGui) DualSense test utility.
//
// Visual counterpart to tools/dualsense_test.cpp: reads the same header-only
// HID reader (src/gpu/dualsense_hid.h) and renders a live controller view:
//   - every button lights up when pressed, with a "tested" checkmark that
//     latches the first time each button is seen pressed (per-input checklist)
//   - both sticks as 2D position plots, L2/R2 analog bars
//   - touchpad drawn to scale with live finger positions (up to 2 fingers)
//   - gyro + accel readouts
//   - output panel: rumble sliders + adaptive-trigger effect controls
//     (off / feedback / weapon / vibration) with per-trigger parameters and a
//     "Full block" button (max resistance from position 0 — trigger cannot
//     be pressed)
//   - mapping wizard: click a button tile, then press that button on the
//     controller — the raw report bit that fired is registered as that
//     button's code (fail-safe: frames where any analog channel moved are
//     never registered).  Produces a raw-code table + C++ mapping snippet
//   - mapping recorder: logs button press/release with the raw report byte
//     changes, touchpad finger contact/movement, and L2/R2 analog travel;
//     the log can be saved to a text file for fixing the button mapping
//
// Button names follow Sony's official DualSense manual
// (controller.dl.playstation.net/controller/lang/en/2100002.html).
// Create has no bit in the SCE_PAD encoding used by Sample, so it is read
// directly from the raw HID report (b1 0x10) for display only.
//

#include "gpu/dualsense_hid.h"

#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"

#include <GLFW/glfw3.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>

namespace {

struct ButtonDef {
    u32 mask;        // SCE_PAD bit; 0 = read from the raw report instead
    const char* name;
    int raw_byte;    // button-block byte 0..2 (report offsets o+7..o+9)
    u8 raw_bit;      // bit within that byte (used when mask == 0)
};

// Official DualSense control names per Sony's manual
// (controller.dl.playstation.net/controller/lang/en/2100002.html).
// SCE_PAD encoding same as tools/dualsense_test.cpp; Create has no SCE_PAD
// bit, so it is read straight from the raw report (b1 0x10).  No Mute tile:
// DS4-layout clones have no mute button and PC drivers do not expose it.
constexpr ButtonDef kButtons[] = {
    { 0x4000,   "Cross",        0, 0    }, // action buttons
    { 0x2000,   "Circle",       0, 0    },
    { 0x8000,   "Square",       0, 0    },
    { 0x1000,   "Triangle",     0, 0    },
    { 0x0010,   "Up",           0, 0    }, // directional buttons
    { 0x0040,   "Down",         0, 0    },
    { 0x0080,   "Left",         0, 0    },
    { 0x0020,   "Right",        0, 0    },
    { 0x0400,   "L1",           0, 0    },
    { 0x0800,   "R1",           0, 0    },
    { 0x0100,   "L2",           0, 0    },
    { 0x0200,   "R2",           0, 0    },
    { 0,        "Create",       1, 0x10 },
    { 0x0008,   "Options",      0, 0    },
    { 0x0002,   "L3",           0, 0    }, // left stick press
    { 0x0004,   "R3",           0, 0    }, // right stick press
    { 0x10000,  "PS",           0, 0    },
    { 0x100000, "Touch pad",    0, 0    }, // touch pad button (pad click)
    { 0,        "Mute",         2, 0x04 }, // mic mute (raw b2 0x04)
};
constexpr int kButtonCount = static_cast<int>(sizeof(kButtons) / sizeof(kButtons[0]));

bool g_tested[kButtonCount] = {}; // latched "seen pressed" per button

// Read a raw-only button bit (Create) straight from the last raw report.
// The button block's absolute offset is layout-dependent (standard DualSense
// vs DS4-style clone), so it comes from the parser.
bool ReadRawButtonBit(int block_byte, u8 bit) {
    u8 raw[256];
    const size_t len = GPU::DualSense::GetLastRawReport(raw, sizeof(raw));
    const size_t pos = GPU::DualSense::GetButtonBlockPos();
    if (len < pos + static_cast<size_t>(block_byte) + 1) {
        return false;
    }
    return (raw[pos + block_byte] & bit) != 0;
}

// Current pressed state of one kButtons entry.  Create and Mute have no
// SCE_PAD bit; their state is supplied per-frame by the active backend.
bool g_create_pressed = false;
bool g_mute_pressed = false;

bool ButtonPressed(int idx, const GPU::DualSense::Sample& s) {
    const ButtonDef& b = kButtons[idx];
    if (b.mask != 0) {
        return (s.buttons & b.mask) != 0;
    }
    return b.raw_byte == 1 ? g_create_pressed : g_mute_pressed;
}


// ── Report-layout sanity check ──────────────────────────────────────────────
// A genuine DualSense report at rest has: d-pad hat nibble <= 8, both touch
// points flagged "not touching" (bit 7 set), and ~1 g of accel somewhere
// (gravity).  If any of these fails, the report layout does not match the
// standard parse offsets (third-party/clone controller or a different report
// variant) and every decoded field is suspect.
struct SanityFlags {
    bool hat_ok = false;
    bool touch_ok = false;
    bool accel_ok = false;
};

SanityFlags CheckReportSanity(const GPU::DualSense::Sample& s) {
    SanityFlags f;
    u8 raw[256];
    const size_t len = GPU::DualSense::GetLastRawReport(raw, sizeof(raw));
    const size_t pos = GPU::DualSense::GetButtonBlockPos();
    const int layout = GPU::DualSense::GetReportLayout();
    // Touch finger 0 offset relative to button byte 0: +25 (DS5) / +30 (DS4).
    const size_t touchp = pos + (layout == 2 ? 30 : 25);
    if (len >= pos + 3) {
        f.hat_ok = (raw[pos] & 0x0F) <= 8;
        // At rest both touch bytes have the "not touching" bit set.  If a
        // finger is genuinely down this check just passes anyway.
        f.touch_ok = touchp + 5 > len ||
                     (((raw[touchp] & 0x80) != 0 || s.touch[0].active) &&
                      ((raw[touchp + 4] & 0x80) != 0 || s.touch[1].active));
        const float mag2 = s.accel[0] * s.accel[0] + s.accel[1] * s.accel[1] +
                           s.accel[2] * s.accel[2];
        f.accel_ok = mag2 > 0.25f && mag2 < 4.0f; // 0.5..2 g
    }
    return f;
}

// ── Mapping recorder ────────────────────────────────────────────────────────
// While recording, every decoded button press/release is logged together with
// the raw button-block byte changes (o+7..o+9) that produced it, so a wrong
// mapping can be traced to the exact report bit.  Touchpad finger contact /
// movement and L2/R2 analog travel are logged too (thresholded so idle noise
// does not flood the log).  The log can be saved to a text file from the UI.
struct Recorder {
    bool active = false;
    bool have_prev = false;
    DWORD start_ms = 0;
    bool prev_state[kButtonCount] = {}; // pressed state per button
    u8 prev_block[3] = {}; // raw bytes o+7, o+8, o+9
    u8 prev_l2 = 0, prev_r2 = 0;
    GPU::PadTouchPoint prev_touch[2] = {};
    std::vector<std::string> lines;
};

Recorder g_rec;

void RecLog(const char* fmt, ...) {
    char body[256];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);
    char line[320];
    std::snprintf(line, sizeof(line), "[%8lu ms] %s",
                  static_cast<unsigned long>(GetTickCount() - g_rec.start_ms), body);
    g_rec.lines.emplace_back(line);
}

// Called every frame with the current state; no-op unless recording.
void RecordFrame(const GPU::DualSense::Sample& s) {
    u8 raw[256];
    const size_t len = GPU::DualSense::GetLastRawReport(raw, sizeof(raw));
    const size_t pos = GPU::DualSense::GetButtonBlockPos();
    if (len < pos + 3) {
        return;
    }
    const u8 block[3] = { raw[pos], raw[pos + 1], raw[pos + 2] };

    bool state[kButtonCount];
    for (int i = 0; i < kButtonCount; ++i) {
        state[i] = ButtonPressed(i, s);
    }

    if (!g_rec.have_prev) {
        g_rec.have_prev = true;
        std::memcpy(g_rec.prev_state, state, sizeof(state));
        g_rec.prev_l2 = s.l2;
        g_rec.prev_r2 = s.r2;
        g_rec.prev_touch[0] = s.touch[0];
        g_rec.prev_touch[1] = s.touch[1];
        std::memcpy(g_rec.prev_block, block, sizeof(block));
        RecLog("recording started (report id 0x%02X, len %zu, block %02X %02X %02X)",
               raw[0], len, block[0], block[1], block[2]);
        return;
    }

    // Raw button-block byte changes.
    for (int i = 0; i < 3; ++i) {
        if (block[i] == g_rec.prev_block[i]) {
            continue;
        }
        const u8 set_bits = static_cast<u8>(block[i] & ~g_rec.prev_block[i]);
        const u8 clr_bits = static_cast<u8>(~block[i] & g_rec.prev_block[i]);
        RecLog("raw byte %zu: %02X -> %02X  (set %02X, cleared %02X)",
               pos + i, g_rec.prev_block[i], block[i], set_bits, clr_bits);
    }

    // Decoded button events.
    for (int i = 0; i < kButtonCount; ++i) {
        const ButtonDef& b = kButtons[i];
        if (state[i] == g_rec.prev_state[i]) {
            continue;
        }
        if (b.mask != 0) {
            RecLog("%-9s %s   (SCE_PAD bit 0x%X)", b.name,
                   state[i] ? "PRESSED " : "released", b.mask);
        } else {
            RecLog("%-9s %s   (raw b%d bit 0x%02X, unmapped in SCE_PAD)", b.name,
                   state[i] ? "PRESSED " : "released", b.raw_byte, b.raw_bit);
        }
    }

    // Trigger analog travel (threshold 16/255 so jitter is not logged).
    if (s.l2 != g_rec.prev_l2 && (s.l2 > g_rec.prev_l2 + 15 || s.l2 + 15 < g_rec.prev_l2)) {
        RecLog("L2 analog %3u -> %3u", g_rec.prev_l2, s.l2);
        g_rec.prev_l2 = s.l2;
    }
    if (s.r2 != g_rec.prev_r2 && (s.r2 > g_rec.prev_r2 + 15 || s.r2 + 15 < g_rec.prev_r2)) {
        RecLog("R2 analog %3u -> %3u", g_rec.prev_r2, s.r2);
        g_rec.prev_r2 = s.r2;
    }

    // Touchpad fingers: contact down/up always logged; movement logged when
    // the finger moved at least 64 pad units (pad is 1920x1080).
    for (int i = 0; i < 2; ++i) {
        const GPU::PadTouchPoint& now = s.touch[i];
        const GPU::PadTouchPoint& was = g_rec.prev_touch[i];
        if (now.active && !was.active) {
            RecLog("touch finger %d DOWN  id=%u x=%4u y=%4u", i + 1, now.id, now.x, now.y);
        } else if (!now.active && was.active) {
            RecLog("touch finger %d UP    (last x=%4u y=%4u)", i + 1, was.x, was.y);
        } else if (now.active && was.active) {
            const int dx = now.x > was.x ? now.x - was.x : was.x - now.x;
            const int dy = now.y > was.y ? now.y - was.y : was.y - now.y;
            if (dx >= 64 || dy >= 64) {
                RecLog("touch finger %d move  x=%4u y=%4u", i + 1, now.x, now.y);
            }
        }
        g_rec.prev_touch[i] = now;
    }

    std::memcpy(g_rec.prev_state, state, sizeof(state));
    std::memcpy(g_rec.prev_block, block, sizeof(block));
}

bool SaveRecording(char* out_path, size_t out_cap) {
    std::snprintf(out_path, out_cap, "dualsense_record_%lu.txt",
                  static_cast<unsigned long>(GetTickCount()));
    FILE* f = nullptr;
    if (fopen_s(&f, out_path, "w") != 0 || f == nullptr) {
        return false;
    }
    std::fprintf(f, "# dualsense_visual mapping recording\n");
    std::fprintf(f, "# raw block bytes are report offsets o+7,o+8,o+9 (o=1 USB, o=2 BT)\n");
    for (const std::string& line : g_rec.lines) {
        std::fprintf(f, "%s\n", line.c_str());
    }
    std::fclose(f);
    return true;
}

// ── Mapping wizard ──────────────────────────────────────────────────────────
// Guided verification: the user clicks a button name in the UI (arming it),
// then presses THE SAME physical button on the controller.  The wizard
// registers the press only when the decoded button matches the armed one —
// and records which raw button-block bit produced it.  If a DIFFERENT decoded
// button fires while armed, that is a mapping error: it is reported as a
// mismatch (with the raw code) and never registered into the armed slot.
//
// Fail-safe: a candidate press is REJECTED whenever any analog channel (both
// sticks, L2/R2 analog travel) moved at the same time, or when the change is
// a pure release (no new bit set).  Analog noise can therefore never be
// registered as a button.
struct MappingWizard {
    int armed = -1;                       // kButtons index waiting for a press
    bool captured[kButtonCount] = {};
    u8 cap_byte[kButtonCount] = {};       // 0..2 within the button block
    u8 cap_mask[kButtonCount] = {};       // bit(s) captured; 0x0F = d-pad hat
    u8 cap_value[kButtonCount] = {};      // byte value (hat: nibble value)
    bool have_prev = false;
    bool prev_state[kButtonCount] = {};   // decoded pressed state per button
    u8 prev_block[3] = {};
    u8 prev_analog[6] = {};               // lx ly rx ry l2 r2
    char status[160] = "idle — click a button, then press the same button on the controller";
};

MappingWizard g_wiz;

// Called every frame with the latest sample + raw report.  Registers the
// armed button only when the decoded press matches it; see the struct
// comment above for the full rules.
void WizardFrame(const GPU::DualSense::Sample& s) {
    u8 raw[256];
    const size_t len = GPU::DualSense::GetLastRawReport(raw, sizeof(raw));
    const size_t pos = GPU::DualSense::GetButtonBlockPos();
    if (len < 6 || len < pos + 3) {
        return;
    }
    const int layout = GPU::DualSense::GetReportLayout();
    // Sticks always sit at o+0..3; trigger analog sits 3 bytes before the
    // button block (DS5) or 3 bytes after it (DS4-style).
    const size_t o = raw[0] == 0x31 ? 2 : 1;
    const size_t l2ap = layout == 2 ? pos + 3 : pos - 3;
    if (len < l2ap + 2) {
        return;
    }
    const u8 block[3] = { raw[pos], raw[pos + 1], raw[pos + 2] };
    const u8 analog[6] = { raw[o + 0], raw[o + 1], raw[o + 2],
                           raw[o + 3], raw[l2ap], raw[l2ap + 1] };

    if (!g_wiz.have_prev) {
        g_wiz.have_prev = true;
        for (int i = 0; i < kButtonCount; ++i) {
            g_wiz.prev_state[i] = ButtonPressed(i, s);
        }
        std::memcpy(g_wiz.prev_block, block, sizeof(block));
        std::memcpy(g_wiz.prev_analog, analog, sizeof(analog));
        return;
    }

    // Fail-safe part 1: analog movement means this frame is analog activity,
    // never a button.  Sticks are always vetoed; a trigger's own analog is
    // exempt when that trigger is the armed button (pressing L2/R2 always
    // moves its analog — otherwise triggers could never be captured), and a
    // trigger's digital bit is valid even while its analog moves.
    // Threshold is 24/255: clone sticks commonly jitter +/-10 at rest.
    bool analog_moved = false;
    int moved_ch = -1;
    static const char* kChNames[6] = { "LX", "LY", "RX", "RY", "L2a", "R2a" };
    for (int i = 0; i < 6; ++i) {
        if (g_wiz.armed >= 0 && i >= 4 &&
            (kButtons[g_wiz.armed].mask == 0x0100 || kButtons[g_wiz.armed].mask == 0x0200) &&
            i == (kButtons[g_wiz.armed].mask == 0x0100 ? 4 : 5)) {
            continue; // the armed trigger's own analog channel
        }
        const int d = analog[i] > g_wiz.prev_analog[i]
                          ? analog[i] - g_wiz.prev_analog[i]
                          : g_wiz.prev_analog[i] - analog[i];
        if (d > 24) {
            analog_moved = true;
            moved_ch = i;
            break;
        }
    }
    const bool block_changed = std::memcmp(block, g_wiz.prev_block, sizeof(block)) != 0;
    if (analog_moved) {
        if (g_wiz.armed >= 0 && block_changed) {
            std::snprintf(g_wiz.status, sizeof(g_wiz.status),
                          "IGNORED: %s moved together with the button block "
                          "(not registered — fail-safe)", kChNames[moved_ch]);
        }
        std::memcpy(g_wiz.prev_block, block, sizeof(block));
        std::memcpy(g_wiz.prev_analog, analog, sizeof(analog));
        return;
    }
    std::memcpy(g_wiz.prev_analog, analog, sizeof(analog));

    // Decoded button transitions this frame (press edges only).
    bool state[kButtonCount];
    int pressed_idx = -1; // first button with a new press edge, -1 = none
    for (int i = 0; i < kButtonCount; ++i) {
        state[i] = ButtonPressed(i, s);
        if (state[i] && !g_wiz.prev_state[i] && pressed_idx < 0) {
            pressed_idx = i;
        }
    }

    if (g_wiz.armed < 0) {
        std::memcpy(g_wiz.prev_state, state, sizeof(state));
        std::memcpy(g_wiz.prev_block, block, sizeof(block));
        return;
    }

    // Raw candidate: d-pad hat = nibble VALUE (mask 0x0F marks "hat") — any
    // transition into a real direction (< 8) counts, even one that only
    // clears bits; other buttons need at least one NEW bit set (a press,
    // never a release).
    int byte = -1;
    u8 mask = 0;
    u8 value = 0;
    const u8 prev_nib = static_cast<u8>(g_wiz.prev_block[0] & 0x0F);
    const u8 new_nib = static_cast<u8>(block[0] & 0x0F);
    if (block_changed && new_nib != prev_nib && new_nib < 8) {
        byte = 0;
        mask = 0x0F;
        value = new_nib;
    } else if (block_changed) {
        for (int i = 0; i < 3; ++i) {
            const u8 set_bits = static_cast<u8>(block[i] & ~g_wiz.prev_block[i]);
            if (set_bits != 0 && byte < 0) {
                byte = i;
                mask = set_bits;
                value = block[i];
            }
        }
    }
    std::memcpy(g_wiz.prev_block, block, sizeof(block));
    std::memcpy(g_wiz.prev_state, state, sizeof(state));

    if (pressed_idx < 0) {
        if (block_changed) {
            std::snprintf(g_wiz.status, sizeof(g_wiz.status),
                          "raw change seen but no button decoded — press %s and HOLD a moment",
                          kButtons[g_wiz.armed].name);
        }
        return;
    }

    if (pressed_idx != g_wiz.armed) {
        // A DIFFERENT button decoded: this is a mapping error.  Report it
        // (with the raw code for the fix) but never register it.
        if (byte >= 0) {
            std::snprintf(g_wiz.status, sizeof(g_wiz.status),
                          "MISMATCH: armed %s but controller sent %s (byte %zu mask 0x%02X value 0x%02X) — mapping wrong, not registered",
                          kButtons[g_wiz.armed].name, kButtons[pressed_idx].name,
                          pos + byte, mask, value);
        } else {
            std::snprintf(g_wiz.status, sizeof(g_wiz.status),
                          "MISMATCH: armed %s but controller sent %s — mapping wrong, not registered",
                          kButtons[g_wiz.armed].name, kButtons[pressed_idx].name);
        }
        return;
    }

    // Same button: register it.  No auto-advance — the user clicks the next
    // tile manually, so disarm after a successful registration.
    const int idx = g_wiz.armed;
    g_wiz.armed = -1;
    g_wiz.captured[idx] = true;
    g_wiz.cap_byte[idx] = static_cast<u8>(byte < 0 ? 0 : byte);
    g_wiz.cap_mask[idx] = mask;
    g_wiz.cap_value[idx] = value;
    if (mask == 0x0F) {
        std::snprintf(g_wiz.status, sizeof(g_wiz.status),
                      "OK: %s registered (b0 hat value 0x%X)", kButtons[idx].name, value);
    } else if (byte >= 0) {
        std::snprintf(g_wiz.status, sizeof(g_wiz.status),
                      "OK: %s registered (byte %zu mask 0x%02X value 0x%02X)",
                      kButtons[idx].name, pos + byte, mask, value);
    } else {
        std::snprintf(g_wiz.status, sizeof(g_wiz.status),
                      "OK: %s registered (raw diff missed — decoded press confirmed)",
                      kButtons[idx].name);
    }
}

// True when a captured code is a d-pad hat value (byte 0 low nibble).
bool IsHatCode(int idx) {
    return g_wiz.cap_byte[idx] == 0 && g_wiz.cap_mask[idx] == 0x0F;
}

bool SaveMapping(char* out_path, size_t out_cap) {
    std::snprintf(out_path, out_cap, "dualsense_mapping_%lu.txt",
                  static_cast<unsigned long>(GetTickCount()));
    FILE* f = nullptr;
    if (fopen_s(&f, out_path, "w") != 0 || f == nullptr) {
        return false;
    }
    std::fprintf(f, "# dualsense_visual mapping capture\n");
    std::fprintf(f, "# b0/b1/b2 = the three button-block bytes (block offset is layout-dependent:\n");
    std::fprintf(f, "# standard DualSense o+7, DS4-style clone o+4)\n\n");
    std::fprintf(f, "Captured raw codes:\n");
    for (int i = 0; i < kButtonCount; ++i) {
        if (!g_wiz.captured[i]) {
            std::fprintf(f, "  %-9s : (not captured)\n", kButtons[i].name);
        } else if (IsHatCode(i)) {
            std::fprintf(f, "  %-9s : b0 hat value 0x%X\n", kButtons[i].name,
                         g_wiz.cap_value[i] & 0x0F);
        } else {
            std::fprintf(f, "  %-9s : b%u bit 0x%02X\n", kButtons[i].name,
                         g_wiz.cap_byte[i], g_wiz.cap_mask[i]);
        }
    }
    std::fprintf(f, "\nC++ mapping snippet for ParseReport (dualsense_hid.h):\n");
    for (int i = 0; i < kButtonCount; ++i) {
        if (!g_wiz.captured[i]) {
            continue;
        }
        const ButtonDef& b = kButtons[i];
        if (IsHatCode(i)) {
            std::fprintf(f, "    case %u: b |= 0x%X; break; // %s\n",
                         g_wiz.cap_value[i] & 0x0F, b.mask, b.name);
        } else if (b.mask != 0) {
            std::fprintf(f, "    if (b%u & 0x%02X) b |= 0x%X; // %s\n",
                         g_wiz.cap_byte[i], g_wiz.cap_mask[i], b.mask, b.name);
        } else {
            std::fprintf(f, "    if (b%u & 0x%02X) b |= 0x???; // %s (no SCE_PAD bit assigned yet)\n",
                         g_wiz.cap_byte[i], g_wiz.cap_mask[i], b.name);
        }
    }
    std::fclose(f);
    return true;
}

void StickPlot(const char* id, u8 x, u8 y) {
    // Y axis is 0 at top in the HID report; ImGui draws the same way.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float size = 120.0f;
    dl->AddRect(pos, ImVec2(pos.x + size, pos.y + size), IM_COL32(120, 120, 120, 255));
    dl->AddLine(ImVec2(pos.x + size / 2, pos.y), ImVec2(pos.x + size / 2, pos.y + size),
                IM_COL32(70, 70, 70, 255));
    dl->AddLine(ImVec2(pos.x, pos.y + size / 2), ImVec2(pos.x + size, pos.y + size / 2),
                IM_COL32(70, 70, 70, 255));
    const float px = pos.x + (x / 255.0f) * size;
    const float py = pos.y + (y / 255.0f) * size;
    dl->AddCircleFilled(ImVec2(px, py), 6.0f, IM_COL32(80, 200, 255, 255));
    ImGui::Dummy(ImVec2(size, size));
    ImGui::SameLine();
    ImGui::Text("%s\nx=%3u\ny=%3u", id, x, y);
}

void TriggerBar(const char* label, u8 value) {
    ImGui::ProgressBar(value / 255.0f, ImVec2(220, 0), "");
    ImGui::SameLine();
    ImGui::Text("%s %3u", label, value);
}

void TouchpadView(const GPU::DualSense::Sample& s) {
    // DualSense touchpad resolves 1920x1080.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float w = 320.0f, h = 180.0f;
    dl->AddRect(pos, ImVec2(pos.x + w, pos.y + h), IM_COL32(120, 120, 120, 255));
    for (int i = 0; i < 2; ++i) {
        const GPU::PadTouchPoint& t = s.touch[i];
        if (!t.active) {
            continue;
        }
        const float px = pos.x + (t.x / 1920.0f) * w;
        const float py = pos.y + (t.y / 1080.0f) * h;
        const ImU32 col = i == 0 ? IM_COL32(255, 170, 60, 255) : IM_COL32(120, 255, 140, 255);
        dl->AddCircleFilled(ImVec2(px, py), 9.0f, col);
        dl->AddText(ImVec2(px + 11, py - 7), col, i == 0 ? "1" : "2");
    }
    ImGui::Dummy(ImVec2(w, h));
    for (int i = 0; i < 2; ++i) {
        const GPU::PadTouchPoint& t = s.touch[i];
        ImGui::Text("finger %d: %s id=%3u x=%4u y=%4u", i + 1,
                    t.active ? "ON " : "off", t.id, t.x, t.y);
    }
}

// ── Adaptive triggers ───────────────────────────────────────────────────────
// Effect types mirror the DS5W TriggerEffect API (NoResitance /
// ContinuousResitance / SectionResitance / EffectEx / Calibrate) and are sent
// through our own output path (GPU::DualSense::SetTriggerEffect).

const char* kEffectNames[] = { "NoResitance", "ContinuousResitance",
                               "SectionResitance", "EffectEx", "Calibrate" };
constexpr int kEffectCount = 5;

struct TriggerUi {
    int effect = 0;              // kEffectNames index
    int start = 0;               // Continuous/Section/EffectEx start position
    int force = 8;               // Continuous force
    int end = 8;                 // Section end position
    bool keep = false;           // EffectEx keepEffect
    int forces[3] = { 8, 8, 8 }; // EffectEx begin/middle/end force
    int frequency = 40;          // EffectEx frequency
    int rumble_large = 0, rumble_small = 0;
};

void ApplyTrigger(bool left, const TriggerUi& ui) {
    u8 mode = 0x00;
    u8 params[10] = {};
    switch (ui.effect) {
        case 1: // ContinuousResitance -> feedback (0x01)
            mode = 0x01;
            params[0] = static_cast<u8>(ui.start > 9 ? 9 : ui.start);
            params[1] = static_cast<u8>(ui.force > 8 ? 8 : ui.force);
            break;
        case 2: // SectionResitance -> weapon (0x02)
            mode = 0x02;
            params[0] = static_cast<u8>(ui.start);
            params[1] = static_cast<u8>(ui.end);
            params[2] = 8;
            break;
        case 3: // EffectEx -> 0x26 extended effect
            mode = 0x26;
            params[0] = static_cast<u8>(255 - ui.start);
            params[1] = ui.keep ? 0x02 : 0x00;
            params[3] = static_cast<u8>(ui.forces[0]);
            params[4] = static_cast<u8>(ui.forces[1]);
            params[5] = static_cast<u8>(ui.forces[2]);
            params[8] = static_cast<u8>(ui.frequency > 0 ? ui.frequency / 2 : 1);
            break;
        case 4: // Calibrate
            mode = 0xFC;
            break;
        default: break; // NoResitance: all-zero effect
    }
    GPU::DualSense::SetTriggerEffect(left, mode, params);
}


// Rumble through the pcsx5 HID output path.
void ApplyRumble(int large_motor, int small_motor) {
    GPU::DualSense::SetRumble(static_cast<u8>(large_motor), static_cast<u8>(small_motor));
}

} // namespace

int main(int, char**) {
    if (!glfwInit()) {
        std::fprintf(stderr, "glfwInit failed\n");
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow* window =
        glfwCreateWindow(980, 800, "PCSX5 DualSense Tester", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "glfwCreateWindow failed\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    GPU::DualSense::EnsureStarted();

    TriggerUi left_ui, right_ui;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        GPU::DualSense::Sample s{};
        const bool connected = GPU::DualSense::GetSample(s);
        g_create_pressed = connected && ReadRawButtonBit(1, 0x10);
        g_mute_pressed = connected && ReadRawButtonBit(2, 0x04);

        if (g_rec.active && connected) {
            RecordFrame(s);
        }
        if (connected) {
            WizardFrame(s);
        }

        bool transport_known = false;
        const bool bt = GPU::DualSense::GetTransport(transport_known);
        const char* transport = !transport_known ? "unknown" : (bt ? "Bluetooth" : "USB");

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize, ImGuiCond_Always);
        ImGui::Begin("DualSense", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

        const int layout = GPU::DualSense::GetReportLayout();
        u16 dev_vid = 0, dev_pid = 0;
        wchar_t dev_product[128] = {};
        GPU::DualSense::GetDeviceInfo(dev_vid, dev_pid, dev_product, 128);
        ImGui::Text("Connected: %s   Transport: %s   Layout: %s   Output report len: %lu",
                    connected ? "YES" : "no", transport,
                    layout == 2 ? "DS4-style (clone/compatible)" : "DualSense",
                    static_cast<unsigned long>(GPU::DualSense::GetOutputReportLength()));
        ImGui::Text("Device: %ls (VID %04X PID %04X)", dev_product, dev_vid, dev_pid);
        if (connected) {
            const SanityFlags sanity = CheckReportSanity(s);
            if (sanity.hat_ok && sanity.touch_ok && sanity.accel_ok) {
                ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1), "Report layout: OK");
            } else {
                ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1),
                                   "Report layout: MISMATCH (hat %s, touch %s, accel %s) — "
                                   "decoded values are unreliable; see Raw HID report below",
                                   sanity.hat_ok ? "ok" : "BAD",
                                   sanity.touch_ok ? "ok" : "BAD",
                                   sanity.accel_ok ? "ok" : "BAD");
            }
        }
        if (!connected) {
            ImGui::TextColored(ImVec4(1, 0.6f, 0.2f, 1),
                               "Waiting for a DualSense (USB or Bluetooth)...");
        }
        ImGui::Separator();

        // ── Buttons with per-button "tested" latching ──────────────────────
        ImGui::TextUnformatted("Buttons ([x] = tested; click a tile, then press the SAME button on the controller):");
        int tested_count = 0;
        for (int i = 0; i < kButtonCount; ++i) {
            const ButtonDef& b = kButtons[i];
            const bool pressed = ButtonPressed(i, s);
            if (pressed) {
                g_tested[i] = true;
            }
            if (g_tested[i]) {
                ++tested_count;
            }
            if (i % 6 != 0) {
                ImGui::SameLine();
            }
            const bool armed = g_wiz.armed == i;
            const ImVec4 col = pressed    ? ImVec4(0.3f, 0.9f, 0.3f, 1.0f)
                               : armed    ? ImVec4(1.0f, 0.85f, 0.2f, 1.0f)
                               : g_tested[i] ? ImVec4(0.9f, 0.9f, 0.9f, 1.0f)
                                          : ImVec4(0.45f, 0.45f, 0.45f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, pressed
                                  ? ImVec4(0.1f, 0.55f, 0.1f, 1.0f)
                                  : armed ? ImVec4(0.55f, 0.45f, 0.05f, 1.0f)
                                          : ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            char label[64];
            std::snprintf(label, sizeof(label), "%s %s%s##%d", b.name,
                          g_tested[i] ? "[x]" : "[ ]",
                          g_wiz.captured[i] ? "*" : "", i);
            // Click a tile to arm the wizard for that button; the next
            // physical press on the controller is registered as its raw code.
            if (ImGui::Button(label, ImVec2(140, 0))) {
                g_wiz.armed = i;
                std::snprintf(g_wiz.status, sizeof(g_wiz.status),
                              "armed: %s — press it on the controller now", b.name);
            }
            if (g_wiz.captured[i] && ImGui::IsItemHovered()) {
                if (IsHatCode(i)) {
                    ImGui::SetTooltip("mapped: b0 hat value 0x%X",
                                      g_wiz.cap_value[i] & 0x0F);
                } else {
                    ImGui::SetTooltip("mapped: b%u bit 0x%02X",
                                      g_wiz.cap_byte[i], g_wiz.cap_mask[i]);
                }
            }
            ImGui::PopStyleColor(2);
        }
        ImGui::Text("Tested: %d / %d", tested_count, kButtonCount);
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset checklist")) {
            std::memset(g_tested, 0, sizeof(g_tested));
        }

        // ── Mapping wizard controls ────────────────────────────────────────
        ImGui::Text("Mapping: %s", g_wiz.status);
        ImGui::SameLine();
        if (ImGui::SmallButton("Disarm")) {
            g_wiz.armed = -1;
            std::snprintf(g_wiz.status, sizeof(g_wiz.status), "disarmed");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset mapping")) {
            // Full reset: captures, codes, arming and the prev-report
            // baseline (so stale state can never leak into a fresh pass).
            g_wiz = MappingWizard();
            std::snprintf(g_wiz.status, sizeof(g_wiz.status), "mapping cleared");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Save mapping")) {
            char path[64];
            if (SaveMapping(path, sizeof(path))) {
                std::snprintf(g_wiz.status, sizeof(g_wiz.status),
                              "mapping saved to %s", path);
            } else {
                std::snprintf(g_wiz.status, sizeof(g_wiz.status),
                              "mapping save FAILED (%s)", path);
            }
        }
        ImGui::Separator();

        // ── Sticks / triggers / touchpad / motion ──────────────────────────
        ImGui::Columns(2, "io", true);

        ImGui::TextUnformatted("Sticks:");
        StickPlot("Left stick", s.lx, s.ly);
        ImGui::SameLine();
        StickPlot("Right stick", s.rx, s.ry);
        ImGui::NewLine();
        TriggerBar("L2", s.l2);
        TriggerBar("R2", s.r2);
        ImGui::Spacing();
        ImGui::Text("Touchpad (%u finger%s):", s.touch_count,
                    s.touch_count == 1 ? "" : "s");
        TouchpadView(s);

        ImGui::NextColumn();

        ImGui::Text("Accel (g):    x=%+.3f y=%+.3f z=%+.3f",
                    s.accel[0], s.accel[1], s.accel[2]);
        ImGui::Text("Gyro (rad/s): pitch=%+.3f yaw=%+.3f roll=%+.3f",
                    s.gyro[0], s.gyro[1], s.gyro[2]);
        ImGui::Text("Battery: %u%%%s%s   Headphones: %s   Trigger feedback L=%u R=%u",
                    s.battery_level,
                    s.battery_charging ? " (charging)" : "",
                    s.battery_full ? " (full)" : "",
                    s.headphone_connected ? "connected" : "none",
                    s.trigger_feedback[0], s.trigger_feedback[1]);
        ImGui::Separator();

        // ── Output: rumble + adaptive triggers ─────────────────────────────
        ImGui::TextUnformatted("Rumble:");
        {
            static int rumble_fmt = 0;
            const char* kFmts[] = { "Auto", "DS5 USB (0x02)", "DS5 BT (0x31)",
                                    "DS4 USB (0x05)", "DS4 BT (0x11)" };
            ImGui::SetNextItemWidth(180);
            if (ImGui::Combo("Wire format", &rumble_fmt, kFmts, 5)) {
                GPU::DualSense::SetOutputFormatOverride(rumble_fmt);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(set a motor slider, then try each)");
        }
        bool rumble_changed = false;
        rumble_changed |= ImGui::SliderInt("Large motor", &left_ui.rumble_large, 0, 255);
        rumble_changed |= ImGui::SliderInt("Small motor", &left_ui.rumble_small, 0, 255);
        if (rumble_changed) {
            ApplyRumble(left_ui.rumble_large, left_ui.rumble_small);
        }
        ImGui::SameLine();
        if (ImGui::Button("Stop rumble")) {
            left_ui.rumble_large = left_ui.rumble_small = 0;
            ApplyRumble(0, 0);
        }
        ImGui::Separator();

        struct Side { const char* name; bool left; TriggerUi* ui; };
        const Side sides[2] = {
            { "L2 trigger", true, &left_ui },
            { "R2 trigger", false, &right_ui },
        };
        for (const Side& side : sides) {
            ImGui::PushID(side.name);
            ImGui::TextUnformatted(side.name);
            bool changed = false;
            changed |= ImGui::Combo("Effect", &side.ui->effect, kEffectNames, kEffectCount);
            if (side.ui->effect == 1) {          // ContinuousResitance
                changed |= ImGui::SliderInt("Start position", &side.ui->start, 0, 255);
                changed |= ImGui::SliderInt("Force", &side.ui->force, 0, 255);
            } else if (side.ui->effect == 2) {   // SectionResitance
                changed |= ImGui::SliderInt("Start position", &side.ui->start, 0, 255);
                changed |= ImGui::SliderInt("End position", &side.ui->end, 0, 255);
            } else if (side.ui->effect == 3) {   // EffectEx
                changed |= ImGui::SliderInt("Start position", &side.ui->start, 0, 255);
                changed |= ImGui::Checkbox("Keep effect", &side.ui->keep);
                changed |= ImGui::SliderInt("Begin force", &side.ui->forces[0], 0, 255);
                changed |= ImGui::SliderInt("Middle force", &side.ui->forces[1], 0, 255);
                changed |= ImGui::SliderInt("End force", &side.ui->forces[2], 0, 255);
                changed |= ImGui::SliderInt("Frequency", &side.ui->frequency, 0, 255);
            } else if (side.ui->effect == 4) {   // Calibrate
                ImGui::TextDisabled("Sends trigger calibration (0xFC)");
            }
            if (changed) {
                ApplyTrigger(side.left, *side.ui);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Full block")) {
                // Continuous resistance from position 0 at max force: the
                // trigger cannot be pressed down.
                side.ui->effect = 1;
                side.ui->start = 0;
                side.ui->force = 255;
                ApplyTrigger(side.left, *side.ui);
            }
            ImGui::PopID();
        }
        if (ImGui::Button("All effects OFF")) {
            left_ui.effect = right_ui.effect = 0;
            left_ui.rumble_large = left_ui.rumble_small = 0;
            ApplyRumble(0, 0);
            ApplyTrigger(true, left_ui);
            ApplyTrigger(false, right_ui);
        }

        // ── Player indicator LEDs ──────────────────────────────────────────
        // Hardware bit order is mirrored vs the DS5W doc: bit 0 is the
        // RIGHTMOST LED, bit 4 the leftmost (verified on hardware).  The
        // checkboxes are labeled by physical position and mapped to bits.
        ImGui::TextUnformatted("Player LEDs:");
        static bool led_bits[5] = {};
        static bool led_fade = false;
        const char* kLedNames[5] = { "Left", "Mid-left", "Middle", "Mid-right", "Right" };
        bool led_changed = false;
        for (int i = 0; i < 5; ++i) {
            if (i > 0) {
                ImGui::SameLine();
            }
            ImGui::PushID(100 + i);
            led_changed |= ImGui::Checkbox(kLedNames[i], &led_bits[i]);
            ImGui::PopID();
        }
        ImGui::SameLine();
        led_changed |= ImGui::Checkbox("Fade", &led_fade);
        if (led_changed) {
            u8 mask = 0;
            for (int i = 0; i < 5; ++i) {
                if (led_bits[i]) {
                    mask = static_cast<u8>(mask | (1u << (4 - i)));
                }
            }
            GPU::DualSense::SetPlayerLeds(mask, led_fade);
        }

        // ── Microphone LED / lightbar / LED options ────────────────────────
        static int mic_led = 0;
        const char* kMicModes[] = { "Mic LED off", "Mic LED on", "Mic LED pulse" };
        ImGui::SetNextItemWidth(150);
        if (ImGui::Combo("Mic", &mic_led, kMicModes, 3)) {
            GPU::DualSense::SetMicLed(static_cast<u8>(mic_led));
        }
        ImGui::SameLine();
        static int lb[3] = { 0, 0, 0 };
        bool lb_changed = false;
        ImGui::SetNextItemWidth(70);
        lb_changed |= ImGui::SliderInt("R", &lb[0], 0, 255);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        lb_changed |= ImGui::SliderInt("G", &lb[1], 0, 255);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        lb_changed |= ImGui::SliderInt("B", &lb[2], 0, 255);
        if (lb_changed) {
            GPU::DualSense::SetLightBar(static_cast<u8>(lb[0]), static_cast<u8>(lb[1]),
                                        static_cast<u8>(lb[2]));
        }
        ImGui::SameLine();
        static int led_brightness = 1;
        static bool leds_disabled = false;
        const char* kBrightness[] = { "High", "Medium", "Low" };
        ImGui::SetNextItemWidth(100);
        bool opt_changed = ImGui::Combo("Brightness", &led_brightness, kBrightness, 3);
        ImGui::SameLine();
        opt_changed |= ImGui::Checkbox("Disable all LEDs", &leds_disabled);
        if (opt_changed) {
            GPU::DualSense::SetLedOptions(static_cast<u8>(led_brightness), leds_disabled);
        }

        ImGui::Columns(1);
        ImGui::Separator();

        // ── Mapping recorder ───────────────────────────────────────────────
        ImGui::TextUnformatted("Mapping recorder (press buttons one at a time while recording):");
        if (!g_rec.active) {
            if (ImGui::Button("Record")) {
                g_rec.lines.clear();
                g_rec.have_prev = false;
                g_rec.start_ms = GetTickCount();
                g_rec.active = true;
                RecLog("recording started — press controller buttons one at a time");
            }
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.1f, 0.1f, 1.0f));
            if (ImGui::Button("Stop")) {
                g_rec.active = false;
                RecLog("recording stopped");
            }
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "REC");
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear log")) {
            g_rec.lines.clear();
        }
        ImGui::SameLine();
        if (ImGui::Button("Save log")) {
            char path[64];
            if (SaveRecording(path, sizeof(path))) {
                RecLog("saved to %s", path);
            } else {
                RecLog("save FAILED (could not open %s for writing)", path);
            }
        }
        ImGui::SameLine();
        ImGui::Text("(%zu lines)", g_rec.lines.size());

        ImGui::BeginChild("reclog", ImVec2(0, 140), true,
                          ImGuiWindowFlags_HorizontalScrollbar);
        for (const std::string& line : g_rec.lines) {
            ImGui::TextUnformatted(line.c_str());
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f) {
            ImGui::SetScrollHereY(1.0f); // follow new events while at the bottom
        }
        ImGui::EndChild();

        // ── Live raw report (diagnostic: exactly what the controller sends) ─
        if (ImGui::CollapsingHeader("Raw HID report (live)")) {
            static u8 prev_raw[256];
            static size_t prev_len = 0;
            u8 raw[256];
            const size_t len = GPU::DualSense::GetLastRawReport(raw, sizeof(raw));
            if (len == 0) {
                ImGui::TextUnformatted("no report yet");
            } else {
                ImGui::Text("%zu bytes, report id 0x%02X; green = changed since last frame",
                            len, raw[0]);
                for (size_t i = 0; i < len; ++i) {
                    if (i % 16 == 0) {
                        ImGui::TextDisabled("%03zu:", i);
                        ImGui::SameLine();
                    } else {
                        ImGui::SameLine();
                    }
                    const bool changed = i >= prev_len || raw[i] != prev_raw[i];
                    if (changed) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 0.9f, 0.3f, 1));
                    }
                    ImGui::Text("%02X", raw[i]);
                    if (changed) {
                        ImGui::PopStyleColor();
                    }
                }
                std::memcpy(prev_raw, raw, len);
                prev_len = len;
            }
        }

        ImGui::End();

        ImGui::Render();
        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.10f, 0.10f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    // Leave the controller in a neutral state on exit.
    GPU::DualSense::SetRumble(0, 0);
    const u8 zero[10] = {};
    GPU::DualSense::SetTriggerEffect(true, 0x00, zero);
    GPU::DualSense::SetTriggerEffect(false, 0x00, zero);
    GPU::DualSense::SetMicLed(0);
    GPU::DualSense::SetLightBar(0, 0, 0);
    GPU::DualSense::SetPlayerLeds(0, false);
    GPU::DualSense::SetLedOptions(1, false);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
