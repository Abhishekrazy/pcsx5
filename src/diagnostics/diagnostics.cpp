#include "diagnostics.h"
#include "../common/log.h"
#include "../hle/hle.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// Link against dbghelp for MiniDumpWriteDump
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "psapi.lib")

namespace Diagnostics {
namespace {

// ---------------------------------------------------------------------------
// Module-local state.  Protected by g_mutex for the bundle path / context
// read; the unhandled-exception filter itself is single-threaded (the OS
// invokes it from the faulting thread before process termination).
// ---------------------------------------------------------------------------
std::mutex             g_mutex;
CrashContext          g_crash;          // populated by the filter
std::atomic<bool>      g_crash_present{false};
std::atomic<bool>      g_handler_installed{false};
std::string            g_bundle_dir;     // root directory for the bundle
LONG WINAPI            CrashFilter(EXCEPTION_POINTERS* ep);

// I6.4: Hang snapshot state / callbacks.
HangSnapshotCallback   g_hang_callback{nullptr};
void*                  g_hang_callback_user = nullptr;
BootTimelineCallback  g_boot_timeline_callback{nullptr};
ConfigSnapshotCallback g_config_snapshot_callback{nullptr};
std::atomic<uint64_t>  g_last_flip_frame{0};
std::atomic<uint64_t>  g_last_flip_timestamp_us{0};

// Microseconds since process start (steady clock).
uint64_t NowUs() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();
}

// Visual C++ runtime error handler (I6.4).
void __cdecl InvalidParameterHandler(const wchar_t* expr, const wchar_t* func,
                                      const wchar_t* file, unsigned int line,
                                      uintptr_t /*reserved*/) {
    LOG_ERROR(Diagnostics, "CRT invalid parameter: expr='%ls' func='%ls' file='%ls' line=%u",
              expr ? expr : L"", func ? func : L"", file ? file : L"", line);
}

int __cdecl CrtReportHook(int report_type, char* msg, int* /*retval*/) {
    if (report_type == _CRT_ASSERT) {
        LOG_ERROR(Diagnostics, "CRT ASSERT: %s", msg ? msg : "");
    } else if (report_type == _CRT_ERROR) {
        LOG_ERROR(Diagnostics, "CRT ERROR: %s", msg ? msg : "");
    }
    return 1; // Suppress default dialog; we handle it ourselves.
}

} // namespace

bool HasCrashReport() {
    return g_crash_present.load(std::memory_order_acquire);
}

const CrashContext& GetCrashContext() {
    return g_crash;
}

void ResetCrashReport() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_crash = CrashContext{};
    g_crash_present.store(false, std::memory_order_release);
}

const std::string& BundleDirectory() {
    return g_bundle_dir;
}

void InstallCrashHandler(const std::string& bundle_dir) {
    bool expected = false;
    if (!g_handler_installed.compare_exchange_strong(expected, true)) {
        return; // already installed
    }
    g_bundle_dir = bundle_dir;
    // Ensure the directory exists.
    std::error_code ec;
    std::filesystem::create_directories(bundle_dir, ec);
    // The filter is called by the OS for any unhandled SEH exception.  We
    // stash the context, write the bundle, and let the OS proceed with
    // default termination.
    SetUnhandledExceptionFilter(CrashFilter);

    // I6.4: Install CRT validation hooks to catch abort/assert without popup.
    _set_invalid_parameter_handler(InvalidParameterHandler);
    _CrtSetReportHook2(_CRT_RPTHOOK_INSTALL, CrtReportHook);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);

    LOG_INFO(General, "Crash-report bundle directory: %s", g_bundle_dir.c_str());
}

// I6.4: Set callbacks for additional diagnostic data.
void SetHangSnapshotCallback(HangSnapshotCallback cb, void* user) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_hang_callback = cb;
    g_hang_callback_user = user;
}

void SetBootTimelineCallback(BootTimelineCallback cb) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_boot_timeline_callback = cb;
}

void SetConfigSnapshotCallback(ConfigSnapshotCallback cb) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_config_snapshot_callback = cb;
}

// I6.4: Record flip timestamps.
void RecordFlipTimestamp(uint64_t frame_counter) {
    g_last_flip_frame.store(frame_counter, std::memory_order_release);
    g_last_flip_timestamp_us.store(NowUs(), std::memory_order_release);
}

uint64_t GetLastFlipFrame() {
    return g_last_flip_frame.load(std::memory_order_acquire);
}

uint64_t GetLastFlipTimestampUs() {
    return g_last_flip_timestamp_us.load(std::memory_order_acquire);
}

// I6.4: Capture thread list from toolhelp snapshot.
std::vector<std::string> CaptureThreadSnapshot() {
    std::vector<std::string> out;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return out;

    DWORD current_pid = GetCurrentProcessId();
    THREADENTRY32 te = { sizeof(THREADENTRY32) };
    if (Thread32First(snapshot, &te)) {
        do {
            if (te.th32OwnerProcessID == current_pid) {
                char buf[128];
                std::snprintf(buf, sizeof(buf), "Thread TID=%lu", te.th32ThreadID);
                out.push_back(buf);
            }
        } while (Thread32Next(snapshot, &te));
    }
    CloseHandle(snapshot);
    return out;
}

// ---------------------------------------------------------------------------
// Internal helpers (namespace-Diagnostics scope so all later code can use them)
// ---------------------------------------------------------------------------

bool WriteAll(HANDLE h, const void* buf, DWORD n) {
    const u8* p = static_cast<const u8*>(buf);
    while (n > 0) {
        DWORD written = 0;
        if (!WriteFile(h, p, n, &written, nullptr) || written == 0) {
            return false;
        }
        p += written;
        n -= written;
    }
    return true;
}

bool WriteTextFile(const std::wstring& path, const std::string& text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = WriteAll(h, text.data(), static_cast<DWORD>(text.size()));
    CloseHandle(h);
    return ok;
}

std::wstring Wide(const std::string& s) {
    return std::wstring(s.begin(), s.end());
}

std::string WideToUtf8(const std::wstring& ws) {
    if (ws.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()),
                                  nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()),
                        out.data(), len, nullptr, nullptr);
    return out;
}

std::string Hex(u64 v, int width) {
    std::ostringstream os;
    os << "0x" << std::uppercase << std::hex << std::setw(width) << std::setfill('0') << v;
    return os.str();
}

std::string NowIso8601() {
    SYSTEMTIME st;
    GetSystemTime(&st);
    char buf[32];
    std::snprintf(buf, sizeof(buf),
                  "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
                  st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}

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
                    char b[8];
                    std::snprintf(b, sizeof(b), "\\u%04x", static_cast<unsigned char>(c));
                    out += b;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Bundle writers (manifest / thread / log helpers).
// ---------------------------------------------------------------------------

bool WriteHangManifest(const std::wstring& dir, const std::string& label) {
    std::ostringstream os;
    os << "{\n"
       << "  \"timestamp_iso\": \"" << NowIso8601() << "\",\n"
       << "  \"snapshot_type\": \"hang\",\n"
       << "  \"label\": \"" << JsonEscape(label) << "\",\n"
       << "  \"uptime_us\": " << ProcessUptimeMicros() << ",\n"
       << "  \"last_flip_frame\": " << g_last_flip_frame.load(std::memory_order_acquire) << ",\n"
       << "  \"last_flip_age_us\": " << (g_last_flip_timestamp_us.load(std::memory_order_acquire) > 0
                                         ? NowUs() - g_last_flip_timestamp_us.load(std::memory_order_acquire)
                                         : 0) << ",\n"
       << "  \"artifacts\": [\n"
       << "    \"hang.json\",\n"
       << "    \"threads.txt\",\n"
       << "    \"recent.log\"\n"
       << "  ]\n"
       << "}\n";
    return WriteTextFile(dir + L"\\hang.json", os.str());
}

bool WriteHangThreadList(const std::wstring& dir) {
    auto threads = CaptureThreadSnapshot();
    std::ostringstream os;
    os << "Thread snapshot (process " << GetCurrentProcessId() << "):\n";
    for (const auto& t : threads) {
        os << "  " << t << "\n";
    }
    return WriteTextFile(dir + L"\\threads.txt", os.str());
}

bool WriteRecentLogs(const std::wstring& dir) {
    auto entries = GetRecentLogEntries(1024);
    std::ostringstream os;
    for (const auto& e : entries) {
        os << "[" << e.timestamp_us << "]["
           << LogCategoryName(e.category) << "]["
           << LogLevelName(e.level) << "] "
           << e.message << "\n";
    }
    return WriteTextFile(dir + L"\\recent.log", os.str());
}

// ---------------------------------------------------------------------------
// Crash-only bundle writers (anonymous namespace, only used by CrashFilter and
// the crash-report-bundle writer).
// ---------------------------------------------------------------------------
namespace {

bool WriteCrashManifest(const std::wstring& dir) {
    std::ostringstream os;
    os << "{\n"
       << "  \"timestamp_iso\": \"" << NowIso8601() << "\",\n"
       << "  \"uptime_us\":     " << g_crash.timestamp_us << ",\n"
       << "  \"thread_id\":     " << g_crash.thread_id << ",\n"
       << "  \"exception_code\": 0x" << std::uppercase << std::hex << g_crash.exc_code << std::dec << ",\n"
       << "  \"fault_address\": " << Hex(g_crash.fault_address) << ",\n"
       << "  \"rip\":           " << Hex(g_crash.rip) << ",\n"
       << "  \"rsp\":           " << Hex(g_crash.rsp) << ",\n"
       << "  \"artifacts\": [\n"
       << "    \"crash.json\",\n"
       << "    \"registers.json\",\n"
       << "    \"recent.log\",\n"
       << "    \"import_trace.txt\",\n"
       << "    \"minidump.dmp\",\n"
       << "    \"system.txt\"\n"
       << "  ]\n"
       << "}\n";
    return WriteTextFile(dir + L"\\crash.json", os.str());
}

bool WriteRegisters(const std::wstring& dir) {
    std::ostringstream os;
    os << "{\n"
       << "  \"rax\": " << Hex(g_crash.rax) << ",\n"
       << "  \"rbx\": " << Hex(g_crash.rbx) << ",\n"
       << "  \"rcx\": " << Hex(g_crash.rcx) << ",\n"
       << "  \"rdx\": " << Hex(g_crash.rdx) << ",\n"
       << "  \"rsi\": " << Hex(g_crash.rsi) << ",\n"
       << "  \"rdi\": " << Hex(g_crash.rdi) << ",\n"
       << "  \"r8\":  " << Hex(g_crash.r8)  << ",\n"
       << "  \"r9\":  " << Hex(g_crash.r9)  << ",\n"
       << "  \"r10\": " << Hex(g_crash.r10) << ",\n"
       << "  \"r11\": " << Hex(g_crash.r11) << ",\n"
       << "  \"r12\": " << Hex(g_crash.r12) << ",\n"
       << "  \"r13\": " << Hex(g_crash.r13) << ",\n"
       << "  \"r14\": " << Hex(g_crash.r14) << ",\n"
       << "  \"r15\": " << Hex(g_crash.r15) << ",\n"
       << "  \"rbp\": " << Hex(g_crash.rbp) << ",\n"
       << "  \"rsp\": " << Hex(g_crash.rsp) << ",\n"
       << "  \"rip\": " << Hex(g_crash.rip) << "\n"
       << "}\n";
    return WriteTextFile(dir + L"\\registers.json", os.str());
}

bool WriteImportTrace(const std::wstring& dir) {
    auto trace = HLE::GetImportTrace(256);
    std::ostringstream os;
    for (const auto& t : trace) {
        os << "[" << t.timestamp_us << "] "
           << t.module_name << "::" << t.name
           << " (id=" << t.symbol_id << ", thunk=0x" << std::hex << t.thunk_address << std::dec << ")"
           << " caller=0x" << std::hex << t.caller_rip << std::dec
           << " args=(0x" << std::hex << t.arg1
           << ", 0x" << t.arg2
           << ", 0x" << t.arg3
           << ", 0x" << t.arg4
           << ", 0x" << t.arg5
           << ", 0x" << t.arg6 << std::dec << ")\n";
    }
    return WriteTextFile(dir + L"\\import_trace.txt", os.str());
}

bool WriteSystemInfo(const std::wstring& dir) {
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
#pragma warning(push)
#pragma warning(disable : 4996)
    GetVersionExW(&vi);
#pragma warning(pop)

    std::ostringstream os;
    os << "timestamp_iso: " << NowIso8601() << "\n"
       << "process:       " << GetCurrentProcessId() << "\n"
       << "os_version:    " << vi.dwMajorVersion << "." << vi.dwMinorVersion
                            << "." << vi.dwBuildNumber << "\n"
       << "processors:    " << si.dwNumberOfProcessors << "\n"
       << "page_size:     " << si.dwPageSize << "\n"
       << "processor_arch:";
    switch (si.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_AMD64: os << " x64"; break;
        case PROCESSOR_ARCHITECTURE_ARM64: os << " arm64"; break;
        case PROCESSOR_ARCHITECTURE_INTEL: os << " x86"; break;
        default: os << " unknown(" << si.wProcessorArchitecture << ")"; break;
    }
    os << "\n";

    PROCESS_MEMORY_COUNTERS pmc = { sizeof(PROCESS_MEMORY_COUNTERS) };
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        os << "working_set_kb: " << (pmc.WorkingSetSize / 1024) << "\n"
           << "pagefile_kb:    " << (pmc.PagefileUsage / 1024) << "\n";
    }

    return WriteTextFile(dir + L"\\system.txt", os.str());
}

bool WriteMiniDump(const std::wstring& dir) {
    HANDLE file = CreateFileW((dir + L"\\minidump.dmp").c_str(),
                              GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = nullptr;
    mei.ClientPointers = FALSE;

    HANDLE process = GetCurrentProcess();
    DWORD process_id = GetCurrentProcessId();

    BOOL ok = MiniDumpWriteDump(
        process, process_id, file,
        MiniDumpWithIndirectlyReferencedMemory,
        &mei, nullptr, nullptr);

    CloseHandle(file);
    return ok != FALSE;
}

// Internal implementation — called by both CrashFilter and WriteCrashReportBundle.
std::string WriteCrashReportBundleImpl(bool force) {
    if (!g_crash_present.load(std::memory_order_acquire) && !force) {
        return {};
    }
    if (g_bundle_dir.empty()) {
        g_bundle_dir = "pcsx5_crash";
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    std::error_code ec;
    std::filesystem::create_directories(g_bundle_dir, ec);
    if (ec) return {};

    std::wstring wdir = Wide(g_bundle_dir);

    WriteCrashManifest(wdir);
    WriteRegisters(wdir);
    WriteRecentLogs(wdir);
    WriteImportTrace(wdir);
    WriteSystemInfo(wdir);
    WriteMiniDump(wdir);

    // I2.3: Additional sidecar artifacts via callbacks.
    if (g_boot_timeline_callback) {
        std::string timeline = g_boot_timeline_callback();
        WriteTextFile(wdir + L"\\boot_timeline.json",
                      "{\n  \"boot_timeline\": " + timeline + "\n}\n");
    }
    if (g_config_snapshot_callback) {
        std::string cfg = g_config_snapshot_callback();
        WriteTextFile(wdir + L"\\config_snapshot.json", cfg);
    }

    return g_bundle_dir;
}

// ---------------------------------------------------------------------------
// Unhandled-exception filter (runs on the faulting thread).
// ---------------------------------------------------------------------------
LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) {
        return EXCEPTION_EXECUTE_HANDLER;
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_crash.timestamp_us    = ProcessUptimeMicros();
        g_crash.thread_id       = GetCurrentThreadId();
        g_crash.exc_code        = static_cast<u32>(ep->ExceptionRecord->ExceptionCode);
        g_crash.fault_address   = ep->ExceptionRecord->ExceptionInformation[1];
        g_crash.rip             = ep->ContextRecord->Rip;
        g_crash.rsp             = ep->ContextRecord->Rsp;
        g_crash.rbp             = ep->ContextRecord->Rbp;
        g_crash.rax             = ep->ContextRecord->Rax;
        g_crash.rbx             = ep->ContextRecord->Rbx;
        g_crash.rcx             = ep->ContextRecord->Rcx;
        g_crash.rdx             = ep->ContextRecord->Rdx;
        g_crash.rsi             = ep->ContextRecord->Rsi;
        g_crash.rdi             = ep->ContextRecord->Rdi;
        g_crash.r8              = ep->ContextRecord->R8;
        g_crash.r9              = ep->ContextRecord->R9;
        g_crash.r10             = ep->ContextRecord->R10;
        g_crash.r11             = ep->ContextRecord->R11;
        g_crash.r12             = ep->ContextRecord->R12;
        g_crash.r13             = ep->ContextRecord->R13;
        g_crash.r14             = ep->ContextRecord->R14;
        g_crash.r15             = ep->ContextRecord->R15;
        g_crash_present.store(true, std::memory_order_release);
    }

    // Best-effort write the bundle.  Win32 file I/O only (heap may be corrupt).
    WriteCrashReportBundleImpl(false);
    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace

// I6.4: Write a hang snapshot bundle.
std::string WriteHangSnapshotBundle(const std::string& label) {
    std::lock_guard<std::mutex> lock(g_mutex);

    std::string dir;
    if (g_bundle_dir.empty()) {
        dir = "pcsx5_hang";
    } else {
        dir = g_bundle_dir + "/hang_snapshots";
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return {};

    std::string timestamp = NowIso8601();
    for (auto& c : timestamp) if (c == ':' || c == '.') c = '-';

    std::wstring wdir = Wide(dir + "/" + label + "_" + timestamp);
    CreateDirectoryW(wdir.c_str(), nullptr);

    // Write hang manifest.
    WriteHangManifest(wdir, label);

    // Write thread snapshot.
    WriteHangThreadList(wdir);

    // Write recent logs.
    WriteRecentLogs(wdir);

    // I2.3: Write boot timeline via callback.
    if (g_boot_timeline_callback) {
        std::string timeline = g_boot_timeline_callback();
        WriteTextFile(wdir + L"\\boot_timeline.json",
                      "{\n  \"boot_timeline\": " + timeline + "\n}\n");
    }

    // I2.3: Write config snapshot via callback.
    if (g_config_snapshot_callback) {
        std::string cfg = g_config_snapshot_callback();
        WriteTextFile(wdir + L"\\config_snapshot.json", cfg);
    }

    // I6.4: Invoke hang callback for last-frame capture if registered.
    if (g_hang_callback) {
        g_hang_callback(wdir, g_hang_callback_user);
    }

    LOG_INFO(General, "Hang snapshot written to %ls", wdir.c_str());
    return WideToUtf8(wdir);
}

// Write the crash report bundle to the configured directory.
std::string WriteCrashReportBundle(bool force) {
    return WriteCrashReportBundleImpl(force);
}

// I2.3: Write a diagnostic snapshot (not crash-triggered).
std::string WriteDiagnosticSnapshot(const std::string& label) {
    std::lock_guard<std::mutex> lock(g_mutex);

    std::string dir = "pcsx5_snapshot";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return {};

    std::string now = NowIso8601();
    for (auto& c : now) if (c == ':' || c == '.') c = '-';
    std::string subdir = dir + "/" + label + "_" + now;
    std::filesystem::create_directories(subdir, ec);
    if (ec) return {};

    std::wstring wdir = Wide(subdir);

    WriteTextFile(wdir + L"\\snapshot.json",
                  "{\"label\":\"" + JsonEscape(label) + "\",\"timestamp\":\"" + NowIso8601() + "\",\"uptime_us\":" + std::to_string(ProcessUptimeMicros()) + "}");
    WriteRecentLogs(wdir);
    WriteSystemInfo(wdir);

    if (g_boot_timeline_callback) {
        std::string timeline = g_boot_timeline_callback();
        WriteTextFile(wdir + L"\\boot_timeline.json",
                      "{\n  \"boot_timeline\": " + timeline + "\n}\n");
    }
    if (g_config_snapshot_callback) {
        std::string cfg = g_config_snapshot_callback();
        WriteTextFile(wdir + L"\\config_snapshot.json", cfg);
    }

    LOG_INFO(General, "Diagnostic snapshot written: %s", subdir.c_str());
    return subdir;
}

// Process uptime in microseconds.
uint64_t ProcessUptimeMicros() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();
}

} // namespace Diagnostics
