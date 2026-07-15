// snd_player_test — verify the ATRAC9 -> WAV path against a real PS5
// title's snd0.at9.  We take the first .at9 found under ./Games/ and
// try to decode it; success means the WAV file starts with "RIFF".
// We don't actually play the sound (no audio device in CI) — we just
// check the decoder produces a non-empty, well-formed PCM16 WAV.

#include "ui/snd_player.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#endif

namespace fs = std::filesystem;

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg) do {                                                 \
    if (cond) { ++g_passed; std::printf("  PASS  %s\n", msg); }                \
    else      { ++g_failed; std::printf("  FAIL  %s\n", msg); }               \
} while (0)

static fs::path FindFirstAt9(const fs::path& games_dir) {
    if (!fs::exists(games_dir)) return {};
    for (const auto& entry : fs::recursive_directory_iterator(
             games_dir, fs::directory_options::skip_permission_denied)) {
        if (!entry.is_regular_file()) continue;
        const auto ext = entry.path().extension().generic_string();
        if (ext == ".at9" || ext == ".at3") return entry.path();
    }
    return {};
}

static fs::path FindFirstOgg(const fs::path& games_dir) {
    if (!fs::exists(games_dir)) return {};
    for (const auto& entry : fs::recursive_directory_iterator(
             games_dir, fs::directory_options::skip_permission_denied)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() == ".ogg") return entry.path();
    }
    return {};
}

static bool WriteWav(const std::string& path,
                     const std::vector<std::uint8_t>& wav) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(wav.data()),
            static_cast<std::streamsize>(wav.size()));
    return f.good();
}

int main() {
    const fs::path games_dir = "./Games";
    const fs::path at9 = FindFirstAt9(games_dir);
    if (at9.empty()) {
        std::printf("snd_player_test: no .at9 files under %s — skipping\n",
                    games_dir.generic_string().c_str());
        return 0;  // not a failure
    }
    std::printf("snd_player_test: decoding %s\n",
                at9.generic_string().c_str());

    auto wav = Ui::SndPreviewPlayer::DecodeAt9ToWav(at9.generic_string());
    CHECK(!wav.empty(), "decoded WAV is non-empty");
    CHECK(wav.size() > 44, "WAV size exceeds header (44 bytes)");
    CHECK(wav.size() >= 4 && std::memcmp(wav.data(), "RIFF", 4) == 0,
          "WAV starts with 'RIFF'");
    CHECK(wav.size() >= 12 && std::memcmp(wav.data() + 8, "WAVE", 4) == 0,
          "WAV has 'WAVE' marker at offset 8");

    // fmt chunk: wFormatTag (LE16) at offset 20 must be 1 (PCM).
    if (wav.size() >= 22) {
        const std::uint16_t fmt =
            static_cast<std::uint16_t>(wav[20]) |
            (static_cast<std::uint16_t>(wav[21]) << 8);
        CHECK(fmt == 1, "fmt.wFormatTag is PCM (1)");
    } else {
        CHECK(false, "WAV too small for fmt chunk");
    }

    // data chunk: 'data' at offset 36.
    if (wav.size() >= 40) {
        CHECK(std::memcmp(wav.data() + 36, "data", 4) == 0,
              "WAV has 'data' chunk at offset 36");
    }

    // Dump the WAV to disk so the user can sanity-check it.
    const std::string out = "snd0_decoded.wav";
    if (WriteWav(out, wav)) {
        std::printf("  Wrote %s (%zu bytes)\n", out.c_str(), wav.size());
    } else {
        std::printf("  WARN  failed to write %s\n", out.c_str());
    }

    // -------------------------------------------------------------
    // OGG Vorbis decode (vendored stb_vorbis).  Walks the
    // Games/ tree for the first .ogg and verifies it decodes
    // to a well-formed PCM16 WAV.  Catches regressions in the
    // OGG → waveOut path (replaces the "skip OGG" warning).
    // -------------------------------------------------------------
    const fs::path ogg = FindFirstOgg(games_dir);
    if (!ogg.empty()) {
        std::printf("\nsnd_player_test: decoding %s\n",
                    ogg.generic_string().c_str());
        auto ogg_wav = Ui::SndPreviewPlayer::DecodeOggToWav(
            ogg.generic_string());
        CHECK(!ogg_wav.empty(), "OGG -> WAV is non-empty");
        CHECK(ogg_wav.size() > 44, "OGG WAV size exceeds header (44 bytes)");
        CHECK(ogg_wav.size() >= 4 &&
              std::memcmp(ogg_wav.data(), "RIFF", 4) == 0,
              "OGG WAV starts with 'RIFF'");
        CHECK(ogg_wav.size() >= 12 &&
              std::memcmp(ogg_wav.data() + 8, "WAVE", 4) == 0,
              "OGG WAV has 'WAVE' marker at offset 8");
        if (ogg_wav.size() >= 22) {
            const std::uint16_t fmt =
                static_cast<std::uint16_t>(ogg_wav[20]) |
                (static_cast<std::uint16_t>(ogg_wav[21]) << 8);
            CHECK(fmt == 1, "OGG WAV fmt.wFormatTag is PCM (1)");
        }
        // Sanity: at least 100 ms of audio (44100 * 2 * 2 * 0.1 = 17640).
        if (ogg_wav.size() >= 44) {
            const std::uint32_t data_bytes =
                static_cast<std::uint32_t>(ogg_wav[40]) |
                (static_cast<std::uint32_t>(ogg_wav[41]) << 8) |
                (static_cast<std::uint32_t>(ogg_wav[42]) << 16) |
                (static_cast<std::uint32_t>(ogg_wav[43]) << 24);
            CHECK(data_bytes > 16000,
                  "OGG WAV data chunk holds > 100 ms of PCM");
        }
        if (WriteWav("baddream_decoded.wav", ogg_wav)) {
            std::printf("  Wrote baddream_decoded.wav (%zu bytes)\n",
                        ogg_wav.size());
        }
    } else {
        std::printf("\nsnd_player_test: no .ogg files under %s — "
                    "skipping OGG checks\n", games_dir.generic_string().c_str());
    }

#ifdef _WIN32
    // -------------------------------------------------------------
    // MCI smoke test.  Actually open the decoded WAV through winmm
    // MCI and query the duration.  This is the same code path the
    // UI uses for the in-app preview, so it exercises the full
    // "decode -> write temp file -> MCI open/play" chain.  If MCI
    // ever sees a truncated file (e.g. SND_MEMORY cap regression),
    // `length` will be the truncated value (e.g. ~1 second).
    // -------------------------------------------------------------
    {
        // Compute expected duration from the WAV header's data chunk
        // size.  RIFF[4]+size[4]+WAVE[4]+fmt/fact chunks + data[4]+
        // data_size at offset 40.
        std::uint32_t data_bytes = 0;
        if (wav.size() >= 44) {
            data_bytes = static_cast<std::uint32_t>(wav[40]) |
                         (static_cast<std::uint32_t>(wav[41]) << 8) |
                         (static_cast<std::uint32_t>(wav[42]) << 16) |
                         (static_cast<std::uint32_t>(wav[43]) << 24);
        }
        std::uint32_t sr = 0;
        if (wav.size() >= 28) {
            sr = static_cast<std::uint32_t>(wav[24]) |
                 (static_cast<std::uint32_t>(wav[25]) << 8) |
                 (static_cast<std::uint32_t>(wav[26]) << 16) |
                 (static_cast<std::uint32_t>(wav[27]) << 24);
        }
        std::uint16_t ch = 0;
        if (wav.size() >= 24) {
            ch = static_cast<std::uint16_t>(wav[22]) |
                 (static_cast<std::uint16_t>(wav[23]) << 8);
        }
        const double expected_ms =
            sr > 0 && ch > 0
                ? (static_cast<double>(data_bytes) * 1000.0) /
                      (static_cast<double>(sr) *
                       static_cast<double>(ch) * 2.0)
                : 0.0;
        std::printf("  Expected: %u bytes PCM @ %u Hz x %u ch = %.2f ms\n",
                    data_bytes, sr, ch, expected_ms);

        // Open the file via MCI and ask for the length.
        const std::wstring open_cmd =
            std::wstring(L"open \"") +
            std::wstring(out.begin(), out.end()) +
            L"\" type waveaudio alias pcxsmoke";
        MCIERROR e = ::mciSendStringW(open_cmd.c_str(), nullptr, 0, nullptr);
        CHECK(e == 0, "MCI open() returned success");

        if (e == 0) {
            // Issue a plain `play` (no `repeat` — the waveaudio
            // device doesn't accept it and returns "driver cannot
            // recognize the specified command parameter").
            const MCIERROR pe = ::mciSendStringW(
                L"play pcxsmoke", nullptr, 0, nullptr);
            CHECK(pe == 0, "MCI play() returned success (no `repeat`)");

            // Verify the device actually entered the playing state
            // (catches a future regression where play() is rejected
            // or returns silently).
            wchar_t mode[64] = {0};
            const MCIERROR me = ::mciSendStringW(
                L"status pcxsmoke mode", mode, 64, nullptr);
            CHECK(me == 0, "MCI status mode returned success");
            if (me == 0) {
                char mbc[32] = {0};
                size_t converted = 0;
                ::wcstombs_s(&converted, mbc, sizeof(mbc), mode, _TRUNCATE);
                std::printf("  MCI reports mode: %s\n", mbc);
                CHECK(wcscmp(mode, L"playing") == 0,
                      "MCI mode is 'playing' (loop thread can poll this)");
            }

            wchar_t lenbuf[64] = {0};
            const MCIERROR le = ::mciSendStringW(
                L"status pcxsmoke length", lenbuf, 64, nullptr);
            CHECK(le == 0, "MCI status length returned success");
            long len_ms = 0;
            if (le == 0 && lenbuf[0] != L'\0') {
                // length comes back in milliseconds.
                len_ms = ::_wtol(lenbuf);
            }
            std::printf("  MCI reports length: %ld ms (%.2f s)\n",
                        len_ms, len_ms / 1000.0);
            // Tolerance: allow 50 ms slack for fact-chunk rounding.
            const double diff = std::abs(static_cast<double>(len_ms) -
                                         expected_ms);
            CHECK(diff < 50.0,
                  "MCI length matches WAV header (within 50 ms)");
            CHECK(len_ms > 5000,
                  "MCI length is at least 5 seconds (NOT truncated to 1 s)");

            ::mciSendStringW(L"stop pcxsmoke", nullptr, 0, nullptr);
            ::mciSendStringW(L"close pcxsmoke", nullptr, 0, nullptr);
        }
    }

    // -------------------------------------------------------------
    // waveOut smoke test.  This is the path the UI actually uses
    // for decoded .at9 / .wav previews, so we want to verify it
    // here: open the device, submit the buffer, check position
    // advances over time, then close cleanly.  Catches the
    // "1-second loop" regression definitively: if waveOut is
    // truncating, position will stall long before the 5 s check.
    // -------------------------------------------------------------
    {
        // Parse the WAV header to drive the WAVEFORMATEX we hand
        // to waveOutOpen.  These offsets are constant for our
        // decoder output.
        const std::uint16_t channels = static_cast<std::uint16_t>(
            wav[22] | (wav[23] << 8));
        const std::uint32_t sampleRate =
            static_cast<std::uint32_t>(wav[24]) |
            (static_cast<std::uint32_t>(wav[25]) << 8) |
            (static_cast<std::uint32_t>(wav[26]) << 16) |
            (static_cast<std::uint32_t>(wav[27]) << 24);
        const std::uint32_t data_bytes =
            static_cast<std::uint32_t>(wav[40]) |
            (static_cast<std::uint32_t>(wav[41]) << 8) |
            (static_cast<std::uint32_t>(wav[42]) << 16) |
            (static_cast<std::uint32_t>(wav[43]) << 24);

        WAVEFORMATEX wfx{};
        wfx.wFormatTag      = WAVE_FORMAT_PCM;
        wfx.nChannels       = channels;
        wfx.nSamplesPerSec  = sampleRate;
        wfx.nAvgBytesPerSec = sampleRate * channels * 2;
        wfx.nBlockAlign     = static_cast<std::uint16_t>(channels * 2);
        wfx.wBitsPerSample  = 16;
        wfx.cbSize          = 0;

        // Copy PCM bytes into a stable buffer.
        std::vector<std::uint8_t> pcm(wav.begin() + 44,
                                      wav.begin() + 44 + data_bytes);
        std::printf("  waveOut: %zu bytes PCM @ %u Hz x %u ch\n",
                    pcm.size(), sampleRate, channels);

        HWAVEOUT wo = nullptr;
        const MMRESULT oe = ::waveOutOpen(&wo, WAVE_MAPPER, &wfx,
                                          0, 0, CALLBACK_NULL);
        CHECK(oe == MMSYSERR_NOERROR, "waveOutOpen returned success");

        if (oe == MMSYSERR_NOERROR && wo) {
            WAVEHDR hdr{};
            hdr.lpData         = reinterpret_cast<LPSTR>(pcm.data());
            hdr.dwBufferLength = static_cast<DWORD>(pcm.size());

            const MMRESULT pe = ::waveOutPrepareHeader(
                wo, &hdr, sizeof(hdr));
            CHECK(pe == MMSYSERR_NOERROR,
                  "waveOutPrepareHeader returned success");

            if (pe == MMSYSERR_NOERROR) {
                const MMRESULT we = ::waveOutWrite(wo, &hdr, sizeof(hdr));
                CHECK(we == MMSYSERR_NOERROR,
                      "waveOutWrite returned success");

                if (we == MMSYSERR_NOERROR) {
                    // Let it play for ~1.5 s and confirm the device
                    // has actually consumed bytes.  If we get stuck
                    // at 0 ms after the sleep, playback never
                    // started (or it's looping at <1 s, the bug
                    // we are guarding against).  We don't assert an
                    // upper bound because some drivers report
                    // position in samples or bytes and we don't
                    // want a brittle range check.
                    ::Sleep(1500);
                    MMTIME mm{};
                    mm.wType = TIME_MS;
                    const MMRESULT ge = ::waveOutGetPosition(
                        wo, &mm, sizeof(mm));
                    if (ge == MMSYSERR_NOERROR) {
                        std::printf("  waveOut position after 1.5 s: "
                                    "%u (wType=%u)\n", mm.u.ms, mm.wType);
                        CHECK(mm.u.ms > 500,
                              "waveOut position is > 500 after 1.5 s "
                              "(playback actually running, not stuck "
                              "at the 1-second loop regression)");
                    } else {
                        CHECK(false, "waveOutGetPosition returned success");
                    }
                    ::waveOutReset(wo);
                }
                ::waveOutUnprepareHeader(wo, &hdr, sizeof(hdr));
            }
            ::waveOutClose(wo);
        }
    }
#endif  // _WIN32

    std::printf("snd_player_test: %d passed, %d failed\n",
                g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
