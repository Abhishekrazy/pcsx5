#include <pcsx5/core/guest_memory.h>
#include <new>
#include <stdexcept>
#include <utility>

namespace pcsx5::core {
namespace {
bool valid_access(guest_memory_access access) noexcept {
    return access == guest_memory_access::none || access == guest_memory_access::read_only ||
        access == guest_memory_access::read_write;
}
}
guest_memory_result<void> guest_memory::map(std::uint64_t base, std::uint64_t size,
    guest_memory_access access, std::unique_ptr<memory_backing>& backing) noexcept {
    const auto range = guest_address_range::make(base, size);
    if (!range) return std::unexpected(guest_memory_error::invalid_range);
    if (!backing) return std::unexpected(guest_memory_error::invalid_state);
    if (backing->size() != size) return std::unexpected(guest_memory_error::invalid_range);
    if (!valid_access(access)) return std::unexpected(guest_memory_error::unsupported);
    for (const auto& entry : mappings_) {
        if (entry.range.overlaps(*range)) return std::unexpected(guest_memory_error::overlap);
    }
    try {
        // Allocate the entry before transferring ownership: failure leaves input intact.
        mappings_.push_back(mapping{*range, access, nullptr});
    } catch (const std::bad_alloc&) {
        return std::unexpected(guest_memory_error::out_of_memory);
    } catch (const std::length_error&) {
        return std::unexpected(guest_memory_error::out_of_memory);
    }
    mappings_.back().backing = std::move(backing);
    return {};
}
guest_memory_result<void> guest_memory::unmap(std::uint64_t base) noexcept {
    for (auto it = mappings_.begin(); it != mappings_.end(); ++it) {
        if (it->range.base() != base) continue;
        const auto released = it->backing->release();
        if (!released) return released;
        mappings_.erase(it);
        return {};
    }
    return std::unexpected(guest_memory_error::unmapped);
}
guest_memory_result<void> guest_memory::protect(std::uint64_t base, guest_memory_access access) noexcept {
    if (!valid_access(access)) return std::unexpected(guest_memory_error::unsupported);
    for (auto& entry : mappings_) {
        if (entry.range.base() == base) { entry.access = access; return {}; }
    }
    return std::unexpected(guest_memory_error::unmapped);
}
guest_memory_result<const guest_memory::mapping*> guest_memory::locate(std::uint64_t address, std::uint64_t size) const noexcept {
    const auto range = guest_address_range::make(address, size);
    if (!range) return std::unexpected(guest_memory_error::invalid_range);
    for (const auto& entry : mappings_) {
        if (!entry.range.contains(address)) continue;
        if (!entry.range.contains(*range)) return std::unexpected(guest_memory_error::invalid_range);
        return &entry;
    }
    return std::unexpected(guest_memory_error::unmapped);
}
guest_memory_result<void> guest_memory::read(std::uint64_t address, std::span<std::byte> out) const noexcept {
    const auto entry = locate(address, out.size());
    if (!entry) return std::unexpected(entry.error());
    if ((*entry)->access == guest_memory_access::none) return std::unexpected(guest_memory_error::access_denied);
    return (*entry)->backing->read(address - (*entry)->range.base(), out);
}
guest_memory_result<void> guest_memory::write(std::uint64_t address, std::span<const std::byte> in) noexcept {
    const auto entry = locate(address, in.size());
    if (!entry) return std::unexpected(entry.error());
    if ((*entry)->access != guest_memory_access::read_write) return std::unexpected(guest_memory_error::access_denied);
    return (*entry)->backing->write(address - (*entry)->range.base(), in);
}
} // namespace pcsx5::core
