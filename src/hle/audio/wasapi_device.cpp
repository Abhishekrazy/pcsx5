// WASAPI audio backend — Windows Core Audio shared-mode, event-driven.
// Drives the audio device from a dedicated render thread that waits
// on the IAudioClient buffer-event handle.

#include "audio_device.h"
#include "../../common/log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#pragma comment(lib, "ole32.lib")

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

class WasapiDevice : public AudioDevice {
public:
    WasapiDevice() = default;
    ~WasapiDevice() override { Close(); }

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
    struct WasapiState {
        IAudioClient*        client = nullptr;
        IAudioRenderClient*  render = nullptr;
        HANDLE               event  = nullptr;
        std::thread          thread;
        std::atomic<bool>    stop{false};
        bool                 own_com    = false;
        bool                 mix_float  = false;
        uint32_t             buffer_frames = 0;
        std::deque<std::vector<s16>> queue;
    };

    void RenderThreadMain();

    bool m_open = false;
    float m_volume = 1.0f;
    AalFormat m_format{};
    WasapiState m_ws;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
};

// ---------------------------------------------------------------------------
// Render thread
// ---------------------------------------------------------------------------
void WasapiDevice::RenderThreadMain() {
    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const uint32_t channels = 2;

    while (!m_ws.stop.load(std::memory_order_acquire)) {
        const DWORD wait = ::WaitForSingleObject(m_ws.event, 200);
        if (m_ws.stop.load(std::memory_order_acquire)) break;
        if (wait != WAIT_OBJECT_0) continue;

        uint32_t padding = 0;
        if (FAILED(m_ws.client->GetCurrentPadding(&padding)) ||
            padding >= m_ws.buffer_frames) {
            continue;
        }

        uint32_t avail = m_ws.buffer_frames - padding;
        while (avail > 0 && !m_ws.stop.load(std::memory_order_acquire)) {
            BYTE* data = nullptr;
            if (FAILED(m_ws.render->GetBuffer(avail, &data)) || !data) break;

            uint32_t written = 0;
            size_t offset = 0;

            // Pull blocks from the queue and write them into the WASAPI buffer.
            while (written < avail) {
                std::vector<s16> current;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (!m_ws.queue.empty()) {
                        current = std::move(m_ws.queue.front());
                        m_ws.queue.pop_front();
                    }
                }
                m_cv.notify_all();

                if (current.empty()) break;  // underrun: zero-fill below

                const uint32_t frames_avail = avail - written;
                const uint32_t frames_in_block =
                    static_cast<uint32_t>(current.size() / channels);
                const uint32_t frames =
                    (std::min)(frames_in_block, frames_avail);

                if (m_ws.mix_float) {
                    float* dst = reinterpret_cast<float*>(data) +
                                 static_cast<size_t>(written) * channels;
                    for (uint32_t i = 0; i < frames * channels; ++i) {
                        dst[i] = current[offset + i] / 32768.0f;
                    }
                } else {
                    s16* dst = reinterpret_cast<s16*>(data) +
                               static_cast<size_t>(written) * channels;
                    std::memcpy(dst, current.data() + offset,
                                frames * channels * sizeof(s16));
                }
                offset += frames * channels;
                written += frames;
            }

            if (written < avail) {
                // Underrun: fill remainder with silence.
                const size_t zero_off =
                    static_cast<size_t>(written) * channels *
                    (m_ws.mix_float ? sizeof(float) : sizeof(s16));
                const size_t zero_len =
                    static_cast<size_t>(avail - written) * channels *
                    (m_ws.mix_float ? sizeof(float) : sizeof(s16));
                std::memset(reinterpret_cast<BYTE*>(data) + zero_off,
                            0, zero_len);
                written = avail;
            }

            m_ws.render->ReleaseBuffer(written, 0);
            avail = 0;
        }
    }
    ::CoUninitialize();
}

// ---------------------------------------------------------------------------
// Open
// ---------------------------------------------------------------------------
bool WasapiDevice::Open(const AalFormat& format) {
    if (m_open) Close();

    WasapiState& ws = m_ws;
    const HRESULT com_hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com_hr) && com_hr != RPC_E_CHANGED_MODE) {
        LOG_WARN(HLE, "WasapiDevice: CoInitializeEx failed (hr=0x%08lX)",
                 static_cast<unsigned long>(com_hr));
        return false;
    }
    ws.own_com = SUCCEEDED(com_hr);

    bool ok = false;
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    WAVEFORMATEX* mix = nullptr;

    do {
        if (FAILED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                      CLSCTX_ALL,
                                      IID_PPV_ARGS(&enumerator)))) break;
        if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole,
                                                       &device))) break;
        if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                    nullptr,
                                    reinterpret_cast<void**>(&ws.client))))
            break;
        if (FAILED(ws.client->GetMixFormat(&mix)) || !mix) break;

        bool mix_float = false;
        if (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT &&
            mix->wBitsPerSample == 32) {
            mix_float = true;
        } else if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                   mix->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) -
                                  sizeof(WAVEFORMATEX)) {
            const auto* ext =
                reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix);
            if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT &&
                mix->wBitsPerSample == 32) {
                mix_float = true;
            } else if (!(ext->SubFormat == KSDATAFORMAT_SUBTYPE_PCM &&
                         mix->wBitsPerSample == 16)) {
                break;
            }
        } else if (!(mix->wFormatTag == WAVE_FORMAT_PCM &&
                     mix->wBitsPerSample == 16)) {
            break;
        }

        if (mix->nChannels != 2 ||
            mix->nSamplesPerSec != format.sample_rate) {
            LOG_WARN(HLE, "WasapiDevice: mix is %u Hz/%u ch (want %u Hz stereo)",
                     mix->nSamplesPerSec, mix->nChannels,
                     format.sample_rate);
            break;
        }
        ws.mix_float = mix_float;

        const REFERENCE_TIME duration = 400000;  // 40ms
        if (FAILED(ws.client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                         AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                         duration, 0, mix, nullptr))) break;
        if (FAILED(ws.client->GetBufferSize(&ws.buffer_frames))) break;
        if (FAILED(ws.client->GetService(IID_PPV_ARGS(&ws.render)))) break;

        ws.event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!ws.event) break;
        if (FAILED(ws.client->SetEventHandle(ws.event))) break;
        if (FAILED(ws.client->Start())) break;

        ws.stop.store(false, std::memory_order_release);
        ws.thread = std::thread(&WasapiDevice::RenderThreadMain, this);
        m_format = format;
        m_open = true;
        ok = true;
    } while (false);

    if (mix) ::CoTaskMemFree(mix);
    if (device) device->Release();
    if (enumerator) enumerator->Release();

    if (!ok) {
        LOG_WARN(HLE, "WasapiDevice: init failed");
        Close();
    }
    return ok;
}

// ---------------------------------------------------------------------------
// Close
// ---------------------------------------------------------------------------
void WasapiDevice::Close() {
    WasapiState& ws = m_ws;
    ws.stop.store(true, std::memory_order_release);
    if (ws.event) ::SetEvent(ws.event);
    if (ws.thread.joinable()) ws.thread.join();

    if (ws.client) { ws.client->Stop(); ws.client->Release(); ws.client = nullptr; }
    if (ws.render) { ws.render->Release(); ws.render = nullptr; }
    if (ws.event) { ::CloseHandle(ws.event); ws.event = nullptr; }

    ws.buffer_frames = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ws.queue.clear();
    }
    if (ws.own_com) { ws.own_com = false; ::CoUninitialize(); }

    m_open = false;
}

// ---------------------------------------------------------------------------
// GetCaps
// ---------------------------------------------------------------------------
AalCaps WasapiDevice::GetCaps() const {
    AalCaps caps;
    caps.backend_name = "WASAPI";
    caps.max_channels = 2;
    caps.max_sample_rate = 192000;
    caps.min_buffer_frames = 64;
    caps.max_buffer_frames = 4096;
    return caps;
}

// ---------------------------------------------------------------------------
// Output — convert to stereo PCM16 and enqueue for the render thread
// ---------------------------------------------------------------------------
void WasapiDevice::Output(const u8* data, uint32_t frame_count) {
    if (!m_open || frame_count == 0) return;

    std::vector<s16> block(static_cast<size_t>(frame_count) * 2);
    // Simple copy (assumes stereo s16 input; conversion from other
    // formats is done by the caller).
    std::memcpy(block.data(), data,
                static_cast<size_t>(frame_count) * 2 * sizeof(s16));

    if (m_volume != 1.0f) {
        for (size_t i = 0; i < block.size(); ++i) {
            block[i] = static_cast<s16>(
                static_cast<float>(block[i]) * m_volume);
        }
    }

    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this] {
            return m_ws.queue.size() < 8;  // max 8 blocks in flight
        });
        m_ws.queue.push_back(std::move(block));
    }
}

// ---------------------------------------------------------------------------
// Reset
// ---------------------------------------------------------------------------
void WasapiDevice::Reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_ws.queue.clear();
}

// ---------------------------------------------------------------------------
// GetLatencyFrames
// ---------------------------------------------------------------------------
uint32_t WasapiDevice::GetLatencyFrames() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<uint32_t>(m_ws.queue.size()) * m_format.sample_rate / 1000 * 50;
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------
AudioDevice* CreateWasapiDevice() {
    return new WasapiDevice();
}
