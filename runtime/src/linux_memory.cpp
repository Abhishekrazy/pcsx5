#include <pcsx5/runtime/memory.h>
#include "linux_memory_api.h"
#include "linux_fault.h"
#include <sys/mman.h>
#include <sys/personality.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

namespace pcsx5::runtime {
namespace {
void* native_map(void*, std::size_t size) noexcept {
    return mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}
int native_unmap(void*, void* base, std::size_t size) noexcept { return munmap(base, size); }
int native_protect(void*, void* base, std::size_t size, int protection) noexcept {
    return mprotect(base, size, protection);
}
int native_discard(void*, void* base, std::size_t size) noexcept {
    return madvise(base, size, MADV_DONTNEED);
}
memory_error native_error() noexcept {
    return errno == ENOMEM ? memory_error::out_of_memory : memory_error::host_failure;
}
memory_result<int> protection(memory_access access) noexcept {
    switch (access) {
    case memory_access::none: return PROT_NONE;
    case memory_access::read_only: return PROT_READ;
    case memory_access::read_write: return PROT_READ | PROT_WRITE;
    }
    return std::unexpected(memory_error::unsupported);
}
struct tracked_page {
    page_info info{page_state::reserved, memory_access::none};
    bool certain{true};
};
class linux_reservation final : public memory_reservation {
public:
    linux_reservation(memory_geometry geometry, std::uint64_t size, detail::linux_memory_api api)
        : geometry_(geometry), size_(size), api_(api),
          pages_(static_cast<std::size_t>(size / geometry.page_size())) {}
    ~linux_reservation() override { if (!release()) std::terminate(); }
    linux_reservation(const linux_reservation&) = delete;
    linux_reservation& operator=(const linux_reservation&) = delete;
    linux_reservation(linux_reservation&&) = delete;
    linux_reservation& operator=(linux_reservation&&) = delete;
    memory_result<void> initialize() noexcept {
        void* mapped = api_.map(api_.context, static_cast<std::size_t>(size_));
        if (mapped == MAP_FAILED) return std::unexpected(native_error());
        base_ = static_cast<std::byte*>(mapped);
        live_ = true; // A mapping at address zero is not the closed sentinel.
        return {};
    }
    memory_geometry geometry() const noexcept override { return geometry_; }
    std::uint64_t size() const noexcept override { return size_; }
    memory_result<void> commit(std::uint64_t offset, std::uint64_t count,
                               memory_access access) noexcept override {
        return change(offset, count, access, page_state::reserved);
    }
    memory_result<void> protect(std::uint64_t offset, std::uint64_t count,
                                memory_access access) noexcept override {
        return change(offset, count, access, page_state::committed);
    }
    memory_result<void> decommit(std::uint64_t offset, std::uint64_t count) noexcept override {
        const auto valid = validate_pages(offset, count, page_state::committed);
        if (!valid) return valid;
        if (api_.protect(api_.context, address(offset), static_cast<std::size_t>(count), PROT_NONE) != 0) {
            const auto error = native_error();
            mark_uncertain(offset, count);
            return std::unexpected(error);
        }
        set_pages(offset, count, page_state::committed, memory_access::none);
        if (api_.discard(api_.context, address(offset), static_cast<std::size_t>(count)) != 0)
            return std::unexpected(native_error());
        set_pages(offset, count, page_state::reserved, memory_access::none);
        return {};
    }
    memory_result<page_info> query(std::uint64_t offset) const noexcept override {
        if (!live_) return std::unexpected(memory_error::invalid_state);
        if (offset >= size_) return std::unexpected(memory_error::invalid_range);
        const auto& page = pages_[static_cast<std::size_t>(offset / geometry_.page_size())];
        if (!page.certain) return std::unexpected(memory_error::host_failure);
        return page.info;
    }
    memory_result<void> read(std::uint64_t offset, std::span<std::byte> destination) const noexcept override {
        const auto valid = validate_copy(offset, destination.size(), false);
        if (!valid) return valid;
        std::memcpy(destination.data(), address(offset), destination.size());
        return {};
    }
    memory_result<void> write(std::uint64_t offset, std::span<const std::byte> source) noexcept override {
        const auto valid = validate_copy(offset, source.size(), true);
        if (!valid) return valid;
        std::memcpy(address(offset), source.data(), source.size());
        return {};
    }
    memory_result<void> release() noexcept override {
        if (!live_) return {};
        if (api_.unmap(api_.context, base_, static_cast<std::size_t>(size_)) != 0)
            return std::unexpected(native_error());
        live_ = false;
        base_ = nullptr;
        return {};
    }
    std::optional<detail::linux_fault_region> fault_region() const noexcept {
        if (!live_) return std::nullopt;
        return detail::linux_fault_region{reinterpret_cast<std::uintptr_t>(base_), size_};
    }
private:
    std::byte* address(std::uint64_t offset) const noexcept {
        return reinterpret_cast<std::byte*>(reinterpret_cast<std::uintptr_t>(base_) + offset);
    }
    void set_pages(std::uint64_t offset, std::uint64_t count, page_state state, memory_access access) noexcept {
        for (auto cursor = offset; cursor < offset + count; cursor += geometry_.page_size())
            pages_[static_cast<std::size_t>(cursor / geometry_.page_size())] = {{state, access}, true};
    }
    void mark_uncertain(std::uint64_t offset, std::uint64_t count) noexcept {
        for (auto cursor = offset; cursor < offset + count; cursor += geometry_.page_size())
            pages_[static_cast<std::size_t>(cursor / geometry_.page_size())].certain = false;
    }
    memory_result<void> validate_pages(std::uint64_t offset, std::uint64_t count, page_state state) const noexcept {
        if (!live_) return std::unexpected(memory_error::invalid_state);
        if (!page_range::make(geometry_, size_, offset, count)) return std::unexpected(memory_error::invalid_range);
        for (auto cursor = offset; cursor < offset + count; cursor += geometry_.page_size()) {
            const auto info = query(cursor);
            if (!info) return std::unexpected(info.error());
            if (info->state != state) return std::unexpected(memory_error::invalid_state);
        }
        return {};
    }
    memory_result<void> change(std::uint64_t offset, std::uint64_t count, memory_access access, page_state state) noexcept {
        const auto valid = validate_pages(offset, count, state);
        if (!valid) return valid;
        const auto native = protection(access);
        if (!native) return std::unexpected(native.error());
        if (api_.protect(api_.context, address(offset), static_cast<std::size_t>(count), *native) != 0) {
            const auto error = native_error();
            mark_uncertain(offset, count);
            return std::unexpected(error);
        }
        set_pages(offset, count, page_state::committed, access);
        return {};
    }
    memory_result<void> validate_copy(std::uint64_t offset, std::uint64_t count, bool writing) const noexcept {
        if (!live_) return std::unexpected(memory_error::invalid_state);
        if (count == 0 || offset > size_ || count > size_ - offset) return std::unexpected(memory_error::invalid_range);
        const auto page = geometry_.page_size();
        for (auto cursor = offset - offset % page; cursor < offset + count; cursor += page) {
            const auto info = query(cursor);
            if (!info) return std::unexpected(info.error());
            if (info->state != page_state::committed) return std::unexpected(memory_error::invalid_state);
            if (info->access == memory_access::none || (writing && info->access != memory_access::read_write))
                return std::unexpected(memory_error::access_denied);
        }
        return {};
    }
    memory_geometry geometry_;
    std::uint64_t size_;
    detail::linux_memory_api api_;
    std::vector<tracked_page> pages_;
    std::byte* base_{};
    bool live_{};
};
} // namespace

std::optional<detail::linux_fault_region> detail::capture_linux_fault_region(
    const memory_reservation& owner) noexcept {
    const auto* native = dynamic_cast<const linux_reservation*>(&owner);
    return native == nullptr ? std::nullopt : native->fault_region();
}

memory_result<memory_geometry> linux_memory_geometry() noexcept {
    const long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) return std::unexpected(memory_error::host_failure);
    return *memory_geometry::make(static_cast<std::uint64_t>(page), static_cast<std::uint64_t>(page));
}
memory_result<std::unique_ptr<memory_reservation>> reserve_linux_memory(std::uint64_t size) noexcept {
    return detail::reserve_linux_memory_with_api(size, {nullptr, native_map, native_unmap, native_protect, native_discard});
}
memory_result<std::unique_ptr<memory_reservation>> detail::reserve_linux_memory_with_api(
    std::uint64_t size, linux_memory_api api) noexcept {
    if (!api.map || !api.unmap || !api.protect || !api.discard) return std::unexpected(memory_error::unsupported);
    const auto geometry = linux_memory_geometry();
    if (!geometry) return std::unexpected(geometry.error());
    if (!page_range::make(*geometry, size, 0, size) || size > std::numeric_limits<std::size_t>::max() ||
        size > static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max()))
        return std::unexpected(memory_error::invalid_range);
    const int flags = personality(0xffffffffUL);
    if (flags == -1) return std::unexpected(memory_error::host_failure);
    if ((static_cast<unsigned int>(flags) & READ_IMPLIES_EXEC) != 0) return std::unexpected(memory_error::unsupported);
    try {
        auto owner = std::make_unique<linux_reservation>(*geometry, size, api);
        const auto initialized = owner->initialize();
        if (!initialized) return std::unexpected(initialized.error());
        return std::unique_ptr<memory_reservation>(std::move(owner));
    } catch (const std::bad_alloc&) { return std::unexpected(memory_error::out_of_memory); }
      catch (const std::length_error&) { return std::unexpected(memory_error::out_of_memory); }
}
} // namespace pcsx5::runtime
