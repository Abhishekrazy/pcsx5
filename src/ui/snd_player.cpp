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
    const int channels   = info.channels;
    const int sampleRate = info.samplingRate;
    const int superframeSamples = info.frameSamples;
    const int superframeBytes   = info.superframeSize;
    const int totalSuperframes  = info.framesInSuperframe;
    // (Atrac9Decode reports `bytesUsed` for each frame; we don't need a
    //  precomputed frameBytes value.)

    if (sample_count == 0) {
        // Fall back to superframe math.
        sample_count = static_cast<std::uint32_t>(totalSuperframes) *
                       static_cast<std::uint32_t>(superframeSamples);
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

    // One superframe contains `framesInSuperframe` frames back-to-back;
    // each Atrac9Decode() call consumes one frame and tells us exactly
    // how many bytes it ate via `bytes_used` (vgmstream does the same).
    // Buffer is padded by 0x10 bytes to absorb the decoder's over-read
    // — see Thealexbarney/LibAtrac9 issue #6.
    constexpr std::size_t kAtrac9ReadSlack = 0x10;
    const std::size_t sf_buf_size = static_cast<std::size_t>(superframeBytes) +
                                    kAtrac9ReadSlack;
    std::vector<std::uint8_t> sf_buf(sf_buf_size, 0);
    // Output PCM16 buffer sized for the entire superframe (interleaved).
    std::vector<std::int16_t> pcm(static_cast<std::size_t>(superframeSamples) *
                                  static_cast<std::size_t>(channels) *
                                  static_cast<std::size_t>(totalSuperframes), 0);

    std::uint32_t written = 0;
    for (int sf = 0; sf < totalSuperframes && written < sample_count; ++sf) {
        const std::size_t sf_src = data_off + static_cast<std::size_t>(sf) *
                                   static_cast<std::size_t>(superframeBytes);
        if (sf_src + superframeBytes > file.size()) break;
        std::memcpy(sf_buf.data(), &file[sf_src], superframeBytes);
        // Zero the slack region (calloc'd above; defensive).
        std::memset(sf_buf.data() + superframeBytes, 0, kAtrac9ReadSlack);

        std::uint8_t* cur      = sf_buf.data();
        std::int16_t* pcm_cur  = pcm.data();
        bool frame_ok = true;
        for (int f = 0; f < totalSuperframes; ++f) {
            int nBytesUsed = 0;
            if (Atrac9Decode(handle, cur, pcm_cur,
                             kAtrac9FormatS16, &nBytesUsed) != 0) {
                LOG_WARN(General,
                         "snd_player: Atrac9Decode failed at superframe %d frame %d",
                         sf, f);
                frame_ok = false;
                break;
            }
            if (nBytesUsed <= 0) {
                // Defensive: a zero-byte advance would loop forever.
                LOG_WARN(General,
                         "snd_player: Atrac9Decode returned 0 bytes used "
                         "at superframe %d frame %d", sf, f);
                frame_ok = false;
                break;
            }
            cur     += nBytesUsed;
            pcm_cur += superframeSamples * channels;
        }
        if (!frame_ok) break;

        // Copy this superframe's samples to the WAV, skipping the
        // encoder delay on the very first superframe only.
        for (int s = 0; s < superframeSamples * totalSuperframes; ++s) {
            if (sf == 0 && static_cast<std::uint32_t>(s) < encoder_delay) {
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
    ::PlaySoundW(nullptr, nullptr, 0);
#endif
    playing_ = false;
    paused_  = true;
}

void SndPreviewPlayer::Resume() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!paused_) return;
    if (pinned_wav_.empty() || !pinned_allocated_) {
        paused_ = false;
        return;
    }
#ifdef _WIN32
    const BOOL ok = ::PlaySoundW(
        reinterpret_cast<LPCWSTR>(pinned_addr_), nullptr,
        SND_MEMORY | SND_ASYNC | SND_LOOP | SND_NODEFAULT);
    playing_ = (ok != FALSE);
#endif
    if (playing_) paused_ = false;
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
#ifdef _WIN32
            else if (EndsWithCi(path, ".wav") || EndsWithCi(path, ".ogg") ||
                     EndsWithCi(path, ".mp3") || EndsWithCi(path, ".flac") ||
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
    if (wav.empty()) {
        // Caller passed an empty wav: this means a non-AT9 file path
        // was just registered.  Use SND_FILENAME so winmm streams
        // from disk via DirectShow (handles .ogg/.mp3/.flac on most
        // Windows installs; WAV plays natively).
        const std::wstring wpath(cached_path_.begin(), cached_path_.end());
        const BOOL ok = ::PlaySoundW(wpath.c_str(), nullptr,
                                     SND_FILENAME | SND_ASYNC | SND_LOOP |
                                     SND_NODEFAULT);
        playing_ = (ok != FALSE);
        paused_  = false;
        return;
    }
    // In-memory WAV: pin the bytes so winmm can read them safely
    // even if we later overwrite `cached_wav_` (we don't, but
    // pin-before-play is the documented contract).
    pinned_wav_ = wav;
    // Round up to an even alignment for winmm's comfort.
    if (pinned_wav_.size() & 1U) pinned_wav_.push_back(0);
    pinned_allocated_ = true;
    // winmm reads from the buffer pointer, so we rely on the vector
    // storage being stable.  Reserve exactly once and never
    // reallocate: in our flow we replace pinned_wav_ via assignment
    // above only when we Stop() first, so the storage survives.
    pinned_addr_ = pinned_wav_.data();
    const BOOL ok = ::PlaySoundW(reinterpret_cast<LPCWSTR>(pinned_addr_),
                                 nullptr,
                                 SND_MEMORY | SND_ASYNC | SND_LOOP |
                                 SND_NODEFAULT);
    playing_ = (ok != FALSE);
    paused_  = false;
    if (!playing_) {
        // winmm rejected the buffer; release the pin so future plays
        // can succeed.
        pinned_wav_.clear();
        pinned_addr_ = nullptr;
        pinned_allocated_ = false;
    }
    last_started_wav_ = wav;
#else
    (void)wav;
    playing_ = false;
    paused_  = false;
#endif
}

void SndPreviewPlayer::StopLocked() {
#ifdef _WIN32
    ::PlaySoundW(nullptr, nullptr, 0);
#endif
    playing_ = false;
    paused_  = false;
    FreePinnedLocked();
}

void SndPreviewPlayer::FreePinnedLocked() {
    pinned_wav_.clear();
    pinned_addr_ = nullptr;
    pinned_allocated_ = false;
}

}  // namespace Ui
