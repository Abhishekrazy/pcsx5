#pragma once
// Shared memory IPC protocol between the emulator core (C++) and the
// frontend (C#).  The frontend creates the file mapping + named pipe,
// launches the core as a child process, and passes the handle names
// via --ipc-map=<name> --ipc-pipe=<name>.

#include <cstdint>

// ── Shared memory layout ───────────────────────────────────────────────────
static constexpr uint32_t IPC_MAGIC        = 0x50504353u;  // "PCSX"
static constexpr uint32_t IPC_VERSION      = 1u;
static constexpr uint32_t IPC_MAX_W        = 1920u;
static constexpr uint32_t IPC_MAX_H        = 1080u;

// Game state written by the core, read by the frontend.
enum IpcGameState : uint32_t {
    IPC_STATE_BOOTING = 0,
    IPC_STATE_RUNNING = 1,
    IPC_STATE_PAUSED  = 2,
    IPC_STATE_CRASHED = 3,
    IPC_STATE_EXITED  = 4,
};

struct IpcShared {
    // ── identity ──────────────────────────────────────────────────────────
    uint32_t magic;
    uint32_t version;

    // ── frame buffer (core → frontend) ────────────────────────────────────
    // frame_counter is incremented (release) after each frame write.
    // The frontend polls this; when it changes, it reads the frame.
    uint64_t        frame_counter;
    uint32_t        frame_width;
    uint32_t        frame_height;
    uint32_t        frame_pitch;   // bytes per row
    uint8_t         frame_data[IPC_MAX_W * IPC_MAX_H * 4];

    // ── input state (frontend → core) ─────────────────────────────────────
    uint64_t        input_buttons; // bitmask
    uint8_t         input_lx, input_ly;
    uint8_t         input_rx, input_ry;
    uint8_t         input_l2, input_r2;
    uint8_t         _pad_input[2];

    // ── game state (core → frontend) ──────────────────────────────────────
    uint32_t        game_state;    // IpcGameState
    uint32_t        crash_code;
    char            crash_message[256];
    uint8_t         _pad_end[64];
};
static_assert(sizeof(IpcShared) < 10 * 1024 * 1024, "IpcShared should be under 10 MB");

// ── Named pipe protocol (UI → Core commands) ──────────────────────────────
static constexpr const char* IPC_PIPE_NAME = "\\\\.\\pipe\\PCSX5_IPC";
static constexpr uint32_t    IPC_PIPE_TIMEOUT_MS = 5000;

enum IpcChannelCmd : uint32_t {
    IPC_CMD_STOP   = 0x01,
    IPC_CMD_KILL   = 0x02,
    IPC_CMD_PAUSE  = 0x03,
    IPC_CMD_RESUME = 0x04,
};

// Wire format: [cmd : u32] [payload...]
struct IpcPacket { uint32_t type; };

// Log message from core → frontend: [type=0x10:u32] [level:s32] [category:s32] [text:char*NUL]
static constexpr uint32_t IPC_MSG_LOG = 0x10;
