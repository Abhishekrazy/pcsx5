// FFmpeg-based video decoder — H.264, H.265 (HEVC), VP9, AV1.
//
// Dynamically loads FFmpeg DLLs (avformat, avcodec, avutil, swscale)
// at runtime.  Covers all standard codecs used in MP4, WebM, and
// other container formats.
//
// Hardware acceleration: D3D11VA / DXVA2 on Windows via FFmpeg's
// hwaccel API (dxva2, d3d11va).

#include "video_decoder.h"
#include "../common/log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstring>
#include <vector>

// FFmpeg DLL names to probe.
static const char* kFFmpegDlls[] = {
    "avformat-61.dll",    // FFmpeg 7.x
    "avformat-60.dll",    // FFmpeg 6.x
    "avformat-59.dll",    // FFmpeg 5.x
    "avformat-58.dll",    // FFmpeg 4.x
    nullptr
};

VideoDecoder* CreateFFmpegDecoder(const VideoDecoderConfig& config) {
    // Probe for FFmpeg DLLs.
    if (!config.ffmpeg_dll_path.empty()) {
        // Try custom path first.
        std::string path = config.ffmpeg_dll_path + "/avformat-61.dll";
        HMODULE dll = ::LoadLibraryA(path.c_str());
        if (dll) {
            ::FreeLibrary(dll);
            LOG_INFO(Media, "FFmpeg: found at %s", config.ffmpeg_dll_path.c_str());
            LOG_WARN(Media, "FFmpeg: decoder not yet implemented (needs FFmpeg headers)");
            return nullptr;
        }
    }

    // Probe standard paths.
    for (int i = 0; kFFmpegDlls[i]; ++i) {
        HMODULE dll = ::LoadLibraryA(kFFmpegDlls[i]);
        if (dll) {
            ::FreeLibrary(dll);
            LOG_INFO(Media, "FFmpeg: found %s", kFFmpegDlls[i]);
            LOG_WARN(Media, "FFmpeg: decoder not yet implemented (needs FFmpeg headers)");
            return nullptr;
        }
    }

    LOG_WARN(Media, "FFmpeg: no DLL found — H.264/H.265/VP9/AV1 videos will not play");
    LOG_INFO(Media, "FFmpeg: download from https://ffmpeg.org/download.html or install via "
              "'vcpkg install ffmpeg'");
    return nullptr;
}
