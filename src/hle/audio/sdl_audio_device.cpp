// SDL audio backend — cross-platform audio via SDL2.
//
// Dynamically loads SDL2.dll at runtime using minimal type definitions
// (no SDL headers required).  Uses the SDL_QueueAudio push model so
// Output() never blocks.

#include "audio_device.h"
#include "../../common/log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

// Minimal SDL type definitions (we load the DLL dynamically so no header needed).
typedef unsigned char       SDL_Uint8;
typedef unsigned short      SDL_Uint16;
typedef unsigned int        SDL_Uint32;
typedef int                 SDL_bool;
typedef SDL_Uint32          SDL_AudioDeviceID;

#define SDL_AUDIO_ALLOW_FREQUENCY_CHANGE 0x0001
#define SDL_QUERY   -1
#define SDL_INIT_AUDIO 0x00000010u

// Audio format constants (copied from SDL_audio.h).
// AUDIO_S16SYS = signed 16-bit, native endian, system byte order.
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#  define AUDIO_S16SYS 0x8010u
#else
#  define AUDIO_S16SYS 0x9010u
#endif

struct SDL_AudioSpec {
    int     freq;
    SDL_Uint16 format;
    SDL_Uint8  channels;
    SDL_Uint8  silence;
    SDL_Uint16 samples;
    SDL_Uint16 padding;
    SDL_Uint32 size;
    void     (*callback)(void* userdata, SDL_Uint8* stream, int len);
    void*    userdata;
};

// Function pointer types for dynamically loaded SDL2 functions.
using SDL_InitFn             = int(*)(SDL_Uint32);
using SDL_QuitFn             = void(*)();
using SDL_OpenAudioDeviceFn  = SDL_AudioDeviceID(*)(const char*, int,
                                                      const SDL_AudioSpec*,
                                                      SDL_AudioSpec*, int);
using SDL_CloseAudioDeviceFn = void(*)(SDL_AudioDeviceID);
using SDL_PauseAudioDeviceFn = void(*)(SDL_AudioDeviceID, int);
using SDL_QueueAudioFn       = int(*)(SDL_AudioDeviceID, const void*, SDL_Uint32);
using SDL_GetQueuedAudioSizeFn = SDL_Uint32(*)(SDL_AudioDeviceID);
using SDL_ClearQueuedAudioFn = void(*)(SDL_AudioDeviceID);
using SDL_GetErrorFn         = const char*(*)();

class SdlAudioDevice : public AudioDevice {
public:
    SdlAudioDevice() = default;
    ~SdlAudioDevice() override { Close(); }

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
    bool LoadSdl();

    HMODULE m_dll = nullptr;
    SDL_OpenAudioDeviceFn  m_open_audio  = nullptr;
    SDL_CloseAudioDeviceFn m_close_audio = nullptr;
    SDL_PauseAudioDeviceFn m_pause_audio = nullptr;
    SDL_QueueAudioFn       m_queue_audio = nullptr;
    SDL_GetQueuedAudioSizeFn m_queued_size = nullptr;
    SDL_ClearQueuedAudioFn m_clear_queued = nullptr;
    SDL_GetErrorFn         m_get_error   = nullptr;

    SDL_AudioDeviceID m_device_id = 0;
    bool m_open = false;
    float m_volume = 1.0f;
    AalFormat m_format{};
    std::mutex m_mutex;
};

bool SdlAudioDevice::LoadSdl() {
    if (m_dll) return true;

    const char* dlls[] = {"SDL2.dll", "SDL.dll", nullptr};
    for (int i = 0; dlls[i]; ++i) {
        m_dll = ::LoadLibraryA(dlls[i]);
        if (!m_dll) continue;

        m_open_audio  = reinterpret_cast<SDL_OpenAudioDeviceFn>(
            ::GetProcAddress(m_dll, "SDL_OpenAudioDevice"));
        m_close_audio = reinterpret_cast<SDL_CloseAudioDeviceFn>(
            ::GetProcAddress(m_dll, "SDL_CloseAudioDevice"));
        m_pause_audio = reinterpret_cast<SDL_PauseAudioDeviceFn>(
            ::GetProcAddress(m_dll, "SDL_PauseAudioDevice"));
        m_queue_audio = reinterpret_cast<SDL_QueueAudioFn>(
            ::GetProcAddress(m_dll, "SDL_QueueAudio"));
        m_queued_size = reinterpret_cast<SDL_GetQueuedAudioSizeFn>(
            ::GetProcAddress(m_dll, "SDL_GetQueuedAudioSize"));
        m_clear_queued = reinterpret_cast<SDL_ClearQueuedAudioFn>(
            ::GetProcAddress(m_dll, "SDL_ClearQueuedAudio"));
        m_get_error   = reinterpret_cast<SDL_GetErrorFn>(
            ::GetProcAddress(m_dll, "SDL_GetError"));

        if (m_open_audio && m_close_audio && m_queue_audio) {
            LOG_INFO(HLE, "SDL audio: loaded %s", dlls[i]);
            return true;
        }
        ::FreeLibrary(m_dll);
        m_dll = nullptr;
    }
    return false;
}

bool SdlAudioDevice::Open(const AalFormat& format) {
    if (m_open) Close();
    if (!LoadSdl()) return false;

    SDL_AudioSpec spec{};
    spec.freq     = static_cast<int>(format.sample_rate);
    spec.format   = AUDIO_S16SYS;
    spec.channels = static_cast<SDL_Uint8>(format.channels > 2 ? 2 : format.channels);
    spec.samples  = 1024;
    spec.callback = nullptr;  // push mode (SDL_QueueAudio)

    m_device_id = m_open_audio(nullptr, 0, &spec, nullptr,
                               SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (m_device_id == 0) {
        LOG_WARN(HLE, "SDL audio: SDL_OpenAudioDevice failed: %s",
                 m_get_error ? m_get_error() : "unknown");
        return false;
    }

    m_pause_audio(m_device_id, 0);
    m_format = format;
    m_volume = 1.0f;
    m_open = true;
    return true;
}

void SdlAudioDevice::Close() {
    if (!m_open || m_device_id == 0) return;
    m_pause_audio(m_device_id, 1);
    m_close_audio(m_device_id);
    m_device_id = 0;
    m_open = false;
}

AalCaps SdlAudioDevice::GetCaps() const {
    AalCaps caps;
    caps.backend_name = "SDL Audio";
    caps.max_channels = 2;
    caps.max_sample_rate = 48000;
    caps.min_buffer_frames = 256;
    caps.max_buffer_frames = 8192;
    return caps;
}

void SdlAudioDevice::Output(const u8* data, uint32_t frame_count) {
    if (!m_open || m_device_id == 0 || frame_count == 0) return;

    const size_t byte_count = static_cast<size_t>(frame_count) * 2 * 2; // stereo s16
    if (m_queue_audio(m_device_id, data, static_cast<SDL_Uint32>(byte_count)) < 0) {
        LOG_WARN(HLE, "SDL audio: SDL_QueueAudio failed");
    }
}

void SdlAudioDevice::Reset() {
    if (m_device_id) m_clear_queued(m_device_id);
}

uint32_t SdlAudioDevice::GetLatencyFrames() const {
    if (!m_device_id) return 0;
    return m_queued_size(m_device_id) / (2 * 2); // stereo s16: 4 bytes/frame
}

// Factory
AudioDevice* CreateSdlAudioDevice() {
    return new SdlAudioDevice();
}
