// Bink 2 video decoder — RAD Game Tools Bink 2 format (.bik2).
//
// Loads bink2w64.dll dynamically (no SDK headers needed — function
// signatures are declared inline).  Falls back gracefully when absent.

#include "video_decoder.h"
#include "../common/log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstring>
#include <vector>

// WIN32_LEAN_AND_MEAN above should exclude mmsystem.h, but some SDK versions
// still define GetCurrentTime as a macro (timeGetTime).  Undefine it so our
// GetCurrentTime override resolves to the method name, not the macro.
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

// ---------------------------------------------------------------------------
// Bink2 public API (declared inline)
// ---------------------------------------------------------------------------
enum Bink2Format : uint32_t {
    BINK2_FORMAT_BGRA8   = 0,
    BINK2_FORMAT_RGBA8   = 1,
    BINK2_FORMAT_YUV420  = 2,
    BINK2_FORMAT_YUVA420 = 3,
};

struct Bink2Info {
    uint32_t width, height;
    Bink2Format format;
    uint32_t maxWidth, maxHeight;
    uint32_t totalFrames;
    float    frameRate;
    uint32_t audioTrackCount;
    uint32_t flags;
};

struct Bink2FramePlane {
    void*    data;
    uint32_t rowBytes;
    uint32_t width, height;
};

struct Bink2FrameData {
    Bink2Format     format;
    uint32_t        width, height;
    Bink2FramePlane planes[4];
    uint64_t        timestampUs;
    uint32_t        frameIndex;
};

struct BINK2;

typedef BINK2*   (__cdecl* Bink2Open_t)(const char* path, uint32_t flags);
typedef uint32_t (__cdecl* Bink2GetInfo_t)(BINK2*, Bink2Info*);
typedef uint32_t (__cdecl* Bink2DecodeFrame_t)(BINK2*, uint32_t flags);
typedef uint32_t (__cdecl* Bink2GetFrameData_t)(BINK2*, Bink2FrameData**);
typedef void     (__cdecl* Bink2Close_t)(BINK2*);

// ---------------------------------------------------------------------------
// Bink2Decoder
// ---------------------------------------------------------------------------
class Bink2Decoder final : public VideoDecoder {
public:
    Bink2Decoder();
    ~Bink2Decoder() override;

    DecoderStatus Open(const std::string& path) override;
    DecoderStatus Open(const uint8_t* data, size_t size) override;
    void Close() override;
    bool IsOpen() const override;

    void SetConfig(const VideoDecoderConfig& config) override;
    VideoDecoderConfig GetConfig() const override;

    VideoInfo GetVideoInfo() const override;
    int GetAudioTrackCount() const override;
    AudioTrackInfo GetAudioTrackInfo(int index) const override;

    DecoderStatus DecodeNextFrame(VideoFrame& out_frame) override;
    DecoderStatus Seek(double timestamp_sec) override;
    double GetCurrentTime() const override;

    void SetActiveAudioTrack(int track_index) override;
    bool GetFrameForGpuUpload(VideoFrame& frame, uint8_t* out_rgba,
                              uint32_t rgba_capacity) override;

public:
    // ResolveDll is public so the factory can check availability without
    // creating an open decoder instance.
    bool ResolveDll();
private:

    HMODULE     m_dll = nullptr;
    BINK2*      m_bink = nullptr;
    Bink2Info   m_info{};
    double      m_currentTime = 0.0;
    uint64_t    m_frameIndex = 0;
    bool        m_eof = false;
    VideoDecoderConfig m_config;

    Bink2Open_t         m_openFn = nullptr;
    Bink2GetInfo_t      m_getInfoFn = nullptr;
    Bink2DecodeFrame_t  m_decodeFn = nullptr;
    Bink2GetFrameData_t m_getFrameFn = nullptr;
    Bink2Close_t        m_closeFn = nullptr;
};

Bink2Decoder::Bink2Decoder() {}

Bink2Decoder::~Bink2Decoder() { Close(); }

bool Bink2Decoder::ResolveDll() {
    if (m_dll) return true;
    const char* names[] = {"bink2w64.dll", "Bink2w64.dll", nullptr};
    for (int i = 0; names[i]; ++i) {
        m_dll = ::LoadLibraryA(names[i]);
        if (m_dll) break;
    }
    if (!m_dll && !m_config.bink_dll_path.empty())
        m_dll = ::LoadLibraryA(m_config.bink_dll_path.c_str());
    if (!m_dll) return false;

    m_openFn     = (Bink2Open_t)::GetProcAddress(m_dll, "Bink2Open");
    m_getInfoFn  = (Bink2GetInfo_t)::GetProcAddress(m_dll, "Bink2GetInfo");
    m_decodeFn   = (Bink2DecodeFrame_t)::GetProcAddress(m_dll, "Bink2DecodeFrame");
    m_getFrameFn = (Bink2GetFrameData_t)::GetProcAddress(m_dll, "Bink2GetFrameData");
    m_closeFn    = (Bink2Close_t)::GetProcAddress(m_dll, "Bink2Close");

    if (!m_openFn || !m_getInfoFn || !m_decodeFn || !m_getFrameFn || !m_closeFn) {
        ::FreeLibrary(m_dll); m_dll = nullptr;
        return false;
    }
    return true;
}

DecoderStatus Bink2Decoder::Open(const std::string& path) {
    Close();
    if (!ResolveDll()) return DecoderStatus::FormatError;

    m_bink = m_openFn(path.c_str(), 0);
    if (!m_bink) return DecoderStatus::FormatError;

    if (m_getInfoFn(m_bink, &m_info) != 0) {
        m_closeFn(m_bink); m_bink = nullptr;
        return DecoderStatus::FormatError;
    }

    LOG_INFO(Media, "Bink2: opened '%s' (%ux%u, %.2f fps, %u frames, %u audio)",
             path.c_str(), m_info.width, m_info.height, m_info.frameRate,
             m_info.totalFrames, m_info.audioTrackCount);
    return DecoderStatus::Ok;
}

DecoderStatus Bink2Decoder::Open(const uint8_t* /*data*/, size_t /*size*/) {
    return DecoderStatus::FormatError; // Bink2 requires file path
}

void Bink2Decoder::Close() {
    if (m_bink) { m_closeFn(m_bink); m_bink = nullptr; }
    if (m_dll) { ::FreeLibrary(m_dll); m_dll = nullptr; }
    m_frameIndex = 0;
    m_currentTime = 0.0;
    m_eof = false;
}

bool Bink2Decoder::IsOpen() const { return m_bink != nullptr; }

void Bink2Decoder::SetConfig(const VideoDecoderConfig& config) { m_config = config; }
VideoDecoderConfig Bink2Decoder::GetConfig() const { return m_config; }

VideoInfo Bink2Decoder::GetVideoInfo() const {
    VideoInfo vi{};
    vi.width        = m_info.width;
    vi.height       = m_info.height;
    vi.frame_rate   = m_info.frameRate > 0 ? m_info.frameRate : 30.0;
    vi.frame_count  = m_info.totalFrames;
    vi.pixel_format = (m_info.format == BINK2_FORMAT_YUV420 || m_info.format == BINK2_FORMAT_YUVA420)
                      ? VideoPixelFormat::I420 : VideoPixelFormat::RGBA8;
    vi.has_alpha    = (m_info.format == BINK2_FORMAT_YUVA420);
    vi.codec_name   = "bink2";
    vi.container    = "bik2";
    return vi;
}

int Bink2Decoder::GetAudioTrackCount() const {
    return static_cast<int>(m_info.audioTrackCount);
}

AudioTrackInfo Bink2Decoder::GetAudioTrackInfo(int /*index*/) const {
    return AudioTrackInfo{}; // Bink2 audio info not queried via this API
}

DecoderStatus Bink2Decoder::DecodeNextFrame(VideoFrame& out_frame) {
    if (!m_bink || m_eof) return DecoderStatus::Eof;

    if (m_decodeFn(m_bink, 0) != 0) {
        m_eof = true;
        return DecoderStatus::Eof;
    }

    Bink2FrameData* fd = nullptr;
    if (m_getFrameFn(m_bink, &fd) != 0 || !fd) {
        m_eof = true;
        return DecoderStatus::Error;
    }

    out_frame = VideoFrame{};
    out_frame.width        = fd->width;
    out_frame.height       = fd->height;
    out_frame.timestamp_sec = m_currentTime;
    out_frame.duration_sec = m_info.frameRate > 0 ? 1.0 / m_info.frameRate : 0.0;
    m_currentTime += out_frame.duration_sec;
    m_frameIndex++;

    if (fd->format == BINK2_FORMAT_BGRA8 || fd->format == BINK2_FORMAT_RGBA8) {
        out_frame.format = VideoPixelFormat::RGBA8;
        const uint32_t pitch = fd->planes[0].rowBytes;
        out_frame.data[0].resize(pitch * out_frame.height);
        std::memcpy(out_frame.data[0].data(), fd->planes[0].data, out_frame.data[0].size());
        out_frame.strides[0] = pitch;
    } else {
        out_frame.format = VideoPixelFormat::I420;
        for (int p = 0; p < 3; ++p) {
            const uint32_t pitch = fd->planes[p].rowBytes;
            const uint32_t h = (p == 0) ? out_frame.height : (out_frame.height + 1) / 2;
            out_frame.data[p].resize(pitch * h);
            std::memcpy(out_frame.data[p].data(), fd->planes[p].data, out_frame.data[p].size());
            out_frame.strides[p] = pitch;
        }
    }
    return DecoderStatus::Ok;
}

DecoderStatus Bink2Decoder::Seek(double /*timestamp_sec*/) {
    return DecoderStatus::Error; // unsupported
}

double Bink2Decoder::GetCurrentTime() const { return m_currentTime; }

void Bink2Decoder::SetActiveAudioTrack(int /*track_index*/) {}

bool Bink2Decoder::GetFrameForGpuUpload(VideoFrame& frame, uint8_t* out_rgba, uint32_t rgba_capacity) {
    if (frame.format != VideoPixelFormat::RGBA8) return false;
    uint32_t needed = frame.width * frame.height * 4;
    if (rgba_capacity < needed || frame.data[0].size() < needed) return false;
    std::memcpy(out_rgba, frame.data[0].data(), needed);
    return true;
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------
VideoDecoder* CreateBink2Decoder(const VideoDecoderConfig& config) {
    auto* dec = new Bink2Decoder();
    dec->SetConfig(config);
    if (!dec->ResolveDll()) {
        LOG_WARN(Media, "Bink2: bink2w64.dll not found");
        delete dec;
        return nullptr;
    }
    LOG_INFO(Media, "Bink2: bink2w64.dll loaded");
    return dec;
}
