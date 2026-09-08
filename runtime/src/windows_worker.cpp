#include <pcsx5/runtime/worker.h>
#include "windows_worker_api.h"

#include <exception>
#include <new>
#include <system_error>
#include <thread>
#include <utility>

namespace pcsx5::runtime {
namespace {

class windows_worker final : public host_worker {
public:
    windows_worker(worker_callback callback, void* context, detail::worker_api api) noexcept
        : callback_(callback), context_(context), api_(api) {}

    ~windows_worker() override {
        if (!join()) std::terminate();
    }
    windows_worker(const windows_worker&) = delete;
    windows_worker& operator=(const windows_worker&) = delete;
    windows_worker(windows_worker&&) = delete;
    windows_worker& operator=(windows_worker&&) = delete;

    void start() {
        // All callback state is initialized before launching. No allocation or
        // throwing work follows successful launch and noexcept thread move.
        thread_ = api_.start(api_.context, run, this);
    }

    [[nodiscard]] worker_result<worker_completion> join() noexcept override {
        if (thread_.joinable()) {
            if (thread_.get_id() == std::this_thread::get_id()) {
                return std::unexpected(worker_error::self_join);
            }
            try {
                api_.join(api_.context, thread_);
            } catch (...) {
                return std::unexpected(worker_error::host_failure);
            }
        }
        // A successful join publishes run()'s sole write; there is no polling
        // accessor that could read completion concurrently with the callback.
        return completion_;
    }

private:
    static void run(void* raw) noexcept {
        auto& self = *static_cast<windows_worker*>(raw);
        try {
            self.callback_(self.context_);
            self.completion_ = worker_completion::returned;
        } catch (...) {
            self.completion_ = worker_completion::callback_threw;
        }
    }

    worker_callback callback_;
    void* context_;
    detail::worker_api api_;
    worker_completion completion_{worker_completion::returned};
    std::thread thread_;
};

std::thread native_start(void*, void (*entry)(void*) noexcept, void* context) {
    return std::thread(entry, context);
}

void native_join(void*, std::thread& thread) { thread.join(); }

} // namespace

worker_result<std::unique_ptr<host_worker>> detail::start_windows_worker_with_api(
    worker_callback callback, void* context, worker_api api) noexcept {
    if (!callback) return std::unexpected(worker_error::invalid_callback);
    if (!api.start || !api.join) return std::unexpected(worker_error::host_failure);
    try {
        auto owner = std::make_unique<windows_worker>(callback, context, api);
        owner->start();
        return std::unique_ptr<host_worker>(std::move(owner));
    } catch (const std::bad_alloc&) {
        return std::unexpected(worker_error::resource_unavailable);
    } catch (const std::system_error& error) {
        if (error.code() == std::errc::resource_unavailable_try_again) {
            return std::unexpected(worker_error::resource_unavailable);
        }
        return std::unexpected(worker_error::host_failure);
    } catch (...) {
        return std::unexpected(worker_error::host_failure);
    }
}

worker_result<std::unique_ptr<host_worker>> start_windows_worker(
    worker_callback callback, void* context) noexcept {
    return detail::start_windows_worker_with_api(callback, context,
        {nullptr, native_start, native_join});
}

} // namespace pcsx5::runtime
