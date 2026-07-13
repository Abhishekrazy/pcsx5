#include "system/system.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

#ifdef _WIN32
// Windows: pull in DXGI for GPU enumeration.  dxgi.lib is part of the
// Windows SDK and is already on the linker's default search path on
// modern MSVC toolchains.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <intrin.h>
#include <dxgi.h>
#pragma comment(lib, "dxgi.lib")

// RtlGetVersion is the supported way to read the real OS version on
// Windows 10+ (GetVersionEx lies about the build number).
typedef LONG (WINAPI* RtlGetVersionPtr)(LPOSVERSIONINFOEXW);
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#else
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif

namespace Sys {

// ------------------------------------------------------------------------
// Helpers
// ------------------------------------------------------------------------

static std::string Trim(const std::string& s) {
    auto issp = [](unsigned char c) { return std::isspace(c); };
    size_t a = 0, b = s.size();
    while (a < b && issp(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && issp(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

static std::string ReadAll(const std::string& path) {
    std::ifstream f(path);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
#if defined(_WIN32) || defined(__APPLE__)
[[maybe_unused]] static auto kReadAllUnused = &ReadAll;
#endif

std::string FormatBytes(std::uint64_t bytes) {
    if (bytes == 0) return "0 B";
    char buf[32];
    constexpr double KB = 1024.0;
    constexpr double MB = KB * 1024.0;
    constexpr double GB = MB * 1024.0;
    constexpr double TB = GB * 1024.0;
    if (bytes >= static_cast<std::uint64_t>(TB))
        std::snprintf(buf, sizeof(buf), "%.2f TB",
                      static_cast<double>(bytes) / TB);
    else if (bytes >= static_cast<std::uint64_t>(GB))
        std::snprintf(buf, sizeof(buf), "%.1f GB",
                      static_cast<double>(bytes) / GB);
    else if (bytes >= static_cast<std::uint64_t>(MB))
        std::snprintf(buf, sizeof(buf), "%.0f MB",
                      static_cast<double>(bytes) / MB);
    else if (bytes >= static_cast<std::uint64_t>(KB))
        std::snprintf(buf, sizeof(buf), "%.0f KB",
                      static_cast<double>(bytes) / KB);
    else
        std::snprintf(buf, sizeof(buf), "%llu B",
                      static_cast<unsigned long long>(bytes));
    return std::string(buf);
}

// ------------------------------------------------------------------------
// CPU
// ------------------------------------------------------------------------

static CpuInfo QueryCpu() {
    CpuInfo c;
#if defined(_WIN32)
    // CPU brand via cpuid leaves 0x80000002..0x80000004.  Each leaf
    // returns 4 ASCII chunks of the 48-byte brand string.
    int regs[4] = {0, 0, 0, 0};
    char brand[49] = {0};
    int cpuInfo[4] = {0, 0, 0, 0};
    __cpuid(cpuInfo, 0x80000000);
    if (static_cast<unsigned>(cpuInfo[0]) >= 0x80000004) {
        __cpuid(regs, 0x80000002);
        std::memcpy(brand,       regs, 16);
        __cpuid(regs, 0x80000003);
        std::memcpy(brand + 16, regs, 16);
        __cpuid(regs, 0x80000004);
        std::memcpy(brand + 32, regs, 16);
        brand[48] = '\0';
        c.brand = Trim(brand);
    }
    c.physical_cores = static_cast<int>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    c.logical_cores  = static_cast<int>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    if (si.dwNumberOfProcessors > 0)
        c.logical_cores = static_cast<int>(si.dwNumberOfProcessors);
    // Best-effort base frequency via the registry (HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\0\~MHz).
    HKEY hk = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                      0, KEY_READ, &hk) == ERROR_SUCCESS) {
        DWORD mhz = 0; DWORD cb = sizeof(mhz); DWORD type = 0;
        if (RegQueryValueExA(hk, "~MHz", nullptr, &type,
                             reinterpret_cast<LPBYTE>(&mhz), &cb) == ERROR_SUCCESS
            && mhz > 0) {
            c.base_ghz = static_cast<double>(mhz) / 1000.0;
        }
        RegCloseKey(hk);
    }
#elif defined(__APPLE__)
    char brand[256] = {0};
    size_t sz = sizeof(brand);
    if (sysctlbyname("machdep.cpu.brand_string", brand, &sz, nullptr, 0) == 0) {
        c.brand = Trim(brand);
    }
    int nproc = 0;
    sz = sizeof(nproc);
    if (sysctlbyname("hw.ncpu", &nproc, &sz, nullptr, 0) == 0) {
        c.logical_cores = nproc;
    }
    int phys = 0;
    sz = sizeof(phys);
    if (sysctlbyname("hw.physicalcpu", &phys, &sz, nullptr, 0) == 0) {
        c.physical_cores = phys;
    }
#else
    std::string ci = ReadAll("/proc/cpuinfo");
    auto pos = ci.find("model name");
    if (pos != std::string::npos) {
        auto eol = ci.find('\n', pos);
        std::string line = ci.substr(pos, eol - pos);
        auto colon = line.find(':');
        if (colon != std::string::npos)
            c.brand = Trim(line.substr(colon + 1));
    }
    c.logical_cores  = static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));
    c.physical_cores = c.logical_cores;  // sysconf can't distinguish
    // Base frequency from cpu MHz.
    auto mhz_pos = ci.find("cpu MHz");
    if (mhz_pos != std::string::npos) {
        auto eol = ci.find('\n', mhz_pos);
        std::string line = ci.substr(mhz_pos, eol - mhz_pos);
        auto colon = line.find(':');
        if (colon != std::string::npos) {
            c.base_ghz = std::atof(line.substr(colon + 1).c_str()) / 1000.0;
        }
    }
#endif
    if (c.brand.empty()) c.brand = "Unknown CPU";
    if (c.logical_cores <= 0) {
        unsigned hc = std::thread::hardware_concurrency();
        c.logical_cores  = hc ? static_cast<int>(hc) : 1;
        c.physical_cores = c.logical_cores;
    }
    return c;
}

// ------------------------------------------------------------------------
// GPU
// ------------------------------------------------------------------------

static GpuInfo QueryGpu() {
    GpuInfo g;
#if defined(_WIN32)
    // Use DXGI to enumerate adapters.  This picks up the primary
    // display adapter; multi-GPU systems may want to surface all of
    // them but for a "GPU" label a single strongest entry is fine.
    IDXGIFactory* factory = nullptr;
    if (CreateDXGIFactory(__uuidof(IDXGIFactory),
                          reinterpret_cast<void**>(&factory)) != S_OK
        || factory == nullptr) {
        g.name = "Unknown GPU";
        return g;
    }
    IDXGIAdapter* adapter = nullptr;
    DXGI_ADAPTER_DESC desc{};
    // Pick the adapter with the largest dedicated video memory.
    SIZE_T best_vram = 0;
    for (UINT i = 0;
         factory->EnumAdapters(i, &adapter) == S_OK && adapter != nullptr;
         ++i) {
        DXGI_ADAPTER_DESC d{};
        if (adapter->GetDesc(&d) == S_OK) {
            if (d.DedicatedVideoMemory > best_vram) {
                best_vram = d.DedicatedVideoMemory;
                desc = d;
            }
        }
        adapter->Release();
    }
    factory->Release();
    if (best_vram > 0) {
        char namebuf[128] = {0};
        WideCharToMultiByte(CP_ACP, 0, desc.Description, -1,
                            namebuf, sizeof(namebuf) - 1, nullptr, nullptr);
        g.name = Trim(namebuf);
        g.vram_bytes   = desc.DedicatedVideoMemory;
        g.shared_bytes = desc.SharedSystemMemory;
    } else {
        g.name = "Unknown GPU";
    }
#elif defined(__APPLE__)
    // Best-effort: Apple Silicon has no discrete GPU to report.  Intel
    // macs sometimes expose a real GPU via SPDisplaysDataType but that
    // requires IOKit; fall back to sysctl hw.model for now.
    g.name = "Integrated / Apple Silicon";
#else
    // Linux: try lspci for a vendor/device string.  We don't shell out
    // by default because not every system has lspci; instead read the
    // sysfs id and look up the most common vendors by hex id.
    g.name = "Unknown GPU";
    for (const auto& card : {"/sys/class/drm/card0/device",
                              "/sys/class/drm/card1/device"}) {
        std::ifstream f(std::string(card) + "/vendor");
        std::string vendor;
        if (f) std::getline(f, vendor);
        std::ifstream f2(std::string(card) + "/device");
        std::string device;
        if (f2) std::getline(f2, device);
        Trim(vendor); Trim(device);
        if (vendor.empty() || device.empty()) continue;
        auto v = "0x" + Trim(vendor).substr(2);
        auto d = "0x" + Trim(device).substr(2);
        // Very small lookup table — covers the common vendors users
        // will see on a developer machine.
        std::string vendor_name;
        if      (vendor == "0x10de") vendor_name = "NVIDIA";
        else if (vendor == "0x1002") vendor_name = "AMD";
        else if (vendor == "0x8086") vendor_name = "Intel";
        g.name = vendor_name + " " + v + ":" + d;
        break;
    }
    // Size: not reliably available without libdrm; leave 0.
#endif
    return g;
}

// ------------------------------------------------------------------------
// OS
// ------------------------------------------------------------------------

static OsInfo QueryOs() {
    OsInfo o;
#if defined(_WIN32)
    OSVERSIONINFOEXW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    HMODULE mod = GetModuleHandleA("ntdll.dll");
    if (mod) {
        auto fn = reinterpret_cast<RtlGetVersionPtr>(
            GetProcAddress(mod, "RtlGetVersion"));
        if (fn && fn(&vi) == 0) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%lu.%lu.%lu",
                          vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
            o.kernel = buf;
        }
    }
    // Friendly OS name from registry.  GetVersionEx reports
    // "Windows 10" for everything since 20H1, so we read the
    // ProductName string.
    HKEY hk = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                      0, KEY_READ, &hk) == ERROR_SUCCESS) {
        char prod[64] = {0}; DWORD cb = sizeof(prod); DWORD t = 0;
        if (RegQueryValueExA(hk, "ProductName", nullptr, &t,
                             reinterpret_cast<LPBYTE>(prod), &cb) == ERROR_SUCCESS) {
            o.name = prod;
        }
        char disp[64] = {0}; cb = sizeof(disp);
        if (RegQueryValueExA(hk, "DisplayVersion", nullptr, &t,
                             reinterpret_cast<LPBYTE>(disp), &cb) == ERROR_SUCCESS) {
            o.version = disp;
        }
        if (o.version.empty()) {
            char rel[64] = {0}; cb = sizeof(rel);
            if (RegQueryValueExA(hk, "ReleaseId", nullptr, &t,
                                 reinterpret_cast<LPBYTE>(rel), &cb) == ERROR_SUCCESS) {
                o.version = rel;
            }
        }
        RegCloseKey(hk);
    }
    if (o.name.empty()) o.name = "Windows";
    o.arch = sizeof(void*) == 8 ? "x86_64" : "x86";
#elif defined(__APPLE__)
    char kern[64] = {0};
    size_t sz = sizeof(kern);
    if (sysctlbyname("kern.osrelease", kern, &sz, nullptr, 0) == 0) {
        o.kernel = kern;
    }
    char ver[64] = {0};
    sz = sizeof(ver);
    if (sysctlbyname("kern.osproductversion", ver, &sz, nullptr, 0) == 0) {
        o.version = ver;
    }
    char brand[64] = {0};
    sz = sizeof(brand);
    if (sysctlbyname("hw.machine", brand, &sz, nullptr, 0) == 0) {
        o.name = std::string("macOS ") + ver + " (" + brand + ")";
    } else {
        o.name = "macOS";
    }
    o.arch = sizeof(void*) == 8 ? "x86_64" : "arm64";
#else
    struct utsname u;
    if (uname(&u) == 0) {
        o.kernel = u.release;
        o.arch   = u.machine;
        // Read /etc/os-release for the friendly name.
        std::string osrel = ReadAll("/etc/os-release");
        auto pos = osrel.find("PRETTY_NAME=");
        if (pos != std::string::npos) {
            auto eol = osrel.find('\n', pos);
            std::string line = osrel.substr(pos, eol - pos);
            auto eq = line.find('=');
            if (eq != std::string::npos) {
                std::string v = line.substr(eq + 1);
                if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
                    v = v.substr(1, v.size() - 2);
                o.name = v;
            }
        }
        if (o.name.empty()) o.name = u.sysname;
    }
#endif
    return o;
}

// ------------------------------------------------------------------------
// Memory
// ------------------------------------------------------------------------

MemorySample Memory() {
    MemorySample m;
#if defined(_WIN32)
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        m.total_bytes = ms.ullTotalPhys;
        m.avail_bytes = ms.ullAvailPhys;
        m.used_bytes  = m.total_bytes - m.avail_bytes;
        if (m.total_bytes > 0)
            m.used_fraction =
                static_cast<double>(m.used_bytes) /
                static_cast<double>(m.total_bytes);
    }
#elif defined(__APPLE__)
    mach_port_t host = mach_host_self();
    vm_size_t pagesize = 0;
    host_page_size(host, &pagesize);
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    host_vm_info64_data_t vm;
    if (host_statistics64(host, HOST_VM_INFO64,
                          reinterpret_cast<host_info64_t>(&vm), &count) == KERN_SUCCESS) {
        uint64_t total = vm.max_mem;
        uint64_t used  = static_cast<uint64_t>(vm.active_count) +
                         static_cast<uint64_t>(vm.inactive_count) +
                         static_cast<uint64_t>(vm.wire_count);
        m.total_bytes = total * pagesize;
        m.used_bytes  = used  * pagesize;
        m.avail_bytes = m.total_bytes > m.used_bytes
                            ? m.total_bytes - m.used_bytes : 0;
        if (m.total_bytes > 0)
            m.used_fraction =
                static_cast<double>(m.used_bytes) /
                static_cast<double>(m.total_bytes);
    }
#else
    struct sysinfo si{};
    if (sysinfo(&si) == 0) {
        m.total_bytes = static_cast<std::uint64_t>(si.totalram) * si.mem_unit;
        m.avail_bytes = static_cast<std::uint64_t>(si.freeram)  * si.mem_unit;
        m.used_bytes  = m.total_bytes - m.avail_bytes;
        if (m.total_bytes > 0)
            m.used_fraction =
                static_cast<double>(m.used_bytes) /
                static_cast<double>(m.total_bytes);
    }
#endif
    return m;
}

// ------------------------------------------------------------------------
// Snapshot
// ------------------------------------------------------------------------

SystemInfo Snapshot() {
    SystemInfo s;
    s.cpu = QueryCpu();
    s.gpu = QueryGpu();
    s.os  = QueryOs();
    return s;
}

}  // namespace Sys
