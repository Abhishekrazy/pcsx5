#pragma once
#include <pcsx5/runtime/memory.h>
#include <pcsx5/runtime/posix.h>
namespace test_host {
#if defined(PCSX5_TEST_POSIX)
inline constexpr auto memory_geometry = pcsx5::runtime::posix_memory_geometry;
inline constexpr auto reserve_memory = pcsx5::runtime::reserve_posix_memory;
#elif defined(PCSX5_TEST_LINUX)
inline constexpr auto memory_geometry = pcsx5::runtime::linux_memory_geometry;
inline constexpr auto reserve_memory = pcsx5::runtime::reserve_linux_memory;
#else
inline constexpr auto memory_geometry = pcsx5::runtime::windows_memory_geometry;
inline constexpr auto reserve_memory = pcsx5::runtime::reserve_windows_memory;
#endif
}
