// Video decoder factory + format detection.

#include "video_decoder.h"
#include "../common/log.h"

#include <algorithm>
#include <cstring>
#include <array>

// Forward declarations for format-specific decoders.
VideoDecoder* CreateBink2Decoder(const VideoDecoderConfig& config);
VideoDecoder* CreateCriUsmDecoder(const VideoDecoderConfig& config);
VideoDecoder* CreateFFmpegDecoder(const VideoDecoderConfig& config);

// ===========================================================================
// VideoDecoder::Create
// ===========================================================================
VideoDecoder* VideoDecoder::Create(Format format,
                                    const VideoDecoderConfig& config) {
    switch (format) {
        case Format::Bink2: {
            auto* d = CreateBink2Decoder(config);
            if (d) return d;
            LOG_WARN(Media, "Bink2 decoder not available");
            return new NullVideoDecoder();
        }
        case Format::CriUsm: {
            auto* d = CreateCriUsmDecoder(config);
            if (d) return d;
            LOG_WARN(Media, "CRI USM decoder not available");
            return new NullVideoDecoder();
        }
        case Format::Mp4:
        case Format::WebM: {
            auto* d = CreateFFmpegDecoder(config);
            if (d) return d;
            LOG_WARN(Media, "FFmpeg decoder not available");
            return new NullVideoDecoder();
        }
        case Format::AutoDetect: {
            // Try FFmpeg first (covers H.264/H.265/VP9/AV1), then Bink2,
            // then CRI USM.
            auto* d = CreateFFmpegDecoder(config);
            if (d) return d;
            d = CreateBink2Decoder(config);
            if (d) return d;
            d = CreateCriUsmDecoder(config);
            if (d) return d;
            LOG_WARN(Media, "No video decoder available");
            return new NullVideoDecoder();
        }
        default:
            return new NullVideoDecoder();
    }
}

// ===========================================================================
// Format detection
// ===========================================================================

// Magic bytes for known formats.
constexpr std::array<uint8_t, 4> kMagicBink2 = {{'K', 'B', 'I', '2'}};  // Bink 2
constexpr std::array<uint8_t, 4> kMagicBink1 = {{'B', 'I', 'K', 'f'}};  // Bink 1
constexpr std::array<uint8_t, 4> kMagicUsm    = {{0x55, 0x53, 0x4D, 0x00}}; // "USM\0" CRI
constexpr std::array<uint8_t, 4> kMagicFtyp   = {{0x00, 0x00, 0x00, 0x18}}; // ftyp box (MP4)
constexpr std::array<uint8_t, 4> kMagicWebM   = {{0x1A, 0x45, 0xDF, 0xA3}}; // WebM/Matroska

VideoDecoder::Format VideoDecoder::DetectFormat(const std::string& path) {
    // Try by extension first (fast path).
    std::string ext;
    auto dot = path.rfind('.');
    if (dot != std::string::npos) {
        ext = path.substr(dot);
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    }

    if (ext == ".bik2" || ext == ".bik") return Format::Bink2;
    if (ext == ".usm" || ext == ".cpk" || ext == ".adx") return Format::CriUsm;
    if (ext == ".mp4" || ext == ".m4v" || ext == ".h264" || ext == ".264")
        return Format::Mp4;
    if (ext == ".webm" || ext == ".ivf" || ext == ".vp9")
        return Format::WebM;

    // Fall back to magic bytes by reading the file header.
    // (In practice, the caller provides the data directly.)
    return Format::AutoDetect;
}

VideoDecoder::Format VideoDecoder::DetectFormat(const uint8_t* data,
                                                 size_t size) {
    if (!data || size < 4) return Format::AutoDetect;

    if (std::memcmp(data, kMagicBink2.data(), 4) == 0) return Format::Bink2;
    if (std::memcmp(data, kMagicBink1.data(), 4) == 0) return Format::Bink2;
    if (std::memcmp(data, kMagicUsm.data(), 4) == 0) return Format::CriUsm;
    if (std::memcmp(data, kMagicFtyp.data(), 4) == 0) return Format::Mp4;
    if (std::memcmp(data, kMagicWebM.data(), 4) == 0) return Format::WebM;

    return Format::AutoDetect;
}

const char* VideoDecoder::FormatName(Format fmt) {
    switch (fmt) {
        case Format::AutoDetect: return "Auto-detect";
        case Format::Bink2:      return "Bink 2";
        case Format::CriUsm:     return "CRI Sofdec2 (USM)";
        case Format::Mp4:        return "MP4 (H.264/H.265)";
        case Format::WebM:       return "WebM (VP9/AV1)";
        default:                 return "Unknown";
    }
}
