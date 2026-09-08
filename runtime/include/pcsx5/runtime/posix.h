#pragma once
#include <pcsx5/runtime/memory.h>
#include <pcsx5/runtime/worker.h>
#include <pcsx5/runtime/timing.h>
namespace pcsx5::runtime {
// Android/Darwin baseline. Native fault observation and child execution are
// separate capabilities, not silently inherited from the Linux provider.
[[nodiscard]] memory_result<memory_geometry> posix_memory_geometry() noexcept;
[[nodiscard]] memory_result<std::unique_ptr<memory_reservation>> reserve_posix_memory(std::uint64_t) noexcept;
[[nodiscard]] worker_result<std::unique_ptr<host_worker>> start_posix_worker(worker_callback, void*) noexcept;
[[nodiscard]] timing_result<tick_frequency> posix_counter_frequency() noexcept;
[[nodiscard]] timing_result<tick_stamp> posix_counter_now() noexcept;
}
