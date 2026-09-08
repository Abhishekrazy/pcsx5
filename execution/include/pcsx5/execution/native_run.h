#pragma once
#include <pcsx5/execution/native_step.h>
#include <vector>
namespace pcsx5::execution {
using native_step_provider = native_result (*)(const std::filesystem::path&,
    const native_request&,std::uint32_t) noexcept;
enum class native_run_stop { returned, requested_exit, guest_fault, unsupported, budget_exhausted, divergence };
struct native_run_request {
    native_request machine{};
    std::uint32_t budget{1}; // 1..256; each step uses a new owned child.
    std::uint64_t return_address{native_code_base+native_page_bytes-1};
};
struct native_run_record {
    cpu_state before{};
    // Physical values are actual hardware. After a successful comparison only
    // known_flags is annotated with oracle-definedness (not a hardware register).
    native_snapshot observed{};
    step_result preflight{}; // Interpreter decode/effects, NOT native evidence.
};
struct native_run_response {
    native_run_stop stop{};
    cpu_state final{};
    std::array<std::byte,native_page_bytes> data{};
    std::uint64_t retired{};
    std::vector<native_run_record> records;
};
// Verification-first native runner over trusted authored images. Interpreter
// preflight whitelists the scalar subset; SYSCALL/unsupported encodings NEVER
// reach the native provider. INT3 is the explicit requested-exit marker and is
// actually executed; its reported post-trap RIP stays in observed/final state.
// Arrival at return_address is a caller-defined normal-return boundary. Data
// and registers are carried between fresh children, not hidden host TLS/state.
// Native/defined-oracle mismatch is divergence, never a successful fallback.
// timeout_ms applies separately to each step, not to the entire run.
[[nodiscard]] std::expected<native_run_response,native_error> run_native(
    const std::filesystem::path& helper,const native_run_request&,
    native_step_provider,std::uint32_t timeout_ms) noexcept;
} // namespace pcsx5::execution
