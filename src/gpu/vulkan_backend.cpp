#include "gpu.h"
#include "../common/log.h"
#include "../memory/memory.h"
#include <windows.h>
#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <vulkan/vulkan.h>
#include <mutex>
#include <vector>
#include <cstring>
#include <cstdlib>

namespace GPU {

    static GLFWwindow* g_window  = nullptr;
    static HWND        g_hwnd    = nullptr;
    static int         g_width   = 1280;
    static int         g_height  = 720;

    static PadButtonState g_pad_state = { 0, 127, 127, 127, 127, 0, 0, {0, 0} };
    static std::mutex     g_pad_mutex;

    // Host-side DIB backing store (BGRA, 32-bit, 1280x720)
    static std::vector<u32> g_dib_buffer;

    // Vulkan dynamic loading entry points (retain for future use)
    typedef VkResult(VKAPI_PTR* PFN_vkCreateInstance)(const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*);
    static PFN_vkCreateInstance pfn_vkCreateInstance = nullptr;
    static HMODULE g_vulkan_dll = nullptr;

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

    static void DrawBootScreen() {
        // Background: deep navy gradient (simulate with 3 bands)
        FillRect(0,   0, g_width, 240, 0xFF0A0A1A); // very dark navy
        FillRect(0, 240, g_width, 240, 0xFF0E0E28); // dark navy
        FillRect(0, 480, g_width, 240, 0xFF12122F); // slightly lighter

        // Scanline effect: thin horizontal lines every 4 pixels
        for (int y = 0; y < g_height; y += 4)
            DrawHLine(0, y, g_width, 0x0AFFFFFF);

        // ── Top accent bar ──────────────────────────────────────────────────
        FillRect(0, 0, g_width, 4, 0xFF00D4FF); // PS5 accent cyan

        // ── PCSX5 large logo text ────────────────────────────────────────────
        DrawText(160, 80, "PCSX5",  0xFF00D4FF, 8); // cyan, 8x scale (40x56px letters)
        DrawText(810, 96, "PS5 EMULATOR", 0xFFFFFFFF, 3);
        DrawText(810, 126, "COMPATIBILITY LAYER", 0xFF8888BB, 2);

        // Logo divider line
        FillRect(155, 148, 970, 3, 0xFF00D4FF);

        // ── Status panel box ────────────────────────────────────────────────
        FillRect(155, 165, 970, 270, 0xFF0F1020); // dark panel background
        FillRect(155, 165, 970,   2, 0xFF2244AA); // top border
        FillRect(155, 433, 970,   2, 0xFF2244AA); // bottom border
        FillRect(155, 165,   2, 270, 0xFF2244AA); // left border
        FillRect(1123,165,  2, 270, 0xFF2244AA); // right border

        // Status title
        DrawText(170, 176, "EMULATOR STATUS", 0xFF2299FF, 2);
        DrawHLine(170, 194, 350, 0xFF2244AA);

        // Status items  (green ticks / orange warnings)
        const struct { const char* label; const char* val; u32 col; } items[] = {
            { "MEMORY MANAGER",    "ONLINE",      0xFF00FF88 },
            { "HLE SUBSYSTEM",     "ONLINE",      0xFF00FF88 },
            { "ELF LOADER",        "ONLINE",      0xFF00FF88 },
            { "KERNEL VEH",        "ONLINE",      0xFF00FF88 },
            { "GLFW WINDOW",       "ONLINE",      0xFF00FF88 },
            { "VULKAN DRIVER",     "LOADED",      0xFF00D4FF },
            { "GUEST FRAMEBUFFER", "WAITING",     0xFFFFAA00 },
            { "GPU BACKEND",       "STUB MODE",   0xFFFFAA00 },
        };
        for (int i = 0; i < 8; ++i) {
            int row_y = 202 + i * 26;
            int col    = (i < 4) ? 170 : 640;
            int row_i  = i < 4 ? i : i - 4;
            row_y = 202 + row_i * 26;
            DrawText(col,       row_y, items[i].label, 0xFFCCCCCC, 2);
            DrawText(col + 330, row_y, items[i].val,   items[i].col, 2);
        }

        // ── Colour bar strip (reference) ─────────────────────────────────────
        const u32 bars[] = {
            0xFFFFFFFF, // white
            0xFF00FFFF, // cyan
            0xFFFF00FF, // magenta
            0xFF0000FF, // blue
            0xFFFFFF00, // yellow
            0xFF00FF00, // green
            0xFFFF0000, // red
            0xFF101010, // near-black
        };
        int bar_w = g_width / 8;
        for (int b = 0; b < 8; ++b)
            FillRect(b * bar_w, 450, bar_w, 80, bars[b]);

        // Text over the bars
        DrawText(g_width/2 - 180, 458, "WAITING FOR GUEST FRAME", 0xFF000000, 2);

        // ── Bottom info bar ──────────────────────────────────────────────────
        FillRect(0, g_height - 36, g_width, 36, 0xFF070713);
        FillRect(0, g_height - 36, g_width,  2, 0xFF00D4FF);
        DrawText(20, g_height - 26, "PCSX5 V0.1  |  PS5 HLE EMULATOR", 0xFF555577, 2);
        DrawText(g_width - 368, g_height - 26, "PRESS ESC TO EXIT", 0xFF555577, 2);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Present g_dib_buffer to the window via GDI StretchDIBits
    // ─────────────────────────────────────────────────────────────────────────

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

        StretchDIBits(hdc,
            0, 0, dst_w, dst_h,
            0, 0, g_width, g_height,
            g_dib_buffer.data(), &bmi,
            DIB_RGB_COLORS, SRCCOPY);

        ReleaseDC(g_hwnd, hdc);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Keyboard callback
    // ─────────────────────────────────────────────────────────────────────────

    static void KeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
        (void)window; (void)scancode; (void)mods;

        u32 button_flag = 0;
        switch (key) {
            case GLFW_KEY_W:    case GLFW_KEY_UP:    button_flag = 0x00000010; break; // PAD_UP
            case GLFW_KEY_S:    case GLFW_KEY_DOWN:  button_flag = 0x00000040; break; // PAD_DOWN
            case GLFW_KEY_A:    case GLFW_KEY_LEFT:  button_flag = 0x00000080; break; // PAD_LEFT
            case GLFW_KEY_D:    case GLFW_KEY_RIGHT: button_flag = 0x00000020; break; // PAD_RIGHT
            case GLFW_KEY_SPACE:                     button_flag = 0x00004000; break; // PAD_CROSS
            case GLFW_KEY_ENTER:                     button_flag = 0x00002000; break; // PAD_CIRCLE
            case GLFW_KEY_LEFT_SHIFT:                button_flag = 0x00008000; break; // PAD_SQUARE
            case GLFW_KEY_C:                         button_flag = 0x00001000; break; // PAD_TRIANGLE
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
                g_pad_state.buttons |= button_flag;
                LOG_DEBUG(GPU, "Key pressed, mapping button flag: 0x%X", button_flag);
            } else if (action == GLFW_RELEASE) {
                g_pad_state.buttons &= ~button_flag;
                LOG_DEBUG(GPU, "Key released, clearing button flag: 0x%X", button_flag);
            }
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Public API
    // ─────────────────────────────────────────────────────────────────────────

    bool Initialize() {
        LOG_INFO(GPU, "Initializing GPU subsystem (GLFW + Vulkan Backend)...");

        if (!glfwInit()) {
            LOG_ERROR(GPU, "Failed to initialize GLFW.");
            return false;
        }

        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE,  GLFW_FALSE);

        g_window = glfwCreateWindow(g_width, g_height, "pcsx5 - PlayStation 5 Emulator", nullptr, nullptr);
        if (!g_window) {
            LOG_ERROR(GPU, "Failed to create GLFW window.");
            glfwTerminate();
            return false;
        }

        g_hwnd = glfwGetWin32Window(g_window);
        glfwSetKeyCallback(g_window, KeyCallback);
        LOG_INFO(GPU, "GLFW Window created successfully (%dx%d).", g_width, g_height);

        // Allocate host DIB buffer (BGRA 32-bit)
        g_dib_buffer.assign(static_cast<size_t>(g_width) * g_height, 0xFF000000u);

        // Dynamically load Vulkan runtime driver DLL
        g_vulkan_dll = LoadLibraryA("vulkan-1.dll");
        if (g_vulkan_dll) {
            pfn_vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(
                GetProcAddress(g_vulkan_dll, "vkCreateInstance")
            );
            if (pfn_vkCreateInstance) {
                LOG_INFO(GPU, "Vulkan driver library 'vulkan-1.dll' loaded successfully.");
            } else {
                LOG_WARN(GPU, "Failed to resolve vkCreateInstance from 'vulkan-1.dll'.");
            }
        } else {
            LOG_WARN(GPU, "Vulkan driver 'vulkan-1.dll' not found. Graphics emulation will run in stub mode.");
        }

        // Draw and present the boot screen immediately ("first frame")
        DrawBootScreen();
        PresentDIB();
        glfwPollEvents();
        LOG_INFO(GPU, "Boot screen presented successfully (first frame displayed).");

        return true;
    }

    void Shutdown() {
        LOG_INFO(GPU, "Shutting down GPU subsystem...");
        if (g_window) {
            glfwDestroyWindow(g_window);
            g_window = nullptr;
            g_hwnd   = nullptr;
        }
        glfwTerminate();
        if (g_vulkan_dll) {
            FreeLibrary(g_vulkan_dll);
            g_vulkan_dll = nullptr;
        }
        g_dib_buffer.clear();
    }

    void RenderFrame(guest_addr_t framebuffer_addr) {
        if (!g_window || !g_hwnd) return;

        if (framebuffer_addr == 0) {
            // No framebuffer yet - redraw the boot screen
            DrawBootScreen();
            PresentDIB();
            LOG_DEBUG(GPU, "RenderFrame: No guest framebuffer, re-presenting boot screen.");
            return;
        }

        // Blit guest RGBA framebuffer → host DIB buffer (BGRA)
        // Guest FB is assumed 1920x1080 RGBA8 - scale-blit to 1280x720 window
        // For now, attempt to read the first 1280x720 rows directly (raw blit)
        __try {
            const u8* src = reinterpret_cast<const u8*>(framebuffer_addr);
            for (int y = 0; y < g_height; ++y) {
                for (int x = 0; x < g_width; ++x) {
                    // Read RGBA bytes from guest memory (guest may use RGBA or BGRA)
                    int src_offset = (y * g_width + x) * 4;
                    u8 r = src[src_offset + 0];
                    u8 g_ch = src[src_offset + 1];
                    u8 b = src[src_offset + 2];
                    u8 a = src[src_offset + 3];
                    (void)a;
                    // Pack as BGRA for GDI StretchDIBits
                    g_dib_buffer[y * g_width + x] = (0xFF << 24) | (r << 16) | (g_ch << 8) | b;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // Guest framebuffer address turned out invalid – fallback to boot screen
            LOG_WARN(GPU, "RenderFrame: Access violation reading guest framebuffer 0x%llx, falling back to boot screen.", framebuffer_addr);
            DrawBootScreen();
        }

        PresentDIB();
        LOG_DEBUG(GPU, "RenderFrame: Presented guest framebuffer 0x%llx.", framebuffer_addr);
    }

    bool ShouldCloseWindow() {
        if (g_window) {
            return glfwWindowShouldClose(g_window) != 0;
        }
        return true;
    }

    void PollEvents() {
        if (g_window) {
            glfwPollEvents();
        }
    }

    // Spin the GLFW event loop until the window is closed.
    // Used to keep the final frame visible after a guest exit or crash.
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

} // namespace GPU
