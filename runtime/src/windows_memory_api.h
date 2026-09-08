#pragma once

// Private Windows leaf seam. Never add this directory to public include paths.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <pcsx5/runtime/memory.h>

namespace pcsx5::runtime::detail {
// Copied per owner; context must outlive that owner. Callbacks obey the native
// API semantics (including SetLastError), are nonthrowing, and must not reenter.
// No global replacement table. Production always supplies the real Win32 calls.
struct windows_memory_api {
    void* context{};
    void* (*allocate)(void*, void*, SIZE_T, DWORD, DWORD) noexcept{};
    BOOL (*free)(void*, void*, SIZE_T, DWORD) noexcept{};
    BOOL (*protect)(void*, void*, SIZE_T, DWORD, DWORD*) noexcept{};
    SIZE_T (*query)(void*, const void*, MEMORY_BASIC_INFORMATION*, SIZE_T) noexcept{};
};

[[nodiscard]] memory_result<std::unique_ptr<memory_reservation>>
reserve_windows_memory_with_api(std::uint64_t size, windows_memory_api api) noexcept;
} // namespace pcsx5::runtime::detail
