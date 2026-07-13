#pragma once

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace Ui {

// Detachable log console.  Owns a ring buffer of log lines plus a free-text
// filter.  Thread-safe: the process watcher pushes lines from a background
// thread; the main thread renders them.
//
// Designed to be drawn from inside a dock host or as its own OS window.
class LogConsole {
public:
    static constexpr std::size_t kMaxLines = 10000;

    void Append(const std::string& line);
    void Append(const char* data, std::size_t n);

    void Clear();

    // Renders the console in the current ImGui window.  Returns true if
    // `detached` changed state (so the caller can spawn/teardown an OS
    // window).
    bool Draw(const char* title, bool* p_open = nullptr);

    bool detached() const { return detached_; }
    void set_detached(bool v) { detached_ = v; }

private:
    std::mutex                              mu_;
    std::deque<std::string>                 lines_;
    char                                    filter_[128] = {};
    bool                                    autoscroll_  = true;
    bool                                    word_wrap_   = true;
    bool                                    detached_    = false;
};

}  // namespace Ui
