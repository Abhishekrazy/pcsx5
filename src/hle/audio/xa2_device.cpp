// XAudio2 2.9 audio backend — source voice fed from a recycled block pool.
// Output blocks when every pool block is queued; OnBufferEnd returns
// blocks to the free list, pacing the guest at real-time rate.

#include "audio_device.h"
#include "../../common/log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xaudio2.h>

#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

class Xa2Device : public AudioDevice {
public:
    Xa2Device() = default;
    ~Xa2Device() override { Close(); }

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
    struct Xa2Block {
        std::vector<s16> data;
        Xa2Device* device = nullptr;
    };

    static constexpr int kMaxBuffersInFlight = 8;

    // Stateless voice callback — routing through XAUDIO2_BUFFER::pContext.
    class VoiceCallback final : public IXAudio2VoiceCallback {
    public:
        void STDMETHODCALLTYPE OnBufferEnd(void* context) override {
            auto* block = static_cast<Xa2Block*>(context);
            if (!block || !block->device) return;
            Xa2Device* dev = block->device;
            {
                std::lock_guard<std::mutex> lock(dev->m_mutex);
                dev->m_free_blocks.push_back(block);
                --dev->m_in_flight;
            }
        }
        void STDMETHODCALLTYPE OnStreamEnd() override {}
        void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
        void STDMETHODCALLTYPE OnVoiceProcessingPassStart(
            UINT32 /*bytes*/) override {}
        void STDMETHODCALLTYPE OnBufferStart(void* /*context*/) override {}
        void STDMETHODCALLTYPE OnLoopEnd(void* /*context*/) override {}
        void STDMETHODCALLTYPE OnVoiceError(
            void* /*context*/, HRESULT /*error*/) override {}
    };

    bool m_open = false;
    float m_volume = 1.0f;
    AalFormat m_format{};

    IXAudio2*              m_engine    = nullptr;
    IXAudio2MasteringVoice* m_mastering = nullptr;
    IXAudio2SourceVoice*   m_voice     = nullptr;
    HMODULE                m_dll       = nullptr;
    VoiceCallback          m_callback;
    std::vector<Xa2Block*> m_pool;
    std::deque<Xa2Block*>  m_free_blocks;
    int                    m_in_flight = 0;
    mutable std::mutex     m_mutex;
};

// ---------------------------------------------------------------------------
// Open
// ---------------------------------------------------------------------------
bool Xa2Device::Open(const AalFormat& format) {
    if (m_open) Close();

    m_dll = ::LoadLibraryW(L"xaudio2_9.dll");
    if (!m_dll) {
        LOG_WARN(HLE, "Xa2Device: xaudio2_9.dll not found");
        return false;
    }

    using XAudio2CreateFn = HRESULT(STDAPICALLTYPE*)(IXAudio2**, UINT32,
                                                       XAUDIO2_PROCESSOR);
    const auto create = reinterpret_cast<XAudio2CreateFn>(
        reinterpret_cast<void*>(
            ::GetProcAddress(m_dll, "XAudio2Create")));
    if (!create ||
        FAILED(create(&m_engine, 0, XAUDIO2_DEFAULT_PROCESSOR))) {
        Close();
        return false;
    }

    if (FAILED(m_engine->CreateMasteringVoice(&m_mastering, 2,
                                              format.sample_rate))) {
        Close();
        return false;
    }

    WAVEFORMATEX wfx{};
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = 2;
    wfx.nSamplesPerSec  = format.sample_rate;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = static_cast<WORD>(2 * 2);
    wfx.nAvgBytesPerSec = format.sample_rate * wfx.nBlockAlign;

    if (FAILED(m_engine->CreateSourceVoice(&m_voice, &wfx, 0,
                                           XAUDIO2_DEFAULT_FREQ_RATIO,
                                           &m_callback))) {
        Close();
        return false;
    }

    // Allocate block pool.
    const size_t block_samples =
        static_cast<size_t>(format.sample_rate / 100) * 2;  // 10ms blocks
    m_pool.reserve(kMaxBuffersInFlight);
    for (int i = 0; i < kMaxBuffersInFlight; ++i) {
        auto* block = new Xa2Block();
        block->data.resize(block_samples);
        block->device = this;
        m_pool.push_back(block);
        m_free_blocks.push_back(block);
    }

    m_format = format;
    m_volume = 1.0f;
    m_voice->Start(0, XAUDIO2_COMMIT_NOW);
    m_open = true;
    return true;
}

// ---------------------------------------------------------------------------
// Close
// ---------------------------------------------------------------------------
void Xa2Device::Close() {
    if (m_voice) {
        m_voice->Stop(0, XAUDIO2_COMMIT_NOW);
        m_voice->DestroyVoice();
        m_voice = nullptr;
    }
    if (m_mastering) { m_mastering->DestroyVoice(); m_mastering = nullptr; }
    if (m_engine) { m_engine->Release(); m_engine = nullptr; }

    for (Xa2Block* block : m_pool) delete block;
    m_pool.clear();
    m_free_blocks.clear();
    m_in_flight = 0;

    if (m_dll) { ::FreeLibrary(m_dll); m_dll = nullptr; }

    m_open = false;
}

// ---------------------------------------------------------------------------
// GetCaps
// ---------------------------------------------------------------------------
AalCaps Xa2Device::GetCaps() const {
    AalCaps caps;
    caps.backend_name = "XAudio2";
    caps.max_channels = 2;
    caps.max_sample_rate = 48000;
    caps.min_buffer_frames = 64;
    caps.max_buffer_frames = 4096;
    return caps;
}

// ---------------------------------------------------------------------------
// Output — submit stereo PCM16 to the source voice
// ---------------------------------------------------------------------------
void Xa2Device::Output(const u8* data, uint32_t frame_count) {
    if (!m_open || !m_voice || frame_count == 0) return;

    Xa2Block* block = nullptr;
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        // Wait for a free block.
        while (m_free_blocks.empty()) {
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            lock.lock();
        }
        block = m_free_blocks.front();
        m_free_blocks.pop_front();
        ++m_in_flight;
    }

    // Copy and convert to stereo PCM16.
    const size_t samples = static_cast<size_t>(frame_count) * 2;
    if (block->data.size() < samples) {
        block->data.resize(samples);
    }
    std::memcpy(block->data.data(), data, samples * sizeof(s16));

    if (m_volume != 1.0f) {
        for (size_t i = 0; i < samples; ++i) {
            block->data[i] = static_cast<s16>(
                static_cast<float>(block->data[i]) * m_volume);
        }
    }

    block->device = this;

    XAUDIO2_BUFFER buf{};
    buf.AudioBytes = static_cast<UINT32>(samples * sizeof(s16));
    buf.pAudioData = reinterpret_cast<const BYTE*>(block->data.data());
    buf.pContext   = block;
    if (FAILED(m_voice->SubmitSourceBuffer(&buf))) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_free_blocks.push_back(block);
        --m_in_flight;
    }
}

// ---------------------------------------------------------------------------
// Reset
// ---------------------------------------------------------------------------
void Xa2Device::Reset() {
    if (m_voice) {
        m_voice->Stop(0, XAUDIO2_COMMIT_NOW);
        m_voice->Start(0, XAUDIO2_COMMIT_NOW);
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_free_blocks.clear();
    for (Xa2Block* block : m_pool) {
        m_free_blocks.push_back(block);
    }
    m_in_flight = 0;
}

// ---------------------------------------------------------------------------
// GetLatencyFrames
// ---------------------------------------------------------------------------
uint32_t Xa2Device::GetLatencyFrames() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<uint32_t>(m_in_flight) * m_format.sample_rate / 1000 * 50;
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------
AudioDevice* CreateXa2Device() {
    return new Xa2Device();
}
