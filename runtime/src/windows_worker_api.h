#pragma once

#include <pcsx5/runtime/worker.h>
#include <thread>

namespace pcsx5::runtime::detail {

// Private verification seam. Copied per owner; context must outlive the owner.
// start must either throw before launching anything or return a joinable thread
// running entry exactly once. join must throw before mutation or actually join.
// No process-global substitution and no native thread access in public headers.
struct worker_api {
    void* context;
    std::thread (*start)(void*, void (*entry)(void*) noexcept, void*);
    void (*join)(void*, std::thread&);
};

[[nodiscard]] worker_result<std::unique_ptr<host_worker>> start_windows_worker_with_api(
    worker_callback callback, void* context, worker_api api) noexcept;

} // namespace pcsx5::runtime::detail
