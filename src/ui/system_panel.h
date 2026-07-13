// System information panel — Kyty-style "About my PC" view rendered
// inside the main content area when the user selects the "System"
// view mode in the sidebar.
//
// The static facts (CPU brand, GPU name, OS version) are loaded once
// when the panel first appears; the user can re-query them with the
// "Refresh" button.  Memory usage is sampled continuously and
// redrawn every frame (it's a single cheap syscall on every
// platform we support).

#pragma once

#include "system/system.h"

#include <string>

namespace Ui {

// Holds the cached system snapshot.  Pass an instance to
// DrawSystemPanel() and let the panel call Refresh() when the user
// hits the refresh button.
class SystemPanelState {
public:
    SystemPanelState() = default;

    // Force a re-query of the static facts (CPU/GPU/OS).
    void Refresh();
    // Resample memory usage (cheap; safe to call every frame).
    void SampleMemory();

    Sys::SystemInfo  info;
    Sys::MemorySample mem;
    bool             has_info = false;
    bool             has_mem  = false;
    std::string      last_error;
};

// Draw the system panel inside the current ImGui window / child.
// Caller is expected to have already pushed font / padding styles.
void DrawSystemPanel(SystemPanelState& s);

}  // namespace Ui
