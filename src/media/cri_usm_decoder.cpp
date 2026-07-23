// CRI Sofdec2 / USM video decoder — CRIWARE's movie format (.usm).
//
// The USM container wraps:
//   - Video: H.264 (AVC) or H.265 (HEVC)
//   - Audio: CRI ADX, HCA, or standard PCM
//
// Decoding strategy:
//   1. Parse USM container headers to find video/audio tracks
//   2. Extract the raw H.264/H.265 bitstream
//   3. Decode video via FFmpeg or D3D11VA hardware decoder
//   4. Decode audio via CRI ADX/HCA decoder or forward to audio device

#include "video_decoder.h"
#include "../common/log.h"

#include <cstdint>
#include <cstring>
#include <vector>

// USM container constants.
constexpr uint32_t kUsmMagic = 0x55534D00u;  // "USM\0"

#pragma pack(push, 1)
struct UsmHeader {
    uint32_t magic;          // 0x00534D55 ("USM\0" LE)
    uint32_t header_size;    // size of this header
    uint32_t data_size;      // total data size
    uint8_t  reserved[16];
};
static_assert(sizeof(UsmHeader) == 28, "UsmHeader size");

struct UsmTrack {
    uint32_t track_type;     // 1=video, 2=audio
    uint32_t codec;          // video: 1=H.264, 2=H.265; audio: 7=ADX, 10=HCA
    uint32_t width;          // video width
    uint32_t height;         // video height
    uint32_t sample_rate;    // audio sample rate
    uint32_t channels;       // audio channels
    uint64_t data_offset;    // offset to track data
    uint64_t data_size;      // size of track data
};
#pragma pack(pop)

VideoDecoder* CreateCriUsmDecoder(const VideoDecoderConfig& /*config*/) {
    // CRI USM decoding requires either FFmpeg or a hardware decoder.
    // The USM container parsing is straightforward, but H.264/H.265
    // decoding needs a decoder backend.
    LOG_WARN(Media, "CRI USM: decoder not yet implemented (needs FFmpeg or D3D11VA)");
    return nullptr;
}
