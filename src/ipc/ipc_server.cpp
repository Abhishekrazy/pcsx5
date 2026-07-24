// Core-side IPC server — connects to shared memory + named pipe created by
// the frontend, writes frames to shared memory, reads input from shared
// memory, and listens for commands on the pipe.
#include "ipc_shared.h"
#include "../common/log.h"

#include <windows.h>
#include <cstring>
#include <thread>
#include <atomic>

namespace IPC {

// ── State ─────────────────────────────────────────────────────────────────
static HANDLE        g_map_handle  = nullptr;
static HANDLE        g_pipe_handle = nullptr;
static IpcShared*    g_shared      = nullptr;
static std::thread   g_pipe_thread;
static std::atomic<bool> g_pipe_stop{false};

static void PipeReaderThread();

// ── Init / Shutdown ───────────────────────────────────────────────────────
bool Initialize(const char* map_name, const char* pipe_name) {
    // Open the file mapping created by the frontend.
    g_map_handle = ::OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, map_name);
    if (!g_map_handle) {
        LOG_ERROR(General, "IPC: OpenFileMapping(%s) failed (err=%lu)", map_name, ::GetLastError());
        return false;
    }

    g_shared = (IpcShared*)::MapViewOfFile(g_map_handle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(IpcShared));
    if (!g_shared) {
        LOG_ERROR(General, "IPC: MapViewOfFile failed (err=%lu)", ::GetLastError());
        ::CloseHandle(g_map_handle);
        g_map_handle = nullptr;
        return false;
    }

    // Verify the shared memory header.
    if (g_shared->magic != IPC_MAGIC || g_shared->version != IPC_VERSION) {
        LOG_WARN(General, "IPC: shared memory magic/version mismatch (got 0x%X/%u)",
                 g_shared->magic, g_shared->version);
    }

    // Connect to the named pipe (the frontend creates it first).
    g_pipe_handle = ::CreateFileA(pipe_name, GENERIC_READ | GENERIC_WRITE,
                                  0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (g_pipe_handle == INVALID_HANDLE_VALUE) {
        LOG_WARN(General, "IPC: CreateFile(%s) failed (err=%lu) — commands disabled",
                 pipe_name, ::GetLastError());
        g_pipe_handle = nullptr;
    } else {
        // Start the pipe reader thread.
        g_pipe_stop.store(false, std::memory_order_relaxed);
        g_pipe_thread = std::thread(PipeReaderThread);
    }

    LOG_INFO(General, "IPC: connected (map=%s pipe=%s)", map_name, pipe_name);
    return true;
}

void Shutdown() {
    g_pipe_stop.store(true, std::memory_order_relaxed);
    if (g_pipe_thread.joinable()) g_pipe_thread.join();

    if (g_pipe_handle) { ::CloseHandle(g_pipe_handle); g_pipe_handle = nullptr; }
    if (g_shared) { ::UnmapViewOfFile(g_shared); g_shared = nullptr; }
    if (g_map_handle) { ::CloseHandle(g_map_handle); g_map_handle = nullptr; }
}

bool IsConnected() { return g_shared != nullptr; }

// ── Frame output ──────────────────────────────────────────────────────────
void WriteFrame(const void* rgba, uint32_t width, uint32_t height,
                uint32_t pitch) {
    if (!g_shared) return;

    // Clamp dimensions to the buffer.
    uint32_t w = (width > IPC_MAX_W) ? IPC_MAX_W : width;
    uint32_t h = (height > IPC_MAX_H) ? IPC_MAX_H : height;
    uint32_t src_pitch = pitch ? pitch : w * 4;
    uint32_t copy_bytes = (src_pitch < w * 4) ? src_pitch : w * 4;

    g_shared->frame_width  = w;
    g_shared->frame_height = h;
    g_shared->frame_pitch  = w * 4;

    // Row-by-row copy (safe even when source pitch differs from dest).
    const uint8_t* src = (const uint8_t*)rgba;
    uint8_t* dst = g_shared->frame_data;
    for (uint32_t y = 0; y < h; ++y) {
        std::memcpy(dst + y * w * 4, src + y * src_pitch, copy_bytes);
    }

    // Publish the frame (release semantics).
    g_shared->frame_counter++;
    std::atomic_thread_fence(std::memory_order_release);
}

// ── Input read ────────────────────────────────────────────────────────────
uint64_t ReadInputButtons() {
    if (!g_shared) return 0;
    std::atomic_thread_fence(std::memory_order_acquire);
    return g_shared->input_buttons;
}

void ReadInputAnalogs(uint8_t& lx, uint8_t& ly, uint8_t& rx, uint8_t& ry,
                       uint8_t& l2, uint8_t& r2) {
    if (!g_shared) { lx=128; ly=128; rx=128; ry=128; l2=0; r2=0; return; }
    std::atomic_thread_fence(std::memory_order_acquire);
    lx = g_shared->input_lx; ly = g_shared->input_ly;
    rx = g_shared->input_rx; ry = g_shared->input_ry;
    l2 = g_shared->input_l2; r2 = g_shared->input_r2;
}

// ── Game state write ──────────────────────────────────────────────────────
void SetGameState(uint32_t state) {
    if (!g_shared) return;
    g_shared->game_state = state;
    std::atomic_thread_fence(std::memory_order_release);
}

void SetCrashed(uint32_t code, const char* msg) {
    if (!g_shared) return;
    g_shared->game_state = IPC_STATE_CRASHED;
    g_shared->crash_code = code;
    if (msg) {
        strncpy_s(g_shared->crash_message, sizeof(g_shared->crash_message), msg, _TRUNCATE);
    }
    std::atomic_thread_fence(std::memory_order_release);
}

// ── Pipe reader thread — processes commands from the frontend ─────────────
static void PipeReaderThread() {
    if (!g_pipe_handle) return;

    uint8_t buf[256];
    DWORD read_bytes = 0;

    while (!g_pipe_stop.load(std::memory_order_relaxed)) {
        BOOL ok = ::ReadFile(g_pipe_handle, buf, sizeof(buf), &read_bytes, nullptr);
        if (!ok) {
            DWORD err = ::GetLastError();
            if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED) {
                LOG_INFO(General, "IPC: pipe disconnected.");
                break; // frontend closed the pipe
            }
            // Non-fatal read error — try again.
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (read_bytes < sizeof(uint32_t)) continue;

        uint32_t cmd = *(uint32_t*)buf;
        switch (cmd) {
            case IPC_CMD_STOP:
                LOG_INFO(General, "IPC: received STOP command.");
                // The RequestStop flag is checked by HleDispatch.
                break;
            case IPC_CMD_KILL:
                LOG_INFO(General, "IPC: received KILL command.");
                // Immediate termination handled by the frontend
                // (it calls TerminateProcess on the core process).
                break;
            case IPC_CMD_PAUSE:
                LOG_INFO(General, "IPC: received PAUSE command.");
                break;
            case IPC_CMD_RESUME:
                LOG_INFO(General, "IPC: received RESUME command.");
                break;
            default:
                LOG_DEBUG(General, "IPC: unknown command 0x%X", cmd);
                break;
        }
    }
}

} // namespace IPC
