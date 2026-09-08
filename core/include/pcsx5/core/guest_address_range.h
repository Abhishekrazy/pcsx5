#pragma once

#include <cstdint>
#include <limits>
#include <optional>

namespace pcsx5::core {

// A nonempty range of guest byte addresses, not a host mapping or pointer.
class guest_address_range {
public:
    [[nodiscard]] static constexpr std::optional<guest_address_range> make(
        std::uint64_t base, std::uint64_t size) noexcept {
        if (size == 0 || size - 1 > std::numeric_limits<std::uint64_t>::max() - base) {
            return std::nullopt;
        }
        return guest_address_range{base, size};
    }

    [[nodiscard]] constexpr std::uint64_t base() const noexcept { return base_; }
    [[nodiscard]] constexpr std::uint64_t size() const noexcept { return size_; }

    [[nodiscard]] constexpr bool contains(std::uint64_t address) const noexcept {
        return address >= base_ && address - base_ < size_;
    }

    [[nodiscard]] constexpr bool contains(guest_address_range other) const noexcept {
        return contains(other.base_) && other.size_ <= size_ - (other.base_ - base_);
    }

    [[nodiscard]] constexpr bool overlaps(guest_address_range other) const noexcept {
        return contains(other.base_) || other.contains(base_);
    }

    [[nodiscard]] constexpr std::optional<std::uint64_t> offset_of(
        std::uint64_t address) const noexcept {
        if (!contains(address)) {
            return std::nullopt;
        }
        return address - base_;
    }

private:
    constexpr guest_address_range(std::uint64_t base, std::uint64_t size) noexcept
        : base_{base}, size_{size} {}

    std::uint64_t base_;
    std::uint64_t size_;
};

} // namespace pcsx5::core
