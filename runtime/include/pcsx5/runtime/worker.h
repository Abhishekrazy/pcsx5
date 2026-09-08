#pragma once

#include <expected>
#include <memory>

namespace pcsx5::runtime {

enum class worker_error { invalid_callback, resource_unavailable, self_join, host_failure };
enum class worker_completion { returned, callback_threw };
template<class T> using worker_result = std::expected<T, worker_error>;
using worker_callback = void (*)(void* context);

// Owns one host worker, not a guest thread or scheduler. Operations and destruction
// require external serialization. No detach, forced stop, native IDs or TLS API.
// The callback and borrowed context must remain valid until successful join or
// destruction completes. A null context is allowed; a null callback is not.
class host_worker {
public:
    virtual ~host_worker() = default;
    host_worker(const host_worker&) = delete;
    host_worker& operator=(const host_worker&) = delete;
    host_worker(host_worker&&) = delete;
    host_worker& operator=(host_worker&&) = delete;

    // Blocks until completion and publishes callback writes. Repeated success
    // returns the same completion. Self-join fails without giving up ownership.
    // A native failure retains ownership for retry. Destruction joins, terminating
    // if that fails; callers must ensure the callback can finish without them.
    // C++ exceptions become callback_threw; native faults are not intercepted.
    [[nodiscard]] virtual worker_result<worker_completion> join() noexcept = 0;

protected:
    host_worker() = default;
};

// Windows leaf only. Success holds a non-null unique owner. The callback can run
// before this function returns. Failure starts no callback.
[[nodiscard]] worker_result<std::unique_ptr<host_worker>>
start_windows_worker(worker_callback callback, void* context) noexcept;

} // namespace pcsx5::runtime
