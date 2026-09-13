#include <pcsx5/core/loader.h>

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

pcsx5::core::guest_memory_result<std::unique_ptr<pcsx5::core::memory_backing>>
test_allocator(std::uint64_t size) noexcept {
    return std::make_unique<test_memory_backing>(size);
}

pcsx5::core::elf64_ehdr make_test_ehdr(std::uint16_t type, std::uint16_t phnum) {
    pcsx5::core::elf64_ehdr hdr{};
    hdr.e_ident[0] = pcsx5::core::elfmag0;
    hdr.e_ident[1] = pcsx5::core::elfmag1;
    hdr.e_ident[2] = pcsx5::core::elfmag2;
    hdr.e_ident[3] = pcsx5::core::elfmag3;
    hdr.e_ident[4] = pcsx5::core::elfclass64;
    hdr.e_ident[5] = pcsx5::core::elfdata2lsb;
    hdr.e_ident[6] = pcsx5::core::ev_current;
    hdr.e_type = type;
    hdr.e_machine = pcsx5::core::em_x86_64;
    hdr.e_version = 1;
    hdr.e_entry = 0x1000;
    hdr.e_phoff = sizeof(pcsx5::core::elf64_ehdr);
    hdr.e_ehsize = sizeof(pcsx5::core::elf64_ehdr);
    hdr.e_phentsize = sizeof(pcsx5::core::elf64_phdr);
    hdr.e_phnum = phnum;
    return hdr;
}

void test_span_calculation() {
    // 1. Valid multi-segment
    auto hdr = make_test_ehdr(pcsx5::core::et_dyn, 2);
    pcsx5::core::elf64_phdr p1{};
    p1.p_type = pcsx5::core::pt_load;
    p1.p_vaddr = 0x1000;
    p1.p_memsz = 0x2000;

    pcsx5::core::elf64_phdr p2{};
    p2.p_type = pcsx5::core::pt_load;
    p2.p_vaddr = 0x4000;
    p2.p_memsz = 0x1500;

    pcsx5::core::parsed_elf elf{};
    elf.header = hdr;
    elf.program_headers = {p1, p2};

    auto span_res = pcsx5::core::calculate_module_span(elf);
    EXPECT(span_res.has_value(), "calculate_module_span should succeed");
    if (span_res.has_value()) {
        EXPECT_EQ(span_res->first, 0x1000ull, "Min vaddr must be 0x1000");
        EXPECT_EQ(span_res->second, 0x4500ull, "Total span must be 0x5500 - 0x1000 = 0x4500");
    }

    // 2. No loadable segments
    pcsx5::core::parsed_elf no_load{};
    no_load.program_headers = {};
    auto no_load_res = pcsx5::core::calculate_module_span(no_load);
    EXPECT(!no_load_res.has_value(), "No loadable segments must fail");
    EXPECT_EQ(static_cast<int>(no_load_res.error()),
              static_cast<int>(pcsx5::core::loader_error::no_loadable_segments),
              "Expected no_loadable_segments error");
}

void test_load_fixed_and_bss() {
    auto ehdr = make_test_ehdr(pcsx5::core::et_exec, 1);
    ehdr.e_entry = 0x400100;

    const std::uint64_t ph_offset = sizeof(ehdr) + sizeof(pcsx5::core::elf64_phdr);

    pcsx5::core::elf64_phdr p_code{};
    p_code.p_type = pcsx5::core::pt_load;
    p_code.p_flags = pcsx5::core::pf_r | pcsx5::core::pf_w;
    p_code.p_offset = ph_offset;
    p_code.p_vaddr = 0x400000;
    p_code.p_filesz = 8;
    p_code.p_memsz = 64; // 56 bytes of BSS!

    std::vector<std::byte> raw(ph_offset + 8);
    std::memcpy(raw.data(), &ehdr, sizeof(ehdr));
    std::memcpy(raw.data() + sizeof(ehdr), &p_code, sizeof(p_code));

    // Write distinctive payload
    const std::uint8_t payload[8] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE};
    std::memcpy(raw.data() + ph_offset, payload, 8);

    auto parsed_res = pcsx5::core::parse_elf(raw);
    EXPECT(parsed_res.has_value(), "ELF parse must succeed");

    pcsx5::core::guest_memory memory{};
    auto load_res = pcsx5::core::load_module(*parsed_res, 0x100000000, memory, test_allocator);
    EXPECT(load_res.has_value(), "load_module must succeed for ET_EXEC");

    if (load_res.has_value()) {
        EXPECT_EQ(load_res->base_address, 0ull, "ET_EXEC base address must be 0");
        EXPECT_EQ(load_res->entry_point, 0x400100ull, "Entry point must match e_entry");

        // Verify loaded payload
        std::uint8_t read_back[8]{};
        auto read_res = memory.read(0x400000, std::as_writable_bytes(std::span{read_back}));
        EXPECT(read_res.has_value(), "Memory read at 0x400000 must succeed");
        EXPECT_EQ(std::memcmp(read_back, payload, 8), 0, "Read payload must match file bytes");

        // Verify BSS is zeroed
        std::uint8_t bss_check[16]{};
        auto bss_res = memory.read(0x400008, std::as_writable_bytes(std::span{bss_check}));
        EXPECT(bss_res.has_value(), "Memory read in BSS must succeed");
        bool all_zero = true;
        for (auto b : bss_check) {
            if (b != 0) all_zero = false;
        }
        EXPECT(all_zero, "BSS region must be zeroed");
    }
}

void test_load_pie_and_permissions() {
    auto ehdr = make_test_ehdr(pcsx5::core::et_dyn, 2);
    ehdr.e_entry = 0x20;

    const std::uint64_t ph_offset = sizeof(ehdr) + 2 * sizeof(pcsx5::core::elf64_phdr);

    // Segment 1: Code (Read-Only)
    pcsx5::core::elf64_phdr p_code{};
    p_code.p_type = pcsx5::core::pt_load;
    p_code.p_flags = pcsx5::core::pf_r | pcsx5::core::pf_x; // RO
    p_code.p_offset = ph_offset;
    p_code.p_vaddr = 0x0000;
    p_code.p_filesz = 16;
    p_code.p_memsz = 16;

    // Segment 2: Data (Read-Write), placed on next 4KB page
    pcsx5::core::elf64_phdr p_data{};
    p_data.p_type = pcsx5::core::pt_load;
    p_data.p_flags = pcsx5::core::pf_r | pcsx5::core::pf_w; // RW
    p_data.p_offset = ph_offset + 16;
    p_data.p_vaddr = 0x1000;
    p_data.p_filesz = 16;
    p_data.p_memsz = 32;

    std::vector<std::byte> raw(ph_offset + 32);
    std::memcpy(raw.data(), &ehdr, sizeof(ehdr));
    std::memcpy(raw.data() + sizeof(ehdr), &p_code, sizeof(p_code));
    std::memcpy(raw.data() + sizeof(ehdr) + sizeof(p_code), &p_data, sizeof(p_data));

    auto parsed_res = pcsx5::core::parse_elf(raw);
    EXPECT(parsed_res.has_value(), "PIE ELF parse must succeed");

    pcsx5::core::guest_memory memory{};
    const std::uint64_t preferred_base = 0x800000000ull;
    auto load_res = pcsx5::core::load_module(*parsed_res, preferred_base, memory, test_allocator);
    EXPECT(load_res.has_value(), "load_module must succeed for PIE");

    if (load_res.has_value()) {
        EXPECT_EQ(load_res->base_address, preferred_base, "PIE base address must match preferred_base");
        EXPECT_EQ(load_res->entry_point, preferred_base + 0x20, "Entry point must include base slide");
        EXPECT_EQ(load_res->segments.size(), 2ull, "Must have 2 loaded segments");

        // Code segment at 0x800000000 is Read-Only: writing must fail with access_denied
        std::array<std::byte, 4> test_data{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
        auto write_code_res = memory.write(preferred_base, test_data);
        EXPECT(!write_code_res.has_value(), "Writing to read-only code segment must be rejected");
        EXPECT_EQ(static_cast<int>(write_code_res.error()),
                  static_cast<int>(pcsx5::core::guest_memory_error::access_denied),
                  "Expected access_denied on read-only code write");

        // Data segment at 0x800001000 is Read-Write: writing must succeed
        auto write_data_res = memory.write(preferred_base + 0x1000, test_data);
        EXPECT(write_data_res.has_value(), "Writing to read-write data segment must succeed");
    }
}

void test_tls_extraction() {
    auto ehdr = make_test_ehdr(pcsx5::core::et_dyn, 2);

    const std::uint64_t ph_offset = sizeof(ehdr) + 2 * sizeof(pcsx5::core::elf64_phdr);

    // Loadable segment
    pcsx5::core::elf64_phdr p_load{};
    p_load.p_type = pcsx5::core::pt_load;
    p_load.p_flags = pcsx5::core::pf_r | pcsx5::core::pf_w;
    p_load.p_offset = ph_offset;
    p_load.p_vaddr = 0x0000;
    p_load.p_filesz = 32;
    p_load.p_memsz = 32;

    // TLS segment
    pcsx5::core::elf64_phdr p_tls{};
    p_tls.p_type = pcsx5::core::pt_tls;
    p_tls.p_flags = pcsx5::core::pf_r;
    p_tls.p_offset = ph_offset + 8;
    p_tls.p_vaddr = 0x0008;
    p_tls.p_filesz = 16;
    p_tls.p_memsz = 64;
    p_tls.p_align = 16;

    std::vector<std::byte> raw(ph_offset + 32, std::byte{0xAA});
    std::memcpy(raw.data(), &ehdr, sizeof(ehdr));
    std::memcpy(raw.data() + sizeof(ehdr), &p_load, sizeof(p_load));
    std::memcpy(raw.data() + sizeof(ehdr) + sizeof(p_load), &p_tls, sizeof(p_tls));

    auto parsed_res = pcsx5::core::parse_elf(raw);
    EXPECT(parsed_res.has_value(), "ELF with TLS must parse");

    pcsx5::core::guest_memory memory{};
    auto load_res = pcsx5::core::load_module(*parsed_res, 0x400000, memory, test_allocator);
    EXPECT(load_res.has_value(), "load_module with TLS must succeed");

    if (load_res.has_value()) {
        EXPECT(load_res->tls.has_value(), "Module must have TLS template");
        if (load_res->tls.has_value()) {
            EXPECT_EQ(load_res->tls->virtual_address, 0x400008ull, "TLS vaddr must match base + 0x8");
            EXPECT_EQ(load_res->tls->file_size, 16ull, "TLS filesz must match");
            EXPECT_EQ(load_res->tls->memory_size, 64ull, "TLS memsz must match");
            EXPECT_EQ(load_res->tls->alignment, 16ull, "TLS align must match");
            EXPECT_EQ(load_res->tls->initialization_image.size(), 16ull, "TLS init image size must match");
        }
    }
}

} // namespace

int main() {
    std::cout << "[RUN ] test_span_calculation\n";
    test_span_calculation();
    std::cout << "[RUN ] test_load_fixed_and_bss\n";
    test_load_fixed_and_bss();
    std::cout << "[RUN ] test_load_pie_and_permissions\n";
    test_load_pie_and_permissions();
    std::cout << "[RUN ] test_tls_extraction\n";
    test_tls_extraction();

    std::cout << "\nTotal checks: " << g_checks << ", Failures: " << g_failures << "\n";
    return (g_failures == 0) ? 0 : 1;
}
