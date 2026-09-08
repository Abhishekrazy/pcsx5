#pragma once

#include <pcsx5/runtime/worker.h>
#include <thread>

namespace pcsx5::runtime::detail {

// Private per-owner verification seam, with the same preconditions as the
// Windows seam: start throws before launch or returns a joinable thread running
// entry exactly once; join throws before mutation or actually joins. Borrowed
// context must outlive the owner. No process-global substitution.
struct linux_worker_api {
    void* context;
    std::thread (*start)(void*, void (*entry)(void*) noexcept, void*);
    void (*join)(void*, std::thread&);
};

[[nodiscard]] worker_result<std::unique_ptr<host_worker>> start_linux_worker_with_api(
    worker_callback callback, void* context, linux_worker_api api) noexcept;

} // namespace pcsx5::runtime::detail
