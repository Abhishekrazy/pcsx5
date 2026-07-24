#pragma once
// IPC server — core side.  Called from main.cpp when --ipc is used.

#include <cstdint>

namespace IPC {

// Connect to shared memory + named pipe created by the frontend.
// map_name / pipe_name are the Win32 object names passed via --ipc-map / --ipc-pipe.
bool Initialize(const char* map_name, const char* pipe_name);

void Shutdown();
bool IsConnected();

// After each rendered frame, call WriteFrame to publish it to the frontend.
void WriteFrame(const void* rgba, uint32_t width, uint32_t height, uint32_t pitch);

// Read input state written by the frontend.
uint64_t ReadInputButtons();
void ReadInputAnalogs(uint8_t& lx, uint8_t& ly, uint8_t& rx, uint8_t& ry,
                       uint8_t& l2, uint8_t& r2);

// Update game state flags (core → frontend).
void SetGameState(uint32_t state);
void SetCrashed(uint32_t code, const char* msg);

} // namespace IPC
