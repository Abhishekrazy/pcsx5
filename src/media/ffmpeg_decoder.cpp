// FFmpeg-based video decoder — H.264, H.265 (HEVC), VP9, AV1.
//
// Dynamically loads FFmpeg DLLs (avformat, avcodec, avutil, swscale)
// at runtime via LoadLibrary/GetProcAddress — no FFmpeg headers or
// compile-time linking required.
//
// Struct layouts match FFmpeg n7.0 (avformat-61 / avcodec-61 / avutil-59)
// ABI.  These are the DLLs SharpEmu ships; the layouts are frozen for
// the lifetime of that major version.
//
// Supported containers: MP4, WebM, MKV, AVI, MOV.
// Supported codecs: H.264, H.265/HEVC, VP9, AV1, MPEG-4, VP8.

#include "video_decoder.h"
#include "../common/log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// GetCurrentTime is a Windows macro that conflicts with VideoDecoder::GetCurrentTime.
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

#include <cstring>
#include <vector>
#include <string>
#include <mutex>
#include <cstdio>

// =============================================================================
// FFmpeg struct layouts (n7.0 / avformat-61 ABI).
// Minimal — only the fields we access, in declaration order matching FFmpeg 7.x.
// =============================================================================
struct AVDictionary;
struct AVBufferRef;
struct AVPacketSideData;

struct AVRational { int num; int den; };

struct AVFrame {
    uint8_t* data[8];
    int linesize[8];
    uint8_t** extended_data;
    int width, height;
    int nb_samples;
    int format;
    int key_frame;
    int pict_type;
    int64_t pts;
    int64_t pkt_dts;
    int64_t pkt_pos;
    int64_t pkt_duration;
};

struct AVCodecParameters {
    int codec_type;
    int codec_id;
    uint32_t codec_tag;
    uint8_t* extradata;
    int extradata_size;
    int format;
    int64_t bit_rate;
    int bits_per_coded_sample;
    int bits_per_raw_sample;
    int profile;
    int level;
    int width;
    int height;
    AVRational sample_aspect_ratio;
    int field_order;
    int color_range;
    int color_primaries;
    int color_trc;
    int color_space;
    int chroma_location;
    int video_delay;
    uint64_t channel_layout;
    int channels;
    int sample_rate;
};

struct AVStream {
    int index;
    int id;
    void* priv_data;
    AVRational time_base;
    int64_t start_time;
    int64_t duration;
    int64_t nb_frames;
    int disposition;
    int _discard_pad;
    AVRational sample_aspect_ratio;
    AVDictionary* metadata;
    AVRational avg_frame_rate;
    AVPacketSideData* side_data;
    int nb_side_data;
    int event_flags;
    AVCodecParameters* codecpar;
};

struct AVInputFormat {
    const char* name;
    const char* long_name;
    int flags;
    const char* extensions;
};

struct AVFormatContext {
    void* av_class;
    AVInputFormat* iformat;
    void* oformat;
    void* priv_data;
    void* pb;
    unsigned int nb_streams;
    AVStream** streams;
    char filename[1024];
    int64_t duration;
    int64_t bit_rate;
    unsigned int packet_size;
    int max_delay;
};

struct AVCodecContext {
    void* av_class;
    int log_level_offset;
    int codec_type;
    const void* codec;
    const char* codec_name;
    AVCodecParameters* codecpar;
    int codec_id;
    int codec_tag;
    unsigned int stream_codec_tag;
    void* priv_data;
    int width, height;
    int coded_width, coded_height;
    int pix_fmt;
};

struct AVCodec {
    const char* name;
    const char* long_name;
    int type;
    int id;
    int capabilities;
};

struct AVPacket {
    AVBufferRef* buf;
    int64_t pts;
    int64_t dts;
    uint8_t* data;
    int size;
    int stream_index;
    int flags;
    AVPacketSideData* side_data;
    int side_data_elems;
    int64_t duration;
    int64_t pos;
};

struct SwsContext { int dummy; };

// FFmpeg pix_fmt / sample_fmt constants
static constexpr int AV_PIX_FMT_NONE    = -1;
static constexpr int AV_PIX_FMT_YUV420P = 0;
static constexpr int AV_PIX_FMT_NV12    = 23;
static constexpr int AV_PIX_FMT_RGBA    = 26;
static constexpr int AV_PIX_FMT_BGRA    = 28;
static constexpr int AV_PIX_FMT_RGB0    = 128;
static constexpr int AV_PIX_FMT_BGR0    = 129;
static constexpr int AV_PIX_FMT_YUVJ420P = 12;

static constexpr int AV_CODEC_ID_H264     = 27;
static constexpr int AV_CODEC_ID_HEVC     = 173;
static constexpr int AV_CODEC_ID_VP9      = 167;
static constexpr int AV_CODEC_ID_AV1      = 228;
static constexpr int AV_CODEC_ID_MPEG4    = 12;
static constexpr int AV_CODEC_ID_VP8      = 139;

static constexpr int AVMEDIA_TYPE_VIDEO = 0;
static constexpr int AVMEDIA_TYPE_AUDIO = 1;
static constexpr int AV_PICTURE_TYPE_I = 1;

static constexpr int AVSEEK_FLAG_BACKWARD = 1;

// ---------------------------------------------------------------------------
// Dynamic function table
// ---------------------------------------------------------------------------
struct FfmpegFuncs {
    HMODULE avformat = nullptr, avcodec = nullptr, avutil = nullptr, swscale = nullptr;

    int  (*avformat_open_input)(AVFormatContext** ps, const char* url, AVFormatContext* fmt, AVDictionary** opts);
    void (*avformat_close_input)(AVFormatContext** s);
    int  (*avformat_find_stream_info)(AVFormatContext* ic, AVDictionary** opts);
    int  (*av_read_frame)(AVFormatContext* s, AVPacket* pkt);
    int  (*av_seek_frame)(AVFormatContext* s, int si, int64_t ts, int flags);
    AVFormatContext* (*avformat_alloc_context)(void);
    void (*avformat_free_context)(AVFormatContext* s);

    const AVCodec* (*avcodec_find_decoder)(int id);
    AVCodecContext* (*avcodec_alloc_context3)(const AVCodec* codec);
    void (*avcodec_free_context)(AVCodecContext** avctx);
    int  (*avcodec_open2)(AVCodecContext* avctx, const AVCodec* codec, AVDictionary** opts);
    int  (*avcodec_send_packet)(AVCodecContext* avctx, const AVPacket* avpkt);
    int  (*avcodec_receive_frame)(AVCodecContext* avctx, AVFrame* frame);
    int  (*avcodec_parameters_to_context)(AVCodecContext* ctx, const AVCodecParameters* par);

    AVFrame* (*av_frame_alloc)(void);
    void (*av_frame_free)(AVFrame** frame);
    int  (*av_frame_get_buffer)(AVFrame* frame, int align);
    void (*av_frame_unref)(AVFrame* frame);
    void* (*av_malloc)(size_t size);
    void (*av_free)(void* ptr);
    int64_t (*av_rescale_q)(int64_t a, AVRational bq, AVRational cq);
    void (*av_dict_free)(AVDictionary** m);
    int  (*av_dict_set)(AVDictionary** pm, const char* k, const char* v, int flags);
    void (*av_log_set_level)(int level);

    SwsContext* (*sws_getContext)(int sw, int sh, int sf, int dw, int dh, int df, int fl, SwsContext* f, const int* f2, const double* p);
    void (*sws_freeContext)(SwsContext* s);
    int  (*sws_scale)(SwsContext* c, const uint8_t* const sl[], const int ss[], int si, int sh, uint8_t* const d[], const int ds[]);
};

static FfmpegFuncs g_ff{};
static std::once_flag g_ff_once;
static bool g_ff_loaded = false;

#define LOAD_FN(dll, name) \
    do { g_ff.name = (decltype(g_ff.name))GetProcAddress(dll, #name); if (!g_ff.name) return false; } while(0)

static bool LoadFfmpeg(const char* dir) {
    auto load_dll = [&](std::initializer_list<const char*> names) -> HMODULE {
        for (const char* n : names) {
            std::string p = dir ? (std::string(dir) + "/" + n) : n;
            HMODULE h = LoadLibraryA(p.c_str());
            if (h) return h;
        }
        return nullptr;
    };

    g_ff.avformat = load_dll({"avformat-61.dll", "avformat-60.dll"});
    if (!g_ff.avformat) return false;
    g_ff.avcodec  = load_dll({"avcodec-61.dll", "avcodec-60.dll"});
    if (!g_ff.avcodec) { FreeLibrary(g_ff.avformat); return false; }
    g_ff.avutil   = load_dll({"avutil-59.dll", "avutil-58.dll"});
    if (!g_ff.avutil) { FreeLibrary(g_ff.avcodec); FreeLibrary(g_ff.avformat); return false; }
    g_ff.swscale  = load_dll({"swscale-8.dll", "swscale-7.dll"});

    LOAD_FN(g_ff.avformat, avformat_open_input);
    LOAD_FN(g_ff.avformat, avformat_close_input);
    LOAD_FN(g_ff.avformat, avformat_find_stream_info);
    LOAD_FN(g_ff.avformat, av_read_frame);
    LOAD_FN(g_ff.avformat, av_seek_frame);

    LOAD_FN(g_ff.avcodec, avcodec_find_decoder);
    LOAD_FN(g_ff.avcodec, avcodec_alloc_context3);
    LOAD_FN(g_ff.avcodec, avcodec_free_context);
    LOAD_FN(g_ff.avcodec, avcodec_open2);
    LOAD_FN(g_ff.avcodec, avcodec_send_packet);
    LOAD_FN(g_ff.avcodec, avcodec_receive_frame);
    LOAD_FN(g_ff.avcodec, avcodec_parameters_to_context);

    LOAD_FN(g_ff.avutil, av_frame_alloc);
    LOAD_FN(g_ff.avutil, av_frame_free);
    LOAD_FN(g_ff.avutil, av_frame_get_buffer);
    LOAD_FN(g_ff.avutil, av_frame_unref);
    LOAD_FN(g_ff.avutil, av_malloc);
    LOAD_FN(g_ff.avutil, av_free);
    LOAD_FN(g_ff.avutil, av_rescale_q);
    LOAD_FN(g_ff.avutil, av_dict_free);
    LOAD_FN(g_ff.avutil, av_dict_set);
    LOAD_FN(g_ff.avutil, av_log_set_level);

    if (g_ff.swscale) {
        LOAD_FN(g_ff.swscale, sws_getContext);
        LOAD_FN(g_ff.swscale, sws_freeContext);
        LOAD_FN(g_ff.swscale, sws_scale);
    }

    return true;
}

#undef LOAD_FN

// =============================================================================
// FFmpegVideoDecoder
// =============================================================================
class FFmpegVideoDecoder : public VideoDecoder {
public:
    ~FFmpegVideoDecoder() override { Close(); }

    DecoderStatus Open(const std::string& path) override;
    DecoderStatus Open(const uint8_t* data, size_t size) override;
    void Close() override;
    bool IsOpen() const override { return m_fmt != nullptr; }

    void SetConfig(const VideoDecoderConfig& config) override { m_cfg = config; }
    VideoDecoderConfig GetConfig() const override { return m_cfg; }

    VideoInfo GetVideoInfo() const override { return m_info; }
    int GetAudioTrackCount() const override { return 0; } // audio not implemented
    AudioTrackInfo GetAudioTrackInfo(int) const override { return {}; }

    DecoderStatus DecodeNextFrame(VideoFrame& out_frame) override;
    DecoderStatus Seek(double ts) override;
    double GetCurrentTime() const override { return 0.0; }
    void SetActiveAudioTrack(int) override {}
    bool GetFrameForGpuUpload(VideoFrame& f, uint8_t* buf, uint32_t cap) override;

private:
    VideoDecoderConfig m_cfg;
    AVFormatContext* m_fmt = nullptr;
    AVCodecContext* m_vctx = nullptr;
    AVFrame* m_frame = nullptr;
    int m_vstream = -1;
    bool m_eof = false;

    SwsContext* m_sws = nullptr;
    int m_sw = 0, m_sh = 0; // cached sws dimensions

    VideoInfo m_info{};

    bool OpenCodec(AVStream* st);
    void ConvertFrame(VideoFrame& out);
};

// =============================================================================
// Factory entry point
// =============================================================================
static bool EnsureLoaded(const std::string& dll_path) {
    std::call_once(g_ff_once, [&]() {
        g_ff_loaded = LoadFfmpeg(dll_path.empty() ? nullptr : dll_path.c_str());
        if (!g_ff_loaded && !dll_path.empty()) g_ff_loaded = LoadFfmpeg(nullptr);
        if (g_ff_loaded) g_ff.av_log_set_level(-8); // AV_LOG_QUIET
    });
    return g_ff_loaded;
}

VideoDecoder* CreateFFmpegDecoder(const VideoDecoderConfig& cfg) {
    if (EnsureLoaded(cfg.ffmpeg_dll_path)) {
        LOG_INFO(Media, "FFmpeg: loaded — creating decoder.");
        return new FFmpegVideoDecoder();
    }
    LOG_WARN(Media, "FFmpeg: DLLs not found — H.264/H.265/VP9 videos not available.");
    return nullptr;
}

// =============================================================================
// Implementation
// =============================================================================

DecoderStatus FFmpegVideoDecoder::Open(const std::string& path) {
    if (!g_ff_loaded) return DecoderStatus::FormatError;
    Close();

    AVFormatContext* fmt = nullptr;
    AVDictionary* opts = nullptr;
    g_ff.av_dict_set(&opts, "probesize", "5000000", 0);
    g_ff.av_dict_set(&opts, "analyzeduration", "5000000", 0);

    int ret = g_ff.avformat_open_input(&fmt, path.c_str(), nullptr, &opts);
    if (opts) g_ff.av_dict_free(&opts);
    if (ret < 0 || !fmt) return DecoderStatus::FormatError;

    ret = g_ff.avformat_find_stream_info(fmt, nullptr);
    if (ret < 0) { g_ff.avformat_close_input(&fmt); return DecoderStatus::FormatError; }

    m_fmt = fmt;

    // Find video stream.
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        auto* st = fmt->streams[i];
        if (!st || !st->codecpar) continue;
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && m_vstream < 0) {
            m_vstream = static_cast<int>(i);
            if (!OpenCodec(st)) { m_vstream = -1; Close(); return DecoderStatus::FormatError; }
        }
    }

    if (m_vstream < 0) { Close(); return DecoderStatus::FormatError; }

    // Fill VideoInfo.
    auto* par = fmt->streams[m_vstream]->codecpar;
    m_info.width  = static_cast<uint32_t>(par->width);
    m_info.height = static_cast<uint32_t>(par->height);
    m_info.pixel_format = VideoPixelFormat::RGBA8;

    auto* st = fmt->streams[m_vstream];
    if (st->avg_frame_rate.den > 0)
        m_info.frame_rate = static_cast<double>(st->avg_frame_rate.num) / st->avg_frame_rate.den;

    m_info.duration_sec = fmt->duration > 0 ? static_cast<double>(fmt->duration) / 1000000.0 : 0.0;
    m_info.frame_count = static_cast<uint64_t>(st->nb_frames);

    if (fmt->iformat && fmt->iformat->name) m_info.container = fmt->iformat->name;

    const char* cn = "?";
    switch (par->codec_id) {
        case AV_CODEC_ID_H264:  cn = "h264"; break;
        case AV_CODEC_ID_HEVC:  cn = "hevc"; break;
        case AV_CODEC_ID_VP9:   cn = "vp9";  break;
        case AV_CODEC_ID_AV1:   cn = "av1";  break;
    }
    m_info.codec_name = cn;

    LOG_INFO(Media, "FFmpeg: %s — %dx%d %.2ffps %s", path.c_str(),
             m_info.width, m_info.height, m_info.frame_rate, cn);
    return DecoderStatus::Ok;
}

DecoderStatus FFmpegVideoDecoder::Open(const uint8_t* d, size_t s) {
    (void)d; (void)s;
    return DecoderStatus::FormatError; // not implemented
}

void FFmpegVideoDecoder::Close() {
    if (m_sws && g_ff.swscale) { g_ff.sws_freeContext(m_sws); m_sws = nullptr; }
    if (m_frame) { g_ff.av_frame_free(&m_frame); }
    if (m_vctx)  { g_ff.avcodec_free_context(&m_vctx); }
    if (m_fmt)   { g_ff.avformat_close_input(&m_fmt); }
    m_vstream = -1; m_eof = false; m_sw = 0; m_sh = 0; m_info = {};
}

bool FFmpegVideoDecoder::OpenCodec(AVStream* st) {
    if (!st->codecpar) return false;
    auto* codec = g_ff.avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) return false;

    m_vctx = g_ff.avcodec_alloc_context3(codec);
    if (!m_vctx) return false;

    if (g_ff.avcodec_parameters_to_context(m_vctx, st->codecpar) < 0) return false;

    AVDictionary* opts = nullptr;
    g_ff.av_dict_set(&opts, "threads", "auto", 0);
    int ret = g_ff.avcodec_open2(m_vctx, codec, &opts);
    if (opts) g_ff.av_dict_free(&opts);
    if (ret < 0) return false;

    m_frame = g_ff.av_frame_alloc();
    return m_frame != nullptr;
}

DecoderStatus FFmpegVideoDecoder::DecodeNextFrame(VideoFrame& out) {
    if (!m_fmt || !m_vctx) return DecoderStatus::Error;

    int ret;
    while (true) {
        ret = g_ff.avcodec_receive_frame(m_vctx, m_frame);
        if (ret == 0) { ConvertFrame(out); return DecoderStatus::Ok; }
        if (ret < 0 && ret != -11) { m_eof = true; return DecoderStatus::Eof; } // EAGAIN

        // Read next video packet.
        AVPacket pkt;
        while ((ret = g_ff.av_read_frame(m_fmt, &pkt)) >= 0) {
            if (pkt.stream_index == m_vstream) {
                g_ff.avcodec_send_packet(m_vctx, &pkt);
                g_ff.av_frame_unref(m_frame);
                break;
            }
            // Non-video packet — discard (freed by av_read_frame internals).
        }
        if (ret < 0) {
            // Flush decoder.
            g_ff.avcodec_send_packet(m_vctx, nullptr);
        }
    }
}

void FFmpegVideoDecoder::ConvertFrame(VideoFrame& out) {
    // Determine output dimensions.
    int src_w = m_frame->width;
    int src_h = m_frame->height;
    int dst_w = src_w, dst_h = src_h;
    float sc = m_cfg.scale_factor;
    if (sc > 0.01f && (sc < 0.99f || sc > 1.01f)) {
        dst_w = std::max(16, (int)(dst_w * sc));
        dst_h = std::max(16, (int)(dst_h * sc));
    }

    out.width  = (uint32_t)dst_w;
    out.height = (uint32_t)dst_h;
    out.format = VideoPixelFormat::RGBA8;
    out.is_keyframe = (m_frame->pict_type == AV_PICTURE_TYPE_I);
    out.strides[0] = (uint32_t)(dst_w * 4);
    out.data[0].resize((size_t)dst_w * dst_h * 4);

    if (m_frame->data[0]) {
        if (!m_sws || m_sw != src_w || m_sh != src_h) {
            if (m_sws && g_ff.swscale) { g_ff.sws_freeContext(m_sws); m_sws = nullptr; }
            if (g_ff.swscale) {
                m_sws = g_ff.sws_getContext(src_w, src_h, m_frame->format,
                                            dst_w, dst_h, AV_PIX_FMT_RGBA, 2, 0, 0, 0);
                m_sw = src_w; m_sh = src_h;
            }
        }

        if (m_sws) {
            const uint8_t* sl[4] = { m_frame->data[0], m_frame->data[1], m_frame->data[2], 0 };
            int ss[4] = { m_frame->linesize[0], m_frame->linesize[1], m_frame->linesize[2], 0 };
            uint8_t* dl = out.data[0].data();
            int ds = (int)out.strides[0];
            g_ff.sws_scale(m_sws, sl, ss, 0, src_h, &dl, &ds);
        }
    }

    if (out.data[0].empty() || !m_sws) {
        // Black frame fallback.
        out.data[0].assign((size_t)dst_w * dst_h * 4, 0);
    }

    g_ff.av_frame_unref(m_frame);
}

DecoderStatus FFmpegVideoDecoder::Seek(double ts) {
    if (!m_fmt || m_vstream < 0) return DecoderStatus::Error;
    m_eof = false;
    int64_t t = (int64_t)(ts * 1000000.0);
    g_ff.av_seek_frame(m_fmt, m_vstream, t, AVSEEK_FLAG_BACKWARD);
    g_ff.avcodec_send_packet(m_vctx, nullptr);
    return DecoderStatus::Ok;
}

bool FFmpegVideoDecoder::GetFrameForGpuUpload(VideoFrame& f, uint8_t* buf, uint32_t cap) {
    size_t n = (size_t)f.width * f.height * 4;
    if (cap < n || !buf || f.data[0].size() < n) return false;
    memcpy(buf, f.data[0].data(), n);
    return true;
}
