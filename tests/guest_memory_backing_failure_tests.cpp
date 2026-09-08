#include <pcsx5/runtime/guest_memory_backing.h>
#include <array>
#include <cstdio>
#include <new>
#include <optional>

namespace {
namespace core = pcsx5::core;
namespace rt = pcsx5::runtime;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (false)
template<class T> bool fails(const core::guest_memory_result<T>& result, core::guest_memory_error error) {
    return !result && result.error() == error;
}
struct state {
    std::uint64_t size = 64;
    std::optional<rt::memory_error> commit_error, read_error, write_error, release_error;
    int commits = 0, reads = 0, writes = 0, releases = 0, destroyed = 0, destructor_cleanup = 0;
    bool live = true;
    std::uint64_t last_offset = 0, last_size = 0;
    rt::memory_access last_access = rt::memory_access::none;
    std::array<std::byte, 4> bytes{};
};
// Synthetic provider only. The context owns its array; destructor cleanup records
// loss of the final reservation owner, without pretending to exercise a host API.
class reservation final : public rt::memory_reservation {
public:
    explicit reservation(state& context) noexcept : context_(context) {}
    ~reservation() override {
        ++context_.destroyed;
        if (context_.live) { ++context_.destructor_cleanup; context_.live = false; }
    }
    rt::memory_geometry geometry() const noexcept override { return *rt::memory_geometry::make(1, 1); }
    std::uint64_t size() const noexcept override { return context_.size; }
    rt::memory_result<void> commit(std::uint64_t offset, std::uint64_t size, rt::memory_access access) noexcept override {
        ++context_.commits; context_.last_offset = offset; context_.last_size = size; context_.last_access = access;
        if (context_.commit_error) return std::unexpected(*context_.commit_error);
        return {};
    }
    rt::memory_result<void> protect(std::uint64_t, std::uint64_t, rt::memory_access) noexcept override {
        return std::unexpected(rt::memory_error::unsupported);
    }
    rt::memory_result<void> decommit(std::uint64_t, std::uint64_t) noexcept override {
        return std::unexpected(rt::memory_error::unsupported);
    }
    rt::memory_result<rt::page_info> query(std::uint64_t) const noexcept override {
        return std::unexpected(rt::memory_error::unsupported);
    }
    rt::memory_result<void> read(std::uint64_t offset, std::span<std::byte> output) const noexcept override {
        ++context_.reads; context_.last_offset = offset; context_.last_size = output.size();
        if (context_.read_error) return std::unexpected(*context_.read_error);
        if (output.size() != context_.bytes.size()) return std::unexpected(rt::memory_error::invalid_range);
        for (std::size_t i = 0; i < output.size(); ++i) output[i] = context_.bytes[i];
        return {};
    }
    rt::memory_result<void> write(std::uint64_t offset, std::span<const std::byte> input) noexcept override {
        ++context_.writes; context_.last_offset = offset; context_.last_size = input.size();
        if (context_.write_error) return std::unexpected(*context_.write_error);
        if (input.size() != context_.bytes.size()) return std::unexpected(rt::memory_error::invalid_range);
        for (std::size_t i = 0; i < input.size(); ++i) context_.bytes[i] = input[i];
        return {};
    }
    rt::memory_result<void> release() noexcept override {
        ++context_.releases;
        if (context_.release_error) return std::unexpected(*context_.release_error);
        context_.live = false;
        return {};
    }
private:
    state& context_;
};
// Factory signature has no context argument. This pointer is scoped to a single
// sequential invocation; no thread, signal handler or production override uses it.
state* factory_context = nullptr;
rt::memory_result<std::unique_ptr<rt::memory_reservation>> provider(std::uint64_t) noexcept {
    if (!factory_context) return std::unexpected(rt::memory_error::host_failure);
    auto owner = std::unique_ptr<rt::memory_reservation>{new (std::nothrow) reservation(*factory_context)};
    if (!owner) return std::unexpected(rt::memory_error::out_of_memory);
    return owner;
}
constexpr std::array errors{
    std::pair{rt::memory_error::invalid_range, core::guest_memory_error::invalid_range},
    std::pair{rt::memory_error::invalid_state, core::guest_memory_error::invalid_state},
    std::pair{rt::memory_error::unsupported, core::guest_memory_error::unsupported},
    std::pair{rt::memory_error::access_denied, core::guest_memory_error::access_denied},
    std::pair{rt::memory_error::out_of_memory, core::guest_memory_error::out_of_memory},
    std::pair{rt::memory_error::host_failure, core::guest_memory_error::host_failure},
    std::pair{static_cast<rt::memory_error>(99), core::guest_memory_error::host_failure}};
} // namespace

int main() {
    for (const auto& [native, normalized] : errors) {
        state context;
        factory_context = &context;
        context.commit_error = native;
        const auto failed = rt::make_guest_memory_backing(64, provider);
        CHECK(fails(failed, normalized));
        CHECK(context.commits == 1 && context.last_offset == 0 && context.last_size == 64);
        CHECK(context.last_access == rt::memory_access::read_write);
        CHECK(context.destroyed == 1 && context.destructor_cleanup == 1 && !context.live);
        CHECK(context.reads == 0 && context.writes == 0 && context.releases == 0);
    }
    {
        state context;
        factory_context = &context;
        CHECK(fails(rt::make_guest_memory_backing(32, provider), core::guest_memory_error::invalid_range));
        CHECK(context.commits == 0 && context.destroyed == 1 && context.destructor_cleanup == 1);
    }
    for (const auto& [native, normalized] : errors) {
        state context;
        factory_context = &context;
        auto backing = rt::make_guest_memory_backing(64, provider);
        CHECK(backing && *backing && (*backing)->size() == 64);
        CHECK(context.commits == 1 && context.destroyed == 0 && context.live);
        core::guest_memory guest;
        CHECK(guest.map(0x1000, 64, core::guest_memory_access::read_write, *backing));
        CHECK(!*backing && guest.mapping_count() == 1);
        std::array<std::byte, 4> output{}, pattern{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
        context.read_error = native;
        CHECK(fails(guest.read(0x1009, output), normalized));
        CHECK(context.reads == 1 && context.last_offset == 9 && context.last_size == 4);
        CHECK((output == std::array<std::byte, 4>{}));
        context.read_error.reset();
        context.write_error = native;
        CHECK(fails(guest.write(0x100b, pattern), normalized));
        CHECK(context.writes == 1 && context.last_offset == 11 && context.last_size == 4);
        CHECK((context.bytes == std::array<std::byte, 4>{}));
        context.write_error.reset();
        context.release_error = native;
        CHECK(fails(guest.unmap(0x1000), normalized));
        CHECK(guest.mapping_count() == 1 && context.releases == 1 && context.destroyed == 0 && context.live);
        // A failed release must not discard the guest mapping or its storage.
        CHECK(guest.write(0x100b, pattern));
        CHECK(guest.read(0x1009, output));
        CHECK(output == pattern);
        context.release_error.reset();
        CHECK(guest.unmap(0x1000));
        CHECK(guest.mapping_count() == 0 && context.releases == 2 && context.destroyed == 1 && !context.live);
        CHECK(context.destructor_cleanup == 0);
        CHECK(fails(guest.read(0x1009, output), core::guest_memory_error::unmapped));
    }
    {
        state context;
        factory_context = &context;
        {
            auto backing = rt::make_guest_memory_backing(64, provider);
            CHECK(backing && *backing);
            core::guest_memory guest;
            CHECK(guest.map(0x1000, 64, core::guest_memory_access::read_write, *backing));
        }
        CHECK(context.destroyed == 1 && context.destructor_cleanup == 1 && !context.live);
    }
    factory_context = nullptr;
    std::puts("Guest memory backing failure propagation and ownership cleanup passed");
}
