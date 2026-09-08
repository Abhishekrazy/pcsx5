#pragma once
#include <cstddef>
#include <expected>
#include <memory>
#include <span>
namespace pcsx5::runtime {
enum class code_isa { x64, arm64 };
enum class code_error { invalid, unavailable, out_of_memory, host_failure };
template<class T> using code_result=std::expected<T,code_error>;
inline constexpr std::size_t maximum_code_bytes=32768;
class executable_code {
public:
    executable_code(const executable_code&)=delete;
    executable_code& operator=(const executable_code&)=delete;
    virtual ~executable_code()=default;
    [[nodiscard]] virtual std::size_t size() const noexcept=0;
    [[nodiscard]] virtual std::size_t page_size() const noexcept=0;
    // Trusted emitter output only. Entry is void(void*), using the host ABI;
    // caller owns valid argument storage for the entire call. Native faults are
    // not caught. This API is not an arbitrary-code security boundary.
    [[nodiscard]] virtual code_result<void> invoke(void* argument) noexcept=0;
    [[nodiscard]] virtual code_result<void> release() noexcept=0;
protected:
    executable_code()=default;
};
// Copy once into owned RW pages, synchronize caches and seal RX before return.
// No writable alias, raw pointer escape or later mutation. Wrong host ISA fails.
// Explicit release is idempotent; failed release retains ownership. Destruction
// retries release and terminates on failure. Calls require external serialization.
[[nodiscard]] code_result<std::unique_ptr<executable_code>> create_posix_code(
    std::span<const std::byte>,code_isa) noexcept;
[[nodiscard]] code_result<std::unique_ptr<executable_code>> create_windows_code(
    std::span<const std::byte>,code_isa) noexcept;
// Apple Silicon: one process-owned MAP_JIT arena, at most one live image.
// Trusted serialized emission uses per-thread W^X; the arena is retained until
// process shutdown. Requires the allow-jit entitlement for hardened executables;
// do not combine with jit-write-allowlist (a different callback-based policy).
// No physical Mac verification has been recorded yet.
[[nodiscard]] code_result<std::unique_ptr<executable_code>> create_darwin_code(
    std::span<const std::byte>,code_isa) noexcept;
} // namespace pcsx5::runtime
