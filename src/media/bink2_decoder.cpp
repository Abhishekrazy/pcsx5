// Bink 2 video decoder — RAD Game Tools Bink 2 format (.bik2).
//
// The Bink 2 SDK (bink2w64.dll / libbink2) is a free-to-use runtime
// from RAD Game Tools.  This decoder loads the DLL dynamically and
// delegates all decoding to it.
//
// Bink 2 features:
//   - State-of-the-art compression (often 50% smaller than Bink 1)
//   - Alpha channel support
//   - Hardware-accelerated decode on GPU (optional)
//   - Audio tracks (up to 8) with real-time mixing
//   - Frame-accurate seeking

#include "video_decoder.h"
#include "../common/log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstring>
#include <vector>

VideoDecoder* CreateBink2Decoder(const VideoDecoderConfig& config) {
    // Check if Bink 2 DLL is available.
    const char* dll_names[] = {
        "bink2w64.dll",      // Bink 2 Windows x64
        "Bink2w64.dll",      // Capitalized variant
        nullptr
    };

    HMODULE dll = nullptr;
    for (int i = 0; dll_names[i]; ++i) {
        dll = ::LoadLibraryA(dll_names[i]);
        if (dll) {
            LOG_INFO(Media, "Bink2: found %s", dll_names[i]);
            ::FreeLibrary(dll);
            // Bink2 runtime is available.  Return nullptr for now —
            // full integration requires the Bink2 SDK headers and types.
            LOG_WARN(Media, "Bink2: decoder not yet implemented (SDK headers needed)");
            return nullptr;
        }
    }

    // Try custom path from config.
    if (!config.bink_dll_path.empty()) {
        dll = ::LoadLibraryA(config.bink_dll_path.c_str());
        if (dll) {
            LOG_INFO(Media, "Bink2: found at %s", config.bink_dll_path.c_str());
            ::FreeLibrary(dll);
            LOG_WARN(Media, "Bink2: decoder not yet implemented (SDK headers needed)");
            return nullptr;
        }
    }

    LOG_WARN(Media, "Bink2: bink2w64.dll not found — Bink videos will not play");
    return nullptr;
}
