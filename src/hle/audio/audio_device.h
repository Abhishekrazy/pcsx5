// Audio Abstraction Layer (AAL) — abstract interface for audio output.
//
// Every audio backend (WASAPI, XAudio2, waveOut, SDL, null/pacing)
// implements this interface.  The libSceAudioOut HLE accesses audio
// exclusively through these types.
//
// Thread safety: Open/Close are called from the HLE dispatch thread.
// Output() is called from the guest audio thread (which may be any
// guest thread).  Other accessors are safe to call from any thread.

#pragma once
#include "../../common/types.h"
#include <cstdint>
#include <string>

// ---------------------------------------------------------------------------
// Backend selection
// ---------------------------------------------------------------------------
enum class AalBackendType : int {
    Off     = 0,    // no device; time-accurate pacing only
    WASAPI  = 1,    // Windows Core Audio (shared-mode)
    XAudio2 = 2,    // XAudio2 2.9
    WaveOut = 3,    // legacy winmm waveOut
    SDL     = 4,    // SDL audio (cross-platform)
    Null    = 5,    // headless / testing
    Auto    = -1,   // probe: SDL → XAudio2 → WASAPI → waveOut → Null
};

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
struct AalConfig {
    AalBackendType backend = AalBackendType::Auto;
    float          volume  = 1.0f;   // master volume 0.0..1.0
    int            buffer_ms = 50;   // target buffer duration in ms
};

// ---------------------------------------------------------------------------
// Format description
// ---------------------------------------------------------------------------
enum class AalSampleFormat : int {
    S16     = 0,    // signed 16-bit PCM
    Float32 = 1,    // IEEE 754 float32 PCM
};

struct AalFormat {
    uint32_t      sample_rate     = 48000;    // Hz
    int           channels        = 2;         // 1 (mono), 2 (stereo), 8 (7.1)
    AalSampleFormat sample_format = AalSampleFormat::S16;
};

// ---------------------------------------------------------------------------
// Capabilities
// ---------------------------------------------------------------------------
struct AalCaps {
    std::string backend_name;       // "WASAPI", "XAudio2", "SDL", "Null"
    uint32_t    max_channels       = 8;
    uint32_t    max_sample_rate    = 192000;
    uint32_t    min_buffer_frames  = 64;
    uint32_t    max_buffer_frames  = 4096;
    float       native_volume      = 1.0f;     // hardware volume support
};

// ---------------------------------------------------------------------------
// AudioDevice interface — every backend implements this
// ---------------------------------------------------------------------------
class AudioDevice {
public:
    virtual ~AudioDevice() = default;

    // ---- lifecycle -------------------------------------------------------
    // `format` is the guest's audio format.  The backend converts to its
    // native format internally.  Returns false if the format is unsupported.
    virtual bool Open(const AalFormat& format) = 0;
    virtual void Close() = 0;
    virtual bool IsOpen() const = 0;

    // ---- capabilities ----------------------------------------------------
    virtual AalCaps GetCaps() const = 0;

    // ---- output ----------------------------------------------------------
    // Deliver `frame_count` frames of PCM audio.  `data` points to the
    // guest buffer (interleaved, in the format specified at Open()).
    // The backend converts, buffers, and submits to the host device.
    // May block for pacing (real-time rate when the buffer is full).
    virtual void Output(const u8* data, uint32_t frame_count) = 0;

    // Immediately discard any buffered audio and reset the device.
    virtual void Reset() = 0;

    // ---- volume ----------------------------------------------------------
    // Software volume scaling applied before submission to the host.
    // 0.0 = silence, 1.0 = unity.
    virtual void SetVolume(float volume) = 0;
    virtual float GetVolume() const = 0;

    // ---- latency ---------------------------------------------------------
    // Estimated number of frames currently queued/buffered in the host
    // audio device.  Useful for the HLE layer to synchronize guest audio
    // timing.  Returns 0 when unknown.
    virtual uint32_t GetLatencyFrames() const = 0;

    // ---- factory ---------------------------------------------------------
    static AudioDevice* Create(AalBackendType type);
};

// ---------------------------------------------------------------------------
// Pacing-only device (null backend)
// ---------------------------------------------------------------------------
class PacingAudioDevice : public AudioDevice {
public:
    bool Open(const AalFormat& format) override;
    void Close() override;
    bool IsOpen() const override { return m_open; }
    AalCaps GetCaps() const override;
    void Output(const u8* data, uint32_t frame_count) override;
    void Reset() override;
    void SetVolume(float) override {}
    float GetVolume() const override { return 1.0f; }
    uint32_t GetLatencyFrames() const override { return 0; }

private:
    bool m_open = false;
    AalFormat m_format{};
    uint64_t m_next_timestamp_us = 0;  // next wall-clock time for output
};
