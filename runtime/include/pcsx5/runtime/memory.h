#pragma once

#include <pcsx5/runtime/page_range.h>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>

namespace pcsx5::runtime {

enum class memory_error { invalid_range, invalid_state, unsupported, access_denied,
                          out_of_memory, host_failure };
enum class memory_access { none, read_only, read_write };
enum class page_state { reserved, committed };
struct page_info {
    page_state state;
    memory_access access; // Reserved pages normalize to none.
};
template<class T> using memory_result = std::expected<T, memory_error>;

// Sole owner of anonymous, non-executable host memory. No guest address/native
// pointer adoption. Serialize all calls and destruction externally. Not safe
// for exception/signal handlers. All consumers rebuild with contract changes.
class memory_reservation {
public:
    virtual ~memory_reservation() = default;
    memory_reservation(const memory_reservation&) = delete;
    memory_reservation& operator=(const memory_reservation&) = delete;
    memory_reservation(memory_reservation&&) = delete;
    memory_reservation& operator=(memory_reservation&&) = delete;

    [[nodiscard]] virtual memory_geometry geometry() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t size() const noexcept = 0;
    // Nonempty page-aligned ranges; checked against THIS live reservation.
    // commit requires every page reserved; protect/decommit require committed.
    // Validation failure changes nothing. Native failure returns host_failure
    // or out_of_memory; query actual state before retrying native failures.
    [[nodiscard]] virtual memory_result<void> commit(std::uint64_t offset,
        std::uint64_t size, memory_access access) noexcept = 0;
    [[nodiscard]] virtual memory_result<void> protect(std::uint64_t offset,
        std::uint64_t size, memory_access access) noexcept = 0;
    [[nodiscard]] virtual memory_result<void> decommit(std::uint64_t offset,
        std::uint64_t size) noexcept = 0;
    // Any byte offset within the reservation, not necessarily page aligned.
    [[nodiscard]] virtual memory_result<page_info> query(std::uint64_t offset) const noexcept = 0;
    // Full range checked before copying. Empty spans are invalid_range.
    // Reserved pages return invalid_state; disallowed committed access returns
    // access_denied. Invalid protection enums return unsupported on valid ranges.
    // Caller supplies valid buffers; this is NOT a fault-catching copy API.
    [[nodiscard]] virtual memory_result<void> read(std::uint64_t offset,
        std::span<std::byte> destination) const noexcept = 0;
    [[nodiscard]] virtual memory_result<void> write(std::uint64_t offset,
        std::span<const std::byte> source) noexcept = 0;
    // Idempotent. Success closes the object; later operations return
    // invalid_state (geometry/size stay descriptive). Failure retains ownership.
    // Destructor retries release, terminating if release fails: no silent leak.
    [[nodiscard]] virtual memory_result<void> release() noexcept = 0;

protected:
    memory_reservation() = default;
};

// Windows leaf factory, linked only on Windows. Size must be nonzero and a
// multiple of host page size; no rounding. Geometry comes from the host.
// A successful result always contains a non-null unique owner.
[[nodiscard]] memory_result<memory_geometry> windows_memory_geometry() noexcept;
[[nodiscard]] memory_result<std::unique_ptr<memory_reservation>>
reserve_windows_memory(std::uint64_t size) noexcept;

} // namespace pcsx5::runtime
