// SndPreviewPlayer — SharpEmu-style preview-music player.
//
// Plays the default music shipped with the selected PS5 title in a loop,
// exactly like the PS5 home screen plays `sce_sys/snd0.at9` while a
// tile is highlighted.  SharpEmu calls this `SndPreviewPlayer`; we
// keep the same name so it's easy to compare implementations.
//
// Decoding strategy (matches the file on disk):
//   * .at9 / .at3  — Sony ATRAC9 (vendored LibAtrac9 C port), decoded
//                    in a worker thread to a PCM16 WAV image written
//                    to a temp file in %TEMP%, then played via winmm
//                    MCI (`mciSendStringW`) with `repeat` for looping.
//   * .wav         — played directly from disk via MCI `open ... type
//                    waveaudio`.
//   * .ogg/.mp3/
//     .flac/.opus  — also routed through MCI; on most Windows installs
//                    MCI uses DirectShow for non-WAV containers.
//
// Why MCI and not `PlaySound`?
//   `PlaySound(SND_MEMORY | SND_LOOP)` silently truncates buffers
//   above ~256KB-1MB and then loops the truncated chunk — the
//   decoded 15MB preview WAV kept looping after ~1 second.  MCI
//   streams the file properly with no practical size limit and
//   supports real looping via the `repeat` flag.
//
// Threading model
//   * `Play(path)` is non-blocking.  It bumps a generation counter
//     (cancelling any in-flight decode) and schedules decode + start
//     onto a background thread.  The UI thread is never blocked.
//   * A 300 ms debounce timer suppresses churn while the user skims
//     the library grid with the mouse/keyboard (SharpEmu does the same).
//   * `Stop()` and `Pause()`/`Resume()` are immediate and thread-safe.
//   * On destruction everything is stopped, the worker joined, and
//     any temp file is deleted.
//
// Platform
//   * Currently Windows-only because we lean on winmm MCI.  All entry
//     points are no-ops on other platforms so a future Linux/macOS
//     port can swap in OpenAL or SDL_mixer without changing callers.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
// The waveOut path uses HWAVEOUT and WAVEHDR as value members, so
// the full definitions must be visible here.  <mmsystem.h> pulls in
// the mmeapi.h types we need without pulling in the rest of the
// <windows.h> surface (we use only a tiny subset in this header).
#include <windows.h>
#include <mmsystem.h>
#endif

namespace Ui {

class SndPreviewPlayer {
public:
    SndPreviewPlayer();
    ~SndPreviewPlayer();

    SndPreviewPlayer(const SndPreviewPlayer&)            = delete;
    SndPreviewPlayer& operator=(const SndPreviewPlayer&) = delete;

    /// Begin looping the given music file after a 300 ms debounce.  If
    /// the path is empty or the file does not exist, the call is a
    /// no-op.  Calling Play again with a different path cancels the
    /// previous in-flight decode (if any) and starts the new one.
    void Play(const std::string& music_path);

    /// Stop playback immediately and cancel any in-flight decode.
    void Stop();

    /// Silences playback but keeps the decoded track cached, so
    /// `Resume()` can restart it (winmm cannot truly pause).
    void Pause();
    void Resume();

    /// Globally mute/unmute.  When muted, Play() will not start
    /// playback.  Useful for the sidebar "Title Music" toggle.
    void SetEnabled(bool enabled);
    bool enabled() const { return enabled_.load(std::memory_order_relaxed); }

    /// True if a track is currently being played.
    bool playing() const;

    /// Path of the most recently requested (or currently playing)
    /// track.  Empty if nothing has been requested yet.
    const std::string& current_path() const;

    /// Decode an AT9 file to an in-memory PCM16 WAV image.  Returns
    /// an empty vector on any error (bad header, unsupported codec,
    /// decode failure).  Exposed for unit tests and the
    /// `pcsx5_snd_decode` CLI tool.
    static std::vector<std::uint8_t> DecodeAt9ToWav(const std::string& path);

    /// Decode an OGG Vorbis file to an in-memory PCM16 WAV image.
    /// Uses the vendored stb_vorbis single-header decoder.  Returns
    /// an empty vector on any error.  Exposed for unit tests.
    static std::vector<std::uint8_t> DecodeOggToWav(const std::string& path);

private:
    void WorkerLoop();
    void EnqueueDecode(int generation, std::string path);
#ifdef _WIN32
    // waveOut path for raw PCM (decoded .at9 or .wav).  The callback
    // re-submits the buffer on WOM_DONE so we get seamless looping
    // without polling.
    void StartWaveOut(const std::vector<std::uint8_t>& wav);
    void StopWaveOut();
    void PauseWaveOut();
    void ResumeWaveOut();
    static void CALLBACK WaveOutProc(HWAVEOUT hwo, UINT uMsg,
                                     DWORD_PTR dwInstance,
                                     DWORD_PTR dwParam1,
                                     DWORD_PTR dwParam2);
    // MCI path for formats waveOut can't handle natively (currently
    // just MP3 via the `mpegvideo` device).  OGG/FLAC/OPUS have no
    // native MCI support and are skipped.
    bool StartMciForPath(const std::string& path);
    void StopMci();
    // Periodically poll MCI mode and re-issue play on natural end.
    void LoopThread();
#endif
    void StartLocked(const std::vector<std::uint8_t>& wav);
    void StopLocked();
    void FreePinnedLocked();

    // State guarded by `mu_`.
    mutable std::mutex             mu_;
    std::condition_variable        cv_;
    std::thread                    worker_;
    bool                           stop_worker_ = false;

    // Generation counter — bumped on every Play()/Stop() so the worker
    // can detect it has been cancelled mid-decode.
    std::atomic<int>                generation_{0};

    // Requested path; the worker only acts on the latest.
    std::string                     requested_path_;
    bool                            requested_pending_ = false;

    // Cached decoded WAV for the most recently played path.  Avoids
    // re-decoding when the user re-selects the same game.
    std::string                     cached_path_;
    std::vector<std::uint8_t>       cached_wav_;

    // Pinned memory for winmm playback.  Kept around in case a future
    // change reintroduces the in-memory path, but with MCI the
    // playback goes through `temp_wav_path_` instead.
    void*                           pinned_addr_  = nullptr;
    bool                            pinned_allocated_ = false;
    std::vector<std::uint8_t>       pinned_wav_;   // legacy

    // Last-started in-memory WAV.  Stashed so Pause/Resume can re-emit
    // it to a fresh temp file if the user toggles playback.
    std::vector<std::uint8_t>       last_started_wav_;

    // On-disk temp WAV.  MCI needs a path; we write the decoded bytes
    // here once and `open type waveaudio` it.  No size limit.  The
    // path is stable for the lifetime of the cache entry, so the file
    // is reused on re-selection of the same game.  Deleted in
    // StopLocked() / destructor.
    std::string                     temp_wav_path_;

    // True when an MCI alias is currently open.  Distinguishes "never
    // played" from "playback ended naturally" so Pause/Resume behave
    // correctly across cache hits.
    bool                            mci_open_      = false;

#ifdef _WIN32
    // waveOut path: a single HWAVEOUT + WAVEHDR for the entire PCM
    // payload.  Looping is done by re-submitting the header in the
    // WOM_DONE callback.  This is the reliable path — it doesn't
    // depend on MCI's `play ... repeat` (rejected for waveaudio) or
    // on a polling loop thread.
    HWAVEOUT                        wave_out_      = nullptr;
    WAVEHDR                         wave_hdr_{};
    std::vector<std::uint8_t>       wave_data_;   // owns PCM bytes
    std::atomic<bool>               wave_active_{false};

    // MCI loop thread — only used for the `mpegvideo` device (MP3).
    // waveOut handles its own looping via the WOM_DONE callback.
    std::thread                     loop_thread_;
    std::atomic<bool>               loop_active_{false};
#endif

    bool                            playing_  = false;
    bool                            paused_   = false;

    std::atomic<bool>               enabled_{true};
};

}  // namespace Ui
