#include <pcsx5/core/dynamic_info.h>
#include <pcsx5/core/relocation.h>

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

std::expected<std::uint64_t, pcsx5::core::relocation_error> test_symbol_resolver(
    std::uint32_t sym_idx, void* /*context*/) noexcept {
    if (sym_idx == 1) {
        return 0x12340000ull;
    }
    if (sym_idx == 2) {
        return 0x7FFF0000ull;
    }
    return std::unexpected(pcsx5::core::relocation_error::symbol_resolution_failed);
}

void test_dynamic_parsing() {
    // Layout:
    // Offset 0x000: PT_LOAD segment covering 0x0000..0x1000
    // Offset 0x100: String table ("\0libkernel.prx\0libc.prx\0")
    // Offset 0x200: RELA table (2 entries)
    // Offset 0x300: Dynamic table

    const std::string strtab = std::string("\0libkernel.prx\0libc.prx\0", 24);
    const std::uint64_t strtab_vaddr = 0x100;
    const std::uint64_t strtab_offset = 0x100;

    const std::uint64_t rela_vaddr = 0x200;
    const std::uint64_t rela_offset = 0x200;

    pcsx5::core::elf64_rela rela1{};
    rela1.r_offset = 0x1000;
    rela1.r_info = (static_cast<std::uint64_t>(0) << 32) | pcsx5::core::r_x86_64_relative;
    rela1.r_addend = 0x500;

    pcsx5::core::elf64_rela rela2{};
    rela2.r_offset = 0x1008;
    rela2.r_info = (static_cast<std::uint64_t>(1) << 32) | pcsx5::core::r_x86_64_64;
    rela2.r_addend = 0x20;

    const std::uint64_t dyn_offset = 0x300;
    std::vector<pcsx5::core::elf64_dyn> dyns = {
        {pcsx5::core::dt_needed, {1}}, // "libkernel.prx" (offset 1 in strtab)
        {pcsx5::core::dt_needed, {15}}, // "libc.prx" (offset 15 in strtab)
        {pcsx5::core::dt_strtab, {strtab_vaddr}},
        {pcsx5::core::dt_strsz, {strtab.size()}},
        {pcsx5::core::dt_rela, {rela_vaddr}},
        {pcsx5::core::dt_relasz, {2 * sizeof(pcsx5::core::elf64_rela)}},
        {pcsx5::core::dt_init, {0x400}},
        {pcsx5::core::dt_fini, {0x480}},
        {pcsx5::core::dt_null, {0}},
    };

    std::vector<std::byte> image(0x1000, std::byte{0});
    std::memcpy(image.data() + strtab_offset, strtab.data(), strtab.size());
    std::memcpy(image.data() + rela_offset, &rela1, sizeof(rela1));
    std::memcpy(image.data() + rela_offset + sizeof(rela1), &rela2, sizeof(rela2));
    std::memcpy(image.data() + dyn_offset, dyns.data(), dyns.size() * sizeof(pcsx5::core::elf64_dyn));

    pcsx5::core::elf64_phdr load_phdr{};
    load_phdr.p_type = pcsx5::core::pt_load;
    load_phdr.p_offset = 0;
    load_phdr.p_vaddr = 0;
    load_phdr.p_filesz = image.size();
    load_phdr.p_memsz = image.size();

    pcsx5::core::elf64_phdr dyn_phdr{};
    dyn_phdr.p_type = pcsx5::core::pt_dynamic;
    dyn_phdr.p_offset = dyn_offset;
    dyn_phdr.p_vaddr = dyn_offset;
    dyn_phdr.p_filesz = dyns.size() * sizeof(pcsx5::core::elf64_dyn);
    dyn_phdr.p_memsz = dyn_phdr.p_filesz;

    std::vector<pcsx5::core::elf64_phdr> all_phdrs = {load_phdr, dyn_phdr};

    auto dyn_res = pcsx5::core::parse_dynamic_table(dyn_phdr, all_phdrs, image);
    EXPECT(dyn_res.has_value(), "parse_dynamic_table should succeed");
    if (dyn_res.has_value()) {
        EXPECT_EQ(dyn_res->needed_libraries.size(), 2ull, "Should find 2 needed libraries");
        if (dyn_res->needed_libraries.size() >= 2) {
            EXPECT_EQ(dyn_res->needed_libraries[0], "libkernel.prx", "First library name must match");
            EXPECT_EQ(dyn_res->needed_libraries[1], "libc.prx", "Second library name must match");
        }
        EXPECT_EQ(dyn_res->init_func, 0x400ull, "Init func vaddr must match");
        EXPECT_EQ(dyn_res->fini_func, 0x480ull, "Fini func vaddr must match");
        EXPECT_EQ(dyn_res->relocations.size(), 2ull, "Should parse 2 RELA relocations");
        if (dyn_res->relocations.size() >= 2) {
            EXPECT_EQ(dyn_res->relocations[0].r_offset, 0x1000ull, "First rela offset must match");
            EXPECT_EQ(dyn_res->relocations[1].r_addend, 0x20ll, "Second rela addend must match");
        }
    }
}

void test_apply_relocations() {
    pcsx5::core::guest_memory memory{};
    std::unique_ptr<pcsx5::core::memory_backing> backing =
        std::make_unique<test_memory_backing>(0x4000);

    const std::uint64_t base_address = 0x800000000ull;
    auto map_res = memory.map(base_address, 0x4000, pcsx5::core::guest_memory_access::read_write, backing);
    EXPECT(map_res.has_value(), "Memory map should succeed");

    std::vector<pcsx5::core::elf64_rela> relas;

    // 1. R_X86_64_RELATIVE at offset 0x100 with addend 0x500
    pcsx5::core::elf64_rela rel_relative{};
    rel_relative.r_offset = 0x100;
    rel_relative.r_info = pcsx5::core::r_x86_64_relative;
    rel_relative.r_addend = 0x500;
    relas.push_back(rel_relative);

    // 2. R_X86_64_64 at offset 0x108 with sym_idx 1 (resolved to 0x12340000) and addend 0x20
    pcsx5::core::elf64_rela rel_64{};
    rel_64.r_offset = 0x108;
    rel_64.r_info = (static_cast<std::uint64_t>(1) << 32) | pcsx5::core::r_x86_64_64;
    rel_64.r_addend = 0x20;
    relas.push_back(rel_64);

    // 3. R_X86_64_JUMP_SLOT at offset 0x110 with sym_idx 2 (resolved to 0x7FFF0000)
    pcsx5::core::elf64_rela rel_jump{};
    rel_jump.r_offset = 0x110;
    rel_jump.r_info = (static_cast<std::uint64_t>(2) << 32) | pcsx5::core::r_x86_64_jump_slot;
    rel_jump.r_addend = 0;
    relas.push_back(rel_jump);

    auto apply_res = pcsx5::core::apply_relocations(relas, base_address, memory, test_symbol_resolver);
    EXPECT(apply_res.has_value(), "apply_relocations should succeed");
    if (apply_res.has_value()) {
        EXPECT_EQ(*apply_res, 3ull, "Must apply 3 relocations");

        // Verify R_X86_64_RELATIVE: base_address (0x800000000) + 0x500 = 0x800000500
        std::uint64_t read_val1 = 0;
        auto r1 = memory.read(base_address + 0x100, std::as_writable_bytes(std::span{&read_val1, 1}));
        EXPECT(r1.has_value(), "read 1 should succeed");
        EXPECT_EQ(read_val1, 0x800000500ull, "Relative relocation value must match base + addend");

        // Verify R_X86_64_64: sym_val (0x12340000) + 0x20 = 0x12340020
        std::uint64_t read_val2 = 0;
        auto r2 = memory.read(base_address + 0x108, std::as_writable_bytes(std::span{&read_val2, 1}));
        EXPECT(r2.has_value(), "read 2 should succeed");
        EXPECT_EQ(read_val2, 0x12340020ull, "Direct 64 relocation value must match sym + addend");

        // Verify R_X86_64_JUMP_SLOT: sym_val (0x7FFF0000)
        std::uint64_t read_val3 = 0;
        auto r3 = memory.read(base_address + 0x110, std::as_writable_bytes(std::span{&read_val3, 1}));
        EXPECT(r3.has_value(), "read 3 should succeed");
        EXPECT_EQ(read_val3, 0x7FFF0000ull, "Jump slot relocation value must match sym_val");
    }

    // 4. Test unmapped target failure
    pcsx5::core::elf64_rela unmapped_rela{};
    unmapped_rela.r_offset = 0x50000; // Far out of bounds!
    unmapped_rela.r_info = pcsx5::core::r_x86_64_relative;
    unmapped_rela.r_addend = 0;

    auto fail_res = pcsx5::core::apply_relocations(std::span{&unmapped_rela, 1}, base_address, memory);
    EXPECT(!fail_res.has_value(), "Unmapped relocation write must fail");
    EXPECT_EQ(static_cast<int>(fail_res.error()),
              static_cast<int>(pcsx5::core::relocation_error::write_failed),
              "Expected write_failed error");
}

} // namespace

int main() {
    std::cout << "[RUN ] test_dynamic_parsing\n";
    test_dynamic_parsing();
    std::cout << "[RUN ] test_apply_relocations\n";
    test_apply_relocations();

    std::cout << "\nTotal checks: " << g_checks << ", Failures: " << g_failures << "\n";
    return (g_failures == 0) ? 0 : 1;
}
