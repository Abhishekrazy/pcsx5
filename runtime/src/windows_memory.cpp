#include <pcsx5/runtime/memory.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <utility>

namespace pcsx5::runtime {
namespace {

[[nodiscard]] memory_error native_error() noexcept {
    const auto error = GetLastError();
    if (error == ERROR_NOT_ENOUGH_MEMORY || error == ERROR_OUTOFMEMORY ||
        error == ERROR_COMMITMENT_LIMIT) {
        return memory_error::out_of_memory;
    }
    return memory_error::host_failure;
}

[[nodiscard]] memory_result<DWORD> native_protection(memory_access access) noexcept {
    switch (access) {
    case memory_access::none: return PAGE_NOACCESS;
    case memory_access::read_only: return PAGE_READONLY;
    case memory_access::read_write: return PAGE_READWRITE;
    }
    return std::unexpected(memory_error::unsupported);
}

class windows_reservation final : public memory_reservation {
public:
    windows_reservation(memory_geometry geometry, std::uint64_t size) noexcept
        : geometry_(geometry), size_(size) {}
    ~windows_reservation() override {
        if (!close()) {
            std::terminate();
        }
    }
    windows_reservation(const windows_reservation&) = delete;
    windows_reservation& operator=(const windows_reservation&) = delete;
    windows_reservation(windows_reservation&&) = delete;
    windows_reservation& operator=(windows_reservation&&) = delete;

    [[nodiscard]] memory_result<void> initialize() noexcept {
        base_ = static_cast<std::byte*>(VirtualAlloc(nullptr,
            static_cast<SIZE_T>(size_), MEM_RESERVE, PAGE_NOACCESS));
        if (base_ == nullptr) {
            return std::unexpected(native_error());
        }
        return {};
    }

    [[nodiscard]] memory_geometry geometry() const noexcept override { return geometry_; }
    [[nodiscard]] std::uint64_t size() const noexcept override { return size_; }

    [[nodiscard]] memory_result<void> commit(std::uint64_t offset,
        std::uint64_t count, memory_access access) noexcept override {
        const auto valid = validate_pages(offset, count, page_state::reserved);
        if (!valid) return valid;
        const auto protection = native_protection(access);
        if (!protection) return std::unexpected(protection.error());
        if (VirtualAlloc(address(offset), static_cast<SIZE_T>(count), MEM_COMMIT,
                         *protection) == nullptr) {
            return std::unexpected(native_error());
        }
        return {};
    }

    [[nodiscard]] memory_result<void> protect(std::uint64_t offset,
        std::uint64_t count, memory_access access) noexcept override {
        const auto valid = validate_pages(offset, count, page_state::committed);
        if (!valid) return valid;
        const auto protection = native_protection(access);
        if (!protection) return std::unexpected(protection.error());
        DWORD previous{};
        if (!VirtualProtect(address(offset), static_cast<SIZE_T>(count),
                            *protection, &previous)) {
            return std::unexpected(native_error());
        }
        return {};
    }

    [[nodiscard]] memory_result<void> decommit(std::uint64_t offset,
        std::uint64_t count) noexcept override {
        const auto valid = validate_pages(offset, count, page_state::committed);
        if (!valid) return valid;
        if (!VirtualFree(address(offset), static_cast<SIZE_T>(count), MEM_DECOMMIT)) {
            return std::unexpected(native_error());
        }
        return {};
    }

    [[nodiscard]] memory_result<page_info> query(std::uint64_t offset) const noexcept override {
        if (base_ == nullptr) return std::unexpected(memory_error::invalid_state);
        if (offset >= size_) return std::unexpected(memory_error::invalid_range);
        MEMORY_BASIC_INFORMATION information{};
        if (VirtualQuery(address(offset), &information, sizeof(information)) !=
                sizeof(information) || information.AllocationBase != base_ ||
            information.Type != MEM_PRIVATE) {
            return std::unexpected(memory_error::host_failure);
        }
        if (information.State == MEM_RESERVE) {
            return page_info{page_state::reserved, memory_access::none};
        }
        if (information.State != MEM_COMMIT) {
            return std::unexpected(memory_error::host_failure);
        }
        switch (information.Protect) {
        case PAGE_NOACCESS: return page_info{page_state::committed, memory_access::none};
        case PAGE_READONLY: return page_info{page_state::committed, memory_access::read_only};
        case PAGE_READWRITE: return page_info{page_state::committed, memory_access::read_write};
        default: return std::unexpected(memory_error::host_failure);
        }
    }

    [[nodiscard]] memory_result<void> read(std::uint64_t offset,
        std::span<std::byte> destination) const noexcept override {
        const auto valid = validate_copy(offset, destination.size(), false);
        if (!valid) return valid;
        std::memcpy(destination.data(), address(offset), destination.size());
        return {};
    }

    [[nodiscard]] memory_result<void> write(std::uint64_t offset,
        std::span<const std::byte> source) noexcept override {
        const auto valid = validate_copy(offset, source.size(), true);
        if (!valid) return valid;
        std::memcpy(address(offset), source.data(), source.size());
        return {};
    }

    [[nodiscard]] memory_result<void> release() noexcept override { return close(); }

private:
    [[nodiscard]] std::byte* address(std::uint64_t offset) const noexcept {
        // Factory bounds the entire reservation to SIZE_T and ptrdiff_t.
        return base_ + static_cast<std::ptrdiff_t>(offset);
    }

    [[nodiscard]] memory_result<void> validate_pages(std::uint64_t offset,
        std::uint64_t count, page_state required) const noexcept {
        if (base_ == nullptr) return std::unexpected(memory_error::invalid_state);
        if (!page_range::make(geometry_, size_, offset, count)) {
            return std::unexpected(memory_error::invalid_range);
        }
        const auto end = offset + count; // Checked containment; size_ <= PTRDIFF_MAX.
        for (auto cursor = offset; cursor < end; cursor += geometry_.page_size()) {
            const auto information = query(cursor);
            if (!information) return std::unexpected(information.error());
            if (information->state != required) {
                return std::unexpected(memory_error::invalid_state);
            }
        }
        return {};
    }

    [[nodiscard]] memory_result<void> validate_copy(std::uint64_t offset,
        std::uint64_t count, bool writing) const noexcept {
        if (base_ == nullptr) return std::unexpected(memory_error::invalid_state);
        if (count == 0 || offset > size_ || count > size_ - offset) {
            return std::unexpected(memory_error::invalid_range);
        }
        const auto page = geometry_.page_size();
        const auto end = offset + count;
        for (auto cursor = offset - offset % page; cursor < end; cursor += page) {
            const auto information = query(cursor);
            if (!information) return std::unexpected(information.error());
            if (information->state != page_state::committed) {
                return std::unexpected(memory_error::invalid_state);
            }
            if (information->access == memory_access::none ||
                (writing && information->access != memory_access::read_write)) {
                return std::unexpected(memory_error::access_denied);
            }
        }
        return {};
    }

    [[nodiscard]] memory_result<void> close() noexcept {
        if (base_ == nullptr) return {};
        if (!VirtualFree(base_, 0, MEM_RELEASE)) {
            return std::unexpected(native_error());
        }
        base_ = nullptr;
        return {};
    }

    memory_geometry geometry_;
    std::uint64_t size_;
    std::byte* base_{}; // Exclusively owned Windows reservation; never exposed.
};

} // namespace

memory_result<memory_geometry> windows_memory_geometry() noexcept {
    SYSTEM_INFO information{};
    GetSystemInfo(&information);
    const auto geometry = memory_geometry::make(information.dwPageSize,
                                                information.dwAllocationGranularity);
    if (!geometry) return std::unexpected(memory_error::host_failure);
    return *geometry;
}

memory_result<std::unique_ptr<memory_reservation>>
reserve_windows_memory(std::uint64_t size) noexcept {
    const auto geometry = windows_memory_geometry();
    if (!geometry) return std::unexpected(geometry.error());
    if (!page_range::make(*geometry, size, 0, size) ||
        size > std::numeric_limits<SIZE_T>::max() ||
        size > static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max())) {
        return std::unexpected(memory_error::invalid_range);
    }
    try {
        // Allocate the owner before acquiring the native mapping.
        auto owner = std::make_unique<windows_reservation>(*geometry, size);
        const auto initialized = owner->initialize();
        if (!initialized) return std::unexpected(initialized.error());
        return std::unique_ptr<memory_reservation>(std::move(owner));
    } catch (const std::bad_alloc&) {
        return std::unexpected(memory_error::out_of_memory);
    }
}

} // namespace pcsx5::runtime
