#include "log.h"
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX // keep std::min / std::max from clashing with the Win32 macros
#include <windows.h>
#endif

namespace {

// ---------------------------------------------------------------------------
// Process start time used to compute LogEntry::timestamp_us.
// ---------------------------------------------------------------------------
const std::chrono::steady_clock::time_point kProcessStart =
    std::chrono::steady_clock::now();

// ---------------------------------------------------------------------------
// Configuration (read on every log call so they can be changed at runtime).
// ---------------------------------------------------------------------------
std::atomic<bool>     g_json_output{false};
std::atomic<bool>     g_file_active{false};
std::mutex            g_file_mutex;
std::ofstream         g_file_stream;
std::string           g_file_path;
LogLevel              g_min_levels[7] = {
    LogLevel::Info, // Loader
    LogLevel::Info, // Memory
    LogLevel::Info, // Kernel
    LogLevel::Info, // HLE
    LogLevel::Info, // GPU
    LogLevel::Info, // Cpu
    LogLevel::Info, // General
};

// ---------------------------------------------------------------------------
// Ring buffer.  Fixed-size circular array of LogEntry.  We snapshot the
// buffer under a lock on demand for the crash report.  Capacity is a power
// of two for cheap wrapping.
// ---------------------------------------------------------------------------
constexpr size_t kRingCapacity = 1024;
struct RingBuffer {
    mutable std::mutex mutex; // mutable so const Snapshot can lock it
    LogEntry        entries[kRingCapacity];
    size_t          write_index = 0; // next slot to write
    size_t          total_writes = 0; // for time-since-first computations

    void Push(const LogEntry& e) {
        std::lock_guard<std::mutex> lock(mutex);
        entries[write_index] = e;
        write_index = (write_index + 1) & (kRingCapacity - 1);
        ++total_writes;
    }

    std::vector<LogEntry> Snapshot(size_t max_count) const {
        std::lock_guard<std::mutex> lock(mutex);
        const size_t valid = std::min(total_writes, kRingCapacity);
        const size_t n = std::min(max_count, valid);
        std::vector<LogEntry> out;
        out.reserve(n);
        // Walk backwards from the most recent entry, up to `n` items.
        for (size_t i = 0; i < n; ++i) {
            const size_t slot = (write_index + kRingCapacity - 1 - i) & (kRingCapacity - 1);
            out.push_back(entries[slot]);
        }
        // Reverse so the output is in chronological order.
        std::reverse(out.begin(), out.end());
        return out;
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& e : entries) e = LogEntry{};
        write_index = 0;
        total_writes = 0;
    }
} g_ring;

} // namespace

u64 ProcessUptimeMicros() {
    auto now = std::chrono::steady_clock::now();
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::microseconds>(now - kProcessStart).count());
}

// ---------------------------------------------------------------------------
// Stringification helpers
// ---------------------------------------------------------------------------
const char* LogCategoryName(LogCategory c) {
    switch (c) {
        case LogCategory::Loader:  return "Loader";
        case LogCategory::Memory:  return "Memory";
        case LogCategory::Kernel:  return "Kernel";
        case LogCategory::HLE:     return "HLE";
        case LogCategory::GPU:     return "GPU";
        case LogCategory::Cpu:     return "Cpu";
        case LogCategory::General: return "General";
    }
    return "Unknown";
}

const char* LogLevelName(LogLevel l) {
    switch (l) {
        case LogLevel::Trace:    return "Trace";
        case LogLevel::Debug:    return "Debug";
        case LogLevel::Info:     return "Info";
        case LogLevel::Warn:     return "Warn";
        case LogLevel::Error:    return "Error";
        case LogLevel::Critical: return "Critical";
    }
    return "Unknown";
}

const char* LevelAnsiColor(LogLevel l) {
    switch (l) {
        case LogLevel::Trace:    return "\033[90m"; // Bright Black (Dark Gray)
        case LogLevel::Debug:    return "\033[37m"; // White
        case LogLevel::Info:     return "\033[36m"; // Cyan
        case LogLevel::Warn:     return "\033[33m"; // Yellow
        case LogLevel::Error:    return "\033[31m"; // Red
        case LogLevel::Critical: return "\033[35m"; // Magenta
    }
    return "\033[0m";
}

bool IsCategoryEnabled(LogCategory c, LogLevel l) {
    const int idx = static_cast<int>(c);
    if (idx < 0 || idx >= 7) return true;
    // "Suppress messages below `level`" (see LogConfig::SetLevel): a record
    // is emitted iff its severity is at or above the category minimum.
    return static_cast<int>(l) >= static_cast<int>(g_min_levels[idx]);
}

// Build a JSON object for a single entry.  Strings are escaped.
std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

std::string FormatJson(const LogEntry& e) {
    std::ostringstream os;
    os << "{\"t\":" << e.timestamp_us
       << ",\"c\":\"" << LogCategoryName(e.category) << "\""
       << ",\"l\":\"" << LogLevelName(e.level) << "\""
       << ",\"file\":\"" << JsonEscape(e.file) << "\""
       << ",\"line\":" << e.line
       << ",\"func\":\"" << JsonEscape(e.function) << "\""
       << ",\"msg\":\"" << JsonEscape(e.message) << "\"}";
    return os.str();
}

std::string FormatAnsi(const LogEntry& e) {
    std::ostringstream os;
    os << LevelAnsiColor(e.level)
       << "[" << LogCategoryName(e.category) << "][" << LogLevelName(e.level) << "] "
       << e.message
       << "\033[0m";
    return os.str();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
namespace LogConfig {
void SetJsonOutput(bool enabled) {
    g_json_output = enabled;
}

void SetFileOutput(const std::string& path, bool append) {
    std::lock_guard<std::mutex> lock(g_file_mutex);
    if (g_file_stream.is_open()) g_file_stream.close();
    g_file_path = path;
    if (path.empty()) {
        g_file_active = false;
        return;
    }
    g_file_stream.open(path, append ? std::ios::app : std::ios::trunc);
    g_file_active = g_file_stream.is_open();
}

void SetLevel(LogCategory category, LogLevel level) {
    const int idx = static_cast<int>(category);
    if (idx < 0 || idx >= 7) return;
    g_min_levels[idx] = level;
}

LogLevel GetLevel(LogCategory category) {
    const int idx = static_cast<int>(category);
    if (idx < 0 || idx >= 7) return LogLevel::Info;
    return g_min_levels[idx];
}
} // namespace LogConfig

std::vector<LogEntry> GetRecentLogEntries(size_t max_count) {
    return g_ring.Snapshot(max_count);
}

void ClearRecentLogEntries() {
    g_ring.Clear();
}

void LogMessageRaw(LogCategory category, LogLevel level,
                   const char* file, int line, const char* function,
                   const char* format, ...) {
    if (!IsCategoryEnabled(category, level)) return;

    LogEntry e;
    e.timestamp_us = ProcessUptimeMicros();
    e.category     = category;
    e.level        = level;
    if (file)     e.file     = file;
    e.line        = line;
    if (function) e.function = function;

    char buffer[2048];
    va_list args;
    va_start(args, format);
    int n = std::vsnprintf(buffer, sizeof(buffer), format ? format : "", args);
    va_end(args);
    if (n < 0) n = 0;
    if (static_cast<size_t>(n) >= sizeof(buffer)) n = sizeof(buffer) - 1;
    e.message.assign(buffer, static_cast<size_t>(n));

    // Record into the ring buffer before writing to stdout / file so the
    // crash report always sees the most recent message.
    g_ring.Push(e);

    // Stdout
    if (g_json_output.load(std::memory_order_relaxed)) {
        std::string json_line = FormatJson(e);
        std::printf("%s\n", json_line.c_str());
    } else {
        std::string ansi_line = FormatAnsi(e);
        std::printf("%s\n", ansi_line.c_str());
    }
    std::fflush(stdout);

    // Optional file mirror
    if (g_file_active.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lock(g_file_mutex);
        if (g_file_stream.is_open()) {
            // Always write plain text to the file (so editors / greps work),
            // even when JSON mode is on for stdout.
            g_file_stream << "[" << e.timestamp_us << "]["
                          << LogCategoryName(category) << "]["
                          << LogLevelName(level) << "] " << e.message << "\n";
            g_file_stream.flush();
        }
    }
}
