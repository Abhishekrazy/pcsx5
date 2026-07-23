// Video Decoder Abstraction Layer (VDAL) — unified interface for
// decoding game cutscenes and UI videos.
//
// Supported formats:
//   - Bink 2 (.bik2) — RAD Game Tools, used in many AAA titles
//   - CRI Sofdec2 (.usm) — CRIWARE, dominant in Japanese games
//   - H.264 / H.265 — via FFmpeg or hardware decoder (DXVA/D3D11VA)
//   - VP9 / AV1 — via FFmpeg
//
// Each format is handled by a VideoDecoder subclass.  The factory
// auto-detects format by file header magic bytes.

#pragma once
#include "../common/types.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Pixel format for decoded video frames
// ---------------------------------------------------------------------------
enum class VideoPixelFormat : int {
    Unknown = 0,
    RGBA8   = 1,    // 32-bit RGBA (byte order R,G,B,A)
    BGRA8   = 2,    // 32-bit BGRA (byte order B,G,R,A)
    NV12    = 3,    // YUV 4:2:0 semi-planar (Y plane + UV interleaved)
    I420    = 4,    // YUV 4:2:0 planar (Y + U + V planes)
    P010    = 5,    // 10-bit YUV 4:2:0 (DXVA hardware decoder output)
};

// ---------------------------------------------------------------------------
// Audio format for decoded audio tracks
// ---------------------------------------------------------------------------
enum class AudioTrackFormat : int {
    Unknown   = 0,
    PCM16     = 1,    // Signed 16-bit PCM
    Float32   = 2,    // IEEE 754 float32
    ADPCM     = 3,    // CRI ADX / Bink ADPCM (needs conversion)
    ATRAC9    = 4,    // PS5 native ATRAC9
};

// ---------------------------------------------------------------------------
// Video metadata
// ---------------------------------------------------------------------------
struct VideoInfo {
    uint32_t        width           = 0;
    uint32_t        height          = 0;
    double          frame_rate      = 0.0;     // fps (e.g. 23.976, 29.97, 60.0)
    uint64_t        frame_count     = 0;       // total frames
    double          duration_sec    = 0.0;     // total duration
    VideoPixelFormat pixel_format   = VideoPixelFormat::Unknown;
    bool            has_alpha       = false;   // transparency channel
    std::string     codec_name;                // "h264", "hevc", "bink2", etc.
    std::string     container;                 // "usm", "bik2", "mp4", "webm"
};

// ---------------------------------------------------------------------------
// Audio track description
// ---------------------------------------------------------------------------
struct AudioTrackInfo {
    int             index           = 0;
    AudioTrackFormat format          = AudioTrackFormat::Unknown;
    uint32_t        sample_rate     = 0;
    int             channels        = 0;
    std::string     language;                   // "jpn", "eng", etc.
};

// ---------------------------------------------------------------------------
// Decoded video frame
// ---------------------------------------------------------------------------
struct VideoFrame {
    // Pixel data — format determined by VideoInfo::pixel_format.
    // For RGBA8/BGRA8: single buffer of width*height*4 bytes, row-major.
    // For NV12: Y plane (width*height) followed by UV plane (width*height/2).
    // For I420: Y plane + U plane + V plane.
    std::vector<uint8_t> data[3];   // up to 3 planes
    uint32_t             strides[3] = {};  // bytes per row per plane
    uint32_t             width      = 0;
    uint32_t             height     = 0;
    VideoPixelFormat     format     = VideoPixelFormat::Unknown;

    // Timing
    double               timestamp_sec = 0.0;  // presentation time
    double               duration_sec  = 0.0;  // frame duration

    // Side data
    bool                 is_keyframe   = false;
};

// ---------------------------------------------------------------------------
// Decoder status
// ---------------------------------------------------------------------------
enum class DecoderStatus : int {
    Ok          = 0,
    Eof         = 1,     // end of video (no more frames)
    Error       = 2,     // unrecoverable error
    FormatError = 3,     // unsupported format/codec
    NoMemory    = 4,     // out of memory
    NotReady    = 5,     // decoder not initialized yet
    Aborted     = 6,     // decoding was aborted by client
};

// ---------------------------------------------------------------------------
// Decoder configuration
// ---------------------------------------------------------------------------
struct VideoDecoderConfig {
    // Target output format (decoder will convert if needed).
    VideoPixelFormat output_format = VideoPixelFormat::RGBA8;

    // Scale factor (1.0 = original size, 0.5 = half, 2.0 = double).
    float scale_factor = 1.0f;

    // Enable hardware acceleration (DXVA, D3D11VA, Vulkan video).
    bool enable_hardware = true;

    // Audio output callback — called for each decoded audio sample.
    // If null, audio is discarded.
    std::function<void(const AudioTrackInfo& track,
                        const uint8_t* data,
                        uint32_t byte_count)> audio_callback;

    // Progress callback — called periodically during decode.
    // Returns false to abort decoding.
    std::function<bool(double progress)> progress_callback;

    // FFmpeg-specific: path to FFmpeg DLLs (avcodec, avformat, swscale).
    // Empty = search standard system paths.
    std::string ffmpeg_dll_path;

    // Bink-specific: path to Bink DLL (bink2w64.dll).
    // Empty = search standard system paths.
    std::string bink_dll_path;
};

// ===========================================================================
// VideoDecoder interface
// ===========================================================================
class VideoDecoder {
public:
    virtual ~VideoDecoder() = default;

    // ---- lifecycle -------------------------------------------------------
    // Open a video from a file path, memory buffer, or guest memory address.
    virtual DecoderStatus Open(const std::string& path) = 0;
    virtual DecoderStatus Open(const uint8_t* data, size_t size) = 0;
    virtual void Close() = 0;
    virtual bool IsOpen() const = 0;

    // ---- configuration ---------------------------------------------------
    virtual void SetConfig(const VideoDecoderConfig& config) = 0;
    virtual VideoDecoderConfig GetConfig() const = 0;

    // ---- metadata --------------------------------------------------------
    virtual VideoInfo GetVideoInfo() const = 0;
    virtual int GetAudioTrackCount() const = 0;
    virtual AudioTrackInfo GetAudioTrackInfo(int index) const = 0;

    // ---- decoding --------------------------------------------------------
    // Decode the next frame.  Returns Ok with a valid frame, or Eof when
    // the video ends, or Error on failure.
    virtual DecoderStatus DecodeNextFrame(VideoFrame& out_frame) = 0;

    // Seek to the nearest keyframe at or before `timestamp_sec`.
    virtual DecoderStatus Seek(double timestamp_sec) = 0;

    // Get current playback position in seconds.
    virtual double GetCurrentTime() const = 0;

    // ---- audio routing ---------------------------------------------------
    // If the decoder handles audio internally (e.g. Bink), this callback
    // is invoked with decoded audio PCM data during DecodeNextFrame().
    // Set via SetConfig().  Returns the audio track index to decode, or
    // -1 to decode all tracks.
    virtual void SetActiveAudioTrack(int track_index) = 0;

    // ---- GPU upload helpers ----------------------------------------------
    // Returns the decoded frame data in a format suitable for direct
    // Vulkan/D3D texture upload.
    virtual bool GetFrameForGpuUpload(VideoFrame& frame,
                                       uint8_t* out_rgba,
                                       uint32_t rgba_capacity) = 0;

    // ---- factory ---------------------------------------------------------
    enum class Format {
        AutoDetect = 0,
        Bink2      = 1,
        CriUsm     = 2,
        Mp4        = 3,
        WebM       = 4,
    };

    // Create a decoder for the given format.  Returns nullptr if the
    // format is not supported (no DLL found).
    static VideoDecoder* Create(Format format,
                                 const VideoDecoderConfig& config = {});

    // Auto-detect format from file extension or magic bytes.
    static Format DetectFormat(const std::string& path);
    static Format DetectFormat(const uint8_t* data, size_t size);

    // Human-readable name for a format.
    static const char* FormatName(Format fmt);
};

// ===========================================================================
// Null decoder (no-op for when video is not needed)
// ===========================================================================
class NullVideoDecoder : public VideoDecoder {
public:
    DecoderStatus Open(const std::string&) override { return DecoderStatus::FormatError; }
    DecoderStatus Open(const uint8_t*, size_t) override { return DecoderStatus::FormatError; }
    void Close() override {}
    bool IsOpen() const override { return false; }
    void SetConfig(const VideoDecoderConfig&) override {}
    VideoDecoderConfig GetConfig() const override { return {}; }
    VideoInfo GetVideoInfo() const override { return {}; }
    int GetAudioTrackCount() const override { return 0; }
    AudioTrackInfo GetAudioTrackInfo(int) const override { return {}; }
    DecoderStatus DecodeNextFrame(VideoFrame&) override { return DecoderStatus::Eof; }
    DecoderStatus Seek(double) override { return DecoderStatus::Eof; }
    double GetCurrentTime() const override { return 0.0; }
    void SetActiveAudioTrack(int) override {}
    bool GetFrameForGpuUpload(VideoFrame&, uint8_t*, uint32_t) override { return false; }
};
