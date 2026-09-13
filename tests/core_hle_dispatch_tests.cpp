#include <pcsx5/core/hle_registry.h>
#include <pcsx5/core/nid.h>
#include <pcsx5/core/relocation.h>
#include <pcsx5/execution/interpreter.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

#define EXPECT(cond, msg) \
    do { \
        ++g_checks; \
        if (!(cond)) { \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << ": " << msg << "\n"; \
            ++g_failures; \
        } \
    } while (0)

#define EXPECT_EQ(a, b, msg) \
    do { \
        ++g_checks; \
        if ((a) != (b)) { \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << ": " << msg \
                      << " (lhs=" << (a) << " rhs=" << (b) << ")\n"; \
            ++g_failures; \
        } \
    } while (0)

class test_memory_backing final : public pcsx5::core::memory_backing {
public:
    explicit test_memory_backing(std::uint64_t size) : storage_(size, std::byte{0}) {}
    std::uint64_t size() const noexcept override { return storage_.size(); }

    pcsx5::core::guest_memory_result<void> read(
        std::uint64_t offset, std::span<std::byte> dst) const noexcept override {
        if (offset > storage_.size() || dst.size() > storage_.size() - offset) {
            return std::unexpected(pcsx5::core::guest_memory_error::invalid_range);
        }
        std::memcpy(dst.data(), storage_.data() + offset, dst.size());
        return {};
    }

    pcsx5::core::guest_memory_result<void> write(
        std::uint64_t offset, std::span<const std::byte> src) noexcept override {
        if (offset > storage_.size() || src.size() > storage_.size() - offset) {
            return std::unexpected(pcsx5::core::guest_memory_error::invalid_range);
        }
        std::memcpy(storage_.data() + offset, src.data(), src.size());
        return {};
    }

    pcsx5::core::guest_memory_result<void> release() noexcept override {
        return {};
    }

private:
    std::vector<std::byte> storage_;
};

void test_nid_codec() {
    // 1. Tag parsing
    EXPECT_EQ(pcsx5::core::nid_type_to_string(pcsx5::core::nid_type::function), "#T#T", "Function tag");
    EXPECT_EQ(pcsx5::core::nid_type_to_string(pcsx5::core::nid_type::data), "#A#B", "Data tag");
    EXPECT_EQ(pcsx5::core::nid_type_to_string(pcsx5::core::nid_type::object), "#S#N", "Object tag");
    EXPECT_EQ(pcsx5::core::nid_type_to_string(pcsx5::core::nid_type::block), "#B#C", "Block tag");

    auto t1 = pcsx5::core::nid_type_from_string("#T#T");
    EXPECT(t1.has_value() && *t1 == pcsx5::core::nid_type::function, "Parse #T#T");
    auto t2 = pcsx5::core::nid_type_from_string("#A#B");
    EXPECT(t2.has_value() && *t2 == pcsx5::core::nid_type::data, "Parse #A#B");
    auto t_bad = pcsx5::core::nid_type_from_string("#XYZ");
    EXPECT(!t_bad.has_value(), "Reject invalid tag");

    // 2. Decode and encode known NID "+P6FRGH4LfA" (memmove)
    auto decoded_bytes = pcsx5::core::decode_nid_bytes("+P6FRGH4LfA");
    EXPECT(decoded_bytes.has_value(), "Decode +P6FRGH4LfA should succeed");
    if (decoded_bytes.has_value()) {
        std::string reencoded = pcsx5::core::encode_nid_bytes(*decoded_bytes);
        EXPECT_EQ(reencoded, "+P6FRGH4LfA", "Round-trip encode should match exactly");
    }

    // 3. Uint64 round-trip
    const std::uint64_t test_nid = 0x0123456789ABCDEFull;
    std::string encoded_u64 = pcsx5::core::encode_nid(test_nid);
    EXPECT_EQ(encoded_u64.size(), 11ull, "Encoded string length must be 11");
    auto decoded_u64 = pcsx5::core::decode_nid(encoded_u64);
    EXPECT(decoded_u64.has_value(), "Decode u64 must succeed");
    EXPECT_EQ(*decoded_u64, test_nid, "Decoded u64 must equal original");

    // 4. parse_nid_string with full 15-char tag
    auto parsed_full = pcsx5::core::parse_nid_string("+P6FRGH4LfA#T#T");
    EXPECT(parsed_full.has_value(), "Parse full NID string should succeed");
    if (parsed_full.has_value()) {
        EXPECT(parsed_full->type == pcsx5::core::nid_type::function, "Should identify function tag");
        EXPECT_EQ(parsed_full->raw_tag, "#T#T", "Raw tag must match");
        EXPECT_EQ(pcsx5::core::encode_nid(parsed_full->nid), "+P6FRGH4LfA", "Parsed NID value must match");
    }

    // 5. Well-known NID lookup
    auto name_opt = pcsx5::core::lookup_nid_name(*pcsx5::core::decode_nid("+P6FRGH4LfA"));
    EXPECT(name_opt.has_value() && *name_opt == "memmove", "Known NID should resolve to memmove");

    auto nid_opt = pcsx5::core::lookup_name_nid("memmove");
    EXPECT(nid_opt.has_value(), "Lookup name memmove should find NID");
    if (nid_opt.has_value()) {
        EXPECT_EQ(pcsx5::core::encode_nid(*nid_opt), "+P6FRGH4LfA", "NID of memmove must match");
    }

    // 6. Invalid string rejections
    EXPECT(!pcsx5::core::decode_nid("short").has_value(), "Reject short string");
    EXPECT(!pcsx5::core::decode_nid("too_long_string_1234").has_value(), "Reject long string");
    EXPECT(!pcsx5::core::decode_nid("inv@lid!ch#").has_value(), "Reject invalid chars");
}

std::uint64_t dummy_add_handler(pcsx5::core::hle_context& ctx) noexcept {
    // rdi + rsi
    return ctx.args[0] + ctx.args[1];
}

std::uint64_t dummy_memmove_handler(pcsx5::core::hle_context& ctx) noexcept {
    // args: dst = rdi, src = rsi, len = rdx
    return ctx.args[0]; // returns dst pointer
}

void test_hle_registry_basic() {
    pcsx5::core::hle_registry reg{};

    // Register handlers
    bool r1 = reg.register_handler("libc", "memmove", dummy_memmove_handler);
    EXPECT(r1, "Register memmove should succeed");

    bool r2 = reg.register_handler("libkernel", "sceKernelAdd", dummy_add_handler);
    EXPECT(r2, "Register sceKernelAdd should succeed");

    // Invalid registrations
    EXPECT(!reg.register_handler("", "", nullptr), "Reject null handler");
    EXPECT(!reg.register_handler("libc", "", dummy_add_handler), "Reject empty symbol name");

    // Lookups
    const auto* e1 = reg.find_handler("libc", "memmove");
    EXPECT(e1 != nullptr, "Find by exact module + name");
    if (e1 != nullptr) {
        EXPECT_EQ(e1->symbol_name, "memmove", "Symbol name must match");
    }

    const auto* e2 = reg.find_handler("sceKernelAdd");
    EXPECT(e2 != nullptr, "Find across modules by symbol name");

    // Lookup canonical name using its NID tag: "+P6FRGH4LfA#T#T" -> maps to memmove
    const auto* e_nid = reg.find_handler("libc", "+P6FRGH4LfA#T#T");
    EXPECT(e_nid != nullptr, "Find by NID tag should resolve to canonical memmove handler");
    if (e_nid != nullptr) {
        EXPECT_EQ(e_nid->symbol_name, "memmove", "Resolved entry must be memmove");
    }
}

void test_thunk_emission_and_dispatch() {
    pcsx5::core::guest_memory memory{};
    std::unique_ptr<pcsx5::core::memory_backing> backing =
        std::make_unique<test_memory_backing>(0x10000);

    const std::uint64_t thunk_base = 0x7FFF0000ull;
    auto map_res = memory.map(thunk_base, 0x10000, pcsx5::core::guest_memory_access::read_write, backing);
    EXPECT(map_res.has_value(), "Map thunk page must succeed");

    pcsx5::core::hle_registry reg{};
    reg.register_handler("libkernel", "sceKernelAdd", dummy_add_handler);
    reg.register_handler("libc", "memmove", dummy_memmove_handler);

    auto emit_res = reg.emit_thunks(thunk_base, memory);
    EXPECT(emit_res.has_value(), "emit_thunks must succeed");
    EXPECT_EQ(*emit_res, 2ull, "Should emit 2 thunks");

    // Verify thunk byte pattern at thunk_base
    std::array<std::byte, 16> thunk_bytes{};
    auto read_res = memory.read(thunk_base, thunk_bytes);
    EXPECT(read_res.has_value(), "Read thunk bytes should succeed");
    EXPECT_EQ(std::to_integer<int>(thunk_bytes[0]), 0x0F, "Opcode byte 0: 0x0F");
    EXPECT_EQ(std::to_integer<int>(thunk_bytes[1]), 0x05, "Opcode byte 1: 0x05 (syscall)");
    EXPECT_EQ(std::to_integer<int>(thunk_bytes[2]), 0xC3, "Opcode byte 2: 0xC3 (ret)");
    EXPECT_EQ(std::to_integer<int>(thunk_bytes[3]), 0x90, "Opcode byte 3: 0x90 (nop)");

    // Test dispatch
    pcsx5::core::hle_context ctx{};
    ctx.args[0] = 40;
    ctx.args[1] = 2;
    ctx.memory = &memory;

    auto disp_res = reg.dispatch(thunk_base, ctx); // entry 0 is sceKernelAdd
    EXPECT(disp_res.has_value(), "Dispatch must succeed");
    EXPECT_EQ(*disp_res, 42ull, "40 + 2 = 42");
    EXPECT_EQ(ctx.rax, 42ull, "ctx.rax must be updated to 42");

    // Test auto-stubbing
    auto stub_res = reg.resolve_or_stub("libkernel", "sceKernelUnknownFunc");
    EXPECT(stub_res.has_value(), "Auto-stub must succeed in non-strict mode");
    EXPECT_EQ(*stub_res, thunk_base + 2 * pcsx5::core::hle_thunk_size, "Auto-stub gets thunk slot 2");

    // Dispatch auto-stub: returns 0
    auto stub_disp = reg.dispatch(*stub_res, ctx);
    EXPECT(stub_disp.has_value(), "Dispatch auto-stub should succeed");
    EXPECT_EQ(*stub_disp, 0ull, "Auto-stub returns 0");

    // Test strict mode rejection
    auto strict_fail = reg.resolve_or_stub("libkernel", "sceKernelNeverHeardOf", true);
    EXPECT(!strict_fail.has_value(), "Strict mode must reject unregistered symbol");
    EXPECT(strict_fail.error() == pcsx5::core::hle_error::symbol_not_found, "Error must be symbol_not_found");
}

void test_relocation_resolver_integration() {
    pcsx5::core::guest_memory memory{};
    std::unique_ptr<pcsx5::core::memory_backing> backing1 =
        std::make_unique<test_memory_backing>(0x10000);

    const std::uint64_t base_address = 0x400000ull;
    auto map_res = memory.map(base_address, 0x10000, pcsx5::core::guest_memory_access::read_write, backing1);
    EXPECT(map_res.has_value(), "Map base page");

    std::unique_ptr<pcsx5::core::memory_backing> backing2 =
        std::make_unique<test_memory_backing>(0x10000);
    const std::uint64_t thunk_base = 0x7FFF0000ull;
    auto thunk_map = memory.map(thunk_base, 0x10000, pcsx5::core::guest_memory_access::read_write, backing2);
    EXPECT(thunk_map.has_value(), "Map thunk page");

    pcsx5::core::hle_registry reg{};
    reg.register_handler("libc", "memmove", dummy_memmove_handler);
    auto emit_res = reg.emit_thunks(thunk_base, memory);
    EXPECT(emit_res.has_value(), "emit_thunks should succeed");

    // Create string table: "\0+P6FRGH4LfA#T#T\0local_sym\0"
    const std::string strtab = std::string("\0+P6FRGH4LfA#T#T\0local_sym\0", 27);

    // Create symbol table:
    // Symbol 0: STN_UNDEF
    // Symbol 1: undefined import (st_shndx == shn_undef) pointing to "+P6FRGH4LfA#T#T" (offset 1)
    // Symbol 2: local symbol (st_shndx == 1, st_value = 0x1234) pointing to "local_sym" (offset 17)
    std::vector<pcsx5::core::elf64_sym> symtab(3);
    symtab[1].st_name = 1;
    symtab[1].st_shndx = pcsx5::core::shn_undef;

    symtab[2].st_name = 17;
    symtab[2].st_shndx = 1;
    symtab[2].st_value = 0x1234;

    pcsx5::core::hle_symbol_context sym_ctx{
        .registry = &reg,
        .symtab = symtab,
        .strtab = std::span<const char>{strtab.data(), strtab.size()},
        .module_name = "libc",
        .base_address = base_address,
        .strict_mode = false,
    };

    // 1. Relocation: R_X86_64_JUMP_SLOT targeting offset 0x200 (for Symbol 1 -> memmove thunk)
    pcsx5::core::elf64_rela rela_jmp{};
    rela_jmp.r_offset = 0x200;
    rela_jmp.r_info = (static_cast<std::uint64_t>(1) << 32) | pcsx5::core::r_x86_64_jump_slot;
    rela_jmp.r_addend = 0;

    // 2. Relocation: R_X86_64_64 targeting offset 0x208 (for Symbol 2 -> base + 0x1234)
    pcsx5::core::elf64_rela rela_local{};
    rela_local.r_offset = 0x208;
    rela_local.r_info = (static_cast<std::uint64_t>(2) << 32) | pcsx5::core::r_x86_64_64;
    rela_local.r_addend = 0x10;

    std::vector<pcsx5::core::elf64_rela> relas = {rela_jmp, rela_local};

    auto apply_res = pcsx5::core::apply_relocations(
        relas, base_address, memory, pcsx5::core::hle_symbol_resolver, &sym_ctx);
    EXPECT(apply_res.has_value(), "apply_relocations should succeed");
    EXPECT_EQ(*apply_res, 2ull, "Should apply 2 relocations");

    // Verify JUMP_SLOT value is the memmove thunk address (thunk_base)
    std::uint64_t got_val = 0;
    auto r1 = memory.read(base_address + 0x200, std::as_writable_bytes(std::span{&got_val, 1}));
    EXPECT(r1.has_value(), "read got_val should succeed");
    EXPECT_EQ(got_val, thunk_base, "GOT entry must point to memmove thunk vaddr");

    // Verify local sym relocation: base (0x400000) + 0x1234 + 0x10 = 0x401244
    std::uint64_t local_val = 0;
    auto r2 = memory.read(base_address + 0x208, std::as_writable_bytes(std::span{&local_val, 1}));
    EXPECT(r2.has_value(), "read local_val should succeed");
    EXPECT_EQ(local_val, base_address + 0x1234 + 0x10, "Local symbol relocation matches base + sym + addend");
}

void test_interpreter_step_into_thunk() {
    pcsx5::core::guest_memory memory{};
    std::unique_ptr<pcsx5::core::memory_backing> b1 =
        std::make_unique<test_memory_backing>(0x10000);
    std::unique_ptr<pcsx5::core::memory_backing> b2 =
        std::make_unique<test_memory_backing>(0x10000);
    std::unique_ptr<pcsx5::core::memory_backing> b3 =
        std::make_unique<test_memory_backing>(0x10000);

    const std::uint64_t code_base = 0x400000ull;
    const std::uint64_t stack_base = 0x600000ull;
    const std::uint64_t thunk_base = 0x7FFF0000ull;

    auto m1 = memory.map(code_base, 0x10000, pcsx5::core::guest_memory_access::read_write, b1);
    auto m2 = memory.map(stack_base, 0x10000, pcsx5::core::guest_memory_access::read_write, b2);
    auto m3 = memory.map(thunk_base, 0x10000, pcsx5::core::guest_memory_access::read_write, b3);
    EXPECT(m1.has_value() && m2.has_value() && m3.has_value(), "Map memory pages");

    pcsx5::core::hle_registry reg{};
    reg.register_handler("libkernel", "sceKernelAdd", dummy_add_handler);
    auto emit_res = reg.emit_thunks(thunk_base, memory);
    EXPECT(emit_res.has_value(), "emit_thunks should succeed");

    // Guest code at code_base:
    // Call rel32 to thunk_base:
    // Offset is thunk_base - (code_base + 5)
    // 0xE8 <rel32>
    // followed by NOP: 0x90
    const std::int32_t rel = static_cast<std::int32_t>(thunk_base - (code_base + 5));
    std::array<std::byte, 6> code{};
    code[0] = std::byte{0xE8};
    std::memcpy(code.data() + 1, &rel, sizeof(rel));
    code[5] = std::byte{0x90};
    auto write_res = memory.write(code_base, code);
    EXPECT(write_res.has_value(), "write code should succeed");

    pcsx5::execution::cpu_state state{};
    state.rip = code_base;
    state.gpr[4] = stack_base + 0x8000; // RSP
    state.gpr[7] = 100; // RDI
    state.gpr[6] = 25;  // RSI

    // Step 1: execute `call thunk_base`
    auto s1 = pcsx5::execution::step(state, memory);
    EXPECT_EQ(static_cast<int>(s1.reason),
              static_cast<int>(pcsx5::execution::stop_reason::completed),
              "Call instruction completes");
    EXPECT_EQ(state.rip, thunk_base, "RIP is now at thunk_base");
    EXPECT_EQ(state.gpr[4], stack_base + 0x8000 - 8, "RSP decremented by 8");

    // Step 2: execute `syscall` (0x0F 0x05) at thunk_base
    auto s2 = pcsx5::execution::step(state, memory);
    EXPECT_EQ(static_cast<int>(s2.reason),
              static_cast<int>(pcsx5::execution::stop_reason::syscall),
              "Thunk syscall triggers stop_reason::syscall");

    // Handle the HLE dispatch!
    pcsx5::core::hle_context ctx{};
    ctx.args[0] = state.gpr[7]; // RDI (100)
    ctx.args[1] = state.gpr[6]; // RSI (25)
    ctx.rip = state.rip;
    ctx.rsp = state.gpr[4];
    ctx.memory = &memory;

    auto disp = reg.dispatch(state.rip, ctx);
    EXPECT(disp.has_value(), "Dispatch at thunk_base must succeed");
    EXPECT_EQ(*disp, 125ull, "100 + 25 = 125");

    // Advance RIP past `0x0F 0x05` to `0xC3` (`ret`) and store RAX
    state.gpr[0] = *disp;
    state.rip += 2;

    // Step 3: execute `ret` (0xC3) at thunk_base + 2
    auto s3 = pcsx5::execution::step(state, memory);
    EXPECT_EQ(static_cast<int>(s3.reason),
              static_cast<int>(pcsx5::execution::stop_reason::completed),
              "Ret instruction completes");
    EXPECT_EQ(state.rip, code_base + 5, "RIP returned to after the call instruction!");
    EXPECT_EQ(state.gpr[4], stack_base + 0x8000, "RSP restored to original stack base");
    EXPECT_EQ(state.gpr[0], 125ull, "RAX holds HLE return value");
}

} // namespace

int main() {
    std::cout << "[RUN ] test_nid_codec\n";
    test_nid_codec();
    std::cout << "[RUN ] test_hle_registry_basic\n";
    test_hle_registry_basic();
    std::cout << "[RUN ] test_thunk_emission_and_dispatch\n";
    test_thunk_emission_and_dispatch();
    std::cout << "[RUN ] test_relocation_resolver_integration\n";
    test_relocation_resolver_integration();
    std::cout << "[RUN ] test_interpreter_step_into_thunk\n";
    test_interpreter_step_into_thunk();

    std::cout << "\nTotal checks: " << g_checks << ", Failures: " << g_failures << "\n";
    return (g_failures == 0) ? 0 : 1;
}
