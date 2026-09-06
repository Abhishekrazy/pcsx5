#pragma once
// Bridge between IPC (ipc_server.cpp, in the CLI process) and GPU
// (vulkan_backend.cpp, in pcsx5_core.dll).
//
// The hook pointers are C++17 inline variables so test targets that compile
// vulkan_backend.cpp without ipc_server.cpp still link. An inline variable
// exists once PER MODULE, though: the CLI executable and the core DLL each
// get their own copy. Until 2026-09-06 the setter below was inline as well,
// so main.cpp filled the EXECUTABLE's copies and RenderFrame, inside the DLL,
// read the DLL's copies -- null forever. The launcher never received a frame
// (TASKS 4.14). The setter is now a real function defined in
// vulkan_backend.cpp and exported from the DLL, so it writes the copies that
// RenderFrame reads. Only the DLL's copies matter; the executable's stay
// unused.

#include <cstdint>

namespace GPU {

inline void (*g_ipc_write_frame)(const void*, uint32_t, uint32_t, uint32_t) = nullptr;
inline bool (*g_ipc_is_connected)() = nullptr;

// Defined in vulkan_backend.cpp (exported from pcsx5_core.dll); declared with
// its export attribute in gpu.h.
void IPC_SetWriteFrame(void (*fn)(const void*, uint32_t, uint32_t, uint32_t),
                       bool (*conn)());

} // namespace GPU
