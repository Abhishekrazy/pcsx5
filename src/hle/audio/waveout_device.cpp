// WaveOut audio backend — legacy winmm waveOut API.
// Stereo PCM16 output at the guest's sample rate, driven by
// WOM_DONE callbacks for buffer recycling.

#include "audio_device.h"
#include "../../common/log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// WaveOut device — wraps HWAVEOUT with a pool of stereo PCM16 buffers.
// ---------------------------------------------------------------------------
class WaveOutDevice : public AudioDevice {
public:
    WaveOutDevice() = default;
    ~WaveOutDevice() override { Close(); }

    bool Open(const AalFormat& format) override;
    void Close() override;
    bool IsOpen() const override { return m_open; }
    AalCaps GetCaps() const override;
    void Output(const u8* data, uint32_t frame_count) override;
    void Reset() override;
    void SetVolume(float volume) override { m_volume = volume; }
    float GetVolume() const override { return m_volume; }
    uint32_t GetLatencyFrames() const override;

private:
    struct OutBuffer {
        WAVEHDR hdr{};
        std::vector<s16> data;  // stereo PCM16
        bool prepared = false;
    };

    static constexpr int kMaxBuffersInFlight = 8;

    static void CALLBACK WaveOutProc(HWAVEOUT hwo, UINT msg,
                                      DWORD_PTR instance,
                                      DWORD_PTR param1,
                                      DWORD_PTR /*param2*/);

    bool m_open = false;
    float m_volume = 1.0f;
    AalFormat m_format{};
    HWAVEOUT m_wave_out = nullptr;
    mutable std::mutex m_mutex;    std::condition_variable m_cv;
    std::deque<OutBuffer*> m_free_buffers;
    int m_in_flight = 0;
};

// ---------------------------------------------------------------------------
// CALLBACK
// ---------------------------------------------------------------------------
void CALLBACK WaveOutDevice::WaveOutProc(HWAVEOUT /*hwo*/, UINT msg,
                                          DWORD_PTR instance,
                                          DWORD_PTR param1,
                                          DWORD_PTR /*param2*/) {
    if (msg != WOM_DONE) return;
    auto* hdr = reinterpret_cast<WAVEHDR*>(param1);
    auto* buf = reinterpret_cast<OutBuffer*>(hdr->dwUser);
    if (!buf) return;

    // The instance pointer is the WaveOutDevice* we passed as callback data.
    auto* self = reinterpret_cast<WaveOutDevice*>(instance);
    if (!self) return;

    {
        std::lock_guard<std::mutex> lock(self->m_mutex);
        self->m_free_buffers.push_back(buf);
        --self->m_in_flight;
    }
    self->m_cv.notify_all();
}

// ---------------------------------------------------------------------------
// Open
// ---------------------------------------------------------------------------
bool WaveOutDevice::Open(const AalFormat& format) {
    if (m_open) Close();

    WAVEFORMATEX wfx{};
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = 2;  // Always stereo
    wfx.nSamplesPerSec  = format.sample_rate;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = static_cast<WORD>(2 * 2);
    wfx.nAvgBytesPerSec = format.sample_rate * wfx.nBlockAlign;
    wfx.cbSize          = 0;

    HWAVEOUT hwo = nullptr;
    MMRESULT res = ::waveOutOpen(&hwo, WAVE_MAPPER, &wfx,
                                  reinterpret_cast<DWORD_PTR>(WaveOutProc),
                                  reinterpret_cast<DWORD_PTR>(this),
                                  CALLBACK_FUNCTION);
    if (res != MMSYSERR_NOERROR) {
        LOG_WARN(HLE, "WaveOutDevice: waveOutOpen failed (err=%u)", res);
        return false;
    }

    m_wave_out = hwo;
    m_format = format;
    m_volume = 1.0f;
    m_in_flight = 0;
    m_open = true;
    return true;
}

// ---------------------------------------------------------------------------
// Close
// ---------------------------------------------------------------------------
void WaveOutDevice::Close() {
    if (!m_wave_out) return;
    HWAVEOUT hwo = m_wave_out;
    m_wave_out = nullptr;
    m_open = false;

    ::waveOutReset(hwo);
    for (int spin = 0; spin < 100; ++spin) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_in_flight == 0) break;
        }
        ::Sleep(1);
    }

    std::deque<OutBuffer*> all;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        all = std::move(m_free_buffers);
        m_in_flight = 0;
    }
    for (OutBuffer* buf : all) {
        if (buf->prepared) {
            ::waveOutUnprepareHeader(hwo, &buf->hdr, sizeof(WAVEHDR));
        }
        delete buf;
    }
    ::waveOutClose(hwo);
}

// ---------------------------------------------------------------------------
// GetCaps
// ---------------------------------------------------------------------------
AalCaps WaveOutDevice::GetCaps() const {
    AalCaps caps;
    caps.backend_name = "waveOut";
    caps.max_channels = 2;
    caps.max_sample_rate = 48000;
    caps.min_buffer_frames = 64;
    caps.max_buffer_frames = 4096;
    return caps;
}

// ---------------------------------------------------------------------------
// Output — convert to stereo PCM16 and submit to waveOut
// ---------------------------------------------------------------------------
void WaveOutDevice::Output(const u8* data, uint32_t frame_count) {
    if (!m_open || !m_wave_out || frame_count == 0) return;

    const size_t out_bytes = static_cast<size_t>(frame_count) * 2 * sizeof(s16);

    OutBuffer* buf = nullptr;
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this] {
            return !m_free_buffers.empty() || m_in_flight < kMaxBuffersInFlight;
        });
        if (!m_free_buffers.empty()) {
            buf = m_free_buffers.front();
            m_free_buffers.pop_front();
        } else {
            buf = new OutBuffer();
            buf->data.resize(out_bytes);
        }
        ++m_in_flight;
    }

    // Convert from guest format to stereo PCM16.
    // FIXME: this should handle mono/stereo/8ch, s16/float32 conversion.
    if (buf->data.size() != out_bytes) {
        buf->data.resize(out_bytes);
    }

    // Simple stereo s16 copy (the guest format is already stereo s16
    // in the common case; conversion from other formats is handled by
    // the caller via ConvertToStereoS16 in the current libaudioout.cpp).
    if (frame_count > 0 && data) {
        std::memcpy(buf->data.data(), data, out_bytes);
    }

    if (m_volume != 1.0f) {
        for (size_t i = 0; i < frame_count * 2; ++i) {
            buf->data[i] = static_cast<s16>(
                static_cast<float>(buf->data[i]) * m_volume);
        }
    }

    if (!buf->prepared) {
        std::memset(&buf->hdr, 0, sizeof(buf->hdr));
        buf->hdr.lpData         = reinterpret_cast<LPSTR>(buf->data.data());
        buf->hdr.dwBufferLength = static_cast<DWORD>(out_bytes);
        buf->hdr.dwUser         = reinterpret_cast<DWORD_PTR>(buf);
        if (::waveOutPrepareHeader(m_wave_out, &buf->hdr,
                                    sizeof(buf->hdr)) != MMSYSERR_NOERROR) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_free_buffers.push_back(buf);
            --m_in_flight;
            return;
        }
        buf->prepared = true;
    }

    if (::waveOutWrite(m_wave_out, &buf->hdr,
                        sizeof(buf->hdr)) != MMSYSERR_NOERROR) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_free_buffers.push_back(buf);
        --m_in_flight;
    }
}

// ---------------------------------------------------------------------------
// Reset
// ---------------------------------------------------------------------------
void WaveOutDevice::Reset() {
    if (m_wave_out) {
        ::waveOutReset(m_wave_out);
    }
}

// ---------------------------------------------------------------------------
// GetLatencyFrames
// ---------------------------------------------------------------------------
uint32_t WaveOutDevice::GetLatencyFrames() const {
    // waveOut doesn't provide a direct latency query.
    // Estimate based on in-flight buffers.
    std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<uint32_t>(m_in_flight) * m_format.sample_rate / 1000 * 50;  // ~50ms per buffer
}

// ---------------------------------------------------------------------------
// Factory entry point
// ---------------------------------------------------------------------------
AudioDevice* CreateWaveOutDevice() {
    return new WaveOutDevice();
}
