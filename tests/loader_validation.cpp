#include "loader/elf.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <cstring>
#include <vector>

namespace {

bool WriteFixture(const std::filesystem::path& path, const void* data, size_t size) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        std::cerr << "Could not create fixture: " << path << '\n';
        return false;
    }
    output.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(output);
}

bool ExpectRejected(const std::filesystem::path& path, const char* label) {
    Loader::LoadedModule module;
    if (Loader::Load(path.string(), module)) {
        std::cerr << "Loader accepted invalid fixture: " << label << '\n';
        return false;
    }
    return true;
}

Loader::Elf64_Ehdr MakeHeader() {
    Loader::Elf64_Ehdr header{};
    header.e_ident[0] = Loader::ELFMAG0;
    header.e_ident[1] = Loader::ELFMAG1;
    header.e_ident[2] = Loader::ELFMAG2;
    header.e_ident[3] = Loader::ELFMAG3;
    header.e_ident[4] = 2;
    header.e_machine = 0x3E;
    header.e_ehsize = sizeof(Loader::Elf64_Ehdr);
    header.e_phoff = sizeof(Loader::Elf64_Ehdr);
    header.e_phentsize = sizeof(Loader::Elf64_Phdr);
    header.e_phnum = 1;
    return header;
}

bool WriteHeaderAndProgramHeader(const std::filesystem::path& path,
                                 const Loader::Elf64_Ehdr& header,
                                 const Loader::Elf64_Phdr& program_header) {
    std::vector<unsigned char> data(sizeof(header) + sizeof(program_header));
    std::memcpy(data.data(), &header, sizeof(header));
    std::memcpy(data.data() + sizeof(header), &program_header, sizeof(program_header));
    return WriteFixture(path, data.data(), data.size());
}

} // namespace

int main() {
    const std::filesystem::path fixture_dir =
        std::filesystem::temp_directory_path() / "pcsx5_loader_validation";
    std::filesystem::create_directories(fixture_dir);

    const std::array<unsigned char, 8> truncated = {0x7f, 'E', 'L', 'F', 2, 1, 1, 0};
    if (!WriteFixture(fixture_dir / "truncated.elf", truncated.data(), truncated.size()) ||
        !ExpectRejected(fixture_dir / "truncated.elf", "truncated header")) {
        return 1;
    }

    Loader::Elf64_Ehdr invalid_magic{};
    invalid_magic.e_ident[0] = 'N';
    invalid_magic.e_ident[1] = 'O';
    invalid_magic.e_ident[2] = 'P';
    invalid_magic.e_ident[3] = 'E';
    if (!WriteFixture(fixture_dir / "invalid_magic.elf", &invalid_magic, sizeof(invalid_magic)) ||
        !ExpectRejected(fixture_dir / "invalid_magic.elf", "invalid magic")) {
        return 1;
    }

    Loader::Elf64_Ehdr wrong_machine = MakeHeader();
    wrong_machine.e_machine = 0x03;
    if (!WriteFixture(fixture_dir / "wrong_machine.elf", &wrong_machine, sizeof(wrong_machine)) ||
        !ExpectRejected(fixture_dir / "wrong_machine.elf", "wrong machine")) {
        return 1;
    }

    Loader::Elf64_Ehdr out_of_bounds_table = MakeHeader();
    out_of_bounds_table.e_phoff = sizeof(Loader::Elf64_Ehdr) + 1024;
    if (!WriteFixture(fixture_dir / "out_of_bounds_table.elf", &out_of_bounds_table, sizeof(out_of_bounds_table)) ||
        !ExpectRejected(fixture_dir / "out_of_bounds_table.elf", "program-header table bounds")) {
        return 1;
    }

    Loader::Elf64_Ehdr valid_header = MakeHeader();
    Loader::Elf64_Phdr file_exceeds_memory{};
    file_exceeds_memory.p_type = Loader::PT_LOAD;
    file_exceeds_memory.p_offset = sizeof(valid_header) + sizeof(file_exceeds_memory);
    file_exceeds_memory.p_filesz = 2;
    file_exceeds_memory.p_memsz = 1;
    if (!WriteHeaderAndProgramHeader(fixture_dir / "file_exceeds_memory.elf", valid_header, file_exceeds_memory) ||
        !ExpectRejected(fixture_dir / "file_exceeds_memory.elf", "PT_LOAD file size exceeds memory size")) {
        return 1;
    }

    Loader::Elf64_Phdr out_of_bounds_segment{};
    out_of_bounds_segment.p_type = Loader::PT_LOAD;
    out_of_bounds_segment.p_offset = std::numeric_limits<u64>::max();
    out_of_bounds_segment.p_filesz = 1;
    out_of_bounds_segment.p_memsz = 1;
    if (!WriteHeaderAndProgramHeader(fixture_dir / "out_of_bounds_segment.elf", valid_header, out_of_bounds_segment) ||
        !ExpectRejected(fixture_dir / "out_of_bounds_segment.elf", "segment file bounds")) {
        return 1;
    }

    Loader::Elf64_Phdr overflowing_vaddr{};
    overflowing_vaddr.p_type = Loader::PT_LOAD;
    overflowing_vaddr.p_vaddr = std::numeric_limits<u64>::max();
    overflowing_vaddr.p_memsz = 1;
    if (!WriteHeaderAndProgramHeader(fixture_dir / "overflowing_vaddr.elf", valid_header, overflowing_vaddr) ||
        !ExpectRejected(fixture_dir / "overflowing_vaddr.elf", "PT_LOAD virtual-address overflow")) {
        return 1;
    }

    std::filesystem::remove_all(fixture_dir);
    std::cout << "Loader validation tests passed.\n";
    return 0;
}