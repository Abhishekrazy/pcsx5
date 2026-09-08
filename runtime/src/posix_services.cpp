#include <pcsx5/runtime/posix.h>
namespace pcsx5::runtime {
// These existing implementations use std::thread and clock_gettime only.
// Reusing them keeps completion/error/overflow behavior under the existing tests.
worker_result<std::unique_ptr<host_worker>> start_posix_worker(worker_callback callback,void* context) noexcept {
    return start_linux_worker(callback,context);
}
timing_result<tick_frequency> posix_counter_frequency() noexcept { return linux_counter_frequency(); }
timing_result<tick_stamp> posix_counter_now() noexcept { return linux_counter_now(); }
}
