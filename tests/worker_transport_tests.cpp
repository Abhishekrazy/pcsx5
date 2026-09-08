#include <pcsx5/execution/worker_protocol.h>
#include <pcsx5/runtime/child_process.h>
#include <cstdio>
#include <fstream>
#include <iterator>
namespace ex = pcsx5::execution;
namespace rt = pcsx5::runtime;
#if defined(PCSX5_TEST_LINUX)
constexpr auto run_child = rt::run_linux_child;
#else
constexpr auto run_child = rt::run_windows_child;
#endif
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while(false)
struct owned_files {
    std::filesystem::path root;
    ~owned_files() {
        // Only these two exact files were created by this fixture. Never recurse.
        std::error_code error;
        std::filesystem::remove(root / "request.bin",error);
        std::filesystem::remove(root / "response.bin",error);
        std::filesystem::remove(root,error);
    }
};
bool write_file(const std::filesystem::path& path,std::span<const std::byte> data) {
    std::ofstream out(path,std::ios::binary|std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data.data()),static_cast<std::streamsize>(data.size()));
    out.close(); return static_cast<bool>(out);
}
std::vector<std::byte> read_file(const std::filesystem::path& path) {
    std::ifstream input(path,std::ios::binary);
    std::vector<std::byte> bytes;
    char byte;
    while (input.get(byte)) {
        if (bytes.size() == ex::max_response_wire_size) return {};
        bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
    }
    if (!input.eof()) return {};
    return bytes;
}
std::string utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(),text.end());
}
int main(int argc,char** argv) {
    CHECK(argc == 3);
    const std::filesystem::path worker(argv[1]),parent(argv[2]);
    std::filesystem::path directory;
    for (unsigned attempt=0;attempt<1000;++attempt) {
        const auto candidate=parent / ("worker-transport-" + std::to_string(attempt));
        if (std::filesystem::create_directory(candidate)) { directory=candidate; break; }
    }
    CHECK(!directory.empty());
    owned_files files{directory};
    const auto input=directory / "request.bin",output=directory / "response.bin";
    const std::array arguments{utf8(input),utf8(output)};
    ex::worker_request request{};
    request.id=0x123456789abcdef0ULL; request.budget=16; request.initial.rip=ex::worker_memory_base;
    request.initial.gpr[3]=0x1800;
    // MOV eax,7; MOV [rbx],eax; INT3.
    request.memory[0]=std::byte{0xb8}; request.memory[1]=std::byte{7};
    request.memory[5]=std::byte{0x89}; request.memory[6]=std::byte{3}; request.memory[7]=std::byte{0xcc};
    std::array<std::byte,ex::request_wire_size> wire{};
    CHECK(ex::encode_request(request,wire)); CHECK(write_file(input,wire));
    CHECK((run_child(worker,arguments,5000,{}) == rt::process_result{rt::process_outcome::exited,0}));
    const auto response_bytes=read_file(output);
    const auto response=ex::decode_response(response_bytes);
    CHECK(response && ex::matches_request(request,*response));
    CHECK(response->final.gpr[0] == 7 && response->final.rip == 0x1007);
    CHECK(response->memory[0x800] == std::byte{7});
    CHECK(response->result.records == 3 && response->result.retired == 2 && response->result.reason == ex::stop_reason::breakpoint);
    const auto local=ex::execute_request(request); CHECK(local);
    const auto local_bytes=ex::encode_response(*local); CHECK(local_bytes && *local_bytes == response_bytes);
    ++request.id; CHECK(!ex::matches_request(request,*response)); // stale response
    CHECK(ex::encode_request(request,wire)); CHECK(write_file(input,wire));
    CHECK((run_child(worker,arguments,5000,{}) == rt::process_result{rt::process_outcome::exited,0}));
    const auto fresh=ex::decode_response(read_file(output)); CHECK(fresh && ex::matches_request(request,*fresh));
    CHECK(!ex::decode_response(std::span(response_bytes).first(response_bytes.size()-1)));
    auto corrupt=response_bytes; corrupt[0]=std::byte{}; CHECK(!ex::decode_response(corrupt));
    CHECK(write_file(input,std::span(wire).first(wire.size()-1)));
    const auto rejected=run_child(worker,arguments,5000,{});
    CHECK((rejected == rt::process_result{rt::process_outcome::exited,2}));
    // Old output can remain after failure: caller must gate consumption on exit0.
    // It must not be promoted to a new result merely because a file exists.
    const auto fresh_bytes=ex::encode_response(*fresh); CHECK(fresh_bytes);
    CHECK(read_file(output) == *fresh_bytes);
    CHECK(write_file(input,wire));
    const std::array same_paths{utf8(input),utf8(input)};
    CHECK((run_child(worker,same_paths,5000,{}) == rt::process_result{rt::process_outcome::exited,2}));
    CHECK(read_file(input) == std::vector<std::byte>(wire.begin(),wire.end()));
    std::puts("worker-transport-v1: child-state memory trace stale malformed failed-exit PASS");
}
