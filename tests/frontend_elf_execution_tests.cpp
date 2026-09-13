#include <pcsx5/core/elf_types.h>
#include <pcsx5/core/self_types.h>
#include <pcsx5/frontend/session.h>

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

// Helper to build a minimal valid ELF64 image
struct simple_elf_builder {
    pcsx5::core::elf64_ehdr ehdr{};
    std::vector<pcsx5::core::elf64_phdr> phdrs;
    std::vector<std::byte> payload;
    std::uint64_t vaddr_base{0x400000};

    simple_elf_builder(std::uint16_t type, std::uint64_t vaddr = 0x400000, std::uint64_t entry_offset = 0)
        : vaddr_base(vaddr) {
        ehdr.e_ident[0] = 0x7F;
        ehdr.e_ident[1] = 'E';
        ehdr.e_ident[2] = 'L';
        ehdr.e_ident[3] = 'F';
        ehdr.e_ident[4] = 2; // 64-bit
        ehdr.e_ident[5] = 1; // little endian
        ehdr.e_ident[6] = 1; // original version
        ehdr.e_ident[7] = 0;

        ehdr.e_type = type;
        ehdr.e_machine = pcsx5::core::em_x86_64;
        ehdr.e_version = 1;
        ehdr.e_entry = vaddr_base + entry_offset;
        ehdr.e_phoff = sizeof(pcsx5::core::elf64_ehdr);
        ehdr.e_shoff = 0;
        ehdr.e_flags = 0;
        ehdr.e_ehsize = sizeof(pcsx5::core::elf64_ehdr);
        ehdr.e_phentsize = sizeof(pcsx5::core::elf64_phdr);
        ehdr.e_phnum = 0;
        ehdr.e_shentsize = 0;
        ehdr.e_shnum = 0;
        ehdr.e_shstrndx = 0;
    }

    void add_load_segment(std::uint32_t flags, std::span<const std::byte> code_bytes) {
        pcsx5::core::elf64_phdr ph{};
        ph.p_type = pcsx5::core::pt_load;
        ph.p_flags = flags;
        ph.p_vaddr = vaddr_base + payload.size();
        ph.p_paddr = ph.p_vaddr;
        ph.p_align = 0x1000;
        ph.p_filesz = code_bytes.size();
        ph.p_memsz = code_bytes.size();

        phdrs.push_back(ph);
        payload.insert(payload.end(), code_bytes.begin(), code_bytes.end());
    }

    std::vector<std::byte> build() {
        ehdr.e_phnum = static_cast<std::uint16_t>(phdrs.size());
        const std::size_t headers_size = sizeof(ehdr) + phdrs.size() * sizeof(pcsx5::core::elf64_phdr);

        // Align payload start to 0x1000
        const std::size_t payload_offset = (headers_size + 0xFFF) & ~0xFFF;

        for (auto& ph : phdrs) {
            ph.p_offset = payload_offset + (ph.p_vaddr - vaddr_base);
        }

        std::vector<std::byte> file_bytes(payload_offset + payload.size(), std::byte{0});
        std::memcpy(file_bytes.data(), &ehdr, sizeof(ehdr));
        std::memcpy(file_bytes.data() + sizeof(ehdr), phdrs.data(), phdrs.size() * sizeof(pcsx5::core::elf64_phdr));
        std::memcpy(file_bytes.data() + payload_offset, payload.data(), payload.size());
        return file_bytes;
    }
};

void test_elf_return_exit_code() {
    // Machine code:
    // mov eax, 42  (0xB8, 0x2A, 0x00, 0x00, 0x00)
    // ret          (0xC3)
    const std::array<std::byte, 6> code = {{
        std::byte{0xB8}, std::byte{0x2A}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0xC3}
    }};

    simple_elf_builder builder(pcsx5::core::et_exec, 0);
    builder.add_load_segment(pcsx5::core::pf_r | pcsx5::core::pf_x, code);
    auto elf_bytes = builder.build();

    auto res = pcsx5::frontend::run_elf(elf_bytes);
    EXPECT(res.has_value(), "run_elf should succeed for simple return ELF");
    if (res.has_value()) {
        EXPECT(res->exited, "Module execution must flag exited");
        EXPECT_EQ(res->exit_code, 42ll, "Exit code must match RAX (42)");
        EXPECT_EQ(static_cast<int>(res->session.stop),
                  static_cast<int>(pcsx5::execution::stop_reason::completed),
                  "Stop reason must be completed");
        EXPECT(res->session.retired > 0, "At least one instruction retired");
    }
}

void test_elf_direct_sys_exit() {
    // Machine code:
    // mov eax, 1   (sys_exit)  : 0xB8, 0x01, 0x00, 0x00, 0x00
    // mov edi, 77  (exit code) : 0xBF, 0x4D, 0x00, 0x00, 0x00
    // syscall                  : 0x0F, 0x05
    const std::array<std::byte, 12> code = {{
        std::byte{0xB8}, std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0xBF}, std::byte{0x4D}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x0F}, std::byte{0x05}
    }};

    simple_elf_builder builder(pcsx5::core::et_exec, 0x400000, 0);
    builder.add_load_segment(pcsx5::core::pf_r | pcsx5::core::pf_x, code);
    auto elf_bytes = builder.build();

    auto res = pcsx5::frontend::run_elf(elf_bytes);
    EXPECT(res.has_value(), "run_elf should succeed for sys_exit ELF");
    if (res.has_value()) {
        EXPECT(res->exited, "Module execution must flag exited");
        EXPECT_EQ(res->exit_code, 77ll, "Exit code must match RDI (77)");
        EXPECT_EQ(static_cast<int>(res->session.stop),
                  static_cast<int>(pcsx5::execution::stop_reason::completed),
                  "Stop reason must be completed");
    }
}

std::uint64_t hle_add_handler(pcsx5::core::hle_context& ctx) noexcept {
    // rdi + rsi
    return ctx.args[0] + ctx.args[1];
}

void test_elf_hle_dispatch_integration() {
    // Call HLE function:
    // mov edi, 30               : 0xBF, 0x1E, 0x00, 0x00, 0x00
    // mov esi, 12               : 0xBE, 0x0C, 0x00, 0x00, 0x00
    // call rel32 (to thunk)     : 0xE8, <rel32>
    // ret                       : 0xC3
    const std::uint64_t vaddr = 0x400000;
    const std::uint64_t thunk_addr = 0x7FFF0000;
    const std::int32_t rel = static_cast<std::int32_t>(thunk_addr - (vaddr + 10 + 5));

    std::vector<std::byte> code(16);
    // mov edi, 30
    code[0] = std::byte{0xBF}; code[1] = std::byte{0x1E}; code[2] = std::byte{0x00};
    code[3] = std::byte{0x00}; code[4] = std::byte{0x00};
    // mov esi, 12
    code[5] = std::byte{0xBE}; code[6] = std::byte{0x0C}; code[7] = std::byte{0x00};
    code[8] = std::byte{0x00}; code[9] = std::byte{0x00};
    // call rel32
    code[10] = std::byte{0xE8};
    std::memcpy(code.data() + 11, &rel, sizeof(rel));
    // ret
    code[15] = std::byte{0xC3};

    simple_elf_builder builder(pcsx5::core::et_exec, vaddr, 0);
    builder.add_load_segment(pcsx5::core::pf_r | pcsx5::core::pf_x, code);
    auto elf_bytes = builder.build();

    pcsx5::core::hle_registry reg{};
    reg.register_handler("libkernel", "sceKernelAdd", hle_add_handler);

    pcsx5::frontend::elf_session_config cfg{};
    cfg.registry = &reg;
    cfg.thunk_base = thunk_addr;

    auto res = pcsx5::frontend::run_elf(elf_bytes, cfg);
    EXPECT(res.has_value(), "run_elf with HLE registry must succeed");
    if (res.has_value()) {
        EXPECT(res->exited, "Module execution must flag exited");
        EXPECT_EQ(res->exit_code, 42ll, "Exit code must match HLE result 30 + 12 = 42");
        EXPECT_EQ(static_cast<int>(res->session.stop),
                  static_cast<int>(pcsx5::execution::stop_reason::completed),
                  "Stop reason must be completed");
    }
}

void test_self_container_execution() {
    // Machine code: mov eax, 100; ret
    const std::array<std::byte, 6> code = {{
        std::byte{0xB8}, std::byte{0x64}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0xC3}
    }};

    simple_elf_builder builder(pcsx5::core::et_exec, 0);
    builder.add_load_segment(pcsx5::core::pf_r | pcsx5::core::pf_x, code);
    auto inner_elf = builder.build();

    // Wrap into Sony SELF container
    pcsx5::core::self_header shdr{};
    shdr.magic = pcsx5::core::self_magic;
    shdr.version = 1;
    shdr.mode = 1;
    shdr.endian = 1;
    shdr.header_size = sizeof(pcsx5::core::self_header) + sizeof(pcsx5::core::self_segment_entry);
    shdr.segment_count = 1;
    shdr.file_size = shdr.header_size + inner_elf.size();

    pcsx5::core::self_segment_entry seg{};
    seg.flags = 0x800; // uncompressed / plain ELF
    seg.offset = shdr.header_size;
    seg.encrypted_compressed_size = inner_elf.size();
    seg.decrypted_uncompressed_size = inner_elf.size();

    std::vector<std::byte> self_bytes(shdr.file_size);
    std::memcpy(self_bytes.data(), &shdr, sizeof(shdr));
    std::memcpy(self_bytes.data() + sizeof(shdr), &seg, sizeof(seg));
    std::memcpy(self_bytes.data() + seg.offset, inner_elf.data(), inner_elf.size());

    auto res = pcsx5::frontend::run_elf(self_bytes);
    EXPECT(res.has_value(), "run_elf should execute SELF container seamlessly");
    if (res.has_value()) {
        EXPECT(res->exited, "Module must flag exited");
        EXPECT_EQ(res->exit_code, 100ll, "Exit code must match RAX (100)");
        EXPECT_EQ(static_cast<int>(res->session.stop),
                  static_cast<int>(pcsx5::execution::stop_reason::completed),
                  "Stop reason must be completed");
    }
}

void test_pie_base_slide() {
    // Machine code: mov eax, 55; ret
    const std::array<std::byte, 6> code = {{
        std::byte{0xB8}, std::byte{0x37}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0xC3}
    }};

    // e_type = ET_DYN (PIE) with base 0
    simple_elf_builder builder(pcsx5::core::et_dyn, 0, 0);
    builder.add_load_segment(pcsx5::core::pf_r | pcsx5::core::pf_x, code);
    auto elf_bytes = builder.build();

    pcsx5::frontend::elf_session_config cfg{};
    cfg.base_address_slide = 0x80000000;

    auto res = pcsx5::frontend::run_elf(elf_bytes, cfg);
    EXPECT(res.has_value(), "run_elf with PIE base slide must succeed");
    if (res.has_value()) {
        EXPECT(res->exited, "Module must flag exited");
        EXPECT_EQ(res->exit_code, 55ll, "Exit code must match RAX (55)");
        EXPECT_EQ(res->module.base_address, 0x80000000ull, "Module base address must match slide");
    }
}

void test_error_handling() {
    // 1. Truncated bytes
    std::array<std::byte, 10> short_bytes{};
    auto r1 = pcsx5::frontend::run_elf(short_bytes);
    EXPECT(!r1.has_value(), "Truncated file must be rejected");

    // 2. Budget exhausted on infinite loop (jmp $ : 0xEB, 0xFE)
    const std::array<std::byte, 2> loop_code = {{std::byte{0xEB}, std::byte{0xFE}}};
    simple_elf_builder builder(pcsx5::core::et_exec, 0x400000, 0);
    builder.add_load_segment(pcsx5::core::pf_r | pcsx5::core::pf_x, loop_code);
    auto elf_bytes = builder.build();

    pcsx5::frontend::elf_session_config cfg{};
    cfg.budget = 100;
    auto r2 = pcsx5::frontend::run_elf(elf_bytes, cfg);
    EXPECT(r2.has_value(), "run_elf returns session result on budget exhaust");
    if (r2.has_value()) {
        EXPECT_EQ(static_cast<int>(r2->session.stop),
                  static_cast<int>(pcsx5::execution::stop_reason::budget_exhausted),
                  "Stop reason must be budget_exhausted");
        EXPECT_EQ(r2->session.retired, 100ull, "Retired exactly 100 instructions");
    }
}

} // namespace

int main() {
    std::cout << "[RUN ] test_elf_return_exit_code\n";
    test_elf_return_exit_code();
    std::cout << "[RUN ] test_elf_direct_sys_exit\n";
    test_elf_direct_sys_exit();
    std::cout << "[RUN ] test_elf_hle_dispatch_integration\n";
    test_elf_hle_dispatch_integration();
    std::cout << "[RUN ] test_self_container_execution\n";
    test_self_container_execution();
    std::cout << "[RUN ] test_pie_base_slide\n";
    test_pie_base_slide();
    std::cout << "[RUN ] test_error_handling\n";
    test_error_handling();

    std::cout << "\nTotal checks: " << g_checks << ", Failures: " << g_failures << "\n";
    return (g_failures == 0) ? 0 : 1;
}
