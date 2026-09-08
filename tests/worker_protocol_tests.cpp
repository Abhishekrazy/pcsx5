#include <pcsx5/execution/worker_protocol.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <vector>

namespace ex = pcsx5::execution;
namespace {
unsigned checks{};
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while(false)
void le(std::span<std::byte> bytes, std::size_t offset, std::uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i) bytes[offset+i] = static_cast<std::byte>((value >> (8*i)) & 255);
}
bool same(const ex::worker_request& a, const ex::worker_request& b) {
    return a.id == b.id && a.budget == b.budget && a.initial == b.initial && a.memory == b.memory;
}
bool same(const ex::worker_response& a, const ex::worker_response& b) {
    return a.id == b.id && a.result.reason == b.result.reason && a.result.records == b.result.records &&
        a.result.retired == b.result.retired && a.final == b.final && a.memory == b.memory && a.trace == b.trace;
}
ex::worker_request request() {
    ex::worker_request q; q.id = 42; q.budget = 20; q.initial.rip = 0x1000;
    // Authored MOV EAX,7; NOP; INT3, not a retail executable.
    q.memory[0] = std::byte{0xb8}; q.memory[1] = std::byte{7};
    q.memory[5] = std::byte{0x90}; q.memory[6] = std::byte{0xcc}; return q;
}
}
int main() {
    const auto q = request();
    std::array<std::byte,4264> wire{}, golden{};
    golden[0]=std::byte{'P'}; golden[1]=std::byte{'X'}; golden[2]=std::byte{'Q'}; golden[3]=std::byte{'1'};
    le(golden,4,42,8); le(golden,12,20,4); le(golden,144,0x1000,8);
    le(golden,152,2,8); le(golden,160,0x8d5,8);
    golden[168]=std::byte{0xb8}; golden[169]=std::byte{7}; golden[173]=std::byte{0x90}; golden[174]=std::byte{0xcc};
    CHECK(ex::request_wire_size == golden.size()); CHECK(ex::encode_request(q,wire)); CHECK(wire == golden);
    auto decoded = q; CHECK(ex::decode_request(wire,decoded)); CHECK(same(decoded,q));
    // Every short length must fail transactionally, including the zero-length prefix.
    for (std::size_t length=0; length<wire.size(); ++length) {
        CHECK(!ex::decode_request(std::span(wire).first(length),decoded)); CHECK(same(decoded,q));
    }
    auto oversized = std::vector<std::byte>(wire.begin(),wire.end()); oversized.push_back(std::byte{});
    CHECK(!ex::decode_request(oversized,decoded)); CHECK(same(decoded,q));
    CHECK(!ex::encode_request(q,std::span(wire).first(wire.size()-1))); CHECK(wire == golden);
    CHECK(!ex::encode_request(q,oversized));
    for (std::size_t offset=0; offset<wire.size(); ++offset) for (unsigned bit=0; bit<8; ++bit) {
        auto changed=wire; changed[offset] ^= static_cast<std::byte>(1U<<bit); decoded=q;
        if (ex::decode_request(changed,decoded)) {
            std::array<std::byte,4264> encoded{}; CHECK(ex::encode_request(decoded,encoded)); CHECK(encoded==changed);
        } else CHECK(same(decoded,q));
    }
    for (const auto budget : {0U,257U,0xffffffffU}) {
        auto invalid=q; invalid.budget=budget; CHECK(!ex::encode_request(invalid,wire)); CHECK(wire==golden);
        auto changed=wire; le(changed,12,budget,4); CHECK(!ex::decode_request(changed,decoded));
    }
    auto invalid=q; invalid.id=0; CHECK(!ex::encode_request(invalid,wire)); CHECK(wire==golden);
    invalid=q; invalid.initial.known_flags |= (1ULL<<63); CHECK(!ex::encode_request(invalid,wire));

    const auto result=ex::execute_request(q); CHECK(result.has_value()); const auto& r=*result;
    CHECK(r.id==42); CHECK(r.result.reason==ex::stop_reason::breakpoint);
    CHECK(r.result.records==3 && r.result.retired==2 && r.trace.size()==3);
    CHECK(r.final.gpr[0]==7 && r.final.rip==0x1006); CHECK(r.memory==q.memory);
    CHECK(r.trace[0].before==q.initial); CHECK(r.trace[0].after.gpr[0]==7);
    CHECK(r.trace[2].before==r.final && r.trace[2].after==r.final);
    CHECK(ex::matches_request(q,r));
    const auto encoded=ex::encode_response(r); CHECK(encoded.has_value());
    CHECK(encoded->size()==4269+3*362);
    // Independently constructed response prefix, not the production response encoder.
    std::vector<std::byte> prefix(4269);
    prefix[0]=std::byte{'P'}; prefix[1]=std::byte{'X'}; prefix[2]=std::byte{'R'}; prefix[3]=std::byte{'1'};
    le(prefix,4,42,8); prefix[12]=std::byte{5}; le(prefix,13,3,4); le(prefix,17,2,4);
    le(prefix,21,7,8); le(prefix,149,0x1006,8); le(prefix,157,2,8); le(prefix,165,0x8d5,8);
    std::copy(q.memory.begin(),q.memory.end(),prefix.begin()+173);
    CHECK(std::equal(prefix.begin(),prefix.end(),encoded->begin()));
    const auto response=ex::decode_response(*encoded); CHECK(response.has_value()); CHECK(same(*response,r));
    for (std::size_t offset=0; offset<encoded->size(); ++offset) for (unsigned bit=0; bit<8; ++bit) {
        auto mutated=*encoded; mutated[offset] ^= static_cast<std::byte>(1U<<bit);
        const auto parsed=ex::decode_response(mutated);
        if (parsed) { const auto again=ex::encode_response(*parsed); CHECK(again.has_value()); CHECK(*again==mutated); }
    }
    for (std::size_t length=0; length<encoded->size(); ++length)
        CHECK(!ex::decode_response(std::span(*encoded).first(length)));
    auto changed=*encoded; changed.push_back(std::byte{}); CHECK(!ex::decode_response(changed));
    for (const auto offset : {0U,3U,12U,13U,16U,17U,20U,172U,4269U,4585U}) {
        changed=*encoded; changed[offset]=std::byte{255}; CHECK(!ex::decode_response(changed));
    }
    changed=*encoded; le(changed,4,0,8); CHECK(!ex::decode_response(changed));
    auto corrupt=r; ++corrupt.id; CHECK(!ex::matches_request(q,corrupt));
    corrupt=r; ++corrupt.final.gpr[0]; CHECK(!ex::matches_request(q,corrupt));
    corrupt=r; corrupt.memory[100]=std::byte{1}; CHECK(!ex::matches_request(q,corrupt));
    corrupt=r; ++corrupt.trace[1].sequence; CHECK(!ex::matches_request(q,corrupt));
    corrupt=r; ++corrupt.trace[1].before.gpr[1]; CHECK(!ex::matches_request(q,corrupt));
    corrupt=r; corrupt.trace[0].result.instruction[1]=std::byte{8}; CHECK(!ex::matches_request(q,corrupt));
    corrupt=r; ++corrupt.result.retired; CHECK(!ex::matches_request(q,corrupt));
    corrupt=r; corrupt.trace.pop_back(); CHECK(!ex::matches_request(q,corrupt)); CHECK(!ex::encode_response(corrupt));

    auto write=q; write.memory.fill(std::byte{}); write.initial.gpr[0]=0x12345678; write.initial.gpr[3]=0x1100;
    write.memory[0]=std::byte{0x89}; write.memory[1]=std::byte{0x03}; write.memory[2]=std::byte{0xcc};
    const auto written=ex::execute_request(write); CHECK(written.has_value()); CHECK(ex::matches_request(write,*written));
    CHECK(written->result.retired==1 && written->result.reason==ex::stop_reason::breakpoint);
    CHECK(written->memory[256]==std::byte{0x78} && written->memory[257]==std::byte{0x56});
    CHECK(written->memory[258]==std::byte{0x34} && written->memory[259]==std::byte{0x12});
    CHECK(written->trace[0].result.wrote_memory && written->trace[0].result.write_size==4);
    corrupt=*written; corrupt.trace[0].result.write_bytes[0]=std::byte{0}; CHECK(!ex::matches_request(write,corrupt));
    write.initial.gpr[3]=0x2000;
    const auto fault=ex::execute_request(write); CHECK(fault.has_value()); CHECK(ex::matches_request(write,*fault));
    CHECK(fault->result.reason==ex::stop_reason::memory_failure && fault->result.retired==0);
    CHECK(fault->trace.size()==1 && fault->trace[0].result.access==ex::access_kind::write);
    CHECK(fault->trace[0].result.fault_address==0x2000 && fault->final==write.initial && fault->memory==write.memory);
    // A preceding write replaces the next instruction; matching must use the
    // progressive image, not the original image for every instruction fetch.
    auto modified=write; modified.initial.gpr[0]=0xcccccccc; modified.initial.gpr[3]=0x1002;
    modified.memory[2]=std::byte{0x90};
    const auto self_modified=ex::execute_request(modified); CHECK(self_modified.has_value());
    CHECK(self_modified->result.reason==ex::stop_reason::breakpoint && self_modified->result.retired==1);
    CHECK(ex::matches_request(modified,*self_modified));
    auto fetch=q; fetch.initial.rip=0x2000;
    const auto fetch_fault=ex::execute_request(fetch); CHECK(fetch_fault.has_value()); CHECK(ex::matches_request(fetch,*fetch_fault));
    CHECK(fetch_fault->result.reason==ex::stop_reason::memory_failure && fetch_fault->trace[0].result.access==ex::access_kind::fetch);
    CHECK(fetch_fault->final==fetch.initial && fetch_fault->memory==fetch.memory);
    auto read=write; read.memory[0]=std::byte{0x8b};
    const auto read_fault=ex::execute_request(read); CHECK(read_fault.has_value()); CHECK(ex::matches_request(read,*read_fault));
    CHECK(read_fault->result.reason==ex::stop_reason::memory_failure && read_fault->trace[0].result.access==ex::access_kind::read);
    auto unknown=q; unknown.initial.known_flags=0; unknown.memory[0]=std::byte{0x74}; unknown.memory[1]=std::byte{0};
    const auto unknown_result=ex::execute_request(unknown); CHECK(unknown_result.has_value()); CHECK(ex::matches_request(unknown,*unknown_result));
    CHECK(unknown_result->result.reason==ex::stop_reason::unknown_flags && unknown_result->result.retired==0);
    auto stopped=q; stopped.memory[0]=std::byte{0x0f}; stopped.memory[1]=std::byte{0x05};
    const auto syscall=ex::execute_request(stopped); CHECK(syscall.has_value()); CHECK(ex::matches_request(stopped,*syscall));
    CHECK(syscall->result.reason==ex::stop_reason::syscall && syscall->result.retired==0);
    stopped.memory[0]=std::byte{0xf4};
    const auto unsupported=ex::execute_request(stopped); CHECK(unsupported.has_value()); CHECK(ex::matches_request(stopped,*unsupported));
    CHECK(unsupported->result.reason==ex::stop_reason::unsupported && unsupported->result.retired==0);
    stopped.initial.rip=0x0000800000000000ULL;
    const auto address=ex::execute_request(stopped); CHECK(address.has_value()); CHECK(ex::matches_request(stopped,*address));
    CHECK(address->result.reason==ex::stop_reason::invalid_address && address->result.retired==0);
    auto bounded=q; bounded.budget=1;
    const auto one=ex::execute_request(bounded); CHECK(one.has_value()); CHECK(ex::matches_request(bounded,*one));
    CHECK(one->result.reason==ex::stop_reason::budget_exhausted && one->result.records==1 && one->result.retired==1);
    CHECK(one->final.gpr[0]==7 && one->final.rip==0x1005);
    bounded.budget=256; bounded.memory[0]=std::byte{0xeb}; bounded.memory[1]=std::byte{0xfe};
    const auto loop=ex::execute_request(bounded); CHECK(loop.has_value()); CHECK(ex::matches_request(bounded,*loop));
    CHECK(loop->result.reason==ex::stop_reason::budget_exhausted && loop->trace.size()==256 && loop->result.retired==256);
    CHECK(loop->final==bounded.initial);
    const auto maximum=ex::encode_response(*loop); CHECK(maximum.has_value()); CHECK(maximum->size()==ex::max_response_wire_size);
    const auto maximum_decoded=ex::decode_response(*maximum); CHECK(maximum_decoded.has_value()); CHECK(same(*maximum_decoded,*loop));
    bounded.budget=0; CHECK(!ex::execute_request(bounded));
    std::printf("worker-protocol: canonical malformed transactional execution matching bounds PASS checks=%u\n",checks);
}
