// Cross-platform system information (Kyty-style "About my PC" panel).
//
// Exposes a small POD bundle (CPU / GPU / RAM / OS) plus a live
// `MemorySample()` for current memory usage.  The values that don't
// change at runtime (CPU brand, GPU name, OS version) are populated
// once and cached; memory usage is sampled cheaply on demand.
//
// Platform backends:
//   * Windows  -> __cpuid for CPU brand, DXGI for GPU name + VRAM,
//                 GlobalMemoryStatusEx for RAM, RtlGetVersion for OS.
//   * Linux    -> /proc/cpuinfo + /proc/meminfo + uname() + lspci.
//   * macOS    -> sysctl for OS, sysctlbyname for CPU brand, host_info
//                 for memory.  GPU brand is "Apple Silicon" on ARM macs
//                 and best-effort on Intel.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Sys {

struct MemorySample {
    std::uint64_t total_bytes     = 0;   // physical RAM installed
    std::uint64_t avail_bytes     = 0;   // currently free
    std::uint64_t used_bytes      = 0;   // total - avail
    double        used_fraction   = 0.0; // 0..1
};

struct CpuInfo {
    std::string brand;          // e.g. "Intel(R) Core(TM) i7-12700K"
    int         physical_cores  = 0;
    int         logical_cores   = 0;
    double      base_ghz        = 0.0;  // best-effort
};

struct GpuInfo {
    std::string name;           // e.g. "NVIDIA GeForce RTX 4070"
    std::string driver_version; // vendor driver string, may be ""
    std::uint64_t vram_bytes    = 0;   // dedicated VRAM (0 if unknown)
    std::uint64_t shared_bytes  = 0;   // shared system memory (0 if unknown)
};

struct OsInfo {
    std::string name;           // e.g. "Windows 10 Pro"
    std::string version;        // e.g. "22H2 (build 19045)"
    std::string kernel;         // e.g. "10.0.19045"
    std::string arch;           // "x86_64" / "arm64" / etc.
};

struct SystemInfo {
    CpuInfo cpu;
    GpuInfo gpu;
    OsInfo  os;
    std::vector<std::string> warnings;  // any backend failures
};

// One-shot snapshot of static facts (CPU, GPU, OS).  Re-querying is
// cheap but the result is stable for the lifetime of the process; the
// UI calls this once at startup and again on demand via "Refresh".
SystemInfo Snapshot();

// Current physical RAM usage.  Sampled on every call (cheap: a single
// syscall on every platform we support).
MemorySample Memory();

// Format a byte count as a short human-readable string ("12.0 GB").
std::string FormatBytes(std::uint64_t bytes);

}  // namespace Sys
