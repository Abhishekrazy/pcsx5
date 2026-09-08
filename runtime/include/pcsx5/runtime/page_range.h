#pragma once

#include <cstdint>
#include <optional>

namespace pcsx5::runtime {

// Host page geometry is supplied by a provider, never inferred from guest state.
class memory_geometry final {
public:
    [[nodiscard]] static constexpr std::optional<memory_geometry> make(
        std::uint64_t page_size, std::uint64_t reservation_alignment) noexcept {
        if (page_size == 0 || reservation_alignment == 0 ||
            reservation_alignment % page_size != 0) {
            return std::nullopt;
        }
        return memory_geometry(page_size, reservation_alignment);
    }

    [[nodiscard]] constexpr std::uint64_t page_size() const noexcept { return page_size_; }
    [[nodiscard]] constexpr std::uint64_t reservation_alignment() const noexcept {
        return reservation_alignment_;
    }

private:
    constexpr memory_geometry(std::uint64_t page, std::uint64_t alignment) noexcept
        : page_size_(page), reservation_alignment_(alignment) {}
    std::uint64_t page_size_;
    std::uint64_t reservation_alignment_;
};

// A validated byte range relative to a reservation, NOT an ownership token.
class page_range final {
public:
    [[nodiscard]] static constexpr std::optional<page_range> make(
        memory_geometry geometry, std::uint64_t reservation_size,
        std::uint64_t offset, std::uint64_t size) noexcept {
        const auto page = geometry.page_size();
        if (reservation_size == 0 || reservation_size % page != 0 ||
            size == 0 || offset % page != 0 || size % page != 0 ||
            offset > reservation_size || size > reservation_size - offset) {
            return std::nullopt;
        }
        return page_range(offset, size);
    }

    [[nodiscard]] constexpr std::uint64_t offset() const noexcept { return offset_; }
    [[nodiscard]] constexpr std::uint64_t size() const noexcept { return size_; }

private:
    constexpr page_range(std::uint64_t offset, std::uint64_t size) noexcept
        : offset_(offset), size_(size) {}
    std::uint64_t offset_;
    std::uint64_t size_;
};

} // namespace pcsx5::runtime
