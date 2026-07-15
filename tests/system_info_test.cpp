// Quick test: dump the Sys::Snapshot() + Sys::Memory() report so we can
// verify the cross-platform system-info module picks up the right
// values for this machine.
#include "system/system.h"

#include <cstdio>
#include <thread>

int main() {
    auto s = Sys::Snapshot();
    std::printf("==== System Snapshot ====\n");
    std::printf("OS     : %s\n", s.os.name.c_str());
    std::printf("Ver    : %s\n", s.os.version.c_str());
    std::printf("Kernel : %s\n", s.os.kernel.c_str());
    std::printf("Arch   : %s\n", s.os.arch.c_str());
    std::printf("\n");
    std::printf("CPU    : %s\n", s.cpu.brand.c_str());
    std::printf("Cores  : %d physical, %d logical\n",
                s.cpu.physical_cores, s.cpu.logical_cores);
    std::printf("Base   : %.2f GHz\n", s.cpu.base_ghz);
    std::printf("\n");
    std::printf("GPU    : %s\n", s.gpu.name.c_str());
    std::printf("VRAM   : %s\n",
                Sys::FormatBytes(s.gpu.vram_bytes).c_str());
    std::printf("Shared : %s\n",
                Sys::FormatBytes(s.gpu.shared_bytes).c_str());
    std::printf("\n");
    for (int i = 0; i < 3; ++i) {
        auto m = Sys::Memory();
        std::printf("Memory : %s / %s (%.1f%% used)\n",
                    Sys::FormatBytes(m.used_bytes).c_str(),
                    Sys::FormatBytes(m.total_bytes).c_str(),
                    m.used_fraction * 100.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return 0;
}
