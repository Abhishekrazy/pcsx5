#include <pcsx5/execution/worker_protocol.h>
#include <filesystem>
#include <fstream>
namespace ex = pcsx5::execution;
int worker_main(const std::filesystem::path& input,const std::filesystem::path& output) {
    if (!input.is_absolute() || !output.is_absolute() || input.lexically_normal() == output.lexically_normal()) return 2;
    std::error_code error;
    if (std::filesystem::equivalent(input,output,error) && !error) return 2;
    std::array<std::byte,ex::request_wire_size> wire{};
    std::ifstream stream(input,std::ios::binary);
    if (!stream.read(reinterpret_cast<char*>(wire.data()),static_cast<std::streamsize>(wire.size())) ||
        stream.peek() != std::char_traits<char>::eof() || stream.bad()) return 2;
    ex::worker_request request{};
    if (!ex::decode_request(wire,request)) return 2;
    const auto result = ex::execute_request(request);
    if (!result || !ex::matches_request(request,*result)) return 3;
    const auto encoded = ex::encode_response(*result);
    if (!encoded) return 3;
    std::ofstream response(output,std::ios::binary|std::ios::trunc);
    response.write(reinterpret_cast<const char*>(encoded->data()),static_cast<std::streamsize>(encoded->size()));
    response.close();
    return response ? 0 : 4;
}
#if defined(_WIN32)
int wmain(int argc,wchar_t** argv) {
#else
int main(int argc,char** argv) {
#endif
    if (argc != 3) return 2;
    try { return worker_main(std::filesystem::path(argv[1]),std::filesystem::path(argv[2])); }
    catch (...) { return 3; }
}
