// SndPreviewPlayer implementation.  See snd_player.h for the public
// contract.  The two non-trivial pieces are:
//
//   1. ATRAC9 -> PCM16 WAV decoding via vendored LibAtrac9 (RPCSX fork).
//   2. winmm `PlaySound` integration with a pinned in-memory WAV so
//      playback survives the temp byte buffer going out of scope.
//
// Everything else is the SharpEmu pattern: generation counter +
// 300 ms debounce + path cache + lock to serialise transitions.

#include "ui/snd_player.h"

#include "common/log.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#endif

extern "C" {
#include "libatrac9/libatrac9.h"
}

// stb_vorbis is a single-header OGG Vorbis decoder.  We compile
// the implementation into this translation unit only (single TU
// pattern, same as stb_image is used in the thumbnail cache).
#define STB_VORBIS_NO_STDIO

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4100) // unreferenced formal parameter
#pragma warning(disable : 4244) // conversion from 'type1' to 'type2', possible loss of data
#pragma warning(disable : 4245) // conversion from 'int' to 'uint32', signed/unsigned mismatch
#pragma warning(disable : 4456) // declaration of 'identifier' hides previous local declaration
#pragma warning(disable : 4457) // declaration of 'identifier' hides function parameter
#pragma warning(disable : 4701) // potentially uninitialized local variable
#pragma warning(disable : 4703) // potentially uninitialized local pointer variable
#endif

#include "stb_vorbis.h"

#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace Ui {

namespace {

// Tiny little/big-endian helpers for WAV header bytes.
void WriteU32LE(std::vector<std::uint8_t>& v, std::size_t off, std::uint32_t x) {
    v[off + 0] = static_cast<std::uint8_t>(x);
    v[off + 1] = static_cast<std::uint8_t>(x >> 8);
    v[off + 2] = static_cast<std::uint8_t>(x >> 16);
    v[off + 3] = static_cast<std::uint8_t>(x >> 24);
}
void WriteU16LE(std::vector<std::uint8_t>& v, std::size_t off, std::uint16_t x) {
    v[off + 0] = static_cast<std::uint8_t>(x);
    v[off + 1] = static_cast<std::uint8_t>(x >> 8);
}

constexpr int kDebounceMs = 300;

// Sony RIFF-WAV-wrapped ATRAC9 stream layout (see SharpEmu's
// SndPreviewPlayer.DecodeAt9ToWav for the equivalent C# walk).
//   RIFF<size>WAVE
//     fmt <size>            -- extensible WAVE_FORMAT_EXTENSIBLE
//         wFormatTag = 0xFFFE
//         SubFormat GUID  = {47E142D2-36BA-4D8D-88FC-61654F8C836C}
//         extra 4 bytes     = ATRAC9 config blob
//     fact <size>
//         dwSampleLength    = total decoded PCM frames (per channel)
//         dwInputOverlapDelay / dwEncoderDelay (each 4 bytes)
//     data <size>           -- ATRAC9 superframes
constexpr std::uint8_t kAtrac9SubFormat[16] = {
    0xD2, 0x42, 0xE1, 0x47, 0xBA, 0x36, 0x8D, 0x4D,
    0x88, 0xFC, 0x61, 0x65, 0x4F, 0x8C, 0x83, 0x6C,
};

bool FindChunk(const std::vector<std::uint8_t>& file,
               const char (&id)[5],
               std::size_t& out_off,
               std::uint32_t& out_size) {
    // Walk RIFF children: each chunk is 4-byte id + 4-byte LE size
    // (+ 1 byte pad if size is odd).  We stop at end-of-file.
    out_off = 0;
    out_size = 0;
    std::size_t p = 12;  // skip "RIFF<size>WAVE"
    while (p + 8 <= file.size()) {
        const char* cid = reinterpret_cast<const char*>(&file[p]);
        std::uint32_t sz = static_cast<std::uint32_t>(file[p + 4]) |
                           (static_cast<std::uint32_t>(file[p + 5]) << 8) |
                           (static_cast<std::uint32_t>(file[p + 6]) << 16) |
                           (static_cast<std::uint32_t>(file[p + 7]) << 24);
        if (std::memcmp(cid, id, 4) == 0) {
            out_off = p + 8;
            out_size = sz;
            return true;
        }
        // 4 id + 4 size + payload + (size & 1) pad
        std::size_t advance = 8 + static_cast<std::size_t>(sz) + (sz & 1U);
        if (advance == 0) break;
        p += advance;
    }
    return false;
}

}  // namespace

// File-local helper.  Lives in the enclosing `namespace Ui` (so it's
// accessible from the SndPreviewPlayer member functions below).
static bool EndsWithCi(const std::string& s, const char* suffix) {
    if (!suffix) return false;
    const size_t sl = std::strlen(suffix);
    if (s.size() < sl) return false;
    return _stricmp(s.c_str() + s.size() - sl, suffix) == 0;
}

#ifdef _WIN32
// ---------------------------------------------------------------------------
// waveOut path (raw PCM, no temp file needed).  Used for decoded .at9
// and .wav previews.  waveOut streams the buffer directly to the
// audio device; the WOM_DONE callback re-submits the same buffer
// to implement seamless looping — no polling, no size cap.
// ---------------------------------------------------------------------------

// Read a little-endian uint32 from a byte buffer.
static std::uint32_t ReadU32LE(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

// Locate the `data` chunk inside a RIFF/WAVE buffer.  Returns
// (offset, size) of the PCM payload, or (0, 0) if not found.  We
// walk past fmt/fact/etc. so the format is opaque to us.
static bool FindWavDataChunk(const std::vector<std::uint8_t>& wav,
                             std::size_t& data_off,
                             std::uint32_t& data_size) {
    data_off = 0;
    data_size = 0;
    if (wav.size() < 12 ||
        std::memcmp(wav.data(), "RIFF", 4) != 0 ||
        std::memcmp(wav.data() + 8, "WAVE", 4) != 0) {
        return false;
    }
    std::size_t p = 12;
    while (p + 8 <= wav.size()) {
        const char* id = reinterpret_cast<const char*>(&wav[p]);
        std::uint32_t sz = ReadU32LE(&wav[p + 4]);
        if (std::memcmp(id, "data", 4) == 0) {
            data_off   = p + 8;
            data_size  = sz;
            return true;
        }
        std::size_t advance = 8 + static_cast<std::size_t>(sz) + (sz & 1U);
        if (advance == 0) break;
        p += advance;
    }
    return false;
}

void SndPreviewPlayer::StartWaveOut(const std::vector<std::uint8_t>& wav) {
    StopWaveOut();
    if (wav.size() < 44) {
        LOG_WARN(General, "snd_player: WAV too small for header");
        return;
    }
    if (wav.size() >= 4 && std::memcmp(wav.data(), "RIFF", 4) != 0) {
        LOG_WARN(General, "snd_player: not a RIFF/WAVE buffer");
        return;
    }
    // Read PCM format.  Standard offsets (matches our decode output).
    const std::uint16_t channels      = static_cast<std::uint16_t>(
        wav[22] | (wav[23] << 8));
    const std::uint32_t sampleRate    = ReadU32LE(&wav[24]);
    const std::uint16_t bitsPerSample = static_cast<std::uint16_t>(
        wav[34] | (wav[35] << 8));
    if (channels == 0 || sampleRate == 0 || bitsPerSample != 16) {
        LOG_WARN(General,
                 "snd_player: unsupported PCM format (ch=%u sr=%u bps=%u)",
                 channels, sampleRate, bitsPerSample);
        return;
    }
    std::size_t   data_off = 0;
    std::uint32_t data_sz  = 0;
    if (!FindWavDataChunk(wav, data_off, data_sz) ||
        data_off + data_sz > wav.size()) {
        LOG_WARN(General, "snd_player: WAV has no 'data' chunk");
        return;
    }
    WAVEFORMATEX wfx{};
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = channels;
    wfx.nSamplesPerSec  = sampleRate;
    wfx.nAvgBytesPerSec = sampleRate * channels * 2;
    wfx.nBlockAlign     = static_cast<std::uint16_t>(channels * 2);
    wfx.wBitsPerSample  = 16;
    wfx.cbSize          = 0;

    MMRESULT res = ::waveOutOpen(&wave_out_, WAVE_MAPPER, &wfx,
                                 reinterpret_cast<DWORD_PTR>(WaveOutProc),
                                 reinterpret_cast<DWORD_PTR>(this),
                                 CALLBACK_FUNCTION);
    if (res != MMSYSERR_NOERROR) {
        LOG_WARN(General, "snd_player: waveOutOpen failed (err=%u)", res);
        wave_out_ = nullptr;
        return;
    }

    // Take a stable copy of the PCM bytes so the device reads from
    // memory we own for the lifetime of playback.
    wave_data_.assign(wav.begin() + data_off,
                      wav.begin() + data_off + data_sz);
    std::memset(&wave_hdr_, 0, sizeof(wave_hdr_));
    wave_hdr_.lpData         = reinterpret_cast<LPSTR>(wave_data_.data());
    wave_hdr_.dwBufferLength = data_sz;
    wave_hdr_.dwFlags        = 0;
    wave_hdr_.dwLoops        = 0;

    res = ::waveOutPrepareHeader(wave_out_, &wave_hdr_, sizeof(wave_hdr_));
    if (res != MMSYSERR_NOERROR) {
        LOG_WARN(General, "snd_player: waveOutPrepareHeader failed (err=%u)", res);
        ::waveOutClose(wave_out_);
        wave_out_ = nullptr;
        return;
    }
    wave_active_.store(true, std::memory_order_release);
    res = ::waveOutWrite(wave_out_, &wave_hdr_, sizeof(wave_hdr_));
    if (res != MMSYSERR_NOERROR) {
        LOG_WARN(General, "snd_player: waveOutWrite failed (err=%u)", res);
        wave_active_.store(false, std::memory_order_release);
        ::waveOutUnprepareHeader(wave_out_, &wave_hdr_, sizeof(wave_hdr_));
        ::waveOutClose(wave_out_);
        wave_out_ = nullptr;
        return;
    }
    playing_ = true;
    paused_  = false;
    LOG_INFO(General,
             "snd_player: waveOut started (%u bytes PCM @ %u Hz x %u ch)",
             data_sz, sampleRate, channels);
}

void SndPreviewPlayer::StopWaveOut() {
    if (!wave_out_) return;
    wave_active_.store(false, std::memory_order_release);
    // waveOutReset returns all in-progress buffers to us and stops
    // playback.  After this call the WOM_DONE callback will not
    // re-submit because wave_active_ is false.
    ::waveOutReset(wave_out_);
    ::waveOutUnprepareHeader(wave_out_, &wave_hdr_, sizeof(wave_hdr_));
    ::waveOutClose(wave_out_);
    wave_out_      = nullptr;
    wave_data_.clear();
    playing_ = false;
    paused_  = false;
}

void SndPreviewPlayer::PauseWaveOut() {
    if (!wave_out_ || !playing_ || paused_) return;
    ::waveOutPause(wave_out_);
    paused_  = true;
    playing_ = false;
}

void SndPreviewPlayer::ResumeWaveOut() {
    if (!wave_out_ || !paused_) return;
    ::waveOutRestart(wave_out_);
    paused_  = false;
    playing_ = true;
}

void CALLBACK SndPreviewPlayer::WaveOutProc(HWAVEOUT hwo, UINT uMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2) {
    (void)dwParam1;
    (void)dwParam2;
    SndPreviewPlayer* self = reinterpret_cast<SndPreviewPlayer*>(dwInstance);
    if (uMsg != WOM_DONE) { return; }
    if (!self->wave_active_.load(std::memory_order_acquire)) { return; }
    if (hwo != self->wave_out_ || !self->wave_out_) { return; }
    ::waveOutWrite(self->wave_out_, &self->wave_hdr_, sizeof(self->wave_hdr_));
}

// MCI playback helpers.  We use a single shared alias
// "pcsx5snd" — the player is single-streamed by design (preview
// music is per-title, not a playlist).
constexpr const wchar_t* kMciAlias = L"pcsx5snd";

// Send an MCI command string.  Logs on failure so the loop bug is
// visible in the console even if no audio is heard.
static bool MciSend(const std::wstring& cmd) {
    const MCIERROR e = ::mciSendStringW(
        cmd.c_str(), nullptr, 0, nullptr);
    if (e != 0) {
        wchar_t errbuf[128] = {0};
        ::mciGetErrorStringW(e, errbuf, 128);
        char mb[256] = {0};
        std::snprintf(mb, sizeof(mb),
                      "snd_player: mciSendStringW('%ls') failed: %ls",
                      cmd.c_str(), errbuf);
        LOG_WARN(General, "%s", mb);
        return false;
    }
    return true;
}

static void MciClose() {
    // "close pcsx5snd" returns an error if it wasn't open; ignore.
    ::mciSendStringW(L"close pcsx5snd", nullptr, 0, nullptr);
}

// Open `path` under the shared alias with the given device type
// and start playing.  Looping for non-`waveaudio` devices is done
// by `LoopThread` (which polls mode and re-issues play on natural
// end).  Returns true on success.
static bool MciOpenAndPlayTyped(const std::string& path,
                                const wchar_t* device_type) {
    if (path.empty()) return false;
    MciClose();
    const std::wstring wpath(path.begin(), path.end());
    const std::wstring open_cmd =
        std::wstring(L"open \"") + wpath + L"\" type " +
        std::wstring(device_type) + L" alias " + std::wstring(kMciAlias);
    if (!MciSend(open_cmd)) return false;
    if (!MciSend(std::wstring(L"play ") + kMciAlias)) {
        MciClose();
        return false;
    }
    return true;
}

// Backwards-compat shim — kept so the MP3 path can ask for
// mpegvideo by name.
static bool MciOpenAndPlayMpeg(const std::string& path) {
    return MciOpenAndPlayTyped(path, L"mpegvideo");
}
#endif  // _WIN32

// ---------------------------------------------------------------------------
// Static decode helper
// ---------------------------------------------------------------------------
std::vector<std::uint8_t> SndPreviewPlayer::DecodeAt9ToWav(
    const std::string& path) {
    std::vector<std::uint8_t> wav;

    // Read entire file.
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        LOG_WARN(General, "snd_player: cannot open '%s'", path.c_str());
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string buf = ss.str();
    const std::vector<std::uint8_t> file(buf.begin(), buf.end());

    if (file.size() < 12 ||
        std::memcmp(&file[0], "RIFF", 4) != 0 ||
        std::memcmp(&file[8], "WAVE", 4) != 0) {
        LOG_WARN(General, "snd_player: '%s' is not a RIFF/WAVE file", path.c_str());
        return {};
    }

    // Walk fmt / fact / data chunks.
    std::size_t fmt_off = 0, fact_off = 0, data_off = 0;
    std::uint32_t fmt_size = 0, fact_size = 0, data_size = 0;
    if (!FindChunk(file, "fmt ", fmt_off, fmt_size) ||
        !FindChunk(file, "fact", fact_off, fact_size) ||
        !FindChunk(file, "data", data_off, data_size)) {
        LOG_WARN(General, "snd_player: '%s' missing fmt/fact/data chunk", path.c_str());
        return {};
    }
    if (fmt_size < 40 ||
        std::memcmp(&file[fmt_off + 24], kAtrac9SubFormat, 16) != 0) {
        LOG_WARN(General, "snd_player: '%s' is not an ATRAC9 stream", path.c_str());
        return {};
    }
    // ATRAC9 config: 4 bytes at fmt_off + 40 (after format tag 2,
    // channel mask 4, GUID 16, version 4 = 26 bytes into fmt payload,
    // wait the math is fmt_off is the START of payload; the C# reader
    // skips 24 bytes then reads 16-byte SubFormat, then 4 bytes
    // (version) then 4 bytes (config).  Total offset from payload
    // start: 24 + 16 + 4 = 44.  We split that into 24 (= SubFormat
    // start) and 44 (= configData start).  Verify:
    //   fmt_off + 0..1   : wFormatTag (0xFFFE)
    //   fmt_off + 2..3   : nChannels
    //   fmt_off + 4..7   : nSamplesPerSec
    //   fmt_off + 8..11  : nAvgBytesPerSec
    //   fmt_off + 12..13 : nBlockAlign
    //   fmt_off + 14..15 : wBitsPerSample
    //   fmt_off + 16..17 : cbSize  (22 for EXTENSIBLE)
    //   fmt_off + 18..19 : wValidBitsPerSample
    //   fmt_off + 20..23 : dwChannelMask
    //   fmt_off + 24..39 : SubFormat GUID (16)
    //   fmt_off + 40..43 : ATRAC9 version/extra
    //   fmt_off + 44..47 : ATRAC9 config (4 bytes)
    if (fmt_size < 48) {
        LOG_WARN(General, "snd_player: '%s' fmt chunk too small for ATRAC9", path.c_str());
        return {};
    }
    std::uint8_t config[ATRAC9_CONFIG_DATA_SIZE];
    std::memcpy(config, &file[fmt_off + 44], ATRAC9_CONFIG_DATA_SIZE);

    // fact chunk: dwSampleLength (4), then 4 bytes unused, then
    // dwMMSectionEncoderDelay / dwEncoderDelay (4).  We use
    // dwSampleLength for the per-channel sample count and the
    // dwEncoderDelay to skip the leading silent frames.
    std::uint32_t sample_count  = 0;
    std::uint32_t encoder_delay = 0;
    if (fact_size >= 4) {
        sample_count = static_cast<std::uint32_t>(file[fact_off]) |
                       (static_cast<std::uint32_t>(file[fact_off + 1]) << 8) |
                       (static_cast<std::uint32_t>(file[fact_off + 2]) << 16) |
                       (static_cast<std::uint32_t>(file[fact_off + 3]) << 24);
    }
    if (fact_size >= 12) {
        encoder_delay = static_cast<std::uint32_t>(file[fact_off + 8]) |
                        (static_cast<std::uint32_t>(file[fact_off + 9]) << 8) |
                        (static_cast<std::uint32_t>(file[fact_off + 10]) << 16) |
                        (static_cast<std::uint32_t>(file[fact_off + 11]) << 24);
    }

    // Initialise LibAtrac9.
    void* handle = Atrac9GetHandle();
    if (!handle) {
        LOG_WARN(General, "snd_player: Atrac9GetHandle returned null");
        return {};
    }
    if (Atrac9InitDecoder(handle, config) != 0) {
        LOG_WARN(General, "snd_player: Atrac9InitDecoder failed for '%s'", path.c_str());
        Atrac9ReleaseHandle(handle);
        return {};
    }
    Atrac9CodecInfo info{};
    if (Atrac9GetCodecInfo(handle, &info) != 0 ||
        info.channels <= 0 || info.frameSamples <= 0 ||
        info.superframeSize <= 0 || info.framesInSuperframe <= 0) {
        LOG_WARN(General, "snd_player: bad codec info for '%s'", path.c_str());
        Atrac9ReleaseHandle(handle);
        return {};
    }
    const int channels             = info.channels;
    const int sampleRate           = info.samplingRate;
    const int frameSamples         = info.frameSamples;          // per FRAME
    const int framesInSuperframe   = info.framesInSuperframe;    // per SUPERFRAME
    const int superframeBytes      = info.superframeSize;        // per SUPERFRAME
    const int superframeSamples    = frameSamples * framesInSuperframe;
    // Number of superframes in the FILE.  NB: `info.framesInSuperframe`
    // is the number of FRAMES packed into one superframe (usually 1 or
    // 2) — not the number of superframes in the file.  Compute the
    // latter from the data chunk length.  This matches vgmstream's
    // `atrac9_bytes_to_samples` and SharpEmu's `SndPreviewPlayer`.
    if (superframeBytes <= 0) {
        LOG_WARN(General, "snd_player: '%s' has zero-sized superframe",
                 path.c_str());
        Atrac9ReleaseHandle(handle);
        return {};
    }
    const std::uint64_t total_superframes_in_file =
        static_cast<std::uint64_t>(data_size) /
        static_cast<std::uint64_t>(superframeBytes);

    if (sample_count == 0) {
        // No fact chunk (or it was zero): derive from data size.
        sample_count = static_cast<std::uint32_t>(
            total_superframes_in_file *
            static_cast<std::uint64_t>(superframeSamples));
        if (sample_count > encoder_delay && encoder_delay > 0) {
            sample_count -= encoder_delay;
        }
    }

    // Allocate output PCM16 buffer (interleaved).
    constexpr std::size_t kWavHeader = 44;
    const std::size_t pcm_bytes = static_cast<std::size_t>(sample_count) *
                                  static_cast<std::size_t>(channels) * 2;
    wav.assign(kWavHeader + pcm_bytes, 0);
    WriteU32LE(wav, 0,  0x46464952U);   // "RIFF"
    WriteU32LE(wav, 4,  static_cast<std::uint32_t>(wav.size() - 8));
    WriteU32LE(wav, 8,  0x45564157U);   // "WAVE"
    WriteU32LE(wav, 12, 0x20746D66U);   // "fmt "
    WriteU32LE(wav, 16, 16);            // fmt chunk size (PCM)
    WriteU16LE(wav, 20, 1);             // PCM
    WriteU16LE(wav, 22, static_cast<std::uint16_t>(channels));
    WriteU32LE(wav, 24, static_cast<std::uint32_t>(sampleRate));
    WriteU32LE(wav, 28, static_cast<std::uint32_t>(sampleRate * channels * 2));
    WriteU16LE(wav, 32, static_cast<std::uint16_t>(channels * 2));
    WriteU16LE(wav, 34, 16);
    WriteU32LE(wav, 36, 0x61746164U);   // "data"
    WriteU32LE(wav, 40, static_cast<std::uint32_t>(pcm_bytes));

    // Scratch buffers.  Each superframe is `superframeBytes` of input
    // (padded by 0x10 to absorb the decoder's over-read — see
    // Thealexbarney/LibAtrac9 issue #6) and `superframeSamples` of
    // interleaved PCM16 output.  Atrac9Decode consumes ONE frame per
    // call and returns the exact bytes used; we loop `framesInSuperframe`
    // times per outer superframe iteration (vgmstream / shadps4 pattern).
    constexpr std::size_t kAtrac9ReadSlack = 0x10;
    const std::size_t sf_buf_size = static_cast<std::size_t>(superframeBytes) +
                                    kAtrac9ReadSlack;
    std::vector<std::uint8_t> sf_buf(sf_buf_size, 0);
    std::vector<std::int16_t> pcm(static_cast<std::size_t>(superframeSamples) *
                                  static_cast<std::size_t>(channels), 0);

    std::uint32_t written = 0;
    for (std::uint64_t sf = 0;
         sf < total_superframes_in_file && written < sample_count; ++sf) {
        const std::size_t sf_src = data_off +
                                   static_cast<std::size_t>(sf) *
                                   static_cast<std::size_t>(superframeBytes);
        if (sf_src + superframeBytes > file.size()) break;
        std::memcpy(sf_buf.data(), &file[sf_src], superframeBytes);
        // Zero the slack region (calloc'd above; defensive).
        std::memset(sf_buf.data() + superframeBytes, 0, kAtrac9ReadSlack);

        std::uint8_t* cur      = sf_buf.data();
        std::int16_t* pcm_cur  = pcm.data();
        bool frame_ok = true;
        for (int f = 0; f < framesInSuperframe; ++f) {
            int nBytesUsed = 0;
            if (Atrac9Decode(handle, cur, pcm_cur,
                             kAtrac9FormatS16, &nBytesUsed) != 0) {
                LOG_WARN(General,
                         "snd_player: Atrac9Decode failed at superframe %llu frame %d",
                         static_cast<unsigned long long>(sf), f);
                frame_ok = false;
                break;
            }
            if (nBytesUsed <= 0) {
                LOG_WARN(General,
                         "snd_player: Atrac9Decode returned 0 bytes used "
                         "at superframe %llu frame %d",
                         static_cast<unsigned long long>(sf), f);
                frame_ok = false;
                break;
            }
            cur     += nBytesUsed;
            pcm_cur += frameSamples * channels;
        }
        if (!frame_ok) break;

        // Copy this superframe's samples to the WAV, skipping the
        // encoder delay on the very first superframe only.
        for (int s = 0; s < superframeSamples; ++s) {
            if (sf == 0 &&
                static_cast<std::uint32_t>(s) < encoder_delay) {
                continue;
            }
            if (written >= sample_count) break;
            const std::size_t sample_off = kWavHeader +
                static_cast<std::size_t>(written) * channels * 2;
            for (int ch = 0; ch < channels; ++ch) {
                const std::int16_t v = pcm[s * channels + ch];
                wav[sample_off + ch * 2 + 0] = static_cast<std::uint8_t>(v);
                wav[sample_off + ch * 2 + 1] = static_cast<std::uint8_t>(v >> 8);
            }
            ++written;
        }
    }
    Atrac9ReleaseHandle(handle);
    LOG_INFO(General, "snd_player: decoded '%s' -> %u samples, %d ch, %d Hz",
             path.c_str(), written, channels, sampleRate);
    return wav;
}

// ---------------------------------------------------------------------------
// OGG Vorbis -> PCM16 WAV decode (vendored stb_vorbis).
//
// Reads the whole .ogg file into memory, asks stb_vorbis for its
// PCM16 sample count, then decodes the entire stream into a single
// interleaved int16 buffer.  Wraps that buffer in a canonical
// 44-byte RIFF/WAVE/PCM header so the output is byte-identical
// (in shape) to what `DecodeAt9ToWav` produces — meaning the
// existing waveOut playback path picks it up with no special
// handling.
// ---------------------------------------------------------------------------
std::vector<std::uint8_t> SndPreviewPlayer::DecodeOggToWav(
    const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        LOG_WARN(General, "snd_player: cannot open '%s'", path.c_str());
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string buf = ss.str();
    if (buf.size() < 4 ||
        static_cast<unsigned char>(buf[0]) != 'O' ||
        static_cast<unsigned char>(buf[1]) != 'g' ||
        static_cast<unsigned char>(buf[2]) != 'g' ||
        static_cast<unsigned char>(buf[3]) != 'S') {
        LOG_WARN(General, "snd_player: '%s' is not an OGG stream", path.c_str());
        return {};
    }
    const std::vector<unsigned char> file(buf.begin(), buf.end());
    int ogg_err = 0;
    stb_vorbis* vorbis = ::stb_vorbis_open_memory(
        file.data(), static_cast<int>(file.size()),
        &ogg_err, nullptr);
    if (!vorbis) {
        LOG_WARN(General,
                 "snd_player: stb_vorbis_open_memory failed for '%s' (err=%d)",
                 path.c_str(), ogg_err);
        return {};
    }
    const stb_vorbis_info info = ::stb_vorbis_get_info(vorbis);
    const int channels   = info.channels   > 0 ? info.channels   : 2;
    const int sampleRate = info.sample_rate > 0
                             ? static_cast<int>(info.sample_rate) : 48000;
    // Allocate an output buffer large enough for the full stream.
    // stb_vorbis_get_samples_short_interleaved returns the number of
    // *frames* it actually decoded (which can be less than the file
    // length on truncated files).  Start with a generous estimate
    // and shrink after.
    const unsigned int total_samples_per_ch =
        static_cast<unsigned int>(::stb_vorbis_stream_length_in_samples(vorbis));
    const std::size_t pcm_frames_est =
        total_samples_per_ch > 0
            ? static_cast<std::size_t>(total_samples_per_ch)
            : (file.size() * 2);  // worst-case fallback
    std::vector<std::int16_t> pcm(pcm_frames_est * channels, 0);

    const int frames_decoded = ::stb_vorbis_get_samples_short_interleaved(
        vorbis, channels, pcm.data(),
        static_cast<int>(pcm.size()));
    ::stb_vorbis_close(vorbis);
    if (frames_decoded <= 0) {
        LOG_WARN(General,
                 "snd_player: stb_vorbis returned 0 frames for '%s'", path.c_str());
        return {};
    }

    constexpr std::size_t kWavHeader = 44;
    const std::size_t pcm_frames = static_cast<std::size_t>(frames_decoded);
    const std::size_t pcm_bytes  = pcm_frames *
                                   static_cast<std::size_t>(channels) * 2U;
    std::vector<std::uint8_t> wav(kWavHeader + pcm_bytes, 0);
    WriteU32LE(wav, 0,  0x46464952U);   // "RIFF"
    WriteU32LE(wav, 4,  static_cast<std::uint32_t>(wav.size() - 8));
    WriteU32LE(wav, 8,  0x45564157U);   // "WAVE"
    WriteU32LE(wav, 12, 0x20746D66U);   // "fmt "
    WriteU32LE(wav, 16, 16);            // fmt chunk size (PCM)
    WriteU16LE(wav, 20, 1);             // wFormatTag = PCM
    WriteU16LE(wav, 22, static_cast<std::uint16_t>(channels));
    WriteU32LE(wav, 24, static_cast<std::uint32_t>(sampleRate));
    WriteU32LE(wav, 28, static_cast<std::uint32_t>(sampleRate * channels * 2));
    WriteU16LE(wav, 32, static_cast<std::uint16_t>(channels * 2));
    WriteU16LE(wav, 34, 16);
    WriteU32LE(wav, 36, 0x61746164U);   // "data"
    WriteU32LE(wav, 40, static_cast<std::uint32_t>(pcm_bytes));
    // Interleaved PCM16 little-endian copy.
    for (std::size_t s = 0; s < pcm_frames * channels; ++s) {
        const std::size_t off = kWavHeader + s * 2;
        const std::int16_t v  = pcm[s];
        wav[off + 0] = static_cast<std::uint8_t>(v);
        wav[off + 1] = static_cast<std::uint8_t>(v >> 8);
    }
    LOG_INFO(General,
             "snd_player: decoded '%s' -> %d samples, %d ch, %d Hz",
             path.c_str(), frames_decoded, channels, sampleRate);
    return wav;
}

// ---------------------------------------------------------------------------
// Member functions
// ---------------------------------------------------------------------------
SndPreviewPlayer::SndPreviewPlayer() {
    worker_ = std::thread([this] { WorkerLoop(); });
}

SndPreviewPlayer::~SndPreviewPlayer() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_worker_ = true;
        requested_pending_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    StopLocked();
}

bool SndPreviewPlayer::playing() const {
    std::lock_guard<std::mutex> lk(mu_);
    return playing_;
}

const std::string& SndPreviewPlayer::current_path() const {
    std::lock_guard<std::mutex> lk(mu_);
    return cached_path_;
}

void SndPreviewPlayer::SetEnabled(bool enabled) {
    enabled_.store(enabled, std::memory_order_relaxed);
    if (!enabled) Stop();
}

void SndPreviewPlayer::Play(const std::string& music_path) {
    if (!enabled_.load(std::memory_order_relaxed)) return;
    if (music_path.empty()) {
        Stop();
        return;
    }
    // Bump generation so any in-flight decode becomes stale.
    int gen = generation_.fetch_add(1, std::memory_order_relaxed) + 1;

    std::lock_guard<std::mutex> lk(mu_);
    // If we're already playing this exact path, no-op.
    if (cached_path_ == music_path && playing_ && !paused_) {
        return;
    }
    requested_path_     = music_path;
    requested_pending_  = true;
    cv_.notify_all();
    (void)gen;
}

void SndPreviewPlayer::Stop() {
    generation_.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(mu_);
        requested_path_    = "";
        requested_pending_ = false;
        StopLocked();
        // Also forget the cached track so a future Play(same_path)
        // has to re-decode.  This matches SharpEmu behaviour: Stop
        // discards the pinned buffer.
        cached_path_.clear();
        cached_wav_.clear();
        last_started_wav_.clear();
    }
}

void SndPreviewPlayer::Pause() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!playing_ || paused_) return;
#ifdef _WIN32
    if (wave_out_) {
        PauseWaveOut();
        return;
    }
    if (mci_open_) {
        ::mciSendStringW(L"pause pcsx5snd", nullptr, 0, nullptr);
        paused_  = true;
        playing_ = false;
        return;
    }
#else
    playing_ = false;
    paused_  = true;
#endif
}

void SndPreviewPlayer::Resume() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!paused_) return;
#ifdef _WIN32
    if (wave_out_) {
        ResumeWaveOut();
        return;
    }
    if (mci_open_) {
        // MCI alias still open — just resume, the loop thread will
        // re-issue play on natural end.
        ::mciSendStringW(L"resume pcsx5snd", nullptr, 0, nullptr);
        playing_ = true;
        paused_  = false;
        return;
    }
    // No engine currently running.  If we have a cached decoded
    // WAV, hand it to waveOut; otherwise try the on-disk path via
    // the dispatch in StartLocked.
    if (!last_started_wav_.empty()) {
        StartWaveOut(last_started_wav_);
        return;
    }
    if (!cached_path_.empty()) {
        StartLocked({});
        return;
    }
    paused_ = false;
    playing_ = false;
#else
    paused_  = false;
    playing_ = false;
#endif
}

// ---------------------------------------------------------------------------
// Worker thread: wait for debounce -> check generation -> decode -> play
// ---------------------------------------------------------------------------
void SndPreviewPlayer::WorkerLoop() {
    using clock = std::chrono::steady_clock;
    while (true) {
        std::string path;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] { return stop_worker_ || requested_pending_; });
            if (stop_worker_) return;
            // Consume the request: clear the pending flag so the cv_
            // doesn't re-fire for the same path on the next loop
            // iteration.  Without this, the worker would call
            // StartLocked (and StartWaveOut) once per 300ms debounce
            // window forever — manifesting as a flood of "waveOut
            // started" log lines and a stack of re-opened devices.
            requested_pending_ = false;
            path = requested_path_;
        }
        if (path.empty()) continue;
        const int my_gen = generation_.load(std::memory_order_relaxed);

        // 300 ms debounce — discard if a newer request arrives.
        std::this_thread::sleep_for(std::chrono::milliseconds(kDebounceMs));
        if (my_gen != generation_.load(std::memory_order_relaxed)) continue;

        // Cache hit?
        std::vector<std::uint8_t> wav;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (cached_path_ == path && !cached_wav_.empty()) {
                wav = cached_wav_;
            }
        }
        if (wav.empty()) {
            if (EndsWithCi(path, ".at9") || EndsWithCi(path, ".at3")) {
                wav = DecodeAt9ToWav(path);
            }
            else if (EndsWithCi(path, ".ogg")) {
                // OGG Vorbis: decode to PCM16 WAV in-memory via the
                // vendored stb_vorbis single-header decoder.  The
                // resulting buffer goes through the same waveOut
                // path as .at9 below — seamless looping, no size
                // cap, no temp file.  This replaces the previous
                // "skip OGG" warning (we used to bail because MCI
                // has no native OGG codec).
                wav = DecodeOggToWav(path);
            }
#ifdef _WIN32
            else if (EndsWithCi(path, ".wav") ||
                     EndsWithCi(path, ".mp3") ||
                     EndsWithCi(path, ".flac") ||
                     EndsWithCi(path, ".opus")) {
                // For container formats, just record the path; the
                // real playback is handled in StartLocked.
                std::lock_guard<std::mutex> lk(mu_);
                cached_path_ = path;
                cached_wav_.clear();
                StartLocked({});
                continue;
            }
#endif
            else {
                LOG_WARN(General, "snd_player: unsupported extension on '%s'",
                         path.c_str());
                continue;
            }
        }
        if (wav.empty()) continue;
        if (my_gen != generation_.load(std::memory_order_relaxed)) continue;

        std::lock_guard<std::mutex> lk(mu_);
        if (cached_path_ != path) {
            cached_path_ = path;
            cached_wav_  = wav;
        } else {
            // Already cached by an earlier Play; just adopt the
            // newer wav (defensive).
            cached_wav_ = wav;
        }
        StartLocked(wav);
    }
}

void SndPreviewPlayer::EnqueueDecode(int /*generation*/,
                                     std::string /*path*/) {
    // Kept for symmetry with SharpEmu's design; not needed because
    // the worker loop inlines the decode call.  Reserved for future
    // thread-pooled decodes.
}

void SndPreviewPlayer::StartLocked(const std::vector<std::uint8_t>& wav) {
#ifdef _WIN32
    StopLocked();
    // First: handle the "decoded .at9 → PCM WAV in memory" case via
    // waveOut.  This is the most reliable path: no temp file, no
    // MCI, no polling loop — the WOM_DONE callback re-submits the
    // same buffer for seamless looping, and waveOut has no size cap.
    if (!wav.empty()) {
        last_started_wav_ = wav;
        StartWaveOut(wav);
        return;
    }
    // No decoded WAV: caller is asking us to play a file on disk.
    // Dispatch by extension:
    //   .wav        → waveOut (parse header, open device, play)
    //   .mp3        → MCI `mpegvideo` (only MCI type that handles it)
    //   .ogg/.flac/
    //   .opus       → no native MCI support, skip with a one-time
    //                 warning (the user can install DirectShow codecs
    //                 or convert to .wav externally).
    const std::string& path = cached_path_;
    if (path.empty()) {
        playing_ = false;
        paused_  = false;
        return;
    }
    if (EndsWithCi(path, ".wav")) {
        // Read WAV from disk into memory and hand to waveOut.
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            LOG_WARN(General, "snd_player: cannot open '%s'", path.c_str());
            playing_ = false;
            paused_  = false;
            return;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        const std::string buf = ss.str();
        const std::vector<std::uint8_t> file(buf.begin(), buf.end());
        last_started_wav_ = file;
        StartWaveOut(file);
        return;
    }
    if (EndsWithCi(path, ".mp3")) {
        if (MciOpenAndPlayMpeg(path)) {
            mci_open_ = true;
            playing_  = true;
            paused_   = false;
            // mpegvideo doesn't accept `repeat` either; spawn the
            // polling loop thread to re-play on natural end.
            if (!loop_active_.load(std::memory_order_relaxed) &&
                !loop_thread_.joinable()) {
                loop_active_.store(true, std::memory_order_relaxed);
                loop_thread_ = std::thread([this] { LoopThread(); });
            }
        } else {
            mci_open_ = false;
            playing_  = false;
            paused_   = false;
        }
        return;
    }
    if (EndsWithCi(path, ".ogg") || EndsWithCi(path, ".flac") ||
        EndsWithCi(path, ".opus")) {
        // .ogg is handled in the worker (stb_vorbis decode →
        // in-memory WAV → waveOut).  .flac and .opus still have no
        // native decoder vendored, so we skip those with a
        // one-time warning (per-path dedup via `last_warned`).
        if (EndsWithCi(path, ".ogg")) {
            // Should not reach here: WorkerLoop intercepts .ogg
            // before StartLocked is ever called with an empty
            // wav.  Defensive log in case the code path shifts.
            LOG_WARN(General,
                     "snd_player: .ogg reached StartLocked (unexpected)");
            playing_ = false;
            paused_  = false;
            return;
        }
        static std::string last_warned;
        if (last_warned != path) {
            last_warned = path;
            LOG_WARN(General,
                     "snd_player: skipping '%s' (no native decoder; "
                     "convert to .wav/.at9 or install a system codec)",
                     path.c_str());
        }
        playing_ = false;
        paused_  = false;
        return;
    }
    // Unknown extension — try waveOut as a last resort.
    {
        std::ifstream in(path, std::ios::binary);
        if (in) {
            std::ostringstream ss;
            ss << in.rdbuf();
            const std::string buf = ss.str();
            const std::vector<std::uint8_t> file(buf.begin(), buf.end());
            last_started_wav_ = file;
            StartWaveOut(file);
            return;
        }
    }
    playing_ = false;
    paused_  = false;
#else
    (void)wav;
    playing_ = false;
    paused_  = false;
#endif
}

void SndPreviewPlayer::StopLocked() {
#ifdef _WIN32
    StopWaveOut();
    // Tell the MCI loop thread to exit and wait for it.  Done
    // before closing the alias so we don't race with a re-play.
    if (loop_active_.load(std::memory_order_relaxed)) {
        loop_active_.store(false, std::memory_order_relaxed);
        if (loop_thread_.joinable()) loop_thread_.join();
    }
    if (mci_open_) {
        MciClose();
        mci_open_ = false;
    }
#endif
    playing_ = false;
    paused_  = false;
    FreePinnedLocked();
}

#ifdef _WIN32
// Polls the MCI waveaudio device's mode and re-issues `play` whenever
// it transitions to `stopped` while we still want playback.  The MCI
// waveaudio device has no native `repeat` parameter, so this is the
// canonical way to loop a single WAV through MCI.  Polling at 200 ms
// is imperceptible (the file is 80+ seconds long) and avoids the
// need for a message-only window + MM_MCINOTIFY pump.
void SndPreviewPlayer::LoopThread() {
    while (loop_active_.load(std::memory_order_relaxed)) {
        ::Sleep(200);
        if (!loop_active_.load(std::memory_order_relaxed)) break;

        // Hold the lock briefly to read mci_open_ / paused_ and to
        // serialize against a concurrent StopLocked.  If we're paused
        // or the alias is closed, skip this tick.
        bool should_play = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            should_play = mci_open_ && playing_ && !paused_;
        }
        if (!should_play) continue;

        // `status <alias> mode` returns one of: stopped / playing /
        // paused / seeking / not ready.
        wchar_t mode[64] = {0};
        const MCIERROR e = ::mciSendStringW(
            L"status pcsx5snd mode", mode, 64, nullptr);
        if (e != 0) continue;
        if (wcscmp(mode, L"stopped") == 0) {
            // File ended naturally; seek to start and re-play.
            ::mciSendStringW(L"seek pcsx5snd to start",
                             nullptr, 0, nullptr);
            ::mciSendStringW(L"play pcsx5snd",
                             nullptr, 0, nullptr);
        }
    }
}
#endif  // _WIN32

void SndPreviewPlayer::FreePinnedLocked() {
    pinned_wav_.clear();
    pinned_addr_ = nullptr;
    pinned_allocated_ = false;
}

}  // namespace Ui
