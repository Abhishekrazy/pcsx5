#include <pcsx5/runtime/guest_memory_backing.h>
#include <new>
#include <utility>

namespace pcsx5::runtime {
namespace {
core::guest_memory_error normalize(memory_error error) noexcept {
    switch (error) {
    case memory_error::invalid_range: return core::guest_memory_error::invalid_range;
    case memory_error::invalid_state: return core::guest_memory_error::invalid_state;
    case memory_error::unsupported: return core::guest_memory_error::unsupported;
    case memory_error::access_denied: return core::guest_memory_error::access_denied;
    case memory_error::out_of_memory: return core::guest_memory_error::out_of_memory;
    case memory_error::host_failure: return core::guest_memory_error::host_failure;
    }
    return core::guest_memory_error::host_failure;
}
core::guest_memory_result<void> normalize(memory_result<void> result) noexcept {
    if (!result) return std::unexpected(normalize(result.error()));
    return {};
}
class runtime_backing final : public core::memory_backing {
public:
    explicit runtime_backing(std::unique_ptr<memory_reservation> owner) noexcept : owner_(std::move(owner)) {}
    std::uint64_t size() const noexcept override { return owner_->size(); }
    core::guest_memory_result<void> read(std::uint64_t offset, std::span<std::byte> out) const noexcept override {
        return normalize(owner_->read(offset, out));
    }
    core::guest_memory_result<void> write(std::uint64_t offset, std::span<const std::byte> in) noexcept override {
        return normalize(owner_->write(offset, in));
    }
    core::guest_memory_result<void> release() noexcept override { return normalize(owner_->release()); }
private:
    std::unique_ptr<memory_reservation> owner_;
};
}
core::guest_memory_result<std::unique_ptr<core::memory_backing>>
make_guest_memory_backing(std::uint64_t size, memory_provider provider) noexcept {
    if (!provider) return std::unexpected(core::guest_memory_error::unsupported);
    auto result = provider(size);
    if (!result) return std::unexpected(normalize(result.error()));
    if (!*result) return std::unexpected(core::guest_memory_error::host_failure);
    auto owner = std::move(*result);
    if (owner->size() != size) return std::unexpected(core::guest_memory_error::invalid_range);
    const auto committed = owner->commit(0, size, memory_access::read_write);
    if (!committed) return std::unexpected(normalize(committed.error()));
    try {
        return std::make_unique<runtime_backing>(std::move(owner));
    } catch (const std::bad_alloc&) {
        return std::unexpected(core::guest_memory_error::out_of_memory);
    }
}
} // namespace pcsx5::runtime
