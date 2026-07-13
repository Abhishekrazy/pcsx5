#include "hle.h"
#include "../gpu/gpu.h"
#include "../common/log.h"
#include "../memory/memory.h"
#include <windows.h>
#include <array>

namespace HLE {

    // Registered guest framebuffer addresses, indexed by bufferIndex.
    // Populated by sceVideoOutRegisterBuffers, consumed by sceVideoOutFlip.
    static std::array<guest_addr_t, 8> g_video_buffers = {};
    static u32 g_video_buffer_count = 0;

    void RegisterLibVideoOut() {
        LOG_INFO(HLE, "Registering libSceVideoOut HLE symbols...");

        // sceVideoOutOpen
        RegisterSymbol("libSceVideoOut", "sceVideoOutOpen", [](const GuestArgs& args) -> u64 {
            u32 userId = static_cast<u32>(args.arg1);
            u32 type   = static_cast<u32>(args.arg2);
            u32 index  = static_cast<u32>(args.arg3);
            guest_addr_t opt = args.arg4;
            (void)opt;

            LOG_INFO(HLE, "sceVideoOutOpen(userId: %u, type: %u, index: %u) called", userId, type, index);

            // Return a fixed mock video output handle
            return 0x4000u;
        });

        // sceVideoOutClose
        RegisterSymbol("libSceVideoOut", "sceVideoOutClose", [](const GuestArgs& args) -> u64 {
            u32 handle = static_cast<u32>(args.arg1);
            LOG_INFO(HLE, "sceVideoOutClose(handle: 0x%X) called", handle);
            return 0; // Success
        });

        // sceVideoOutRegisterBuffers
        // Signature: sceVideoOutRegisterBuffers(handle, startIndex, addresses*, count, attr*)
        RegisterSymbol("libSceVideoOut", "sceVideoOutRegisterBuffers", [](const GuestArgs& args) -> u64 {
            u32          handle     = static_cast<u32>(args.arg1);
            u32          startIndex = static_cast<u32>(args.arg2);
            guest_addr_t addresses  = args.arg3;
            u32          count      = static_cast<u32>(args.arg4);
            guest_addr_t attr       = args.arg5;
            (void)attr;

            LOG_INFO(HLE, "sceVideoOutRegisterBuffers(handle: 0x%X, start: %u, count: %u) called",
                     handle, startIndex, count);

            // Store the guest framebuffer addresses indexed by (startIndex + i)
            for (u32 i = 0; i < count; ++i) {
                u32 slot = startIndex + i;
                if (slot >= static_cast<u32>(g_video_buffers.size())) {
                    LOG_WARN(HLE, "  Buffer slot %u out of range, skipping.", slot);
                    continue;
                }

                guest_addr_t addr = 0;
                if (addresses) {
                    addr = Memory::Read<guest_addr_t>(addresses + static_cast<u64>(i) * sizeof(guest_addr_t));
                }
                g_video_buffers[slot] = addr;
                g_video_buffer_count  = slot + 1;
                LOG_DEBUG(HLE, "  Buffer #%u Address: 0x%llx", slot, addr);
            }

            return 0; // Success
        });

        // sceVideoOutFlip
        // Signature: sceVideoOutFlip(handle, bufferIndex, mode, flipArg)
        RegisterSymbol("libSceVideoOut", "sceVideoOutFlip", [](const GuestArgs& args) -> u64 {
            u32  handle      = static_cast<u32>(args.arg1);
            s32  bufferIndex = static_cast<s32>(args.arg2);
            u32  mode        = static_cast<u32>(args.arg3);
            s64  flipArg     = static_cast<s64>(args.arg4);
            (void)mode;
            (void)flipArg;

            LOG_DEBUG(HLE, "sceVideoOutFlip(handle: 0x%X, bufferIndex: %d) called", handle, bufferIndex);

            // Resolve the guest framebuffer address for this flip
            guest_addr_t fb_addr = 0;
            if (bufferIndex >= 0 && static_cast<u32>(bufferIndex) < g_video_buffer_count) {
                fb_addr = g_video_buffers[static_cast<u32>(bufferIndex)];
            }

            // Blit guest framebuffer (or boot screen if address is unknown) to the window
            GPU::RenderFrame(fb_addr);
            GPU::PollEvents();

            // Intercept window close events to cleanly shut down execution
            if (GPU::ShouldCloseWindow()) {
                LOG_INFO(HLE, "GLFW Window close requested. Terminating emulator.");
                ExitProcess(0);
            }

            return 0; // Success
        });
    }
} // namespace HLE
