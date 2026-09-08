#include <pcsx5/runtime/memory.h>

#include "windows_memory_api.h"
#include "windows_fault.h"

#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <utility>

namespace pcsx5::runtime {
namespace {

void* native_allocate(void*, void* address, SIZE_T size, DWORD type,
                      DWORD protection) noexcept {
    return VirtualAlloc(address, size, type, protection);
}

BOOL native_free(void*, void* address, SIZE_T size, DWORD type) noexcept {
    return VirtualFree(address, size, type);
}

BOOL native_protect(void*, void* address, SIZE_T size, DWORD protection,
                    DWORD* previous) noexcept {
    return VirtualProtect(address, size, protection, previous);
}

SIZE_T native_query(void*, const void* address, MEMORY_BASIC_INFORMATION* information,
                    SIZE_T size) noexcept {
    return VirtualQuery(address, information, size);
}

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
    windows_reservation(memory_geometry geometry, std::uint64_t size,
                        detail::windows_memory_api api) noexcept
        : geometry_(geometry), size_(size), api_(api) {}
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
        base_ = static_cast<std::byte*>(api_.allocate(api_.context, nullptr,
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
        if (api_.allocate(api_.context, address(offset), static_cast<SIZE_T>(count), MEM_COMMIT,
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
        if (!api_.protect(api_.context, address(offset), static_cast<SIZE_T>(count),
                            *protection, &previous)) {
            return std::unexpected(native_error());
        }
        return {};
    }

    [[nodiscard]] memory_result<void> decommit(std::uint64_t offset,
        std::uint64_t count) noexcept override {
        const auto valid = validate_pages(offset, count, page_state::committed);
        if (!valid) return valid;
        if (!api_.free(api_.context, address(offset), static_cast<SIZE_T>(count), MEM_DECOMMIT)) {
            return std::unexpected(native_error());
        }
        return {};
    }

    [[nodiscard]] memory_result<page_info> query(std::uint64_t offset) const noexcept override {
        if (base_ == nullptr) return std::unexpected(memory_error::invalid_state);
        if (offset >= size_) return std::unexpected(memory_error::invalid_range);
        MEMORY_BASIC_INFORMATION information{};
        if (api_.query(api_.context, address(offset), &information, sizeof(information)) !=
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

    [[nodiscard]] fault_observation observe(const EXCEPTION_RECORD& record) const noexcept {
        if (base_ == nullptr) return {fault_route::unowned, {}};
        const bool in_page = record.ExceptionCode == EXCEPTION_IN_PAGE_ERROR;
        if ((!in_page && record.ExceptionCode != EXCEPTION_ACCESS_VIOLATION) ||
            record.NumberParameters < (in_page ? 3u : 2u) ||
            record.NumberParameters > EXCEPTION_MAXIMUM_PARAMETERS)
            return {fault_route::unsupported, {}};
        fault_access access{};
        switch (record.ExceptionInformation[0]) {
        case 0: access = fault_access::read; break;
        case 1: access = fault_access::write; break;
        case 8: access = fault_access::execute; break;
        default: return {fault_route::unsupported, {}};
        }
        const auto address_value = record.ExceptionInformation[1];
        const auto base_value = reinterpret_cast<ULONG_PTR>(base_);
        if (address_value < base_value || address_value - base_value >= size_)
            return {fault_route::unowned, {}};
        return {fault_route::owned_memory, make_fault_record(size_, address_value - base_value,
            access, in_page ? fault_cause::backing_store_error : fault_cause::access_violation)};
    }

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
        if (!api_.free(api_.context, base_, 0, MEM_RELEASE)) {
            return std::unexpected(native_error());
        }
        base_ = nullptr;
        return {};
    }

    memory_geometry geometry_;
    std::uint64_t size_;
    detail::windows_memory_api api_;
    std::byte* base_{}; // Exclusively owned Windows reservation; never exposed.
};

} // namespace

fault_observation detail::observe_windows_fault(const memory_reservation& owner,
    const EXCEPTION_RECORD& record) noexcept {
    const auto* native_owner = dynamic_cast<const windows_reservation*>(&owner);
    if (native_owner == nullptr) return {fault_route::unsupported, {}};
    return native_owner->observe(record);
}

LONG detail::windows_fault_filter(const memory_reservation& owner,
    const EXCEPTION_RECORD& record, fault_observation& output) noexcept {
    output = observe_windows_fault(owner, record);
    return EXCEPTION_CONTINUE_SEARCH;
}

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
    return detail::reserve_windows_memory_with_api(size,
        {nullptr, native_allocate, native_free, native_protect, native_query});
}

memory_result<std::unique_ptr<memory_reservation>>
detail::reserve_windows_memory_with_api(std::uint64_t size,
                                       windows_memory_api api) noexcept {
    if (api.allocate == nullptr || api.free == nullptr || api.protect == nullptr ||
        api.query == nullptr) {
        return std::unexpected(memory_error::unsupported);
    }
    const auto geometry = windows_memory_geometry();
    if (!geometry) return std::unexpected(geometry.error());
    if (!page_range::make(*geometry, size, 0, size) ||
        size > std::numeric_limits<SIZE_T>::max() ||
        size > static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max())) {
        return std::unexpected(memory_error::invalid_range);
    }
    try {
        // Allocate the owner before acquiring the native mapping.
        auto owner = std::make_unique<windows_reservation>(*geometry, size, api);
        const auto initialized = owner->initialize();
        if (!initialized) return std::unexpected(initialized.error());
        return std::unique_ptr<memory_reservation>(std::move(owner));
    } catch (const std::bad_alloc&) {
        return std::unexpected(memory_error::out_of_memory);
    }
}

} // namespace pcsx5::runtime
