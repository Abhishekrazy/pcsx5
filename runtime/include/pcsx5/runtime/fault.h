#pragma once
#include <cstdint>
#include <optional>

namespace pcsx5::runtime {
enum class fault_access { read, write, execute };
enum class fault_cause { access_violation, backing_store_error };
enum class fault_route { unowned, unsupported, owned_memory };
struct fault_record {
    std::uint64_t offset;
    fault_access access;
    fault_cause cause;
};
struct fault_observation {
    fault_route route;
    std::optional<fault_record> record;
};

// Arithmetic normalization only; the host adapter must first establish live
// ownership. No native address, instruction pointer or guest ABI is inferred.
[[nodiscard]] constexpr std::optional<fault_record> make_fault_record(
    std::uint64_t size, std::uint64_t offset, fault_access access, fault_cause cause) noexcept {
    if (offset >= size) return std::nullopt;
    if (access != fault_access::read && access != fault_access::write && access != fault_access::execute)
        return std::nullopt;
    if (cause != fault_cause::access_violation && cause != fault_cause::backing_store_error)
        return std::nullopt;
    return fault_record{offset, access, cause};
}
} // namespace pcsx5::runtime
