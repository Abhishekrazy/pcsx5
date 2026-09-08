#pragma once
#include <pcsx5/core/guest_address_range.h>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <vector>

namespace pcsx5::core {
enum class guest_memory_error { invalid_range, invalid_state, overlap, unmapped,
    access_denied, unsupported, out_of_memory, host_failure };
enum class guest_memory_access { none, read_only, read_write };
template<class T> using guest_memory_result = std::expected<T, guest_memory_error>;

// Runtime implements this core-owned port. Live storage has immutable size.
// Calls are externally serialized; buffers must be valid. Release is idempotent,
// retains ownership on failure; destructor is responsible for final cleanup.
class memory_backing {
public:
    virtual ~memory_backing() = default;
    memory_backing(const memory_backing&) = delete;
    memory_backing& operator=(const memory_backing&) = delete;
    memory_backing(memory_backing&&) = delete;
    memory_backing& operator=(memory_backing&&) = delete;
    [[nodiscard]] virtual std::uint64_t size() const noexcept = 0;
    [[nodiscard]] virtual guest_memory_result<void> read(std::uint64_t offset, std::span<std::byte>) const noexcept = 0;
    [[nodiscard]] virtual guest_memory_result<void> write(std::uint64_t offset, std::span<const std::byte>) noexcept = 0;
    [[nodiscard]] virtual guest_memory_result<void> release() noexcept = 0;
protected:
    memory_backing() = default;
};

// Software guest permissions; not host isolation. No native pointers or pages.
class guest_memory {
public:
    guest_memory() = default;
    ~guest_memory() = default;
    guest_memory(const guest_memory&) = delete;
    guest_memory& operator=(const guest_memory&) = delete;
    guest_memory(guest_memory&&) = delete;
    guest_memory& operator=(guest_memory&&) = delete;
    // Consumes backing only on success; it must be live and exclusively owned.
    [[nodiscard]] guest_memory_result<void> map(std::uint64_t base, std::uint64_t size,
        guest_memory_access, std::unique_ptr<memory_backing>& backing) noexcept;
    [[nodiscard]] guest_memory_result<void> unmap(std::uint64_t base) noexcept;
    [[nodiscard]] guest_memory_result<void> protect(std::uint64_t base, guest_memory_access) noexcept;
    [[nodiscard]] guest_memory_result<void> read(std::uint64_t address, std::span<std::byte>) const noexcept;
    [[nodiscard]] guest_memory_result<void> write(std::uint64_t address, std::span<const std::byte>) noexcept;
    [[nodiscard]] std::size_t mapping_count() const noexcept { return mappings_.size(); }
private:
    struct mapping { guest_address_range range; guest_memory_access access; std::unique_ptr<memory_backing> backing; };
    [[nodiscard]] guest_memory_result<const mapping*> locate(std::uint64_t address, std::uint64_t size) const noexcept;
    std::vector<mapping> mappings_;
};
} // namespace pcsx5::core
