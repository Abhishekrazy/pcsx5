#include "gpu.h"
#include "vk_context.h"
#include "vk_present.h"
#include "vk_draw.h"
#include "dualsense_hid.h"
#include "../common/log.h"
#include "../memory/memory.h"
#include <windows.h>
#include <xinput.h>
#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <vulkan/vulkan.h>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <cstdio>

namespace GPU {

    static GLFWwindow* g_window  = nullptr;
    static HWND        g_hwnd    = nullptr;
    static int         g_width   = 1280;
    static int         g_height  = 720;

    // Real Vulkan backend (Phase 5 M1).  g_vk is non-null when instance +
    // device came up; g_vk_ready when the swapchain present path is usable.
    // Both null/false -> the legacy GDI DIB path presents instead.
    static VkContext*  g_vk       = nullptr;
    static bool        g_vk_ready = false;
    // Full-resolution BGRA8 conversion buffer for the guest framebuffer.
    static std::vector<u32> g_vk_pixels;

    static PadButtonState g_pad_state = { 0, 127, 127, 127, 127, 0, 0, {0, 0} };
    static u32            g_keyboard_buttons = 0;
    static std::mutex     g_pad_mutex;

    // --embed mode: window starts hidden; the launcher reparents it via the
    // HWND printed to stdout (PCSX5_WINDOW_HANDLE=...).
    static bool           g_embed_mode = false;

    void SetEmbeddedMode(bool enabled) { g_embed_mode = enabled; }

    // In-process window-handle callback (see gpu.h).  When set, the HWND is
    // delivered through the callback and the stdout line is suppressed.
    static WindowCreatedCallback g_window_created_cb = nullptr;
    static void*                 g_window_created_cb_user = nullptr;

    void SetWindowCreatedCallback(WindowCreatedCallback callback, void* user) {
        g_window_created_cb = callback;
        g_window_created_cb_user = user;
    }

    // Borderless-fullscreen state (F11 toggle).  Windowed pos/size is saved
    // on entry so leaving fullscreen restores the exact window placement.
    static bool g_fullscreen = false;
    static int  g_saved_x = 0, g_saved_y = 0, g_saved_w = 0, g_saved_h = 0;

    static void ToggleFullscreen() {
        if (!g_window) return;
        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        if (!monitor) return;
        if (!g_fullscreen) {
            glfwGetWindowPos(g_window, &g_saved_x, &g_saved_y);
            glfwGetWindowSize(g_window, &g_saved_w, &g_saved_h);
            const GLFWvidmode* mode = glfwGetVideoMode(monitor);
            glfwSetWindowMonitor(g_window, monitor, 0, 0,
                                 mode->width, mode->height, mode->refreshRate);
            g_fullscreen = true;
            LOG_INFO(GPU, "Fullscreen ON (%dx%d @ %dHz).", mode->width, mode->height, mode->refreshRate);
        } else {
            glfwSetWindowMonitor(g_window, nullptr,
                                 g_saved_x, g_saved_y, g_saved_w, g_saved_h, 0);
            g_fullscreen = false;
            LOG_INFO(GPU, "Fullscreen OFF (restored %dx%d window).", g_saved_w, g_saved_h);
        }
        // The surface size changed: recreate the Vulkan swapchain to match,
        // or drop to the GDI fallback if recreation fails.
        if (g_vk && g_vk_ready) {
            int fb_w = 0, fb_h = 0;
            glfwGetFramebufferSize(g_window, &fb_w, &fb_h);
            if (fb_w > 0 && fb_h > 0 &&
                !VkPresentResize(g_vk, static_cast<u32>(fb_w), static_cast<u32>(fb_h))) {
                g_vk_ready = false;
            }
        }
    }

    // Dynamically loaded Windows XInput GetState/SetState
    typedef DWORD(WINAPI* PFN_XInputGetState)(DWORD dwUserIndex, XINPUT_STATE* pState);
    typedef DWORD(WINAPI* PFN_XInputSetState)(DWORD dwUserIndex, XINPUT_VIBRATION* pVibration);
    static PFN_XInputGetState g_XInputGetState = nullptr;
    static PFN_XInputSetState g_XInputSetState = nullptr;
    static HMODULE g_xinput_dll = nullptr;

    static void InitializeXInput() {
        const char* dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
        for (const auto& dll : dlls) {
            g_xinput_dll = LoadLibraryA(dll);
            if (g_xinput_dll) {
                g_XInputGetState = (PFN_XInputGetState)GetProcAddress(g_xinput_dll, "XInputGetState");
                g_XInputSetState = (PFN_XInputSetState)GetProcAddress(g_xinput_dll, "XInputSetState");
                if (g_XInputGetState) {
                    LOG_INFO(GPU, "Successfully loaded Windows XInput API from %s (rumble: %s)",
                             dll, g_XInputSetState ? "yes" : "no");
                    break;
                }
                FreeLibrary(g_xinput_dll);
                g_xinput_dll = nullptr;
            }
        }
        if (!g_XInputGetState) {
            LOG_WARN(GPU, "Failed to load Windows XInput DLL. Controller support in emulator is disabled.");
        }
    }

    // Host-side DIB backing store (BGRA, 32-bit, 1280x720)
    static std::vector<u32> g_dib_buffer;

    static u32 g_fb_width = 1920;
    static u32 g_fb_height = 1080;
    static u32 g_fb_format = 0; // 0 = RGBA8, 1 = BGRA8, 2 = RGB565

    typedef HRESULT(WINAPI* PFN_DwmFlush)();
    static PFN_DwmFlush pfn_DwmFlush = nullptr;

    // ─────────────────────────────────────────────────────────────────────────
    // Boot status state (SetBootStatus / first-flip handover)
    // ─────────────────────────────────────────────────────────────────────────

    // g_boot_active: the boot screen still owns the window.  Cleared on the
    // first presented guest frame, after which SetBootStatus is a no-op.
    static std::atomic<bool> g_boot_active{true};

    struct BootStatusState {
        std::string stage;   // milestone currently executing
        int done  = -1;      // determinate progress (total > 0)
        int total = -1;
        std::deque<std::string> log; // recently completed stages
    };
    static constexpr size_t kBootLogMax = 6;

    // Boot-screen state + the mutex guarding it (and every boot-screen GDI
    // render, so the guest thread and the main thread cannot tear a frame).
    // Lives in a never-destroyed function-local singleton: SetBootStatus can
    // be reached from any thread at any time, and this avoids static
    // destruction-order hazards at process exit.
    struct BootState {
        BootStatusState status;
        std::mutex      mutex;
    };
    static BootState& Boot() {
        static BootState* s = new BootState();
        return *s;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Boot-screen primitives (software rendering into g_dib_buffer)
    // ─────────────────────────────────────────────────────────────────────────

    static void FillRect(int x, int y, int w, int h, u32 bgra) {
        for (int row = y; row < y + h; ++row) {
            if (row < 0 || row >= g_height) continue;
            for (int col = x; col < x + w; ++col) {
                if (col < 0 || col >= g_width) continue;
                g_dib_buffer[row * g_width + col] = bgra;
            }
        }
    }

    static void DrawHLine(int x, int y, int len, u32 bgra) {
        FillRect(x, y, len, 1, bgra);
    }

    // Simple 5x7 pixel-font bitmap for ASCII 0x20-0x5F (uppercase + digits + symbols)
    static const u8 k_font5x7[64][7] = {
        {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ' '
        {0x04,0x04,0x04,0x04,0x00,0x04,0x00}, // '!'
        {0x0A,0x0A,0x00,0x00,0x00,0x00,0x00}, // '"'
        {0x0A,0x1F,0x0A,0x1F,0x0A,0x00,0x00}, // '#'
        {0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04}, // '$'
        {0x18,0x19,0x02,0x04,0x09,0x03,0x00}, // '%'
        {0x0C,0x12,0x14,0x08,0x15,0x12,0x0D}, // '&'
        {0x04,0x04,0x00,0x00,0x00,0x00,0x00}, // '\''
        {0x02,0x04,0x08,0x08,0x08,0x04,0x02}, // '('
        {0x08,0x04,0x02,0x02,0x02,0x04,0x08}, // ')'
        {0x00,0x04,0x15,0x0E,0x15,0x04,0x00}, // '*'
        {0x00,0x04,0x04,0x1F,0x04,0x04,0x00}, // '+'
        {0x00,0x00,0x00,0x00,0x00,0x04,0x08}, // ','
        {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}, // '-'
        {0x00,0x00,0x00,0x00,0x00,0x04,0x00}, // '.'
        {0x00,0x01,0x02,0x04,0x08,0x10,0x00}, // '/'
        {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, // '0'
        {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, // '1'
        {0x0E,0x11,0x01,0x06,0x08,0x10,0x1F}, // '2'
        {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}, // '3'
        {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, // '4'
        {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, // '5'
        {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, // '6'
        {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, // '7'
        {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, // '8'
        {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, // '9'
        {0x00,0x04,0x00,0x00,0x00,0x04,0x00}, // ':'
        {0x00,0x04,0x00,0x00,0x00,0x04,0x08}, // ';'
        {0x02,0x04,0x08,0x10,0x08,0x04,0x02}, // '<'
        {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00}, // '='
        {0x08,0x04,0x02,0x01,0x02,0x04,0x08}, // '>'
        {0x0E,0x11,0x01,0x02,0x04,0x00,0x04}, // '?'
        {0x0E,0x11,0x17,0x15,0x17,0x10,0x0E}, // '@'
        {0x04,0x0A,0x11,0x11,0x1F,0x11,0x11}, // 'A'
        {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, // 'B'
        {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, // 'C'
        {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}, // 'D'
        {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, // 'E'
        {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, // 'F'
        {0x0E,0x11,0x10,0x17,0x11,0x11,0x0E}, // 'G'
        {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, // 'H'
        {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, // 'I'
        {0x01,0x01,0x01,0x01,0x01,0x11,0x0E}, // 'J'
        {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, // 'K'
        {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, // 'L'
        {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, // 'M'
        {0x11,0x11,0x19,0x15,0x13,0x11,0x11}, // 'N'
        {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, // 'O'
        {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, // 'P'
        {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, // 'Q'
        {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, // 'R'
        {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, // 'S'
        {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, // 'T'
        {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, // 'U'
        {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, // 'V'
        {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}, // 'W'
        {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, // 'X'
        {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, // 'Y'
        {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}, // 'Z'
        {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E}, // '['
        {0x00,0x10,0x08,0x04,0x02,0x01,0x00}, // '\'
        {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E}, // ']'
        {0x04,0x0A,0x11,0x00,0x00,0x00,0x00}, // '^'
        {0x00,0x00,0x00,0x00,0x00,0x00,0x1F}, // '_'
    };

    static void DrawChar(int px, int py, char c, u32 bgra, int scale = 2) {
        if (c < 0x20 || c > 0x5F) c = '?';
        const u8* glyph = k_font5x7[c - 0x20];
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5; ++col) {
                if (glyph[row] & (0x10 >> col)) {
                    FillRect(px + col * scale, py + row * scale, scale, scale, bgra);
                }
            }
        }
    }

    static void DrawText(int px, int py, const char* text, u32 bgra, int scale = 2) {
        int x = px;
        for (const char* p = text; *p; ++p) {
            if (*p == '\n') { py += 7 * scale + scale; x = px; continue; }
            DrawChar(x, py, *p, bgra, scale);
            x += 6 * scale;
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Draw the PCSX5 boot status screen into g_dib_buffer
    // ─────────────────────────────────────────────────────────────────────────

    // Uppercase-copy into a bounded buffer (the 5x7 font covers 0x20-0x5F).
    static void BootUpper(char* dst, size_t dst_size, const std::string& src) {
        size_t i = 0;
        for (; i + 1 < dst_size && i < src.size(); ++i) {
            const char c = src[i];
            dst[i] = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
        }
        dst[i] = '\0';
    }

    // Draw the PCSX5 boot status screen into g_dib_buffer.  Caller must hold
    // Boot().mutex.  Shows only real milestones recorded via SetBootStatus.
    static void DrawBootScreen() {
        // Background: deep navy gradient (simulate with 3 bands)
        FillRect(0,   0, g_width, 240, 0xFF0A0A1A); // very dark navy
        FillRect(0, 240, g_width, 240, 0xFF0E0E28); // dark navy
        FillRect(0, 480, g_width, 240, 0xFF12122F); // slightly lighter

        // ── Top accent bar ──────────────────────────────────────────────────
        FillRect(0, 0, g_width, 4, 0xFF00D4FF); // PS5 accent cyan

        // ── PCSX5 large logo text ────────────────────────────────────────────
        DrawText(160, 80, "PCSX5",  0xFF00D4FF, 8); // cyan, 8x scale (40x56px letters)
        DrawText(810, 96, "PS5 EMULATOR", 0xFFFFFFFF, 3);
        DrawText(810, 126, "BOOT IN PROGRESS", 0xFF8888BB, 2);

        // Logo divider line
        FillRect(155, 148, 970, 3, 0xFF00D4FF);

        // ── Status panel box ────────────────────────────────────────────────
        FillRect(155, 165, 970, 270, 0xFF0F1020); // dark panel background
        FillRect(155, 165, 970,   2, 0xFF2244AA); // top border
        FillRect(155, 433, 970,   2, 0xFF2244AA); // bottom border
        FillRect(155, 165,   2, 270, 0xFF2244AA); // left border
        FillRect(1123,165,  2, 270, 0xFF2244AA); // right border

        // Current stage (the milestone executing right now)
        DrawText(170, 176, "CURRENT STAGE", 0xFF2299FF, 2);
        DrawHLine(170, 194, 350, 0xFF2244AA);

        char line[96];
        BootUpper(line, sizeof(line), Boot().status.stage.empty()
                                          ? "STARTING UP" : Boot().status.stage);
        DrawText(170, 206, line, 0xFFFFFFFF, 3);

        // Determinate progress bar when the stage reports done/total
        if (Boot().status.total > 0) {
            int done = Boot().status.done;
            if (done < 0) done = 0;
            if (done > Boot().status.total) done = Boot().status.total;
            const int bar_x = 170, bar_y = 246, bar_w = 940, bar_h = 18;
            FillRect(bar_x - 1, bar_y - 1, bar_w + 2, bar_h + 2, 0xFF2244AA);
            FillRect(bar_x, bar_y, bar_w, bar_h, 0xFF070713);
            const int fill_w = static_cast<int>(
                (static_cast<long long>(done) * bar_w) / Boot().status.total);
            if (fill_w > 0) FillRect(bar_x, bar_y, fill_w, bar_h, 0xFF00D4FF);
            char count[32];
            std::snprintf(count, sizeof(count), "%d / %d", done, Boot().status.total);
            DrawText(bar_x + bar_w - 150, bar_y + 26, count, 0xFF8888BB, 2);
        }

        // Recently completed stages (real, in execution order, oldest first)
        DrawText(170, 300, "COMPLETED", 0xFF2299FF, 2);
        DrawHLine(170, 318, 350, 0xFF2244AA);
        int row_y = 326;
        for (const auto& entry : Boot().status.log) {
            BootUpper(line, sizeof(line), entry);
            DrawText(170, row_y, line, 0xFF00FF88, 2);
            row_y += 18;
        }

        // ── Bottom info bar ──────────────────────────────────────────────────
        FillRect(0, g_height - 36, g_width, 36, 0xFF070713);
        FillRect(0, g_height - 36, g_width,  2, 0xFF00D4FF);
        DrawText(20, g_height - 26, "PCSX5 V0.1  |  PS5 HLE EMULATOR", 0xFF555577, 2);
        DrawText(g_width - 368, g_height - 26, "PRESS ESC TO EXIT", 0xFF555577, 2);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Present g_dib_buffer to the window via GDI StretchDIBits
    // ─────────────────────────────────────────────────────────────────────────

    // Largest centered rect of aspect src_w:src_h inside dst_w x dst_h
    // (letterbox/pillarbox).
    static void FitRect(int src_w, int src_h, int dst_w, int dst_h,
                        int* out_x, int* out_y, int* out_w, int* out_h) {
        int x = 0, y = 0, w = dst_w, h = dst_h;
        if (src_w > 0 && src_h > 0 && dst_w > 0 && dst_h > 0) {
            if (static_cast<long long>(src_w) * dst_h > static_cast<long long>(src_h) * dst_w) {
                h = static_cast<int>((static_cast<long long>(dst_w) * src_h) / src_w);
                y = (dst_h - h) / 2;
            } else if (static_cast<long long>(src_w) * dst_h < static_cast<long long>(src_h) * dst_w) {
                w = static_cast<int>((static_cast<long long>(dst_h) * src_w) / src_h);
                x = (dst_w - w) / 2;
            }
        }
        *out_x = x; *out_y = y; *out_w = w; *out_h = h;
    }

    static void PresentDIB() {
        if (!g_hwnd) return;
        HDC hdc = GetDC(g_hwnd);
        if (!hdc) return;

        RECT client_rect;
        GetClientRect(g_hwnd, &client_rect);
        int dst_w = client_rect.right  - client_rect.left;
        int dst_h = client_rect.bottom - client_rect.top;

        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth       = g_width;
        bmi.bmiHeader.biHeight      = -g_height; // top-down
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        // Aspect-correct: blit into the largest centered rect of the DIB's
        // aspect ratio; black-fill the bars when the aspects differ.
        int fx, fy, fw, fh;
        FitRect(g_width, g_height, dst_w, dst_h, &fx, &fy, &fw, &fh);
        if (fx != 0 || fy != 0 || fw != dst_w || fh != dst_h) {
            HBRUSH black = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
            ::FillRect(hdc, &client_rect, black);
        }

        StretchDIBits(hdc,
            fx, fy, fw, fh,
            0, 0, g_width, g_height,
            g_dib_buffer.data(), &bmi,
            DIB_RGB_COLORS, SRCCOPY);

        ReleaseDC(g_hwnd, hdc);

        if (pfn_DwmFlush) {
            pfn_DwmFlush();
        }
    }

    // Draw + present the boot screen under Boot().mutex.  No-op without a
    // window/DIB (headless tests) or after the first guest frame took over.
    static void RenderBootScreenNow() {
        std::lock_guard<std::mutex> lock(Boot().mutex);
        if (!g_hwnd || g_dib_buffer.empty()) return;
        DrawBootScreen();
        PresentDIB();
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Keyboard callback
    // ─────────────────────────────────────────────────────────────────────────

    static void KeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
        (void)window; (void)scancode; (void)mods;

        // F11: borderless fullscreen toggle (not a pad binding).
        if (key == GLFW_KEY_F11) {
            if (action == GLFW_PRESS) ToggleFullscreen();
            return;
        }

        u32 button_flag = 0;
        switch (key) {
            case GLFW_KEY_W:    case GLFW_KEY_UP:    button_flag = 0x00000010; break; // PAD_UP
            case GLFW_KEY_S:    case GLFW_KEY_DOWN:  button_flag = 0x00000040; break; // PAD_DOWN
            case GLFW_KEY_A:    case GLFW_KEY_LEFT:  button_flag = 0x00000080; break; // PAD_LEFT
            case GLFW_KEY_D:    case GLFW_KEY_RIGHT: button_flag = 0x00000020; break; // PAD_RIGHT
            case GLFW_KEY_SPACE:                     button_flag = 0x00004000; break; // PAD_CROSS
            case GLFW_KEY_Z:                         button_flag = 0x00004000; break; // PAD_CROSS (alias)
            case GLFW_KEY_ENTER:                     button_flag = 0x00002000; break; // PAD_CIRCLE
            case GLFW_KEY_X:                         button_flag = 0x00002000; break; // PAD_CIRCLE (alias)
            case GLFW_KEY_LEFT_SHIFT:                button_flag = 0x00008000; break; // PAD_SQUARE
            case GLFW_KEY_V:                         button_flag = 0x00008000; break; // PAD_SQUARE (alias)
            case GLFW_KEY_C:                         button_flag = 0x00001000; break; // PAD_TRIANGLE
            case GLFW_KEY_Q:                         button_flag = 0x00000400; break; // PAD_L1
            case GLFW_KEY_E:                         button_flag = 0x00000800; break; // PAD_R1
            case GLFW_KEY_R:                         button_flag = 0x00000100; break; // PAD_L2 (digital)
            case GLFW_KEY_F:                         button_flag = 0x00000200; break; // PAD_R2 (digital)
            case GLFW_KEY_TAB:                       button_flag = 0x00000008; break; // PAD_OPTIONS
            case GLFW_KEY_T:                         button_flag = 0x00100000; break; // PAD_TOUCHPAD (click)
            case GLFW_KEY_ESCAPE:
                // Signal close
                if (g_window) glfwSetWindowShouldClose(g_window, GLFW_TRUE);
                button_flag = 0x00100000; // PAD_OPTIONS
                break;
            default: break;
        }

        if (button_flag != 0) {
            std::lock_guard<std::mutex> lock(g_pad_mutex);
            if (action == GLFW_PRESS || action == GLFW_REPEAT) {
                g_keyboard_buttons |= button_flag;
                LOG_DEBUG(GPU, "Key pressed, mapping button flag: 0x%X", button_flag);
            } else if (action == GLFW_RELEASE) {
                g_keyboard_buttons &= ~button_flag;
                LOG_DEBUG(GPU, "Key released, clearing button flag: 0x%X", button_flag);
            }
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Public API
    // ─────────────────────────────────────────────────────────────────────────

    void SetBootStatus(const char* stage, int done, int total) {
        if (!g_boot_active.load(std::memory_order_acquire)) return;
        std::lock_guard<std::mutex> lock(Boot().mutex);
        const std::string new_stage = stage ? stage : "";
        if (new_stage != Boot().status.stage) {
            // Stage change: retire the previous stage into the completed log.
            if (!Boot().status.stage.empty()) {
                Boot().status.log.push_back(Boot().status.stage);
                while (Boot().status.log.size() > kBootLogMax) Boot().status.log.pop_front();
            }
            Boot().status.stage = new_stage;
        }
        Boot().status.done  = done;
        Boot().status.total = total;
        // Render immediately (GDI on the window HDC is thread-safe per-DC);
        // this keeps progress visible while the main thread is blocked in
        // subsystem init or module load.  No window yet -> state is recorded
        // and rendered once GPU::Initialize presents the first frame.
        if (g_hwnd && !g_dib_buffer.empty()) {
            DrawBootScreen();
            PresentDIB();
        }
    }

    bool IsBootScreenActive() {
        return g_boot_active.load(std::memory_order_acquire);
    }

    bool Initialize() {
        LOG_INFO(GPU, "Initializing GPU subsystem (GLFW + Vulkan Backend)...");

        if (!glfwInit()) {
            LOG_ERROR(GPU, "Failed to initialize GLFW.");
            return false;
        }

        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE,  GLFW_FALSE);
        if (g_embed_mode) {
            // Embedded in the launcher UI: stay hidden until the UI reparents
            // us into its window (avoids a standalone-window flash).
            glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        }

        g_window = glfwCreateWindow(g_width, g_height, "pcsx5 - PlayStation 5 Emulator", nullptr, nullptr);
        if (!g_window) {
            LOG_ERROR(GPU, "Failed to create GLFW window.");
            glfwTerminate();
            return false;
        }
        g_hwnd = glfwGetWin32Window(g_window);
        glfwSetKeyCallback(g_window, KeyCallback);
        LOG_INFO(GPU, "GLFW Window created successfully (%dx%d).", g_width, g_height);

        // Machine-parseable line for the launcher UI: it reparents this HWND
        // into its own window to render the game embedded.  In-process hosts
        // receive the handle through the registered callback instead.
        if (g_window_created_cb) {
            g_window_created_cb(static_cast<unsigned long long>(
                                    reinterpret_cast<uintptr_t>(g_hwnd)),
                                g_window_created_cb_user);
        } else {
            std::printf("PCSX5_WINDOW_HANDLE=%llu\n",
                        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_hwnd)));
            std::fflush(stdout);
        }

        // Allocate host DIB buffer (BGRA 32-bit)
        g_dib_buffer.assign(static_cast<size_t>(g_width) * g_height, 0xFF000000u);

        // Bring up the real Vulkan backend (dynamic vulkan-1.dll loading, no
        // SDK).  Any failure leaves g_vk null and we stay on the GDI DIB path.
        SetBootStatus("Creating Vulkan device", -1, -1);
        g_vk = VkContextCreate(g_window);
        if (g_vk) {
            SetBootStatus(g_vk->device_name[0] ? (std::string("Vulkan device: ") + g_vk->device_name).c_str()
                                               : "Vulkan device created", -1, -1);
            SetBootStatus("Creating Vulkan swapchain", -1, -1);
            g_vk_ready = VkPresentInitialize(g_vk, static_cast<u32>(g_width),
                                             static_cast<u32>(g_height));
            if (g_vk_ready) {
                LOG_INFO(GPU, "Vulkan backend active on '%s' — guest frames present via swapchain.",
                         g_vk->device_name);
            } else {
                LOG_WARN(GPU, "Vulkan swapchain/present init failed — GDI fallback for presents.");
            }
            // M3.2c draw executor: independent of the swapchain path (draws
            // render into offscreen guest images).
            if (VkDrawInitialize(g_vk)) {
                LOG_INFO(GPU, "Guest draw executor ready.");
            }
        } else {
            LOG_WARN(GPU, "Vulkan device init failed — GDI fallback for presents.");
        }

        // Dynamically load DWM API for VSync (DwmFlush)
        HMODULE hDwmapi = LoadLibraryA("dwmapi.dll");
        if (hDwmapi) {
            pfn_DwmFlush = reinterpret_cast<PFN_DwmFlush>(GetProcAddress(hDwmapi, "DwmFlush"));
            if (pfn_DwmFlush) {
                LOG_INFO(GPU, "DWM composition API 'dwmapi.dll' resolved successfully for VSync.");
            }
        }

        // Draw and present the boot screen immediately ("first frame").  The
        // stage log already holds every milestone posted before the window
        // existed (config, NID db, subsystem init), so they show up here.
        SetBootStatus("GPU subsystem ready", -1, -1);
        glfwPollEvents();
        LOG_INFO(GPU, "Boot screen presented successfully (first frame displayed).");

        return true;
    }

    void Shutdown() {
        LOG_INFO(GPU, "Shutting down GPU subsystem...");
        if (g_vk) {
            VkDrawShutdown();
            VkPresentShutdown(g_vk);
            VkContextDestroy(g_vk);
            g_vk = nullptr;
            g_vk_ready = false;
        }
        g_vk_pixels.clear();
        if (g_window) {
            glfwDestroyWindow(g_window);
            g_window = nullptr;
            g_hwnd   = nullptr;
        }
        glfwTerminate();
        {
            // Reset boot-screen state so a later Initialize() starts clean.
            std::lock_guard<std::mutex> lock(Boot().mutex);
            Boot().status = BootStatusState{};
            Boot().status.log.clear();
            g_boot_active.store(true, std::memory_order_release);
        }
        if (g_xinput_dll) {
            FreeLibrary(g_xinput_dll);
            g_xinput_dll = nullptr;
            g_XInputGetState = nullptr;
            g_XInputSetState = nullptr;
        }
        g_dib_buffer.clear();
    }

    // Converts the guest framebuffer to full-resolution BGRA8 in g_vk_pixels
    // (the layout swapchain blits expect).  Returns false on a guest-memory
    // fault.  Pitch is assumed tightly packed, as in the GDI path.
    static bool ConvertFramebufferToBgra(guest_addr_t fb) {
        g_vk_pixels.resize(static_cast<size_t>(g_fb_width) * g_fb_height);
        __try {
            const u8* src = reinterpret_cast<const u8*>(fb);
            for (u32 y = 0; y < g_fb_height; ++y) {
                const u8* row = src + static_cast<size_t>(y) * g_fb_width *
                                          (g_fb_format == 2 ? 2 : 4);
                u32* dst = &g_vk_pixels[static_cast<size_t>(y) * g_fb_width];
                if (g_fb_format == 1) { // BGRA8: already the target layout
                    std::memcpy(dst, row, static_cast<size_t>(g_fb_width) * 4);
                } else if (g_fb_format == 2) { // RGB565
                    for (u32 x = 0; x < g_fb_width; ++x) {
                        const u16 p = reinterpret_cast<const u16*>(row)[x];
                        const u8 r = ((p >> 11) & 0x1F) * 255 / 31;
                        const u8 g = ((p >> 5) & 0x3F) * 255 / 63;
                        const u8 b = (p & 0x1F) * 255 / 31;
                        dst[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
                    }
                } else { // RGBA8 (default): swizzle to BGRA
                    for (u32 x = 0; x < g_fb_width; ++x) {
                        const u8* p = row + static_cast<size_t>(x) * 4;
                        dst[x] = 0xFF000000u | (p[0] << 16) | (p[1] << 8) | p[2];
                    }
                }
            }
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // SEH-safe helper: blits the guest framebuffer into the host DIB buffer
    // with aspect-correct scaling.  Extracted from RenderFrame so MSVC's
    // C2712 (__try + C++ object unwinding) does not fire.
    static bool BlitGuestFramebufferToDib(guest_addr_t framebuffer_addr) {
        int fit_x = 0, fit_y = 0, fit_w = g_width, fit_h = g_height;
        FitRect(static_cast<int>(g_fb_width), static_cast<int>(g_fb_height),
                g_width, g_height, &fit_x, &fit_y, &fit_w, &fit_h);
        __try {
            const u8* src = reinterpret_cast<const u8*>(framebuffer_addr);
            for (int y = 0; y < fit_h; ++y) {
                int src_y = (y * static_cast<int>(g_fb_height)) / fit_h;
                for (int x = 0; x < fit_w; ++x) {
                    int src_x = (x * static_cast<int>(g_fb_width)) / fit_w;

                    u8 r = 0, g_ch = 0, b = 0;
                    if (g_fb_format == 2) { // RGB565
                        int src_offset = (src_y * static_cast<int>(g_fb_width) + src_x) * 2;
                        u16 pixel = *reinterpret_cast<const u16*>(src + src_offset);
                        r = ((pixel >> 11) & 0x1F) * 255 / 31;
                        g_ch = ((pixel >> 5) & 0x3F) * 255 / 63;
                        b = (pixel & 0x1F) * 255 / 31;
                    } else if (g_fb_format == 1) { // BGRA8
                        int src_offset = (src_y * static_cast<int>(g_fb_width) + src_x) * 4;
                        const u8* pixel = src + src_offset;
                        b = pixel[0];
                        g_ch = pixel[1];
                        r = pixel[2];
                    } else { // RGBA8 (default)
                        int src_offset = (src_y * static_cast<int>(g_fb_width) + src_x) * 4;
                        const u8* pixel = src + src_offset;
                        r = pixel[0];
                        g_ch = pixel[1];
                        b = pixel[2];
                    }

                    g_dib_buffer[(fit_y + y) * g_width + (fit_x + x)] = (0xFF << 24) | (r << 16) | (g_ch << 8) | b;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        return true;
    }

    void RenderFrame(guest_addr_t framebuffer_addr) {
        if (!g_window || !g_hwnd) return;

        if (framebuffer_addr == 0) {
            // No framebuffer yet - redraw the boot screen (only while the
            // boot screen still owns the window).
            if (g_boot_active.load(std::memory_order_acquire)) {
                RenderBootScreenNow();
                LOG_DEBUG(GPU, "RenderFrame: No guest framebuffer, re-presenting boot screen.");
            }
            return;
        }

        // First real guest frame: the boot screen hands the window over to
        // the game; SetBootStatus becomes a no-op from here on.
        if (g_boot_active.exchange(false, std::memory_order_acq_rel)) {
            LOG_INFO(GPU, "First guest frame presented - boot screen complete.");
        }

        // Preferred path: when the vk_draw image model already has a GPU
        // image for the flipped guest address (M3.2d), blit straight from it
        // — the render target is the source of truth.  Otherwise upload the
        // CPU-side BGRA conversion and present that (swapchain blit scaling).
        // Falls through to the GDI path when Vulkan is unavailable or a
        // single present fails.
        if (g_vk_ready) {
            VkImage rt_image = VK_NULL_HANDLE;
            u32 rt_w = 0, rt_h = 0;
            VkFormat rt_format = VK_FORMAT_UNDEFINED;
            if (VkDrawLookupRenderTarget(framebuffer_addr, &rt_image, &rt_w, &rt_h, &rt_format)) {
                if (VkPresentFromImage(g_vk, rt_image, rt_format, rt_w, rt_h)) {
                    LOG_DEBUG(GPU, "RenderFrame: Vulkan present of GPU image for guest buffer 0x%llx.",
                              framebuffer_addr);
                    return;
                }
                LOG_WARN(GPU, "RenderFrame: GPU-image present failed — falling back to CPU upload.");
            }
            if (ConvertFramebufferToBgra(framebuffer_addr)) {
                if (VkPresentFrame(g_vk, g_vk_pixels.data(), g_fb_width, g_fb_height)) {
                    LOG_DEBUG(GPU, "RenderFrame: Vulkan present of guest framebuffer 0x%llx.",
                              framebuffer_addr);
                    return;
                }
                LOG_WARN(GPU, "RenderFrame: Vulkan present failed — falling back to GDI this frame.");
            } else {
                LOG_WARN(GPU, "RenderFrame: fault reading guest framebuffer 0x%llx for Vulkan upload.",
                         framebuffer_addr);
            }
        }

        std::fill(g_dib_buffer.begin(), g_dib_buffer.end(), 0xFF000000u);
        if (!BlitGuestFramebufferToDib(framebuffer_addr)) {
            LOG_WARN(GPU, "RenderFrame: Access violation reading guest framebuffer 0x%llx, falling back to boot screen.", framebuffer_addr);
            RenderBootScreenNow();
            return;
        }

        PresentDIB();
        LOG_DEBUG(GPU, "RenderFrame: Presented guest framebuffer 0x%llx.", framebuffer_addr);
    }

    void SetFramebufferConfig(u32 width, u32 height, u32 format) {
        g_fb_width = width > 0 ? width : 1920;
        g_fb_height = height > 0 ? height : 1080;
        g_fb_format = format;
        LOG_INFO(GPU, "Framebuffer config updated: %ux%u format=%u", g_fb_width, g_fb_height, g_fb_format);
    }

    bool ShouldCloseWindow() {
        if (g_window) {
            return glfwWindowShouldClose(g_window) != 0;
        }
        return true;
    }

    bool HasWindow() {
        return g_window != nullptr;
    }

    void PumpWindowEvents() {
        // GLFW event processing is only valid on the thread that created the
        // window (the process main thread); the guest worker thread never
        // calls this — it goes through GetCurrentPadState() instead.
        if (g_window) {
            glfwPollEvents();
            // While the boot screen owns the window there is no frame
            // presenter, so repaint it here: WM_PAINT/WM_SIZE (e.g. after the
            // launcher reparents or resizes the window for embedding)
            // otherwise leave the client area blank until the next stage
            // change happens to redraw it.
            if (IsBootScreenActive()) {
                RenderBootScreenNow();
            }
        }
    }

    void PollEvents() {
        // NOTE: no glfwPollEvents() here — GLFW event processing lives in
        // PumpWindowEvents(), driven by the main thread's window loop.  This
        // function only refreshes the pad state (keyboard + XInput).

        // Initialize XInput if not already done
        static bool xinput_inited = false;
        if (!xinput_inited) {
            InitializeXInput();
            xinput_inited = true;
        }

        PadButtonState new_state = { 0, 127, 127, 127, 127, 0, 0, {0, 0} };

        // Base state from keyboard
        {
            std::lock_guard<std::mutex> lock(g_pad_mutex);
            new_state.buttons = g_keyboard_buttons;
            // Map keyboard WASD to left stick
            if (g_window) {
                if (glfwGetKey(g_window, GLFW_KEY_W) == GLFW_PRESS) new_state.left_analog_y = 0;
                else if (glfwGetKey(g_window, GLFW_KEY_S) == GLFW_PRESS) new_state.left_analog_y = 255;
                if (glfwGetKey(g_window, GLFW_KEY_A) == GLFW_PRESS) new_state.left_analog_x = 0;
                else if (glfwGetKey(g_window, GLFW_KEY_D) == GLFW_PRESS) new_state.left_analog_x = 255;
                // Map keyboard IJKL to right stick
                if (glfwGetKey(g_window, GLFW_KEY_I) == GLFW_PRESS) new_state.right_analog_y = 0;
                else if (glfwGetKey(g_window, GLFW_KEY_K) == GLFW_PRESS) new_state.right_analog_y = 255;
                if (glfwGetKey(g_window, GLFW_KEY_J) == GLFW_PRESS) new_state.right_analog_x = 0;
                else if (glfwGetKey(g_window, GLFW_KEY_L) == GLFW_PRESS) new_state.right_analog_x = 255;
                // Digital R/F keys drive full analog trigger levels
                if (glfwGetKey(g_window, GLFW_KEY_R) == GLFW_PRESS) new_state.l2_trigger = 255;
                if (glfwGetKey(g_window, GLFW_KEY_F) == GLFW_PRESS) new_state.r2_trigger = 255;
            }
        }

        if (g_XInputGetState) {
            XINPUT_STATE xstate;
            // Poll primary controller (dwUserIndex = 0)
            if (g_XInputGetState(0, &xstate) == ERROR_SUCCESS) {
                u32 buttons = 0;
                WORD wButtons = xstate.Gamepad.wButtons;

                // Map XInput buttons to PlayStation HLE button bitmask (SCE_PAD values)
                if (wButtons & XINPUT_GAMEPAD_A) buttons |= 0x00004000; // Cross
                if (wButtons & XINPUT_GAMEPAD_B) buttons |= 0x00002000; // Circle
                if (wButtons & XINPUT_GAMEPAD_X) buttons |= 0x00008000; // Square
                if (wButtons & XINPUT_GAMEPAD_Y) buttons |= 0x00001000; // Triangle

                if (wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) buttons |= 0x00000400; // L1
                if (wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) buttons |= 0x00000800; // R1

                if (wButtons & XINPUT_GAMEPAD_DPAD_UP) buttons |= 0x00000010; // Up
                if (wButtons & XINPUT_GAMEPAD_DPAD_DOWN) buttons |= 0x00000040; // Down
                if (wButtons & XINPUT_GAMEPAD_DPAD_LEFT) buttons |= 0x00000080; // Left
                if (wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) buttons |= 0x00000020; // Right

                if (wButtons & XINPUT_GAMEPAD_START) buttons |= 0x00000008; // Options
                if (wButtons & XINPUT_GAMEPAD_BACK) buttons |= 0x00100000;  // Share / TouchPad
                if (wButtons & XINPUT_GAMEPAD_LEFT_THUMB) buttons |= 0x00000002; // L3
                if (wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) buttons |= 0x00000004; // R3

                // Guide button (PS Button) support
                if (wButtons & 0x0400) buttons |= 0x00010000;

                new_state.buttons |= buttons;

                // Analog stick translation (XInput is -32768..32767; ScePad is 0..255)
                auto map_stick = [](SHORT val) -> u8 {
                    return static_cast<u8>(((val + 32768) * 255) / 65535);
                };
                new_state.left_analog_x = map_stick(xstate.Gamepad.sThumbLX);
                new_state.left_analog_y = map_stick(-xstate.Gamepad.sThumbLY); // invert Y for standard direction
                new_state.right_analog_x = map_stick(xstate.Gamepad.sThumbRX);
                new_state.right_analog_y = map_stick(-xstate.Gamepad.sThumbRY);

                new_state.l2_trigger = xstate.Gamepad.bLeftTrigger;
                new_state.r2_trigger = xstate.Gamepad.bRightTrigger;

                if (new_state.l2_trigger > 50) new_state.buttons |= 0x00000100; // L2
                if (new_state.r2_trigger > 50) new_state.buttons |= 0x00000200; // R2
            }
        }

        // DualSense native HID feed (M4).  When a DualSense is attached it
        // supersedes XInput as the primary pad source: buttons are OR'd in
        // (same SCE_PAD bitmask), deflected sticks win over the current
        // values, triggers take the max, and the touch/motion fields — which
        // XInput cannot provide — are filled from the HID reports.
        DualSense::EnsureStarted();
        DualSense::Sample ds;
        if (DualSense::GetSample(ds)) {
            auto merge_axis = [](u8 ds_axis, u8 current) -> u8 {
                const int deflection = static_cast<int>(ds_axis) - 128;
                return (deflection > 12 || deflection < -12) ? ds_axis : current;
            };
            new_state.buttons |= ds.buttons;
            new_state.left_analog_x  = merge_axis(ds.lx, new_state.left_analog_x);
            new_state.left_analog_y  = merge_axis(ds.ly, new_state.left_analog_y);
            new_state.right_analog_x = merge_axis(ds.rx, new_state.right_analog_x);
            new_state.right_analog_y = merge_axis(ds.ry, new_state.right_analog_y);
            new_state.l2_trigger = ds.l2 > new_state.l2_trigger ? ds.l2 : new_state.l2_trigger;
            new_state.r2_trigger = ds.r2 > new_state.r2_trigger ? ds.r2 : new_state.r2_trigger;
            new_state.dualsense_connected = 1;
            new_state.touch_count = ds.touch_count;
            new_state.touch[0] = ds.touch[0];
            new_state.touch[1] = ds.touch[1];
            for (int i = 0; i < 3; ++i) {
                new_state.accel[i] = ds.accel[i];
                new_state.gyro[i] = ds.gyro[i];
            }
        }

        std::lock_guard<std::mutex> lock(g_pad_mutex);
        g_pad_state = new_state;
    }

    // Spin the GLFW event loop until the window is closed.
    // Used to keep the final frame visible after a guest exit or crash.
    void SetVrrConfig(bool vsync, bool vrr) {
        VkPresentSetVrrConfig(vsync, vrr);
    }

    void RunIdleLoop() {
        LOG_INFO(GPU, "Emulator halted - keeping window open until closed by user.");
        while (g_window && !glfwWindowShouldClose(g_window)) {
            glfwPollEvents();
            PresentDIB(); // keep window responsive and repaint on expose events
            Sleep(16);    // ~60 fps idle refresh
        }
    }

    PadButtonState GetCurrentPadState() {
        std::lock_guard<std::mutex> lock(g_pad_mutex);
        return g_pad_state;
    }

    void SetPadVibration(u8 large_motor, u8 small_motor) {
        // A live DualSense supersedes XInput for the primary pad (M4/Phase 6).
        DualSense::Sample ds;
        if (DualSense::GetSample(ds)) {
            DualSense::SetRumble(large_motor, small_motor);
            return;
        }
        if (!g_XInputSetState) return;
        XINPUT_VIBRATION vibration = {};
        // XInput motor speeds are 0..65535; the ScePadVibrationParam is 0..255.
        vibration.wLeftMotorSpeed  = static_cast<WORD>(large_motor) * 257;
        vibration.wRightMotorSpeed = static_cast<WORD>(small_motor) * 257;
        g_XInputSetState(0, &vibration);
    }

    void SetPadAdaptiveTrigger(bool left, u8 mode, const u8 params[10]) {
        DualSense::SetTriggerEffect(left, mode, params);
    }

} // namespace GPU
