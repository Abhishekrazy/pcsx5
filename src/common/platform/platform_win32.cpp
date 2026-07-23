// PAL implementation — Windows backend (Win32 API).
//
// Every function is a thin zero-overhead wrapper that translates
// between the platform-neutral Platform::* types and the native
// Win32 API.

#include "platform.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <intrin.h>

#include <cstring>
#include <string>
#include <vector>

namespace Platform {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Convert a UTF-8 C string to a wide string allocated on the stack
// (via _alloca in the caller's scope).  Callers use the ALLOCA_WIDE
// macro below which evaluates `expr` in the caller's scope.
#define ALLOCA_WIDE(var_name, utf8_str)                                    \
    do {                                                                   \
        const char* _u = (utf8_str);                                       \
        int _len = ::MultiByteToWideChar(CP_UTF8, 0, _u, -1, nullptr, 0);  \
        if (_len > 0) {                                                    \
            var_name = static_cast<wchar_t*>(                               \
                _alloca(static_cast<size_t>(_len) * sizeof(wchar_t)));      \
            ::MultiByteToWideChar(CP_UTF8, 0, _u, -1, var_name, _len);     \
        }                                                                  \
    } while (0)

// ===========================================================================
// Memory management
// ===========================================================================

void* VirtualAlloc(void* address_hint, size_t size,
                    MemoryType type, MemoryProtection prot) {
    DWORD alloc_type = 0;
    switch (type) {
        case MemoryType::Reserve:     alloc_type = MEM_RESERVE; break;
        case MemoryType::Commit:      alloc_type = MEM_RESERVE | MEM_COMMIT; break;
        case MemoryType::LargePages:  alloc_type = MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES; break;
    }

    DWORD win_prot = PAGE_NOACCESS;
    switch (prot) {
        case MemoryProtection::None:              win_prot = PAGE_NOACCESS; break;
        case MemoryProtection::Read:              win_prot = PAGE_READONLY; break;
        case MemoryProtection::Write:             win_prot = PAGE_READWRITE; break;
        case MemoryProtection::ReadWrite:         win_prot = PAGE_READWRITE; break;
        case MemoryProtection::Execute:           win_prot = PAGE_EXECUTE; break;
        case MemoryProtection::ReadExecute:       win_prot = PAGE_EXECUTE_READ; break;
        case MemoryProtection::ReadWriteExecute:  win_prot = PAGE_EXECUTE_READWRITE; break;
    }

    return ::VirtualAlloc(address_hint, size, alloc_type, win_prot);
}

bool VirtualFree(void* address, size_t /*size*/) {
    return ::VirtualFree(address, 0, MEM_RELEASE) != 0;
}

bool VirtualProtect(void* address, size_t size, MemoryProtection prot) {
    DWORD win_prot = PAGE_NOACCESS;
    switch (prot) {
        case MemoryProtection::None:              win_prot = PAGE_NOACCESS; break;
        case MemoryProtection::Read:              win_prot = PAGE_READONLY; break;
        case MemoryProtection::Write:             win_prot = PAGE_READWRITE; break;
        case MemoryProtection::ReadWrite:         win_prot = PAGE_READWRITE; break;
        case MemoryProtection::Execute:           win_prot = PAGE_EXECUTE; break;
        case MemoryProtection::ReadExecute:       win_prot = PAGE_EXECUTE_READ; break;
        case MemoryProtection::ReadWriteExecute:  win_prot = PAGE_EXECUTE_READWRITE; break;
    }
    DWORD old_prot = 0;
    return ::VirtualProtect(address, size, win_prot, &old_prot) != 0;
}

bool VirtualQuery(const void* address,
                  void** out_base, size_t* out_size,
                  MemoryProtection* out_prot) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (::VirtualQuery(address, &mbi, sizeof(mbi)) == 0) return false;
    if (out_base) *out_base = mbi.AllocationBase;
    if (out_size) *out_size = mbi.RegionSize;
    if (out_prot) {
        switch (mbi.Protect & 0xFF) {
            case PAGE_NOACCESS:          *out_prot = MemoryProtection::None; break;
            case PAGE_READONLY:          *out_prot = MemoryProtection::Read; break;
            case PAGE_READWRITE:         *out_prot = MemoryProtection::ReadWrite; break;
            case PAGE_EXECUTE:           *out_prot = MemoryProtection::Execute; break;
            case PAGE_EXECUTE_READ:      *out_prot = MemoryProtection::ReadExecute; break;
            case PAGE_EXECUTE_READWRITE: *out_prot = MemoryProtection::ReadWriteExecute; break;
            default:                     *out_prot = MemoryProtection::None; break;
        }
    }
    return true;
}

bool EnableLargePages() {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(),
                            TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!::LookupPrivilegeValueA(nullptr, SE_LOCK_MEMORY_NAME,
                                 &tp.Privileges[0].Luid)) {
        ::CloseHandle(token);
        return false;
    }
    bool ok = ::AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr) != 0;
    ::CloseHandle(token);
    return ok;
}

size_t LargePageSize() {
    return ::GetLargePageMinimum();
}

// ===========================================================================
// Thread utilities
// ===========================================================================

uint64_t GetCurrentThreadId() {
    return static_cast<uint64_t>(::GetCurrentThreadId());
}

bool SetThreadName(const char* name) {
    // Use SetThreadDescription (Windows 10+).
    HMODULE h_mod = ::GetModuleHandleW(L"kernel32.dll");
    if (!h_mod) return false;
    using SetThreadDescFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    auto fn = reinterpret_cast<SetThreadDescFn>(
        ::GetProcAddress(h_mod, "SetThreadDescription"));
    if (!fn) return false;

    wchar_t* wide = nullptr;
    ALLOCA_WIDE(wide, name);
    if (!wide) return false;
    return SUCCEEDED(fn(::GetCurrentThread(), wide));
}

// ===========================================================================
// Dynamic libraries
// ===========================================================================

void* LoadLibrary(const char* path) {
    wchar_t* wide = nullptr;
    ALLOCA_WIDE(wide, path);
    if (!wide) return nullptr;
    return reinterpret_cast<void*>(::LoadLibraryW(wide));
}

void* GetProcAddress(void* library, const char* name) {
    return reinterpret_cast<void*>(
        ::GetProcAddress(static_cast<HMODULE>(library), name));
}

bool FreeLibrary(void* library) {
    return ::FreeLibrary(static_cast<HMODULE>(library)) != 0;
}

bool LibraryIsLoaded(const char* path) {
    wchar_t* wide = nullptr;
    ALLOCA_WIDE(wide, path);
    if (!wide) return false;
    return ::GetModuleHandleW(wide) != nullptr;
}

// ===========================================================================
// Exception / signal handling
// ===========================================================================

namespace {

    thread_local FaultHandlerCallback t_fault_callback = nullptr;
    thread_local void*               t_fault_user    = nullptr;

    LONG WINAPI PalVectoredExceptionHandler(PEXCEPTION_POINTERS ep) {
        if (!ep || !ep->ExceptionRecord || !t_fault_callback) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        if (ep->ExceptionRecord->NumberParameters < 2) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        void* fault_addr = reinterpret_cast<void*>(
            ep->ExceptionRecord->ExceptionInformation[1]);
        if (t_fault_callback(fault_addr,
                             ep->ExceptionRecord->ExceptionCode,
                             t_fault_user)) {
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    PVOID g_pal_veh = nullptr;

}  // anonymous namespace

FaultHandlerHandle InstallFaultHandler(FaultHandlerCallback callback,
                                        void* user_data) {
    if (!g_pal_veh) {
        g_pal_veh = ::AddVectoredExceptionHandler(1, PalVectoredExceptionHandler);
        if (!g_pal_veh) return nullptr;
    }
    t_fault_callback = callback;
    t_fault_user = user_data;
    return reinterpret_cast<FaultHandlerHandle>(g_pal_veh);
}

bool RemoveFaultHandler(FaultHandlerHandle /*handle*/) {
    t_fault_callback = nullptr;
    t_fault_user = nullptr;
    return true;
}

// ===========================================================================
// Process info
// ===========================================================================

uint64_t GetProcessId() {
    return static_cast<uint64_t>(::GetCurrentProcessId());
}

// ===========================================================================
// CPU feature detection
// ===========================================================================

CpuFeatures QueryCpuFeatures() {
    CpuFeatures f{};
    int info[4] = {};
    __cpuid(info, 0);
    if (info[0] >= 1) {
        __cpuid(info, 1);
        f.has_avx = (info[2] & (1 << 28)) != 0;
    }
    if (info[0] >= 7) {
        __cpuid(info, 7);
        f.has_bmi1  = (info[1] & (1 << 3)) != 0;
        f.has_bmi2  = (info[1] & (1 << 8)) != 0;
        f.has_avx2  = (info[1] & (1 << 5)) != 0;
        f.has_abm   = (info[1] & (1 << 0)) != 0;  // POPCNT
    }
    // Extended leaves for SSE4a / LZCNT (AMD).
    __cpuid(info, 0x80000000);
    if (static_cast<unsigned>(info[0]) >= 0x80000001) {
        __cpuid(info, 0x80000001);
        f.has_sse4a = (info[2] & (1 << 6)) != 0;
        if (!f.has_abm) {
            f.has_abm = (info[2] & (1 << 5)) != 0;  // LZCNT
        }
    }
    return f;
}

}  // namespace Platform
