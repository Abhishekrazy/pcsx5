#include "self_test.h"
#include <charconv>
#include <cstdio>
#include <fstream>
#include <string_view>
int main(int argc,char** argv) {
    if (argc==2 && std::string_view(argv[1])=="--self-test") {
        const bool passed=pcsx5::frontend::self_test();
        std::printf("PCSX5 experimental scalar self-test: %s (%s)\n",passed?"PASS":"FAIL",
            pcsx5::frontend::host_code_factory()?"ARM64 + interpreter":"interpreter");
        return passed?0:1;
    }
    if (argc==2 && std::string_view(argv[1])=="--version") {
        std::puts("PCSX5 0.1.0 experimental scalar developer build; no PS5 game compatibility claim."); return 0;
    }
    if (argc==4 && std::string_view(argv[1])=="--run") {
        std::uint64_t budget{};
        const std::string_view value(argv[3]);
        const auto parsed=std::from_chars(value.data(),value.data()+value.size(),budget);
        if (parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size()) return 2;
        std::ifstream stream(argv[2],std::ios::binary);
        if (!stream) { std::fputs("Cannot read input.\n",stderr); return 2; }
        std::array<std::byte,4097> bytes{};
        stream.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
        if (stream.bad()) return 2;
        const auto result=pcsx5::frontend::run_program(std::span(bytes).first(static_cast<std::size_t>(stream.gcount())),
            budget,pcsx5::frontend::host_code_factory());
        if (!result) { std::fprintf(stderr,"Session error: %d\n",static_cast<int>(result.error())); return 1; }
        std::printf("stop=%u retired=%llu translated=%llu interpreted=%llu RIP=%llx RAX=%llx\n",
            static_cast<unsigned>(result->stop),static_cast<unsigned long long>(result->retired),
            static_cast<unsigned long long>(result->translated),static_cast<unsigned long long>(result->interpreted),
            static_cast<unsigned long long>(result->state.rip),static_cast<unsigned long long>(result->state.gpr[0]));
        return result->stop==pcsx5::execution::stop_reason::breakpoint?0:1;
    }
    std::puts("PCSX5 experimental developer CLI\n--self-test\n--version\n--run <authored raw x86-64 file, 1..4096 bytes> <budget, 1..1000000>\nNot a PS5 ELF/game loader. INT3 ends a successful synthetic run.");
    return argc==1 || (argc==2 && std::string_view(argv[1])=="--help")?0:2;
}
