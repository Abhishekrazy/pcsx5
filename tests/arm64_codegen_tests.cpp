#include <pcsx5/execution/arm64_jit.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>
using namespace pcsx5::execution;
namespace {
unsigned checks{};
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::exit(1); } } while(false)
auto lower(std::initializer_list<unsigned> input) {
    std::vector<std::byte> bytes;
    for (auto value:input) bytes.push_back(static_cast<std::byte>(value));
    return lower_scalar(bytes,0x1000);
}
std::uint32_t word(const std::vector<std::byte>& bytes,std::size_t offset) {
    std::uint32_t value{};
    for (unsigned i=0;i<4;++i) value|=std::to_integer<std::uint32_t>(bytes[offset+i])<<(i*8);
    return value;
}
}
int main() {
    CHECK(lower({}).error()==jit_error::truncated);
    CHECK(lower({0x48}).error()==jit_error::truncated);
    CHECK(lower({0x48,0x48}).error()==jit_error::unsupported);
    CHECK(lower({0x40,0x90}).error()==jit_error::unsupported);
    for (auto op:{0xcc,0xc3,0xe9,0x50,0x66,0x0f}) CHECK(lower({static_cast<unsigned>(op)}).error()==jit_error::unsupported);
    CHECK(lower({0x89,0x00}).error()==jit_error::unsupported);
    CHECK(lower({0x81,0xd0}).error()==jit_error::unsupported);
    CHECK(lower({0xb8,1,2,3}).error()==jit_error::truncated);
    auto mov=lower({0x49,0xb9,1,2,3,4,5,6,7,8});
    CHECK(mov && mov->width==64 && mov->destination==9 && mov->length==10 && mov->value==0x0807060504030201ull);
    auto imm=lower({0x48,0x83,0xe8,0x80});
    CHECK(imm && imm->operation==scalar_operation::subtract && imm->value==0xffffffffffffff80ull);
    auto reg=lower({0x4d,0x8b,0xc1});
    CHECK(reg && reg->destination==8 && reg->source==9 && !reg->immediate);
    for (unsigned opcode:{1u,3u,9u,11u,33u,35u,41u,43u,49u,51u,57u,59u,133u}) {
        const auto decoded=lower({opcode,0xca});
        CHECK(decoded && decoded->width==32 && decoded->length==2);
    }
    auto nop=lower({0x90}); CHECK(nop);
    const std::array one{*nop}; auto code=emit_arm64(one); CHECK(code);
    CHECK(word(*code,code->size()-4)==0xd65f03c0u);
    // Golden MOVZ X9,#0x1001; STR X9,[X0,#128]; RET X30.
    CHECK(code->size()==12 && word(*code,0)==0xd2820029u && word(*code,4)==0xf9004009u);
    const std::array immediate_move{*mov};
    const auto move_code=emit_arm64(immediate_move); CHECK(move_code);
    // LDR X9,[X0,#72], materialize X10, MOV X11,X10, STR X11,[X0,#72].
    CHECK(word(*move_code,0)==0xf9402409u);
    CHECK(word(*move_code,20)==0xaa0a03ebu && word(*move_code,24)==0xf900240bu);
    // Every ModRM register pair, REX extension and width has explicit operands.
    for (unsigned rex=0x40;rex<=0x4f;++rex) {
        for (unsigned dest=0;dest<8;++dest) for (unsigned source=0;source<8;++source) {
            const auto modrm=0xc0u|(source<<3)|dest;
            const auto forward=lower({rex,0x89,modrm});
            const auto reverse=lower({rex,0x8b,modrm});
            CHECK(forward && reverse);
            CHECK(forward->destination==dest+((rex&1)?8:0));
            CHECK(forward->source==source+((rex&4)?8:0));
            CHECK(reverse->destination==forward->source && reverse->source==forward->destination);
            CHECK(forward->width==((rex&8)?64:32));
        }
    }
    const std::array long_move{std::byte{0x49},std::byte{0xb9},std::byte{1},std::byte{2},std::byte{3},std::byte{4},std::byte{5},std::byte{6},std::byte{7},std::byte{8}};
    for (std::size_t count=0;count<long_move.size();++count)
        CHECK(lower_scalar(std::span(long_move).first(count),0x1000).error()==jit_error::truncated);
    CHECK(lower({0x48,0x81,0xc0,0,0,0,0x80})->value==0xffffffff80000000ull);
    CHECK(lower({0x83,0xc0,0xff})->value==std::numeric_limits<std::uint64_t>::max());
    CHECK(lower({0x90,0xcc})->length==1); // no speculative fetch of next instruction
    const std::array nop_byte{std::byte{0x90}};
    CHECK(lower_scalar(nop_byte,0x0000800000000000ULL).error()==jit_error::invalid);
    CHECK(lower_scalar(nop_byte,0x00007fffffffffffULL).error()==jit_error::invalid);
    CHECK(lower_scalar(std::span(long_move),std::numeric_limits<std::uint64_t>::max()-9).error()==jit_error::invalid);
    CHECK(emit_arm64({}).error()==jit_error::invalid);
    auto bad=*nop; bad.width=8; CHECK(emit_arm64(std::span(&bad,1)).error()==jit_error::invalid);
    bad=*nop; bad.destination=16; CHECK(!emit_arm64(std::span(&bad,1)));
    bad=*nop; bad.operation=static_cast<scalar_operation>(99); CHECK(!emit_arm64(std::span(&bad,1)));
    bad=*nop; bad.length=0; CHECK(!emit_arm64(std::span(&bad,1)));
    bad=*nop; bad.length=16; CHECK(!emit_arm64(std::span(&bad,1)));
    bad=*nop; bad.rip=std::numeric_limits<std::uint64_t>::max(); CHECK(!emit_arm64(std::span(&bad,1)));
    std::array pair{*nop,*nop}; CHECK(!emit_arm64(pair)); pair[1].rip++; CHECK(emit_arm64(pair));
    std::vector<scalar_instruction> block(arm64_max_instructions,*imm);
    for (std::size_t i=0;i<block.size();++i) block[i].rip=0x1000+i*imm->length;
    code=emit_arm64(block); CHECK(code && code->size()<=arm64_max_code_bytes);
    block.push_back(*nop); CHECK(!emit_arm64(block));
    std::printf("arm64-codegen: %u checks PASS\n",checks);
}
