#include <pcsx5/core/hle_registry.h>

#include <array>
#include <cstring>
#include <limits>
#include <string_view>

namespace pcsx5::core {

namespace {

std::uint64_t default_stub_handler(hle_context& /*ctx*/) noexcept {
    return 0;
}

} // namespace

void hle_registry::update_indices(std::size_t index) {
    const auto& entry = entries_[index];
    if (!entry.module_name.empty() && !entry.symbol_name.empty()) {
        std::string mod_sym = entry.module_name + ":" + entry.symbol_name;
        by_module_and_name_[mod_sym] = index;
    }
    if (!entry.symbol_name.empty()) {
        by_name_[entry.symbol_name] = index;
    }
    if (entry.nid != 0) {
        by_nid_[entry.nid] = index;
    }
    if (entry.thunk_vaddr != 0) {
        by_thunk_vaddr_[entry.thunk_vaddr] = index;
    }
}

bool hle_registry::write_single_thunk(std::size_t index) noexcept {
    if (bound_memory_ == nullptr || index >= entries_.size()) {
        return false;
    }
    const auto& entry = entries_[index];
    if (entry.thunk_vaddr == 0) {
        return false;
    }

    std::array<std::byte, hle_thunk_size> code{};
    // 0x0F 0x05 (syscall)
    code[0] = std::byte{0x0F};
    code[1] = std::byte{0x05};
    // 0xC3 (ret)
    code[2] = std::byte{0xC3};
    // 0x90 (nop) padding
    for (std::size_t i = 3; i < hle_thunk_size; ++i) {
        code[i] = std::byte{0x90};
    }

    auto write_res = bound_memory_->write(entry.thunk_vaddr, code);
    return write_res.has_value();
}

bool hle_registry::register_handler(
    std::string_view module_name,
    std::string_view symbol_name,
    hle_handler_fn handler,
    void* user_data) {
    if (symbol_name.empty() || handler == nullptr) {
        return false;
    }

    std::uint64_t nid_val = 0;
    auto parsed = parse_nid_string(symbol_name);
    if (parsed) {
        nid_val = parsed->nid;
    } else {
        auto looked_up = lookup_name_nid(symbol_name);
        if (looked_up) {
            nid_val = *looked_up;
        }
    }

    hle_entry entry{
        .module_name = std::string(module_name),
        .symbol_name = std::string(symbol_name),
        .nid = nid_val,
        .handler = handler,
        .user_data = user_data,
        .thunk_vaddr = 0,
        .thunk_index = static_cast<std::uint32_t>(entries_.size()),
        .is_auto_stub = false,
        .call_count = 0,
    };

    if (thunk_base_vaddr_ != 0) {
        entry.thunk_vaddr = thunk_base_vaddr_ + entry.thunk_index * hle_thunk_size;
    }

    const std::size_t idx = entries_.size();
    entries_.push_back(std::move(entry));
    update_indices(idx);

    if (bound_memory_ != nullptr && thunk_base_vaddr_ != 0) {
        write_single_thunk(idx);
    }
    return true;
}

bool hle_registry::register_handler(
    std::string_view module_name,
    std::uint64_t nid,
    hle_handler_fn handler,
    void* user_data) {
    if (nid == 0 || handler == nullptr) {
        return false;
    }

    std::string sym_name;
    auto looked_up_name = lookup_nid_name(nid);
    if (looked_up_name) {
        sym_name = std::string(*looked_up_name);
    } else {
        sym_name = encode_nid(nid);
    }

    hle_entry entry{
        .module_name = std::string(module_name),
        .symbol_name = std::move(sym_name),
        .nid = nid,
        .handler = handler,
        .user_data = user_data,
        .thunk_vaddr = 0,
        .thunk_index = static_cast<std::uint32_t>(entries_.size()),
        .is_auto_stub = false,
        .call_count = 0,
    };

    if (thunk_base_vaddr_ != 0) {
        entry.thunk_vaddr = thunk_base_vaddr_ + entry.thunk_index * hle_thunk_size;
    }

    const std::size_t idx = entries_.size();
    entries_.push_back(std::move(entry));
    update_indices(idx);

    if (bound_memory_ != nullptr && thunk_base_vaddr_ != 0) {
        write_single_thunk(idx);
    }
    return true;
}

const hle_entry* hle_registry::find_handler(
    std::string_view module_name,
    std::string_view symbol_or_nid) const noexcept {
    if (!module_name.empty()) {
        std::string mod_key = std::string(module_name) + ":" + std::string(symbol_or_nid);
        auto it = by_module_and_name_.find(mod_key);
        if (it != by_module_and_name_.end()) {
            return &entries_[it->second];
        }
    }

    auto parsed = parse_nid_string(symbol_or_nid);
    if (parsed) {
        auto nid_it = by_nid_.find(parsed->nid);
        if (nid_it != by_nid_.end()) {
            const auto& candidate = entries_[nid_it->second];
            if (module_name.empty() || candidate.module_name == module_name) {
                return &candidate;
            }
        }
        auto name_opt = lookup_nid_name(parsed->nid);
        if (name_opt) {
            std::string mod_key = std::string(module_name) + ":" + std::string(*name_opt);
            auto it = by_module_and_name_.find(mod_key);
            if (it != by_module_and_name_.end()) {
                return &entries_[it->second];
            }
        }
    } else {
        auto nid_opt = lookup_name_nid(symbol_or_nid);
        if (nid_opt) {
            auto nid_it = by_nid_.find(*nid_opt);
            if (nid_it != by_nid_.end()) {
                const auto& candidate = entries_[nid_it->second];
                if (module_name.empty() || candidate.module_name == module_name) {
                    return &candidate;
                }
            }
        }
    }

    return nullptr;
}

const hle_entry* hle_registry::find_handler(std::string_view symbol_or_nid) const noexcept {
    auto it = by_name_.find(std::string(symbol_or_nid));
    if (it != by_name_.end()) {
        return &entries_[it->second];
    }

    auto parsed = parse_nid_string(symbol_or_nid);
    if (parsed) {
        auto nid_it = by_nid_.find(parsed->nid);
        if (nid_it != by_nid_.end()) {
            return &entries_[nid_it->second];
        }
        auto name_opt = lookup_nid_name(parsed->nid);
        if (name_opt) {
            auto name_it = by_name_.find(std::string(*name_opt));
            if (name_it != by_name_.end()) {
                return &entries_[name_it->second];
            }
        }
    } else {
        auto nid_opt = lookup_name_nid(symbol_or_nid);
        if (nid_opt) {
            auto nid_it = by_nid_.find(*nid_opt);
            if (nid_it != by_nid_.end()) {
                return &entries_[nid_it->second];
            }
        }
    }

    return nullptr;
}

const hle_entry* hle_registry::find_handler(std::uint64_t nid) const noexcept {
    auto it = by_nid_.find(nid);
    if (it != by_nid_.end()) {
        return &entries_[it->second];
    }
    return nullptr;
}

const hle_entry* hle_registry::find_handler_by_thunk(std::uint64_t thunk_vaddr) const noexcept {
    auto it = by_thunk_vaddr_.find(thunk_vaddr);
    if (it != by_thunk_vaddr_.end()) {
        return &entries_[it->second];
    }
    return nullptr;
}

std::expected<std::size_t, hle_error> hle_registry::emit_thunks(
    std::uint64_t thunk_base_vaddr,
    guest_memory& memory) noexcept {
    if (thunk_base_vaddr == 0) {
        return std::unexpected(hle_error::invalid_argument);
    }

    thunk_base_vaddr_ = thunk_base_vaddr;
    bound_memory_ = &memory;
    by_thunk_vaddr_.clear();

    for (std::size_t i = 0; i < entries_.size(); ++i) {
        entries_[i].thunk_index = static_cast<std::uint32_t>(i);
        entries_[i].thunk_vaddr = thunk_base_vaddr_ + i * hle_thunk_size;
        by_thunk_vaddr_[entries_[i].thunk_vaddr] = i;

        if (!write_single_thunk(i)) {
            return std::unexpected(hle_error::thunk_write_failed);
        }
    }

    return entries_.size();
}

std::expected<std::uint64_t, hle_error> hle_registry::resolve_or_stub(
    std::string_view module_name,
    std::string_view symbol_name,
    bool strict_mode) noexcept {
    const hle_entry* found = find_handler(module_name, symbol_name);
    if (found == nullptr) {
        found = find_handler(symbol_name);
    }

    if (found != nullptr) {
        return found->thunk_vaddr;
    }

    if (strict_mode) {
        return std::unexpected(hle_error::symbol_not_found);
    }

    std::uint64_t nid_val = 0;
    auto parsed = parse_nid_string(symbol_name);
    if (parsed) {
        nid_val = parsed->nid;
    } else {
        auto looked_up = lookup_name_nid(symbol_name);
        if (looked_up) {
            nid_val = *looked_up;
        }
    }

    hle_entry stub{
        .module_name = std::string(module_name),
        .symbol_name = std::string(symbol_name),
        .nid = nid_val,
        .handler = default_stub_handler,
        .user_data = nullptr,
        .thunk_vaddr = 0,
        .thunk_index = static_cast<std::uint32_t>(entries_.size()),
        .is_auto_stub = true,
        .call_count = 0,
    };

    if (thunk_base_vaddr_ != 0) {
        stub.thunk_vaddr = thunk_base_vaddr_ + stub.thunk_index * hle_thunk_size;
    }

    const std::size_t idx = entries_.size();
    entries_.push_back(std::move(stub));
    update_indices(idx);

    if (bound_memory_ != nullptr && thunk_base_vaddr_ != 0) {
        if (!write_single_thunk(idx)) {
            return std::unexpected(hle_error::thunk_write_failed);
        }
    }

    return entries_[idx].thunk_vaddr;
}

std::expected<std::uint64_t, hle_error> hle_registry::dispatch(
    std::uint64_t thunk_vaddr,
    hle_context& ctx) noexcept {
    auto it = by_thunk_vaddr_.find(thunk_vaddr);
    if (it == by_thunk_vaddr_.end()) {
        return std::unexpected(hle_error::symbol_not_found);
    }

    auto& entry = entries_[it->second];
    ++entry.call_count;

    if (entry.handler == nullptr) {
        ctx.rax = 0;
        return 0;
    }

    const std::uint64_t res = entry.handler(ctx);
    ctx.rax = res;
    return res;
}

std::expected<std::uint64_t, relocation_error> hle_symbol_resolver(
    std::uint32_t symbol_index, void* context) noexcept {
    if (context == nullptr) {
        return std::unexpected(relocation_error::symbol_resolution_failed);
    }
    auto* ctx = static_cast<hle_symbol_context*>(context);
    if (ctx->registry == nullptr || symbol_index >= ctx->symtab.size()) {
        return std::unexpected(relocation_error::symbol_resolution_failed);
    }

    const auto& sym = ctx->symtab[symbol_index];
    if (sym.st_shndx != shn_undef) {
        // Internal module symbol: base + st_value
        return ctx->base_address + sym.st_value;
    }

    // External import: read name from strtab
    if (sym.st_name >= ctx->strtab.size()) {
        return std::unexpected(relocation_error::symbol_resolution_failed);
    }

    const char* name_ptr = ctx->strtab.data() + sym.st_name;
    const std::size_t max_len = ctx->strtab.size() - sym.st_name;
    std::size_t len = 0;
    while (len < max_len && name_ptr[len] != '\0') {
        ++len;
    }
    const std::string_view sym_name(name_ptr, len);

    auto res = ctx->registry->resolve_or_stub(ctx->module_name, sym_name, ctx->strict_mode);
    if (!res) {
        return std::unexpected(relocation_error::symbol_resolution_failed);
    }
    return *res;
}

} // namespace pcsx5::core
