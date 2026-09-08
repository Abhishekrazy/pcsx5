#include <pcsx5/runtime/executable_code.h>
#include <array>
#include <cstdint>
#include <cstdio>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif
namespace rt=pcsx5::runtime;
#if defined(_WIN32)
constexpr auto create_code=rt::create_windows_code;
#else
constexpr auto create_code=rt::create_posix_code;
#endif
bool mapping(std::uintptr_t address,bool require_rx) {
#if defined(_WIN32)
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<void*>(address),&info,sizeof(info))!=sizeof(info) || info.State!=MEM_COMMIT) return false;
    return !require_rx || info.Protect==PAGE_EXECUTE_READ;
#else
    std::FILE* file=std::fopen("/proc/self/maps","r"); if (!file) return false;
    char line[512]; bool found{};
    while (std::fgets(line,sizeof(line),file)) {
        unsigned long long start{},end{}; char permissions[5]{};
        if (std::sscanf(line,"%llx-%llx %4s",&start,&end,permissions)==3 && address>=start && address<end) {
            found=!require_rx || (permissions[0]=='r' && permissions[1]=='-' && permissions[2]=='x'); break;
        }
    }
    std::fclose(file); return found;
#endif
}
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while(false)
int main() {
#if defined(__aarch64__) || defined(_M_ARM64)
    constexpr auto isa=rt::code_isa::arm64, other=rt::code_isa::x64;
    // ADR X9,.; STR X9,[X0]; RET. Test-only address observation, not an API escape.
    std::array<std::byte,12> address_code{std::byte{9},std::byte{0},std::byte{0},std::byte{0x10},
        std::byte{9},std::byte{0},std::byte{0},std::byte{0xf9},std::byte{0xc0},std::byte{3},std::byte{0x5f},std::byte{0xd6}};
    auto value_code=address_code;
    value_code[0]=std::byte{0x49}; value_code[1]=std::byte{5}; value_code[2]=std::byte{0x80}; value_code[3]=std::byte{0xd2};
#else
    constexpr auto isa=rt::code_isa::x64, other=rt::code_isa::arm64;
#if defined(_WIN32)
    constexpr auto store=std::byte{1}; // [RCX]
#else
    constexpr auto store=std::byte{7}; // [RDI]
#endif
    std::array<std::byte,11> address_code{std::byte{0x48},std::byte{0x8d},std::byte{5},std::byte{0xf9},
        std::byte{0xff},std::byte{0xff},std::byte{0xff},std::byte{0x48},std::byte{0x89},store,std::byte{0xc3}};
    auto value_code=address_code;
    value_code[1]=std::byte{0xc7}; value_code[2]=std::byte{0xc0}; value_code[3]=std::byte{42};
    value_code[4]=value_code[5]=value_code[6]=std::byte{};
#endif
    CHECK(!create_code({},isa));
    std::array<std::byte,16> wrong_isa{};
    const auto rejected=create_code(wrong_isa,other);
    CHECK(!rejected && rejected.error()==rt::code_error::unavailable);
    CHECK(!create_code(wrong_isa,static_cast<rt::code_isa>(255)));
    std::array<std::byte,rt::maximum_code_bytes+1> oversized{};
    CHECK(!create_code(oversized,isa));
    for (unsigned repeat=0;repeat<32;++repeat) {
        auto code=create_code(address_code,isa); CHECK(code);
        CHECK((*code)->size()==address_code.size() && (*code)->page_size()!=0);
        CHECK(!(*code)->invoke(nullptr));
        std::uintptr_t address{}; CHECK((*code)->invoke(&address));
        CHECK(address!=0 && mapping(address,true));
        CHECK((*code)->release()); CHECK((*code)->release());
        CHECK(!mapping(address,false)); CHECK(!(*code)->invoke(&address));
        auto value=create_code(value_code,isa); CHECK(value);
        std::uint64_t result{}; CHECK((*value)->invoke(&result)); CHECK(result==42);
        // Destruction owns cleanup even without an explicit release.
    }
    std::puts("executable-code: actual native call RX mapping bounds ISA closed repeated PASS");
}
