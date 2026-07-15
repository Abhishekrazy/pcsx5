// pcsx5 desktop UI — Dear ImGui + GLFW + OpenGL3 backend.
//
// Public API:  a single `RunUi(int argc, char** argv)` function in `ui.h`.
// Internally split into:
//   * ui.{h,cpp}     — GLFW/GL/ImGui lifecycle, main window, docking
//   * library.{h,cpp}— game library panel (scans Games/, uses Compat DB)
//   * console.{h,cpp}— detachable log console panel
//   * process.{h,cpp}— child-process launcher that pipes stdout/stderr
//
// The UI is *out of process* with the emulator: it spawns `pcsx5.exe`
// (or `pcsx5` on POSIX) and pipes its log into the in-app console.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Ui {

// CLI options consumed before the main loop.
struct Options {
    std::string games_dir   = "./Games";
    std::string compat_dir  = "./compat";
    std::string covers_dir  = "./Covers";   // PNG/JPG cover art per title_id
    std::string backend_bin = "pcsx5";     // name on POSIX; "pcsx5.exe" on Win
    std::string language    = "en-US";     // BCP-47 tag: "en-US", "ja-JP", etc.
};

// Pre-scan of `games_dir`.  Each entry is a directory under games_dir that
// contains an `eboot.bin` (or `eboot.bin.esbak`).
struct GameEntry {
    std::string title_id;       // derived from directory name (PPSAxxxxx)
    std::string display_name;   // human-readable name from param.json, or dir
    std::string elf_path;       // full path to eboot.bin(.esbak)
    std::string status_text;    // "untested" | "intro" | ... | "missing"
    std::string dir_path;       // absolute path to the game directory
    std::uint64_t size_bytes = 0;   // total install size of the directory
    std::string cover_path;     // absolute path to cover art ("" if none)
    std::string music_path;     // default-music candidate inside the game
    std::string background_path;// large hero / background image (pic0/1/2)
};

// Discover all games on disk.  Sorted by title_id.  Each entry is enriched
// with the total install size (recursive walk) and a best-effort pointer
// to its cover art (looked up in `covers_dir`).
std::vector<GameEntry> ScanGames(const std::string& games_dir,
                                 const std::string& covers_dir = "");

// Find the backend binary (pcsx5.exe) by searching relative to the UI exe
// and the current working directory.  Returns "" if not found.
std::string LocateBackend(const std::string& hint);

// Resolve the cover art path for a title id.  Looks in `covers_dir` for
// `<title_id>.png`, `.jpg`, `.jpeg`, `.webp` (case-insensitive).  Returns
// "" when no cover is present.
std::string ResolveCoverPath(const std::string& title_id,
                             const std::string& covers_dir);

// Top-level entry point.  Returns 0 on normal exit, non-zero on error.
int  RunUi(int argc, char** argv, const Options& opts);

}  // namespace Ui
