#pragma once

#include <cstdint>

#if defined(_WIN32)
#define PCSX5_API __declspec(dllexport)
#else
#define PCSX5_API __attribute__((visibility("default")))
#endif

extern "C" {

// Plugin metadata and initialization
struct Pcsx5PluginInfo {
    const char* name;
    const char* version;
    uint32_t api_version;
};

// Interface for the Memory subsystem plugin
struct IPcsx5Memory {
    virtual ~IPcsx5Memory() = default;
    virtual bool Initialize() = 0;
    virtual void Shutdown() = 0;
    
    // Core memory operations
    virtual void* Translate(uint64_t guest_addr) = 0;
    virtual uint64_t GetGuestAddress(void* host_ptr) = 0;
};

// Interface for the HLE subsystem plugin
struct IPcsx5Hle {
    virtual ~IPcsx5Hle() = default;
    virtual bool Initialize(IPcsx5Memory* memory) = 0;
    virtual void Shutdown() = 0;
    
    // NID Resolution and HLE registration
    virtual uint64_t ResolveNID(const char* module_name, uint64_t nid) = 0;
};

// Interface for the GPU subsystem plugin
struct IPcsx5Gpu {
    virtual ~IPcsx5Gpu() = default;
    virtual bool Initialize(IPcsx5Memory* memory) = 0;
    virtual void Shutdown() = 0;
    
    // Graphics operations
    virtual void Present() = 0;
};

// Exported factory function from each plugin
typedef bool (*Pcsx5PluginInitFunc)(Pcsx5PluginInfo* out_info);

} // extern "C"
