// pcsx5 desktop UI — Dear ImGui + GLFW + OpenGL3 backend.
//
// Public API:  a single `RunUi(int argc, char** argv)` function in `ui.h`.
// Internally split into:
//   * ui.{h,cpp}     — GLFW/GL/ImGui lifecycle, main window
//   * ui_scan.cpp    — ScanGames + LocateBackend (no ImGui dep, testable)
//   * library.{h,cpp}— game library panel (scans Games/, uses Compat DB)
//   * console.{h,cpp}— detachable log console panel
//   * process.{h,cpp}— child-process launcher that pipes stdout/stderr
//
// The UI is *out of process* with the emulator: it spawns `pcsx5.exe`
// (or `pcsx5` on POSIX) and pipes its log into the in-app console.

#include "ui/ui.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace Ui {

// Top-level entry point.  Returns 0 on normal exit, non-zero on error.
int  RunUi(int argc, char** argv, const Options& opts);

}  // namespace Ui

// ---------------------------------------------------------------------------
// main: thin wrapper that just delegates to Ui::RunUi
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    return Ui::RunUi(argc, argv, Ui::Options{});
}
