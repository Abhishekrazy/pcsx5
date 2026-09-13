#include <pcsx5/core/relocation.h>

#include <array>
#include <cstring>
#include <limits>

namespace pcsx5::core {

std::expected<std::size_t, relocation_error> apply_relocations(
    std::span<const elf64_rela> relocations,
    std::uint64_t base_address,
    guest_memory& memory,
    symbol_resolver resolver,
    void* resolver_context) noexcept {
    std::size_t applied_count = 0;

    for (const auto& rela : relocations) {
        const auto type = static_cast<std::uint32_t>(rela.r_info & 0xFFFFFFFF);
        const auto sym_idx = static_cast<std::uint32_t>(rela.r_info >> 32);

        if (type == r_x86_64_none) {
            continue;
        }

        if (rela.r_offset > std::numeric_limits<std::uint64_t>::max() - base_address) {
            return std::unexpected(relocation_error::overflow);
        }

        const std::uint64_t target_vaddr = base_address + rela.r_offset;

        if (type == r_x86_64_relative) {
            if (rela.r_addend < 0 && static_cast<std::uint64_t>(-rela.r_addend) > base_address) {
                return std::unexpected(relocation_error::overflow);
            }
            const std::uint64_t computed = base_address + static_cast<std::uint64_t>(rela.r_addend);
            std::array<std::byte, sizeof(std::uint64_t)> bytes{};
            std::memcpy(bytes.data(), &computed, sizeof(computed));

            auto write_res = memory.write(target_vaddr, bytes);
            if (!write_res) {
                return std::unexpected(relocation_error::write_failed);
            }
            ++applied_count;
        } else if (type == r_x86_64_64) {
            if (resolver == nullptr) {
                return std::unexpected(relocation_error::symbol_resolution_failed);
            }
            auto sym_res = resolver(sym_idx, resolver_context);
            if (!sym_res) {
                return std::unexpected(sym_res.error());
            }

            const std::uint64_t computed = *sym_res + static_cast<std::uint64_t>(rela.r_addend);
            std::array<std::byte, sizeof(std::uint64_t)> bytes{};
            std::memcpy(bytes.data(), &computed, sizeof(computed));

            auto write_res = memory.write(target_vaddr, bytes);
            if (!write_res) {
                return std::unexpected(relocation_error::write_failed);
            }
            ++applied_count;
        } else if (type == r_x86_64_glob_dat || type == r_x86_64_jump_slot) {
            if (resolver == nullptr) {
                return std::unexpected(relocation_error::symbol_resolution_failed);
            }
            auto sym_res = resolver(sym_idx, resolver_context);
            if (!sym_res) {
                return std::unexpected(sym_res.error());
            }

            const std::uint64_t computed = *sym_res;
            std::array<std::byte, sizeof(std::uint64_t)> bytes{};
            std::memcpy(bytes.data(), &computed, sizeof(computed));

            auto write_res = memory.write(target_vaddr, bytes);
            if (!write_res) {
                return std::unexpected(relocation_error::write_failed);
            }
            ++applied_count;
        } else {
            return std::unexpected(relocation_error::unsupported_relocation);
        }
    }

    return applied_count;
}

} // namespace pcsx5::core
