// libSceAudioOut HLE module.
//
// Per-port audio output: sceAudioOutInit/Open/Close/Output/SetVolume/
// GetPortState/GetInfo.  The host backend is winmm waveOut (48 kHz s16
// stereo), mirroring SharpEmu's AudioOutExports.cs + WindowsWaveOutAudio.
//
// When AudioConfig.backend == 0 (Off) no wave device is opened; Output
// keeps real-time pacing (PaceSilence) so games that time their audio
// thread off submission rate still run at the correct speed.

#include "hle.h"
#include "../common/log.h"
#include "../memory/memory.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

namespace HLE {
namespace {

// Orbis-style error codes (gen2 generic invalid argument, matching
// SharpEmu's OrbisGen2Result.ORBIS_GEN2_ERROR_* usage).
constexpr u64 kErrorInvalidArgument = 0x8002000D; // SCE_KERNEL_ERROR_EINVAL
constexpr u64 kErrorMemoryFault     = 0x8002000E; // SCE_KERNEL_ERROR_EFAULT

constexpr int kMaxPorts       = 8;
constexpr int kMaxBuffersInFlight = 8; // ~40 ms of queue at 256f/48k, paces the guest

struct AudioOutPort;

struct OutBuffer {
    WAVEHDR hdr{};
    std::vector<u8> data;
    AudioOutPort* port = nullptr;
    bool prepared = false;
};

struct AudioOutPort {
    int      handle         = 0;
    int      user_id        = 0;
    int      type           = 0;
    u32      buffer_length  = 0;  // frames per output() call
    u32      frequency      = 0;  // Hz
    int      format         = 0;
    int      channels       = 0;
    int      bytes_per_sample = 0; // 2 = s16, 4 = float32
    bool     is_float       = false;
    float    volume         = 1.0f;

    // Host backend (null when audio is configured Off or waveOutOpen failed).
    HWAVEOUT wave_out = nullptr;
    std::mutex mu;
    std::condition_variable cv;
    std::deque<OutBuffer*> free_buffers;
    int in_flight = 0;

    // Silent-path pacing: next wall-clock instant an output() may complete.
    std::chrono::steady_clock::time_point next_silent_output{};
};

std::mutex g_ports_mutex;
AudioOutPort g_ports[kMaxPorts];
bool g_ports_used[kMaxPorts] = {};
std::atomic<u64> g_output_count{0};

// ---------------------------------------------------------------------------
// Config: main() pushes AudioConfig via HLE::SetAudioOutConfig after loading
// the config service (test targets link this file without config.cpp, so the
// module never references ConfigService directly).  Defaults: backend off.
// ---------------------------------------------------------------------------
std::atomic<int>   g_configured_backend{0};
std::atomic<float> g_configured_volume{1.0f};

int GetConfiguredBackend() {
    return g_configured_backend.load(std::memory_order_relaxed);
}

float GetConfiguredVolume() {
    return g_configured_volume.load(std::memory_order_relaxed);
}

// SharpEmu TryGetFormat: low byte selects layout.
bool TryGetFormat(int raw_format, int& channels, int& bytes_per_sample, bool& is_float) {
    const int format = raw_format & 0xFF;
    switch (format) {
        case 0: case 3: channels = 1; break;
        case 1: case 4: channels = 2; break;
        case 2: case 5: case 6: case 7: channels = 8; break;
        default: channels = 0; break;
    }
    bytes_per_sample = ((format >= 3 && format <= 5) || format == 7) ? 4 : 2;
    is_float = bytes_per_sample == 4;
    return channels != 0;
}

// ---------------------------------------------------------------------------
// waveOut streaming backend
// ---------------------------------------------------------------------------
void CALLBACK WaveOutProc(HWAVEOUT /*hwo*/, UINT msg, DWORD_PTR /*instance*/,
                          DWORD_PTR param1, DWORD_PTR /*param2*/) {
    if (msg != WOM_DONE) return;
    auto* hdr = reinterpret_cast<WAVEHDR*>(param1);
    auto* buf = reinterpret_cast<OutBuffer*>(hdr->dwUser);
    if (!buf || !buf->port) return;
    AudioOutPort* port = buf->port;
    {
        std::lock_guard<std::mutex> lock(port->mu);
        port->free_buffers.push_back(buf);
        --port->in_flight;
    }
    port->cv.notify_all();
}

// Open a stereo PCM16 stream at `frequency`.  Returns true on success.
bool OpenWaveBackend(AudioOutPort& port, u32 frequency) {
    WAVEFORMATEX wfx{};
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = 2;
    wfx.nSamplesPerSec  = frequency;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = static_cast<WORD>(2 * 2);
    wfx.nAvgBytesPerSec = frequency * wfx.nBlockAlign;
    wfx.cbSize          = 0;

    HWAVEOUT hwo = nullptr;
    MMRESULT res = ::waveOutOpen(&hwo, WAVE_MAPPER, &wfx,
                                 reinterpret_cast<DWORD_PTR>(WaveOutProc),
                                 0, CALLBACK_FUNCTION);
    if (res != MMSYSERR_NOERROR) {
        LOG_WARN(HLE, "sceAudioOut: waveOutOpen failed (err=%u); port will run silent-paced", res);
        return false;
    }
    port.wave_out = hwo;
    return true;
}

void CloseWaveBackend(AudioOutPort& port) {
    if (!port.wave_out) return;
    HWAVEOUT hwo = port.wave_out;
    port.wave_out = nullptr;
    // waveOutReset returns every in-flight buffer via WOM_DONE on the
    // callback thread; give it a moment to drain into free_buffers.
    ::waveOutReset(hwo);
    for (int spin = 0; spin < 100; ++spin) {
        {
            std::lock_guard<std::mutex> lock(port.mu);
            if (port.in_flight == 0) break;
        }
        ::Sleep(1);
    }
    std::deque<OutBuffer*> all;
    {
        std::lock_guard<std::mutex> lock(port.mu);
        all = port.free_buffers;
        port.free_buffers.clear();
        port.in_flight = 0;
    }
    for (OutBuffer* buf : all) {
        if (buf->prepared) {
            ::waveOutUnprepareHeader(hwo, &buf->hdr, sizeof(WAVEHDR));
        }
        delete buf;
    }
    ::waveOutClose(hwo);
}

// Convert the guest buffer (mono/stereo/8ch, s16 or float32) to stereo
// PCM16 with volume scaling, then submit it to waveOut.  Blocks (paced by
// WOM_DONE) when too many buffers are queued.
void SubmitToBackend(AudioOutPort& port, const u8* src) {
    const u32 frames = port.buffer_length;
    const size_t out_bytes = static_cast<size_t>(frames) * 2 * sizeof(s16);

    OutBuffer* buf = nullptr;
    {
        std::unique_lock<std::mutex> lock(port.mu);
        port.cv.wait(lock, [&] {
            return !port.free_buffers.empty() || port.in_flight < kMaxBuffersInFlight;
        });
        if (!port.free_buffers.empty()) {
            buf = port.free_buffers.front();
            port.free_buffers.pop_front();
        } else {
            buf = new OutBuffer();
            buf->data.resize(out_bytes);
            buf->port = &port;
        }
        ++port.in_flight;
    }

    const float vol = port.volume * GetConfiguredVolume();
    s16* dst = reinterpret_cast<s16*>(buf->data.data());
    const int in_ch = port.channels;
    for (u32 f = 0; f < frames; ++f) {
        float l = 0.0f, r = 0.0f;
        const u8* frame = src + static_cast<size_t>(f) * in_ch * port.bytes_per_sample;
        if (port.is_float) {
            const float* s = reinterpret_cast<const float*>(frame);
            if (in_ch == 1) { l = r = s[0]; }
            else            { l = s[0]; r = s[1]; }
        } else {
            const s16* s = reinterpret_cast<const s16*>(frame);
            if (in_ch == 1) { l = r = s[0] / 32768.0f; }
            else            { l = s[0] / 32768.0f; r = s[1] / 32768.0f; }
        }
        l *= vol; r *= vol;
        if (l > 1.0f) l = 1.0f; else if (l < -1.0f) l = -1.0f;
        if (r > 1.0f) r = 1.0f; else if (r < -1.0f) r = -1.0f;
        dst[f * 2 + 0] = static_cast<s16>(l * 32767.0f);
        dst[f * 2 + 1] = static_cast<s16>(r * 32767.0f);
    }

    if (!buf->prepared) {
        std::memset(&buf->hdr, 0, sizeof(buf->hdr));
        buf->hdr.lpData         = reinterpret_cast<LPSTR>(buf->data.data());
        buf->hdr.dwBufferLength = static_cast<DWORD>(out_bytes);
        buf->hdr.dwUser         = reinterpret_cast<DWORD_PTR>(buf);
        if (::waveOutPrepareHeader(port.wave_out, &buf->hdr, sizeof(buf->hdr)) != MMSYSERR_NOERROR) {
            std::lock_guard<std::mutex> lock(port.mu);
            delete buf;
            --port.in_flight;
            return;
        }
        buf->prepared = true;
    }
    if (::waveOutWrite(port.wave_out, &buf->hdr, sizeof(buf->hdr)) != MMSYSERR_NOERROR) {
        std::lock_guard<std::mutex> lock(port.mu);
        port.free_buffers.push_back(buf);
        --port.in_flight;
    }
}

// No host device: sleep so the *next* output() completes at real-time rate.
void PaceSilence(AudioOutPort& port) {
    using clock = std::chrono::steady_clock;
    const auto now = clock::now();
    if (port.next_silent_output < now) {
        port.next_silent_output = now;
    }
    const auto delay = port.next_silent_output - now;
    port.next_silent_output += std::chrono::duration_cast<clock::duration>(
        std::chrono::duration<double>(
            static_cast<double>(port.buffer_length) / port.frequency));
    if (delay > std::chrono::steady_clock::duration::zero()) {
        std::this_thread::sleep_for(delay);
    }
}

AudioOutPort* FindPort(int handle) {
    if (handle < 1 || handle > kMaxPorts || !g_ports_used[handle - 1]) {
        return nullptr;
    }
    return &g_ports[handle - 1];
}

} // namespace

void SetAudioOutConfig(int backend, float volume) {
    g_configured_backend.store(backend, std::memory_order_relaxed);
    g_configured_volume.store(volume, std::memory_order_relaxed);
}

void RegisterLibAudioOut() {
    LOG_INFO(HLE, "Registering libSceAudioOut HLE symbols...");

    auto AudioOutInit = [](const GuestArgs& args) -> u64 {
        (void)args;
        LOG_INFO(HLE, "sceAudioOutInit() -> 0");
        return 0;
    };
    RegisterSymbol("libSceAudioOut", "sceAudioOutInit", AudioOutInit);
    RegisterSymbol("libSceAudioOut", "JfEPXVxhFqA", AudioOutInit);

    auto AudioOutOpen = [](const GuestArgs& args) -> u64 {
        const s32 user_id       = static_cast<s32>(args.arg1);
        const s32 type          = static_cast<s32>(args.arg2);
        const u32 buffer_length = static_cast<u32>(args.arg4); // rcx
        const u32 frequency     = static_cast<u32>(args.arg5); // r8
        const s32 format        = static_cast<s32>(args.arg6); // r9

        int channels = 0, bytes_per_sample = 0;
        bool is_float = false;
        if (buffer_length == 0 || frequency == 0 ||
            !TryGetFormat(format, channels, bytes_per_sample, is_float)) {
            LOG_WARN(HLE, "sceAudioOutOpen: invalid args (len=%u freq=%u fmt=%d)",
                     buffer_length, frequency, format);
            return kErrorInvalidArgument;
        }

        std::lock_guard<std::mutex> lock(g_ports_mutex);
        int slot = -1;
        for (int i = 0; i < kMaxPorts; ++i) {
            if (!g_ports_used[i]) { slot = i; break; }
        }
        if (slot < 0) {
            LOG_WARN(HLE, "sceAudioOutOpen: no free ports");
            return 0x802E0009; // SCE_AUDIO_OUT_ERROR_PORT_FULL
        }

        AudioOutPort& port = g_ports[slot];
        // Cannot assign a fresh AudioOutPort (std::mutex member); reset
        // fields individually.  The port was fully closed on Close, so
        // backend fields are already clean.
        port.handle          = slot + 1;
        port.user_id         = user_id;
        port.type            = type;
        port.buffer_length   = buffer_length;
        port.frequency       = frequency;
        port.format          = format;
        port.channels        = channels;
        port.bytes_per_sample = bytes_per_sample;
        port.is_float        = is_float;
        port.volume          = 1.0f;
        g_ports_used[slot]   = true;

        const char* backend_name = "silent";
        if (GetConfiguredBackend() != 0) {
            if (OpenWaveBackend(port, frequency)) {
                backend_name = "waveOut";
            }
        }
        LOG_INFO(HLE, "sceAudioOutOpen: port %d: %u Hz, %d ch, %s, %u frames, backend=%s",
                 port.handle, frequency, channels, is_float ? "float32" : "s16",
                 buffer_length, backend_name);
        return static_cast<u64>(port.handle);
    };
    RegisterSymbol("libSceAudioOut", "sceAudioOutOpen", AudioOutOpen);
    RegisterSymbol("libSceAudioOut", "ekNvsT22rsY", AudioOutOpen);

    auto AudioOutClose = [](const GuestArgs& args) -> u64 {
        const s32 handle = static_cast<s32>(args.arg1);
        std::lock_guard<std::mutex> lock(g_ports_mutex);
        AudioOutPort* port = FindPort(handle);
        if (!port) return kErrorInvalidArgument;
        CloseWaveBackend(*port);
        g_ports_used[handle - 1] = false;
        LOG_INFO(HLE, "sceAudioOutClose(%d) -> 0", handle);
        return 0;
    };
    RegisterSymbol("libSceAudioOut", "sceAudioOutClose", AudioOutClose);
    RegisterSymbol("libSceAudioOut", "s1--uE9mBFw", AudioOutClose);

    auto AudioOutOutput = [](const GuestArgs& args) -> u64 {
        const s32 handle = static_cast<s32>(args.arg1);
        const guest_addr_t src = args.arg2;

        AudioOutPort* port = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_ports_mutex);
            port = FindPort(handle);
        }
        if (!port) return kErrorInvalidArgument;
        if (!src) {
            // NULL source = one buffer period of silence; keep pacing only.
            if (!port->wave_out) PaceSilence(*port);
            return 0;
        }

        const size_t byte_len = static_cast<size_t>(port->buffer_length) *
                                port->channels * port->bytes_per_sample;
        std::vector<u8> guest_buf(byte_len);
        Memory::ReadBuffer(src, guest_buf.data(), byte_len);

        const u64 n = g_output_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 4 || (n % 500) == 0) {
            LOG_INFO(HLE, "sceAudioOutOutput #%llu: handle=%d bytes=%zu",
                     static_cast<unsigned long long>(n), handle, byte_len);
        }

        if (!port->wave_out) {
            PaceSilence(*port);
            return 0;
        }
        SubmitToBackend(*port, guest_buf.data());
        return 0;
    };
    RegisterSymbol("libSceAudioOut", "sceAudioOutOutput", AudioOutOutput);
    RegisterSymbol("libSceAudioOut", "QOQtbeDqsT4", AudioOutOutput);

    auto AudioOutSetVolume = [](const GuestArgs& args) -> u64 {
        const s32 handle = static_cast<s32>(args.arg1);
        const u32 channel_flags = static_cast<u32>(args.arg2);
        const guest_addr_t volumes_addr = args.arg3;

        std::lock_guard<std::mutex> lock(g_ports_mutex);
        AudioOutPort* port = FindPort(handle);
        if (!port) return kErrorInvalidArgument;

        constexpr int kUnityVolume = 32768;
        if (volumes_addr) {
            int max_volume = 0;
            bool found = false;
            for (int ch = 0; ch < 8; ++ch) {
                if ((channel_flags & (1u << ch)) == 0) continue;
                s32 value = 0;
                Memory::ReadBuffer(volumes_addr + ch * sizeof(s32), &value, sizeof(value));
                if (value > max_volume) max_volume = value;
                found = true;
            }
            if (found) {
                float v = static_cast<float>(max_volume) / kUnityVolume;
                if (v < 0.0f) v = 0.0f; else if (v > 1.0f) v = 1.0f;
                port->volume = v;
            }
        }
        return 0;
    };
    RegisterSymbol("libSceAudioOut", "sceAudioOutSetVolume", AudioOutSetVolume);
    RegisterSymbol("libSceAudioOut", "b+uAV89IlxE", AudioOutSetVolume);

    // sceAudioOutGetPortState(handle, SceAudioOutPortState* out) — report a
    // connected output at full volume so pacing/mixing code sees a live port.
    auto AudioOutGetPortState = [](const GuestArgs& args) -> u64 {
        const s32 handle = static_cast<s32>(args.arg1);
        const guest_addr_t state_addr = args.arg2;
        std::lock_guard<std::mutex> lock(g_ports_mutex);
        AudioOutPort* port = FindPort(handle);
        if (!state_addr || !port) return kErrorInvalidArgument;

        u8 state[16] = {};
        state[0] = 1;                       // output = connected (u16 LE)
        state[2] = static_cast<u8>(port->channels); // channel count (u16 LE)
        state[7] = 127;                     // volume
        Memory::WriteBuffer(state_addr, state, sizeof(state));
        return 0;
    };
    RegisterSymbol("libSceAudioOut", "sceAudioOutGetPortState", AudioOutGetPortState);
    RegisterSymbol("libSceAudioOut", "GrQ9s4IrNaQ", AudioOutGetPortState);

    // sceAudioOutGetInfo(handle, SceAudioOutPortInfo* out) — minimal info:
    // zeroed struct is accepted by titles that only probe for errors.
    auto AudioOutGetInfo = [](const GuestArgs& args) -> u64 {
        const s32 handle = static_cast<s32>(args.arg1);
        const guest_addr_t info_addr = args.arg2;
        std::lock_guard<std::mutex> lock(g_ports_mutex);
        AudioOutPort* port = FindPort(handle);
        if (!info_addr || !port) return kErrorInvalidArgument;
        u8 info[64] = {};
        Memory::WriteBuffer(info_addr, info, sizeof(info));
        return 0;
    };
    RegisterSymbol("libSceAudioOut", "sceAudioOutGetInfo", AudioOutGetInfo);

    // GameMaker titles (Dreaming Sarah) import the audio entry points from
    // libkernel instead of libSceAudioOut (same NIDs, #N#O suffix).  Route
    // those aliases to the real handlers — the paced Output matters: a
    // return-0 stub lets the guest audio thread spin unthrottled.
    RegisterSymbol("libkernel", "ekNvsT22rsY#N#O", AudioOutOpen);
    RegisterSymbol("libkernel", "QOQtbeDqsT4#N#O", AudioOutOutput);
    RegisterSymbol("libkernel", "b+uAV89IlxE#N#O", AudioOutSetVolume);
    RegisterSymbol("libkernel", "s1--uE9mBFw#N#O", AudioOutClose);
}

} // namespace HLE
