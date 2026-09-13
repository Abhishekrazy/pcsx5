#include <pcsx5/frontend/session.h>

#include <pcsx5/core/dynamic_info.h>
#include <pcsx5/core/elf_parser.h>
#include <pcsx5/core/hle_registry.h>
#include <pcsx5/core/loader.h>
#include <pcsx5/core/relocation.h>
#include <pcsx5/execution/arm64_step.h>
#include <pcsx5/execution/interpreter.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <vector>

namespace pcsx5::frontend {

namespace {

class raw_image_backing final : public core::memory_backing {
public:
    explicit raw_image_backing(std::span<const std::byte> input) {
        std::copy(input.begin(), input.end(), data_.begin());
    }
    std::uint64_t size() const noexcept override { return data_.size(); }
    core::guest_memory_result<void> release() noexcept override { return {}; }
    core::guest_memory_result<void> read(std::uint64_t offset, std::span<std::byte> output) const noexcept override {
        if (output.size() > data_.size() || offset > data_.size() - output.size()) {
            return std::unexpected(core::guest_memory_error::invalid_range);
        }
        std::copy_n(data_.begin() + static_cast<std::size_t>(offset), output.size(), output.begin());
        return {};
    }
    core::guest_memory_result<void> write(std::uint64_t offset, std::span<const std::byte> input) noexcept override {
        if (input.size() > data_.size() || offset > data_.size() - input.size()) {
            return std::unexpected(core::guest_memory_error::invalid_range);
        }
        std::copy(input.begin(), input.end(), data_.begin() + static_cast<std::size_t>(offset));
        return {};
    }
private:
    std::array<std::byte, 4096> data_{};
};

class heap_memory_backing final : public core::memory_backing {
public:
    explicit heap_memory_backing(std::uint64_t size) : storage_(static_cast<std::size_t>(size), std::byte{0}) {}
    std::uint64_t size() const noexcept override { return storage_.size(); }
    core::guest_memory_result<void> release() noexcept override { return {}; }
    core::guest_memory_result<void> read(std::uint64_t offset, std::span<std::byte> output) const noexcept override {
        if (output.size() > storage_.size() || offset > storage_.size() - output.size()) {
            return std::unexpected(core::guest_memory_error::invalid_range);
        }
        std::memcpy(output.data(), storage_.data() + offset, output.size());
        return {};
    }
    core::guest_memory_result<void> write(std::uint64_t offset, std::span<const std::byte> input) noexcept override {
        if (input.size() > storage_.size() || offset > storage_.size() - input.size()) {
            return std::unexpected(core::guest_memory_error::invalid_range);
        }
        std::memcpy(storage_.data() + offset, input.data(), input.size());
        return {};
    }
private:
    std::vector<std::byte> storage_;
};

core::guest_memory_result<std::unique_ptr<core::memory_backing>> default_heap_backing_factory(
    std::uint64_t size) noexcept {
    try {
        return std::make_unique<heap_memory_backing>(size);
    } catch (...) {
        return std::unexpected(core::guest_memory_error::out_of_memory);
    }
}

std::optional<std::uint64_t> vaddr_to_file_offset(
    std::uint64_t vaddr,
    std::span<const core::elf64_phdr> phdrs) noexcept {
    for (const auto& phdr : phdrs) {
        if (phdr.p_type == core::pt_load) {
            if (vaddr >= phdr.p_vaddr && vaddr < phdr.p_vaddr + phdr.p_filesz) {
                return phdr.p_offset + (vaddr - phdr.p_vaddr);
            }
        }
    }
    return std::nullopt;
}

} // namespace

std::expected<session_result, session_error> run_program(
    std::span<const std::byte> input,
    std::uint64_t budget,
    execution::code_factory factory) noexcept {
    if (input.empty() || input.size() > 4096 || !budget || budget > 1'000'000) {
        return std::unexpected(session_error::invalid_input);
    }
    try {
        core::guest_memory memory;
        std::unique_ptr<core::memory_backing> backing = std::make_unique<raw_image_backing>(input);
        if (!memory.map(0x1000, 4096, core::guest_memory_access::read_write, backing)) {
            return std::unexpected(session_error::out_of_memory);
        }
        session_result result{};
        result.state.rip = 0x1000;
        result.state.gpr[4] = 0x2000;
        for (std::uint64_t i = 0; i < budget; ++i) {
            execution::step_result step{};
            bool translated = false;
            if (factory) {
                auto next = execution::step_arm64(result.state, memory, factory);
                if (!next) return std::unexpected(session_error::runtime_failure);
                step = next->result;
                translated = next->translated;
            } else {
                step = execution::step(result.state, memory);
            }
            result.stop = step.reason;
            if (step.reason != execution::stop_reason::completed) {
                return result;
            }
            ++result.retired;
            translated ? ++result.translated : ++result.interpreted;
        }
        result.stop = execution::stop_reason::budget_exhausted;
        return result;
    } catch (const std::bad_alloc&) {
        return std::unexpected(session_error::out_of_memory);
    }
}

std::expected<elf_session_result, session_error> run_elf(
    std::span<const std::byte> elf_or_self_bytes,
    const elf_session_config& config) noexcept {
    if (elf_or_self_bytes.empty() || !config.budget || config.budget > 10'000'000) {
        return std::unexpected(session_error::invalid_input);
    }

    try {
        auto parsed_res = core::parse_self_or_elf(elf_or_self_bytes);
        if (!parsed_res) {
            return std::unexpected(session_error::invalid_input);
        }
        const auto& parsed = *parsed_res;

        core::backing_factory b_factory = (config.backing_factory != nullptr)
            ? config.backing_factory
            : default_heap_backing_factory;

        core::guest_memory memory;
        const std::uint64_t preferred_base = config.base_address_slide.value_or(0x400000);
        auto load_res = core::load_module(
            parsed, preferred_base, memory, b_factory);
        if (!load_res) {
            return std::unexpected(session_error::runtime_failure);
        }

        elf_session_result result{};
        result.module = std::move(*load_res);

        // Map stack
        auto stack_backing_res = b_factory(config.stack_size);
        if (!stack_backing_res) {
            return std::unexpected(session_error::out_of_memory);
        }
        auto stack_backing = std::move(*stack_backing_res);
        if (!memory.map(config.stack_base, config.stack_size, core::guest_memory_access::read_write, stack_backing)) {
            return std::unexpected(session_error::out_of_memory);
        }

        // Set up stack top with a sentinel return address (0) so returning from entry point cleanly stops
        const std::uint64_t initial_rsp = config.stack_base + config.stack_size - 8;
        const std::uint64_t zero_ret = 0;
        std::array<std::byte, 8> ret_bytes{};
        std::memcpy(ret_bytes.data(), &zero_ret, sizeof(zero_ret));
        auto write_ret = memory.write(initial_rsp, ret_bytes);
        if (!write_ret) {
            return std::unexpected(session_error::runtime_failure);
        }

        // If an HLE registry is provided, map the thunk page and emit thunks
        if (config.registry != nullptr) {
            auto thunk_backing_res = b_factory(0x10000);
            if (thunk_backing_res) {
                auto thunk_backing = std::move(*thunk_backing_res);
                auto thunk_map = memory.map(
                    config.thunk_base, 0x10000, core::guest_memory_access::read_write, thunk_backing);
                if (thunk_map) {
                    auto emit_res = config.registry->emit_thunks(config.thunk_base, memory);
                    (void)emit_res;
                }
            }
        }

        // Process PT_DYNAMIC and relocations if present
        for (const auto& phdr : parsed.program_headers) {
            if (phdr.p_type == core::pt_dynamic) {
                auto dyn_res = core::parse_dynamic_table(phdr, parsed.program_headers, parsed.image_bytes);
                if (dyn_res) {
                    result.dynamic = std::move(*dyn_res);

                    if (config.registry != nullptr) {

                        // Prepare symbol resolver context
                        core::hle_symbol_context sym_ctx{};
                        sym_ctx.registry = config.registry;
                        sym_ctx.module_name = "main";
                        sym_ctx.base_address = result.module.base_address;
                        sym_ctx.strict_mode = config.strict_import_mode;

                        // Find symtab and strtab in image
                        std::vector<core::elf64_sym> symtab_vec;
                        std::vector<char> strtab_vec;

                        if (result.dynamic->symtab_vaddr != 0) {
                            auto sym_off = vaddr_to_file_offset(result.dynamic->symtab_vaddr, parsed.program_headers);
                            if (sym_off && *sym_off < parsed.image_bytes.size()) {
                                const std::size_t avail = parsed.image_bytes.size() - *sym_off;
                                const std::size_t sym_count = avail / sizeof(core::elf64_sym);
                                symtab_vec.resize(sym_count);
                                std::memcpy(symtab_vec.data(), parsed.image_bytes.data() + *sym_off, sym_count * sizeof(core::elf64_sym));
                                sym_ctx.symtab = symtab_vec;
                            }
                        }

                        if (result.dynamic->strtab_vaddr != 0) {
                            auto str_off = vaddr_to_file_offset(result.dynamic->strtab_vaddr, parsed.program_headers);
                            if (str_off && *str_off < parsed.image_bytes.size()) {
                                const std::size_t avail = parsed.image_bytes.size() - *str_off;
                                const std::size_t str_len = (result.dynamic->strtab_size > 0)
                                    ? std::min(result.dynamic->strtab_size, static_cast<std::uint64_t>(avail))
                                    : avail;
                                strtab_vec.resize(str_len);
                                std::memcpy(strtab_vec.data(), parsed.image_bytes.data() + *str_off, str_len);
                                sym_ctx.strtab = strtab_vec;
                            }
                        }

                        auto rel_res = core::apply_relocations(
                            result.dynamic->relocations, result.module.base_address, memory,
                            core::hle_symbol_resolver, &sym_ctx);
                        (void)rel_res;

                        auto plt_res = core::apply_relocations(
                            result.dynamic->plt_relocations, result.module.base_address, memory,
                            core::hle_symbol_resolver, &sym_ctx);
                        (void)plt_res;
                    } else {
                        auto rel_res = core::apply_relocations(
                            result.dynamic->relocations, result.module.base_address, memory);
                        (void)rel_res;

                        auto plt_res = core::apply_relocations(
                            result.dynamic->plt_relocations, result.module.base_address, memory);
                        (void)plt_res;
                    }
                }
                break;
            }
        }

        // Initialize CPU state
        result.session.state.rip = result.module.entry_point;
        result.session.state.gpr[4] = initial_rsp; // RSP

        // Execution loop
        for (std::uint64_t i = 0; i < config.budget; ++i) {
            execution::step_result step{};
            bool translated = false;

            if (config.code_factory) {
                auto next = execution::step_arm64(result.session.state, memory, config.code_factory);
                if (!next) {
                    return std::unexpected(session_error::runtime_failure);
                }
                step = next->result;
                translated = next->translated;
            } else {
                step = execution::step(result.session.state, memory);
            }

            if (step.reason == execution::stop_reason::completed) {
                ++result.session.retired;
                translated ? ++result.session.translated : ++result.session.interpreted;

                // Entry point returned via ret and popped the 0 sentinel return address
                if (result.session.state.rip == 0) {
                    result.session.stop = execution::stop_reason::completed;
                    result.exited = true;
                    result.exit_code = static_cast<std::int64_t>(result.session.state.gpr[0]); // RAX
                    return result;
                }
                continue;
            }

            if (step.reason == execution::stop_reason::syscall) {
                // Check if RIP is in HLE thunk range
                if (config.registry != nullptr &&
                    result.session.state.rip >= config.thunk_base &&
                    result.session.state.rip < config.thunk_base + 0x10000) {
                    core::hle_context hle_ctx{};
                    hle_ctx.args[0] = result.session.state.gpr[7]; // RDI
                    hle_ctx.args[1] = result.session.state.gpr[6]; // RSI
                    hle_ctx.args[2] = result.session.state.gpr[2]; // RDX
                    hle_ctx.args[3] = result.session.state.gpr[1]; // RCX
                    hle_ctx.args[4] = result.session.state.gpr[8]; // R8
                    hle_ctx.args[5] = result.session.state.gpr[9]; // R9
                    hle_ctx.rip = result.session.state.rip;
                    hle_ctx.rsp = result.session.state.gpr[4];
                    hle_ctx.memory = &memory;

                    auto disp = config.registry->dispatch(result.session.state.rip, hle_ctx);
                    if (disp) {
                        result.session.state.gpr[0] = *disp; // RAX
                        result.session.state.rip += 2; // Advance past syscall (0x0F 0x05) to ret (0xC3)
                        ++result.session.retired;
                        continue;
                    }
                }

                // Direct guest syscall: System V conventions
                const std::uint64_t sys_nr = result.session.state.gpr[0]; // RAX
                if (sys_nr == 1) { // sys_exit
                    result.session.stop = execution::stop_reason::completed;
                    result.exited = true;
                    result.exit_code = static_cast<std::int64_t>(result.session.state.gpr[7]); // RDI = exit status
                    result.session.state.rip += 2;
                    ++result.session.retired;
                    return result;
                } else if (sys_nr == 4) { // sys_write
                    result.session.state.gpr[0] = result.session.state.gpr[2]; // return byte count
                    result.session.state.rip += 2;
                    ++result.session.retired;
                    continue;
                } else {
                    // Unknown syscall: return 0, advance RIP
                    result.session.state.gpr[0] = 0;
                    result.session.state.rip += 2;
                    ++result.session.retired;
                    continue;
                }
            }

            result.session.stop = step.reason;
            return result;
        }

        result.session.stop = execution::stop_reason::budget_exhausted;
        return result;
    } catch (const std::bad_alloc&) {
        return std::unexpected(session_error::out_of_memory);
    }
}

} // namespace pcsx5::frontend
