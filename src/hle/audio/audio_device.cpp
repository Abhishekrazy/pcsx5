// Audio device factory + PacingAudioDevice implementation.
//
// The factory probes available audio backends in priority order:
//   SDL → XAudio2 → WASAPI → waveOut → Pacing
// and returns the first one that initialises successfully.

#include "audio_device.h"
#include "../../common/log.h"
#include "../../memory/memory.h"

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

// ---------------------------------------------------------------------------
// AudioDevice::OutputDirect — default implementation (heap-copy fallback).
// Reads from guest memory and delegates to Output().  Backends that support
// zero-copy guest-memory access override this.
// ---------------------------------------------------------------------------
uint32_t AudioDevice::OutputDirect(u64 guest_addr, uint32_t frame_count) {
    if (guest_addr == 0 || frame_count == 0) return 0;
    const size_t byte_count = static_cast<size_t>(frame_count) * 2 * sizeof(s16);
    auto buf = std::make_unique<u8[]>(byte_count);
    u64 got = 0;
    if (!Memory::GuardedRead(buf.get(), guest_addr, byte_count, &got) ||
        got != byte_count) {
        // Playing an unread buffer emits whatever the allocator last left
        // there -- audible noise, and nothing to explain it.
        LOG_WARN(HLE, "audio OutputDirect: read %llu of %zu bytes at 0x%llx; "
                      "dropping the block rather than playing uninitialised memory",
                 (unsigned long long)got, byte_count,
                 (unsigned long long)guest_addr);
        return 0;
    }
    Output(buf.get(), frame_count);
    return frame_count;
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

// ---------------------------------------------------------------------------
// OutputDirect — the Pacing device doesn't need the data; just pace.
// ---------------------------------------------------------------------------
uint32_t PacingAudioDevice::OutputDirect(u64 /*guest_addr*/, uint32_t frame_count) {
    if (!m_open || frame_count == 0) return 0;
    Output(nullptr, frame_count);
    return frame_count;
}

void PacingAudioDevice::Reset() {
    m_next_timestamp_us = 0;
}
