// SndPreviewPlayer — SharpEmu-style preview-music player.
//
// Plays the default music shipped with the selected PS5 title in a loop,
// exactly like the PS5 home screen plays `sce_sys/snd0.at9` while a
// tile is highlighted.  SharpEmu calls this `SndPreviewPlayer`; we
// keep the same name so it's easy to compare implementations.
//
// Decoding strategy (matches the file on disk):
//   * .at9 / .at3  — Sony ATRAC9 (vendored LibAtrac9 C port), decoded
//                    in a worker thread to a PCM16 WAV image held in
//                    memory, then handed to winmm `PlaySound` via
//                    `SND_MEMORY | SND_ASYNC | SND_LOOP | SND_NODEFAULT`.
//   * .wav         — played directly from disk via `SND_FILENAME`.
//   * .ogg/.mp3/
//     .flac/.opus  — passed to `SND_FILENAME` too; Windows' DirectShow
//                    codec chain handles them on most installs (if
//                    playback fails we fall back to silent, never crash).
//
// Threading model
//   * `Play(path)` is non-blocking.  It bumps a generation counter
//     (cancelling any in-flight decode) and schedules decode + start
//     onto a background thread.  The UI thread is never blocked.
//   * A 300 ms debounce timer suppresses churn while the user skims
//     the library grid with the mouse/keyboard (SharpEmu does the same).
//   * `Stop()` and `Pause()`/`Resume()` are immediate and thread-safe.
//   * On destruction everything is stopped and the worker joined.
//
// Platform
//   * Currently Windows-only because we lean on winmm `PlaySound`.  All
//     entry points are no-ops on other platforms so a future Linux/macOS
//     port can swap in OpenAL or SDL_mixer without changing callers.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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

private:
    void WorkerLoop();
    void EnqueueDecode(int generation, std::string path);
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

    // Pinned memory for winmm playback.  Must stay alive for as long
    // as PlaySound is using it.
    void*                           pinned_addr_  = nullptr;
    bool                            pinned_allocated_ = false;
    std::vector<std::uint8_t>       pinned_wav_;   // owns the storage

    // Last-started in-memory WAV (so Pause/Resume can re-Play it).
    std::vector<std::uint8_t>       last_started_wav_;

    bool                            playing_  = false;
    bool                            paused_   = false;

    std::atomic<bool>               enabled_{true};
};

}  // namespace Ui
