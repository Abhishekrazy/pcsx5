// Audio device factory + PacingAudioDevice implementation.
//
// The factory probes available audio backends in priority order:
//   SDL → XAudio2 → WASAPI → waveOut → Pacing
// and returns the first one that initialises successfully.

#include "audio_device.h"
#include "../../common/log.h"

#include <chrono>
#include <cstring>
#include <thread>

// ---------------------------------------------------------------------------
// Forward declarations for each backend's factory function
// ---------------------------------------------------------------------------

#ifdef _WIN32
AudioDevice* CreateWaveOutDevice();
AudioDevice* CreateWasapiDevice();
AudioDevice* CreateXa2Device();
#endif

// SDL audio (cross-platform, dynamically loaded).
AudioDevice* CreateSdlAudioDevice();

// ---------------------------------------------------------------------------
// AudioDevice::Create — factory
// ---------------------------------------------------------------------------

AudioDevice* AudioDevice::Create(AalBackendType type) {
    switch (type) {
        case AalBackendType::Off:
        case AalBackendType::Null:
            return new PacingAudioDevice();

        case AalBackendType::WaveOut:
#ifdef _WIN32
            return CreateWaveOutDevice();
#else
            LOG_WARN(HLE, "AAL: waveOut is Windows-only; falling back to Pacing");
            return new PacingAudioDevice();
#endif

        case AalBackendType::WASAPI:
#ifdef _WIN32
            return CreateWasapiDevice();
#else
            LOG_WARN(HLE, "AAL: WASAPI is Windows-only; falling back to Pacing");
            return new PacingAudioDevice();
#endif

        case AalBackendType::XAudio2:
#ifdef _WIN32
            return CreateXa2Device();
#else
            LOG_WARN(HLE, "AAL: XAudio2 is Windows-only; falling back to Pacing");
            return new PacingAudioDevice();
#endif

        case AalBackendType::SDL: {
            AudioDevice* d = CreateSdlAudioDevice();
            if (d && d->Open(AalFormat{})) { d->Close(); return d; }
            delete d;
            LOG_WARN(HLE, "AAL: SDL audio backend not available");
            return new PacingAudioDevice();
        }

        case AalBackendType::Auto: {
            // Probe priority: SDL → XAudio2 → WASAPI → waveOut → Pacing
            AudioDevice* d = CreateSdlAudioDevice();
            if (d) return d;
#ifdef _WIN32
            d = CreateXa2Device();
            if (d) return d;
            d = CreateWasapiDevice();
            if (d) return d;
            d = CreateWaveOutDevice();
            if (d) return d;
#endif
            LOG_INFO(HLE, "AAL: no hardware audio backend available; using Pacing");
            return new PacingAudioDevice();
        }

        default:
            LOG_ERROR(HLE, "AAL: unknown backend type %d",
                      static_cast<int>(type));
            return new PacingAudioDevice();
    }
}

// ===========================================================================
// PacingAudioDevice — time-accurate pacing without a host audio device.
// Keeps guest audio threads running at real-time rate by sleeping for
// the duration of each audio buffer.  Used when no host audio backend
// is available (audio.backend=0) or when all backends failed.
// ===========================================================================

bool PacingAudioDevice::Open(const AalFormat& format) {
    m_format = format;
    m_next_timestamp_us = 0;
    m_open = true;
    return true;
}

void PacingAudioDevice::Close() {
    m_open = false;
}

AalCaps PacingAudioDevice::GetCaps() const {
    AalCaps caps;
    caps.backend_name = "Pacing (null)";
    caps.max_channels = 8;
    caps.max_sample_rate = 192000;
    caps.min_buffer_frames = 1;
    caps.max_buffer_frames = 4096;
    return caps;
}

void PacingAudioDevice::Output(const u8* /*data*/, uint32_t frame_count) {
    if (!m_open || frame_count == 0) return;

    using clock = std::chrono::steady_clock;
    const auto now = clock::now();

    if (m_next_timestamp_us == 0) {
        m_next_timestamp_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                now.time_since_epoch()).count());
    }

    const uint64_t now_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch()).count());

    const uint64_t frame_duration_us = static_cast<uint64_t>(
        static_cast<double>(frame_count) / m_format.sample_rate * 1000000.0);

    if (m_next_timestamp_us > now_us) {
        const uint64_t sleep_us = m_next_timestamp_us - now_us;
        if (sleep_us > 0 && sleep_us < 1000000) {  // Cap at 1 second
            std::this_thread::sleep_for(
                std::chrono::microseconds(sleep_us));
        }
    }

    m_next_timestamp_us = (std::max)(now_us, m_next_timestamp_us) +
                          frame_duration_us;
}

void PacingAudioDevice::Reset() {
    m_next_timestamp_us = 0;
}
