#include "hle.h"
#include "../common/log.h"
#include "../memory/memory.h"
#include <windows.h>

namespace HLE {

    void RegisterLibAgc() {
        LOG_INFO(HLE, "Registering libSceAgc / libSceAgcDriver HLE symbols...");

        // libSceAgc stubs
        RegisterSymbol("libSceAgc", "sceAgcInitialize", [](const GuestArgs& args) -> u64 {
            (void)args;
            LOG_INFO(HLE, "sceAgcInitialize() called");
            return 0; // Success
        });

        RegisterSymbol("libSceAgc", "sceAgcCreateContext", [](const GuestArgs& args) -> u64 {
            guest_addr_t ctx_out = args.arg1;
            LOG_INFO(HLE, "sceAgcCreateContext() called");
            if (ctx_out) {
                Memory::Write<u64>(ctx_out, 0x5000); // Mock context handle
            }
            return 0; // Success
        });

        RegisterSymbol("libSceAgc", "sceAgcRegisterContext", [](const GuestArgs& args) -> u64 {
            u64 ctx = args.arg1;
            LOG_INFO(HLE, "sceAgcRegisterContext(context: 0x%llx) called", ctx);
            return 0; // Success
        });

        RegisterSymbol("libSceAgc", "sceAgcSubmitCommandBuffer", [](const GuestArgs& args) -> u64 {
            u64 ctx = args.arg1;
            guest_addr_t cmd_buf = args.arg2;
            u32 size = static_cast<u32>(args.arg3);
            LOG_DEBUG(HLE, "sceAgcSubmitCommandBuffer(context: 0x%llx, cmd_buf: 0x%llx, size: %u) called", 
                      ctx, cmd_buf, size);
            return 0; // Success
        });

        // libSceAgcDriver stubs
        RegisterSymbol("libSceAgcDriver", "sceAgcDriverInitialize", [](const GuestArgs& args) -> u64 {
            (void)args;
            LOG_INFO(HLE, "sceAgcDriverInitialize() called");
            return 0; // Success
        });

        RegisterSymbol("libSceAgcDriver", "sceAgcDriverCreateDevice", [](const GuestArgs& args) -> u64 {
            guest_addr_t dev_out = args.arg1;
            LOG_INFO(HLE, "sceAgcDriverCreateDevice() called");
            if (dev_out) {
                Memory::Write<u64>(dev_out, 0x6000); // Mock device handle
            }
            return 0; // Success
        });

        RegisterSymbol("libSceAgcDriver", "sceAgcDriverMapMemory", [](const GuestArgs& args) -> u64 {
            u64 dev = args.arg1;
            guest_addr_t addr = args.arg2;
            u64 size = args.arg3;
            u32 type = static_cast<u32>(args.arg4);
            LOG_INFO(HLE, "sceAgcDriverMapMemory(device: 0x%llx, addr: 0x%llx, size: %llu, type: %u)", 
                     dev, addr, size, type);
            return 0; // Success
        });
    }
}
