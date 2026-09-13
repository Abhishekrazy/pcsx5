#include <pcsx5/core/elf_parser.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
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

pcsx5::core::elf64_ehdr make_valid_ehdr(std::uint16_t type = pcsx5::core::et_dyn) {
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
    hdr.e_entry = 0x400000;
    hdr.e_phoff = sizeof(pcsx5::core::elf64_ehdr);
    hdr.e_ehsize = sizeof(pcsx5::core::elf64_ehdr);
    hdr.e_phentsize = sizeof(pcsx5::core::elf64_phdr);
    hdr.e_phnum = 1;
    return hdr;
}

std::vector<std::byte> build_minimal_elf(const pcsx5::core::elf64_ehdr& ehdr,
                                         const std::vector<pcsx5::core::elf64_phdr>& phdrs,
                                         const std::vector<std::byte>& payload = {}) {
    std::vector<std::byte> buf(sizeof(ehdr) + phdrs.size() * sizeof(pcsx5::core::elf64_phdr) + payload.size());
    std::memcpy(buf.data(), &ehdr, sizeof(ehdr));
    if (!phdrs.empty()) {
        std::memcpy(buf.data() + sizeof(ehdr), phdrs.data(), phdrs.size() * sizeof(pcsx5::core::elf64_phdr));
    }
    if (!payload.empty()) {
        std::memcpy(buf.data() + sizeof(ehdr) + phdrs.size() * sizeof(pcsx5::core::elf64_phdr),
                    payload.data(), payload.size());
    }
    return buf;
}

void test_truncated_buffer() {
    std::array<std::byte, 16> short_buf{};
    auto res = pcsx5::core::parse_elf(short_buf);
    EXPECT(!res.has_value(), "Truncated buffer must be rejected");
    EXPECT_EQ(static_cast<int>(res.error()), static_cast<int>(pcsx5::core::elf_error::buffer_too_small),
              "Expected buffer_too_small error");
}

void test_invalid_magic() {
    auto hdr = make_valid_ehdr();
    hdr.e_ident[0] = 0x00;
    std::array<std::byte, sizeof(hdr)> buf{};
    std::memcpy(buf.data(), &hdr, sizeof(hdr));
    auto res = pcsx5::core::parse_elf(buf);
    EXPECT(!res.has_value(), "Invalid magic must be rejected");
    EXPECT_EQ(static_cast<int>(res.error()), static_cast<int>(pcsx5::core::elf_error::invalid_magic),
              "Expected invalid_magic error");
}

void test_unsupported_class_and_endian() {
    auto hdr32 = make_valid_ehdr();
    hdr32.e_ident[4] = pcsx5::core::elfclass32;
    std::array<std::byte, sizeof(hdr32)> buf32{};
    std::memcpy(buf32.data(), &hdr32, sizeof(hdr32));
    auto res32 = pcsx5::core::parse_elf(buf32);
    EXPECT(!res32.has_value(), "32-bit ELF must be rejected");
    EXPECT_EQ(static_cast<int>(res32.error()), static_cast<int>(pcsx5::core::elf_error::unsupported_class),
              "Expected unsupported_class error");

    auto hdr_be = make_valid_ehdr();
    hdr_be.e_ident[5] = 2; // Big Endian
    std::array<std::byte, sizeof(hdr_be)> buf_be{};
    std::memcpy(buf_be.data(), &hdr_be, sizeof(hdr_be));
    auto res_be = pcsx5::core::parse_elf(buf_be);
    EXPECT(!res_be.has_value(), "Big endian ELF must be rejected");
    EXPECT_EQ(static_cast<int>(res_be.error()), static_cast<int>(pcsx5::core::elf_error::unsupported_endian),
              "Expected unsupported_endian error");
}

void test_unsupported_machine_and_type() {
    auto hdr_arm = make_valid_ehdr();
    hdr_arm.e_machine = 0xB7; // AArch64
    std::array<std::byte, sizeof(hdr_arm)> buf_arm{};
    std::memcpy(buf_arm.data(), &hdr_arm, sizeof(hdr_arm));
    auto res_arm = pcsx5::core::parse_elf(buf_arm);
    EXPECT(!res_arm.has_value(), "Non-x86_64 ELF must be rejected");
    EXPECT_EQ(static_cast<int>(res_arm.error()), static_cast<int>(pcsx5::core::elf_error::unsupported_machine),
              "Expected unsupported_machine error");

    auto hdr_rel = make_valid_ehdr(pcsx5::core::et_rel);
    std::array<std::byte, sizeof(hdr_rel)> buf_rel{};
    std::memcpy(buf_rel.data(), &hdr_rel, sizeof(hdr_rel));
    auto res_rel = pcsx5::core::parse_elf(buf_rel);
    EXPECT(!res_rel.has_value(), "ET_REL object file must be rejected");
    EXPECT_EQ(static_cast<int>(res_rel.error()), static_cast<int>(pcsx5::core::elf_error::unsupported_type),
              "Expected unsupported_type error");
}

void test_bounds_validation() {
    // 1. phoff out of bounds
    auto hdr_oob = make_valid_ehdr();
    hdr_oob.e_phoff = 10000;
    std::vector<pcsx5::core::elf64_phdr> phdrs(1);
    auto buf_oob = build_minimal_elf(hdr_oob, phdrs);
    auto res_oob = pcsx5::core::parse_elf(buf_oob);
    EXPECT(!res_oob.has_value(), "Out of bounds phoff must be rejected");
    EXPECT_EQ(static_cast<int>(res_oob.error()), static_cast<int>(pcsx5::core::elf_error::invalid_program_header_table),
              "Expected invalid_program_header_table error");

    // 2. p_filesz > p_memsz
    auto hdr_filesz = make_valid_ehdr();
    pcsx5::core::elf64_phdr phdr_bad_size{};
    phdr_bad_size.p_type = pcsx5::core::pt_load;
    phdr_bad_size.p_offset = sizeof(hdr_filesz) + sizeof(pcsx5::core::elf64_phdr);
    phdr_bad_size.p_filesz = 100;
    phdr_bad_size.p_memsz = 50; // memsz < filesz!
    std::vector<std::byte> payload(100);
    auto buf_filesz = build_minimal_elf(hdr_filesz, {phdr_bad_size}, payload);
    auto res_filesz = pcsx5::core::parse_elf(buf_filesz);
    EXPECT(!res_filesz.has_value(), "p_filesz > p_memsz must be rejected");
    EXPECT_EQ(static_cast<int>(res_filesz.error()), static_cast<int>(pcsx5::core::elf_error::file_size_exceeds_memory_size),
              "Expected file_size_exceeds_memory_size error");

    // 3. Segment offset + filesz extends past file end
    auto hdr_past_end = make_valid_ehdr();
    pcsx5::core::elf64_phdr phdr_past_end{};
    phdr_past_end.p_type = pcsx5::core::pt_load;
    phdr_past_end.p_offset = sizeof(hdr_past_end) + sizeof(pcsx5::core::elf64_phdr);
    phdr_past_end.p_filesz = 100;
    phdr_past_end.p_memsz = 100;
    // But payload is only 10 bytes!
    std::vector<std::byte> small_payload(10);
    auto buf_past_end = build_minimal_elf(hdr_past_end, {phdr_past_end}, small_payload);
    auto res_past_end = pcsx5::core::parse_elf(buf_past_end);
    EXPECT(!res_past_end.has_value(), "Segment extending past end must be rejected");
    EXPECT_EQ(static_cast<int>(res_past_end.error()), static_cast<int>(pcsx5::core::elf_error::segment_out_of_bounds),
              "Expected segment_out_of_bounds error");
}

void test_valid_elf_and_ps5_types() {
    // Standard ET_EXEC
    auto hdr_exec = make_valid_ehdr(pcsx5::core::et_exec);
    pcsx5::core::elf64_phdr phdr_load{};
    phdr_load.p_type = pcsx5::core::pt_load;
    phdr_load.p_flags = pcsx5::core::pf_r | pcsx5::core::pf_x;
    phdr_load.p_offset = sizeof(hdr_exec) + sizeof(pcsx5::core::elf64_phdr);
    phdr_load.p_vaddr = 0x400000;
    phdr_load.p_filesz = 64;
    phdr_load.p_memsz = 128; // 64 bytes BSS
    std::vector<std::byte> payload(64, std::byte{0x90});

    auto buf_exec = build_minimal_elf(hdr_exec, {phdr_load}, payload);
    auto res_exec = pcsx5::core::parse_elf(buf_exec);
    EXPECT(res_exec.has_value(), "Valid ET_EXEC must parse successfully");
    if (res_exec.has_value()) {
        EXPECT_EQ(res_exec->is_pie, false, "ET_EXEC should not be PIE");
        EXPECT_EQ(res_exec->is_ps5_module, false, "ET_EXEC is not PS5 module");
        EXPECT_EQ(res_exec->program_headers.size(), 1ull, "Should have 1 phdr");
        EXPECT_EQ(res_exec->program_headers[0].p_memsz, 128ull, "memsz must match");
    }

    // PS5 SDK Dynamic module: ET_SCE_DYNAMIC (0xFE18)
    auto hdr_ps5 = make_valid_ehdr(pcsx5::core::et_sce_dynamic);
    hdr_ps5.e_phnum = 2;
    pcsx5::core::elf64_phdr phdr_proc_param{};
    phdr_proc_param.p_type = pcsx5::core::pt_sce_proc_param;
    phdr_proc_param.p_flags = pcsx5::core::pf_r;
    phdr_proc_param.p_offset = sizeof(hdr_ps5) + 2 * sizeof(pcsx5::core::elf64_phdr);
    phdr_proc_param.p_vaddr = 0;
    phdr_proc_param.p_filesz = 32;
    phdr_proc_param.p_memsz = 32;

    phdr_load.p_offset = phdr_proc_param.p_offset + 32;
    std::vector<std::byte> ps5_payload(32 + 64);

    auto buf_ps5 = build_minimal_elf(hdr_ps5, {phdr_proc_param, phdr_load}, ps5_payload);
    auto res_ps5 = pcsx5::core::parse_elf(buf_ps5);
    EXPECT(res_ps5.has_value(), "Valid PS5 SDK module must parse successfully");
    if (res_ps5.has_value()) {
        EXPECT_EQ(res_ps5->is_pie, true, "PS5 module must be treated as PIE");
        EXPECT_EQ(res_ps5->is_ps5_module, true, "Must be recognized as PS5 module");
        EXPECT_EQ(res_ps5->program_headers.size(), 2ull, "Should have 2 phdrs");
        EXPECT(pcsx5::core::is_ps5_segment_type(res_ps5->program_headers[0].p_type),
               "First phdr must be recognized as PS5 segment type");
    }
}

void test_self_parsing() {
    // 1. Non-SELF buffer
    pcsx5::core::elf64_phdr phdr_load{};
    phdr_load.p_type = pcsx5::core::pt_load;
    phdr_load.p_flags = pcsx5::core::pf_r;
    phdr_load.p_offset = sizeof(pcsx5::core::elf64_ehdr) + sizeof(pcsx5::core::elf64_phdr);
    phdr_load.p_filesz = 16;
    phdr_load.p_memsz = 16;
    std::vector<std::byte> payload(16);
    auto elf_buf = build_minimal_elf(make_valid_ehdr(), {phdr_load}, payload);
    EXPECT(!pcsx5::core::is_self_format(elf_buf), "Standard ELF must not be reported as SELF");

    // 2. Valid SELF wrapper containing an ELF
    pcsx5::core::self_header self_hdr{};
    self_hdr.magic = pcsx5::core::self_magic;
    self_hdr.version = 1;
    self_hdr.endian = 1;
    self_hdr.header_size = sizeof(pcsx5::core::self_header);
    self_hdr.file_size = sizeof(pcsx5::core::self_header) + elf_buf.size();
    self_hdr.segment_count = 0;

    std::vector<std::byte> self_bytes(sizeof(self_hdr) + elf_buf.size());
    std::memcpy(self_bytes.data(), &self_hdr, sizeof(self_hdr));
    std::memcpy(self_bytes.data() + sizeof(self_hdr), elf_buf.data(), elf_buf.size());

    EXPECT(pcsx5::core::is_self_format(self_bytes), "SELF buffer must be detected");

    auto inner = pcsx5::core::extract_self_inner_elf(self_bytes);
    EXPECT(inner.has_value(), "Inner ELF extraction must succeed");
    if (inner.has_value()) {
        EXPECT_EQ(inner->size(), elf_buf.size(), "Extracted inner size must match");
    }

    auto parsed = pcsx5::core::parse_self_or_elf(self_bytes);
    EXPECT(parsed.has_value(), "parse_self_or_elf must parse encapsulated ELF");
    if (parsed.has_value()) {
        EXPECT_EQ(parsed->header.e_machine, pcsx5::core::em_x86_64, "Inner ELF machine must match");
    }
}

} // namespace

int main() {
    std::cout << "[RUN ] test_truncated_buffer\n";
    test_truncated_buffer();
    std::cout << "[RUN ] test_invalid_magic\n";
    test_invalid_magic();
    std::cout << "[RUN ] test_unsupported_class_and_endian\n";
    test_unsupported_class_and_endian();
    std::cout << "[RUN ] test_unsupported_machine_and_type\n";
    test_unsupported_machine_and_type();
    std::cout << "[RUN ] test_bounds_validation\n";
    test_bounds_validation();
    std::cout << "[RUN ] test_valid_elf_and_ps5_types\n";
    test_valid_elf_and_ps5_types();
    std::cout << "[RUN ] test_self_parsing\n";
    test_self_parsing();

    std::cout << "\nTotal checks: " << g_checks << ", Failures: " << g_failures << "\n";
    return (g_failures == 0) ? 0 : 1;
}

