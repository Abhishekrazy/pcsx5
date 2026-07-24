#pragma once
// Bridge between IPC (ipc_server.cpp) and GPU (vulkan_backend.cpp).
// Uses C++17 inline variables so the pointers have definitions in every
// translation unit — test targets that include vulkan_backend.cpp but not
// ipc_server.cpp compile without linker errors.  When --ipc is not active
// the pointers stay null and the GPU skips the IPC write.

#include <cstdint>

namespace GPU {

inline void (*g_ipc_write_frame)(const void*, uint32_t, uint32_t, uint32_t) = nullptr;
inline bool (*g_ipc_is_connected)() = nullptr;

inline void IPC_SetWriteFrame(void (*fn)(const void*, uint32_t, uint32_t, uint32_t),
                               bool (*conn)()) {
    g_ipc_write_frame = fn;
    g_ipc_is_connected = conn;
}

} // namespace GPU
