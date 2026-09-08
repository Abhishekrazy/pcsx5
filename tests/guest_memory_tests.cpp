#include <pcsx5/core/guest_memory.h>
#include <array>
#include <cstdio>
#include <limits>
#include <vector>

using namespace pcsx5::core;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (false)
struct counters { int releases{}; int reads{}; int writes{}; bool fail_release{}; };
class fake_backing final : public memory_backing {
public:
    explicit fake_backing(counters& counts) : counts_(counts) {}
    std::uint64_t size() const noexcept override { return 8; }
    guest_memory_result<void> read(std::uint64_t offset, std::span<std::byte> out) const noexcept override {
        ++counts_.reads;
        for (std::size_t i = 0; i < out.size(); ++i) out[i] = data_[static_cast<std::size_t>(offset) + i];
        return {};
    }
    guest_memory_result<void> write(std::uint64_t offset, std::span<const std::byte> in) noexcept override {
        ++counts_.writes;
        for (std::size_t i = 0; i < in.size(); ++i) data_[static_cast<std::size_t>(offset) + i] = in[i];
        return {};
    }
    guest_memory_result<void> release() noexcept override {
        ++counts_.releases;
        if (counts_.fail_release) return std::unexpected(guest_memory_error::host_failure);
        return {};
    }
private:
    counters& counts_;
    std::array<std::byte, 8> data_{};
};
template<class T> bool fails(const guest_memory_result<T>& r, guest_memory_error e) { return !r && r.error() == e; }
int main() {
    guest_memory space;
    counters first, second;
    std::unique_ptr<memory_backing> a = std::make_unique<fake_backing>(first);
    std::unique_ptr<memory_backing> b = std::make_unique<fake_backing>(second);
    CHECK(fails(space.map(0, 0, guest_memory_access::read_write, a), guest_memory_error::invalid_range));
    CHECK(fails(space.map(0, 7, guest_memory_access::read_write, a), guest_memory_error::invalid_range));
    CHECK(fails(space.map(0, 8, static_cast<guest_memory_access>(99), a), guest_memory_error::unsupported));
    CHECK(a && space.mapping_count() == 0);
    CHECK(space.map(16, 8, guest_memory_access::read_write, a));
    CHECK(!a && space.mapping_count() == 1);
    CHECK(fails(space.map(23, 8, guest_memory_access::read_write, b), guest_memory_error::overlap));
    CHECK(b);
    CHECK(space.map(24, 8, guest_memory_access::read_only, b));
    std::array<std::byte, 2> pattern{std::byte{0x12}, std::byte{0x34}}, out{};
    CHECK(space.write(20, pattern)); CHECK(space.read(20, out)); CHECK(out == pattern);
    CHECK(fails(space.write(23, pattern), guest_memory_error::invalid_range));
    CHECK(first.writes == 1 && second.writes == 0);
    CHECK(fails(space.write(24, pattern), guest_memory_error::access_denied));
    CHECK(fails(space.read(15, out), guest_memory_error::unmapped));
    CHECK(fails(space.read(16, {}), guest_memory_error::invalid_range));
    CHECK(fails(space.protect(17, guest_memory_access::none), guest_memory_error::unmapped));
    CHECK(space.protect(16, guest_memory_access::none));
    CHECK(fails(space.read(16, out), guest_memory_error::access_denied));
    CHECK(space.protect(16, guest_memory_access::read_write));
    first.fail_release = true;
    CHECK(fails(space.unmap(16), guest_memory_error::host_failure)); CHECK(space.mapping_count() == 2);
    CHECK(space.read(20, out)); CHECK(out == pattern);
    first.fail_release = false; CHECK(space.unmap(16)); CHECK(first.releases == 2);
    CHECK(fails(space.unmap(16), guest_memory_error::unmapped));
    CHECK(space.unmap(24)); CHECK(second.releases == 1);
    std::unique_ptr<memory_backing> edge = std::make_unique<fake_backing>(first);
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    CHECK(fails(space.map(maximum - 6, 8, guest_memory_access::read_write, edge), guest_memory_error::invalid_range));
    CHECK(space.map(maximum - 7, 8, guest_memory_access::read_write, edge));
    CHECK(space.write(maximum - 1, pattern)); CHECK(space.read(maximum - 1, out)); CHECK(out == pattern);
    CHECK(fails(space.read(maximum, out), guest_memory_error::invalid_range));
    CHECK(space.unmap(maximum - 7));
    CHECK(fails(space.map(0, 8, guest_memory_access::read_write, edge), guest_memory_error::invalid_state));
    std::puts("guest-memory: bounds ownership permissions cleanup PASS");
}
