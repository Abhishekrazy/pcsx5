#pragma once
#include "../common/types.h"
#include <cstdio>
#include <chrono>
#include <string>
#include <vector>

enum class LogCategory {
    Loader,
    Memory,
    Kernel,
    HLE,
    GPU,
    Cpu,
    Media,
    General
};

enum class LogLevel {
    Trace,    // Extremely verbose; per-instruction / per-call detail
    Debug,    // Developer diagnostic; may be noisy
    Info,     // Normal operational messages
    Warn,     // Recoverable issues; something is suboptimal
    Error,    // Hard failures in a subsystem; emulation may continue
    Critical  // Fatal condition; the process cannot safely continue
};

// One captured log record.  Used by the diagnostics ring buffer so a crash
// report can include the last N messages leading up to the failure.
struct LogEntry {
    u64                timestamp_us = 0;   // microseconds since process start
    LogCategory        category      = LogCategory::General;
    LogLevel           level         = LogLevel::Info;
    std::string        file;              // source file (empty if not provided)
    int                line         = 0;  // source line (0 if not provided)
    std::string        function;          // source function (empty if not provided)
    std::string        message;
};

// ---------------------------------------------------------------------------
// Public configuration
// ---------------------------------------------------------------------------
namespace LogConfig {
    // Emit one JSON object per log line to stdout.  Useful for tooling.
    void SetJsonOutput(bool enabled);

    // Mirror log lines to a text file.  Empty path disables file logging.
    // The file is opened in truncate mode unless `append` is true.
    void SetFileOutput(const std::string& path, bool append = false);

    // Suppress messages below `level` for the given category.  Use
    // LogLevel::Trace to enable absolutely everything, or
    // LogLevel::Critical to only see fatal conditions.
    void SetLevel(LogCategory category, LogLevel level);
    LogLevel GetLevel(LogCategory category);

    // Callback sink for in-process hosts (the core DLL embedded in the WPF
    // app): every emitted record is forwarded to `callback` in addition to
    // the stdout/file sinks.  Level/category are passed as their plain enum
    // int values so the ABI stays C-friendly.  The callback runs on the
    // logging thread without any internal locks held; `user` is passed
    // through unchanged.  Null callback disables the sink.
    using LogCallback = void (*)(int level, int category, const char* message, void* user);
    void SetLogCallback(LogCallback callback, void* user);
}

// Stringification helpers used by diagnostics, the test harness, and the
// crash-report writer.  Defined in log.cpp.
const char* LogCategoryName(LogCategory c);
const char* LogLevelName(LogLevel l);

// ---------------------------------------------------------------------------
// Ring buffer access (used by diagnostics)
// ---------------------------------------------------------------------------
std::vector<LogEntry> GetRecentLogEntries(size_t max_count = 1024);
void ClearRecentLogEntries();

// ---------------------------------------------------------------------------
// Emit a structured log record.
//
// The LOG_INFO / LOG_WARN / LOG_ERROR / LOG_DEBUG macros below pass
// __FILE__ / __LINE__ / __FUNCTION__ so the resulting LogEntry contains the
// full diagnostic context.  Other callers can use LogMessageRaw instead.
// ---------------------------------------------------------------------------
void LogMessageRaw(LogCategory category, LogLevel level,
                   const char* file, int line, const char* function,
                   const char* format, ...);

// ---------------------------------------------------------------------------
// Convenience macros.  Note that we deliberately swallow a missing variadic
// arg for zero-arg calls (##__VA_ARGS__ is a GCC/Clang extension); MSVC
// also accepts it via the /Zc:preprocessor flag.
// ---------------------------------------------------------------------------
#define LOG_TRACE(cat, fmt, ...)   ::LogMessageRaw(::LogCategory::cat, ::LogLevel::Trace,    __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(cat, fmt, ...)   ::LogMessageRaw(::LogCategory::cat, ::LogLevel::Debug,    __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__)
#define LOG_INFO(cat, fmt, ...)    ::LogMessageRaw(::LogCategory::cat, ::LogLevel::Info,     __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__)
#define LOG_WARN(cat, fmt, ...)    ::LogMessageRaw(::LogCategory::cat, ::LogLevel::Warn,     __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__)
#define LOG_ERROR(cat, fmt, ...)   ::LogMessageRaw(::LogCategory::cat, ::LogLevel::Error,    __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__)
#define LOG_CRITICAL(cat, fmt, ...)::LogMessageRaw(::LogCategory::cat, ::LogLevel::Critical, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__)

// ---------------------------------------------------------------------------
// Process start time.  Use ProcessUptimeMicros() to add a monotonic baseline
// to LogEntry::timestamp_us.
// ---------------------------------------------------------------------------
u64 ProcessUptimeMicros();
