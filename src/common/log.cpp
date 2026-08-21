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
#include <unordered_map>
#include <filesystem>

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
LogConfig::LogCallback g_log_callback = nullptr;
void*                 g_log_callback_user = nullptr;
LogLevel              g_min_levels[7] = {
    LogLevel::Info,  // Loader
    LogLevel::Info,  // Memory
    LogLevel::Info,  // Kernel
    LogLevel::Debug, // HLE
    LogLevel::Info,  // GPU
    LogLevel::Info,  // Cpu
    LogLevel::Info,  // General
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

// ---------------------------------------------------------------------------
// Dedup state — collapses repeated identical messages (category + level +
// message text) into one line with a "(xN)" counter.  Applied to stdout /
// file / callback sinks only; the ring buffer always sees every raw event.
// Guarded by g_dedup_mutex.  Locks are released before EmitToSinks so the
// callback sink never runs holding an internal lock (log.h contract).
// ---------------------------------------------------------------------------
std::atomic<bool> g_dedup_enabled{true};
std::atomic<bool> g_dedup_never_on{false};   // fast path for SetDedup(false)
std::mutex        g_dedup_mutex;
u64               g_dedup_window_us = 1'000'000; // default 1 s
LogLevel          g_dedup_max_level = LogLevel::Critical; // dedup everything
bool              g_dedup_category_mask[7] = {true,true,true,true,true,true,true};

// Composite key: "<cat>|<level>|<message>".  Catches identical formatted text.
struct DedupPending {
    std::string  key;
    std::string  message;        // original formatted text (for the annotation)
    LogCategory  category = LogCategory::General;
    LogLevel     level    = LogLevel::Info;
    const char*  file     = nullptr;  // __FILE__ literal — static storage
    int          line     = 0;
    const char*  function = nullptr;  // __FUNCTION__ literal — static storage
    u64          first_ts_us = 0;
    u64          total       = 0;     // cumulative occurrences of THIS key
    bool         valid       = false;
};
std::unordered_map<std::string, u64> g_dedup_totals; // counts for non-pending keys
DedupPending g_dedup_pending;
constexpr size_t kMaxDedupKeys = 16384;

static bool DedupAppliesTo(LogCategory cat, LogLevel lvl) {
    const int idx = static_cast<int>(cat);
    if (idx < 0 || idx >= 7) return true;
    return g_dedup_category_mask[idx] && static_cast<int>(lvl) <= static_cast<int>(g_dedup_max_level);
}

static LogEntry MakeFlushEntry(const DedupPending& pending, u64 now_us) {
    LogEntry e;
    e.timestamp_us = now_us;
    e.category     = pending.category;
    e.level        = pending.level;
    if (pending.file)     e.file     = pending.file;
    e.line         = pending.line;
    if (pending.function) e.function = pending.function;
    e.message      = pending.message;
    if (pending.total > 1) {
        e.message += " (x" + std::to_string(pending.total) + ")";
    }
    return e;
}

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
        case LogCategory::Media:   return "Media";
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

// Forward-declared: FlushDedup (below) calls EmitToSinks, but EmitToSinks is
// defined after LogConfig to keep the sink block near LogMessageRaw.
static void EmitToSinks(const LogEntry& e);

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

    std::string target_path = path;
    bool is_absolute = (path.size() >= 2 && path[1] == ':') ||
                       (path.size() >= 1 && (path[0] == '/' || path[0] == '\\'));
    if (!is_absolute) {
        std::error_code ec;
        std::filesystem::create_directories("logs", ec);
        target_path = "logs/" + path;
    }

    g_file_stream.open(target_path, append ? std::ios::app : std::ios::trunc);
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

void SetLogCallback(LogCallback callback, void* user) {
    g_log_callback = callback;
    g_log_callback_user = user;
}

void SetDedup(bool enabled) {
    g_dedup_enabled.store(enabled, std::memory_order_relaxed);
    g_dedup_never_on.store(!enabled, std::memory_order_relaxed);
}

void SetDedupWindow(u64 microseconds) {
    g_dedup_window_us = microseconds;
}

void SetDedupMaxLevel(LogLevel level) {
    g_dedup_max_level = level;
}

void SetDedupCategory(LogCategory c, bool enabled) {
    const int idx = static_cast<int>(c);
    if (idx >= 0 && idx < 7) g_dedup_category_mask[idx] = enabled;
}

void FlushDedup() {
    LogEntry flush_entry;
    {
        std::lock_guard<std::mutex> lock(g_dedup_mutex);
        if (!g_dedup_pending.valid) return;
        flush_entry = MakeFlushEntry(g_dedup_pending, ProcessUptimeMicros());
        g_dedup_pending.valid = false;
        g_dedup_totals.erase(g_dedup_pending.key);
    }
    if (flush_entry.timestamp_us) EmitToSinks(flush_entry);
}
} // namespace LogConfig

std::vector<LogEntry> GetRecentLogEntries(size_t max_count) {
    return g_ring.Snapshot(max_count);
}

void ClearRecentLogEntries() {
    g_ring.Clear();
}

// Emit a single entry to all sinks: stdout (ANSI/JSON), file mirror, callback.
// Must be callable WITHOUT any internal lock held — the callback sink may
// call back into LogMessageRaw (log.h contract).
static void EmitToSinks(const LogEntry& e) {
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
            g_file_stream << "[" << e.timestamp_us << "]["
                          << LogCategoryName(e.category) << "]["
                          << LogLevelName(e.level) << "] " << e.message << "\n";
            g_file_stream.flush();
        }
    }

    // Optional callback sink (in-process host console panel)
    if (g_log_callback) {
        g_log_callback(static_cast<int>(e.level), static_cast<int>(e.category),
                       e.message.c_str(), g_log_callback_user);
    }
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

    // Record into the ring buffer BEFORE any dedup gate — the crash report
    // always sees every raw event regardless of suppression.
    g_ring.Push(e);

    // Critical messages bypass the gate and flush any pending annotation
    // first, so crash markers are never delayed.
    if (level == LogLevel::Critical)
        LogConfig::FlushDedup();

    // Dedup gate — collapse repeated identical messages.
    if (g_dedup_enabled.load(std::memory_order_relaxed) && DedupAppliesTo(category, level)) {
        LogEntry flush_entry;
        bool     suppress_current = false;
        {
            std::lock_guard<std::mutex> lock(g_dedup_mutex);
            const u64 now = ProcessUptimeMicros();
            std::string key = std::to_string(static_cast<int>(category)) + "|" +
                              std::to_string(static_cast<int>(level)) + "|" + e.message;

            u64& total = g_dedup_totals[key];
            if (total == 0 && g_dedup_totals.size() >= kMaxDedupKeys) {
                g_dedup_totals.clear();
                total = g_dedup_totals[key];
            }
            ++total;

            if (g_dedup_pending.valid && g_dedup_pending.key != key) {
                // Key change: flush the PREVIOUS message with its cumulative count.
                flush_entry = MakeFlushEntry(g_dedup_pending, now);
                g_dedup_pending = DedupPending{ key, e.message, category, level,
                                                file, line, function, now, total, true };
                suppress_current = true;
            } else if (g_dedup_pending.valid) {
                // Same key: bump count, maybe flush on window expiry.
                g_dedup_pending.total = total;
                if (now - g_dedup_pending.first_ts_us >= g_dedup_window_us && total > 1) {
                    flush_entry = MakeFlushEntry(g_dedup_pending, now);
                    g_dedup_pending.first_ts_us = now; // restart the window
                }
                suppress_current = true;
            } else {
                // First distinct message: hold pending, do not emit yet.
                g_dedup_pending = DedupPending{ key, e.message, category, level,
                                                file, line, function, now, total, true };
                suppress_current = true;
            }
        } // lock released — callback/sinks never run under g_dedup_mutex

        if (flush_entry.timestamp_us) EmitToSinks(flush_entry);
        if (suppress_current) return;
    }
    EmitToSinks(e);
}
