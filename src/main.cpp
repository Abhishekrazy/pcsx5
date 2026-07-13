#include "common/log.h"
#include "common/types.h"
#include "memory/memory.h"
#include "kernel/kernel.h"
#include "hle/hle.h"
#include "gpu/gpu.h"
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    // Disable stdout/stderr buffering for instant logging on crash
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    // Print emulator banner
    std::printf("\033[95m==================================================\n");
    std::printf("  pcsx5 - PlayStation 5 Emulator & Compatibility Layer\n");
    std::printf("==================================================\033[0m\n");

    if (argc < 2) {
        std::printf("Usage: pcsx5.exe <path_to_eboot.bin_or_elf>\n");
        return 1;
    }

    std::string target_path = argv[1];

    // 1. Initialize Subsystems
    if (!Memory::Initialize()) {
        std::printf("FATAL: Failed to initialize Memory subsystem.\n");
        return -1;
    }

    if (!HLE::Initialize()) {
        std::printf("FATAL: Failed to initialize HLE subsystem.\n");
        Memory::Shutdown();
        return -1;
    }

    if (!Kernel::Initialize()) {
        std::printf("FATAL: Failed to initialize Kernel subsystem.\n");
        HLE::Shutdown();
        Memory::Shutdown();
        return -1;
    }

    if (!GPU::Initialize()) {
        std::printf("FATAL: Failed to initialize GPU subsystem.\n");
        Kernel::Shutdown();
        HLE::Shutdown();
        Memory::Shutdown();
        return -1;
    }

    LOG_INFO(General, "All subsystems initialized successfully.");

    // 2. Load the main ELF/SELF module
    Loader::LoadedModule main_module;
    if (!Kernel::LoadModule(target_path, main_module)) {
        LOG_ERROR(General, "Failed to load target module: %s", target_path.c_str());
        GPU::Shutdown();
        Kernel::Shutdown();
        HLE::Shutdown();
        Memory::Shutdown();
        return -1;
    }

    // 3. Execute the guest application
    LOG_INFO(General, "Starting guest execution loop...");
    bool run_success = Kernel::Execute(main_module);

    if (run_success) {
        LOG_INFO(General, "Guest application completed execution successfully.");
    } else {
        LOG_ERROR(General, "Guest application execution crashed or failed.");
    }

    // 4. Shutdown Subsystems
    GPU::Shutdown();
    Kernel::Shutdown();
    HLE::Shutdown();
    Memory::Shutdown();

    LOG_INFO(General, "pcsx5 shutdown cleanly.");
    return run_success ? 0 : -1;
}
