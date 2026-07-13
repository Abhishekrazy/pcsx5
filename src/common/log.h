#pragma once

enum class LogCategory {
    Loader,
    Memory,
    Kernel,
    HLE,
    GPU,
    General
};

enum class LogLevel {
    Info,
    Warn,
    Error,
    Debug
};

void LogMessage(LogCategory category, LogLevel level, const char* format, ...);

#define LOG_INFO(cat, fmt, ...)  LogMessage(LogCategory::cat, LogLevel::Info, fmt, ##__VA_ARGS__)
#define LOG_WARN(cat, fmt, ...)  LogMessage(LogCategory::cat, LogLevel::Warn, fmt, ##__VA_ARGS__)
#define LOG_ERROR(cat, fmt, ...) LogMessage(LogCategory::cat, LogLevel::Error, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(cat, fmt, ...) LogMessage(LogCategory::cat, LogLevel::Debug, fmt, ##__VA_ARGS__)
