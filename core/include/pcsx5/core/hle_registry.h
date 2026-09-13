#pragma once

#include <pcsx5/core/dynamic_info.h>
#include <pcsx5/core/elf_types.h>
#include <pcsx5/core/guest_memory.h>
#include <pcsx5/core/nid.h>
#include <pcsx5/core/relocation.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pcsx5::core {

inline constexpr std::size_t hle_thunk_size = 16;

struct hle_context {
    // System V AMD64 ABI argument registers: RDI, RSI, RDX, RCX, R8, R9
    std::uint64_t args[6]{};
    std::uint64_t rax{0};
    std::uint64_t rip{0};
    std::uint64_t rsp{0};
    guest_memory* memory{nullptr};
    void* user_data{nullptr};
};

using hle_handler_fn = std::uint64_t (*)(hle_context& ctx) noexcept;

struct hle_entry {
    std::string module_name;
    std::string symbol_name;
    std::uint64_t nid{0};
    hle_handler_fn handler{nullptr};
    void* user_data{nullptr};
    std::uint64_t thunk_vaddr{0};
    std::uint32_t thunk_index{0};
    bool is_auto_stub{false};
    std::uint64_t call_count{0};
};

enum class hle_error {
    none,
    symbol_not_found,
    thunk_allocation_failed,
    thunk_write_failed,
    invalid_argument,
};

class hle_registry {
public:
    hle_registry() = default;

    // Registers a handler by module name and canonical symbol name or encoded NID string.
    bool register_handler(
        std::string_view module_name,
        std::string_view symbol_name,
        hle_handler_fn handler,
        void* user_data = nullptr);

    // Registers a handler by module name and 64-bit NID.
    bool register_handler(
        std::string_view module_name,
        std::uint64_t nid,
        hle_handler_fn handler,
        void* user_data = nullptr);

    // Finds entry by exact module and symbol name / NID string.
    [[nodiscard]] const hle_entry* find_handler(
        std::string_view module_name,
        std::string_view symbol_or_nid) const noexcept;

    // Finds entry across all modules by symbol name or NID string.
    [[nodiscard]] const hle_entry* find_handler(
        std::string_view symbol_or_nid) const noexcept;

    // Finds entry by 64-bit NID.
    [[nodiscard]] const hle_entry* find_handler(
        std::uint64_t nid) const noexcept;

    // Finds entry by thunk virtual address.
    [[nodiscard]] const hle_entry* find_handler_by_thunk(
        std::uint64_t thunk_vaddr) const noexcept;

    // Writes trampoline / thunk opcodes into guest memory at thunk_base_vaddr.
    // Each thunk occupies 16 bytes:
    //   0x0F 0x05  (syscall)
    //   0xC3        (ret)
    //   0x90 ...    (nop padding)
    [[nodiscard]] std::expected<std::size_t, hle_error> emit_thunks(
        std::uint64_t thunk_base_vaddr,
        guest_memory& memory) noexcept;

    // Resolves a symbol name to its thunk virtual address, creating an auto-stub if not found.
    // If strict_mode is true, returns symbol_not_found instead of auto-stubbing.
    [[nodiscard]] std::expected<std::uint64_t, hle_error> resolve_or_stub(
        std::string_view module_name,
        std::string_view symbol_name,
        bool strict_mode = false) noexcept;

    // Dispatches a call to the HLE function at thunk_vaddr.
    // Returns the value that should be placed in guest RAX.
    [[nodiscard]] std::expected<std::uint64_t, hle_error> dispatch(
        std::uint64_t thunk_vaddr,
        hle_context& ctx) noexcept;

    // Total registered entries (including auto stubs).
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    // Read-only access to all entries.
    [[nodiscard]] std::span<const hle_entry> entries() const noexcept { return entries_; }

    [[nodiscard]] std::uint64_t thunk_base_vaddr() const noexcept { return thunk_base_vaddr_; }

private:
    void update_indices(std::size_t index);
    bool write_single_thunk(std::size_t index) noexcept;

    std::vector<hle_entry> entries_;
    std::unordered_map<std::string, std::size_t> by_module_and_name_;
    std::unordered_map<std::string, std::size_t> by_name_;
    std::unordered_map<std::uint64_t, std::size_t> by_nid_;
    std::unordered_map<std::uint64_t, std::size_t> by_thunk_vaddr_;
    std::uint64_t thunk_base_vaddr_{0};
    guest_memory* bound_memory_{nullptr};
};

struct hle_symbol_context {
    hle_registry* registry{nullptr};
    std::span<const elf64_sym> symtab;
    std::span<const char> strtab;
    std::string module_name;
    std::uint64_t base_address{0};
    bool strict_mode{false};
};

// Callback matching `symbol_resolver` for `apply_relocations`.
[[nodiscard]] std::expected<std::uint64_t, relocation_error> hle_symbol_resolver(
    std::uint32_t symbol_index, void* context) noexcept;

} // namespace pcsx5::core
