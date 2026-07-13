#include "log.h"
#include <cstdio>
#include <cstdarg>
#include <mutex>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#endif

static std::mutex g_log_mutex;

static const char* GetCategoryName(LogCategory category) {
    switch (category) {
        case LogCategory::Loader:  return "Loader";
        case LogCategory::Memory:  return "Memory";
        case LogCategory::Kernel:  return "Kernel";
        case LogCategory::HLE:     return "HLE   ";
        case LogCategory::GPU:     return "GPU   ";
        case LogCategory::General: return "Gen   ";
        default:                   return "Unknown";
    }
}

void LogMessage(LogCategory category, LogLevel level, const char* format, ...) {
    std::lock_guard<std::mutex> lock(g_log_mutex);

    // Setup color codes for terminal
#ifdef _WIN32
    // Basic console color setup for Windows if VT100 is not enabled, 
    // but modern Windows terminals support ANSI escape sequences out of the box.
    // We'll use ANSI escape sequences for simplicity and modern styling.
#endif

    const char* color_code = "\033[0m"; // Reset
    const char* level_name = "INFO";

    switch (level) {
        case LogLevel::Info:
            color_code = "\033[36m"; // Cyan
            level_name = "INFO";
            break;
        case LogLevel::Warn:
            color_code = "\033[33m"; // Yellow
            level_name = "WARN";
            break;
        case LogLevel::Error:
            color_code = "\033[31m"; // Red
            level_name = "ERRO";
            break;
        case LogLevel::Debug:
            color_code = "\033[90m"; // Dark Gray
            level_name = "DBUG";
            break;
    }

    char buffer[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    // Output formatted string
    std::printf("%s[%s][%s] %s\033[0m\n", color_code, GetCategoryName(category), level_name, buffer);
    std::fflush(stdout);
}
