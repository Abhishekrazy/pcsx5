#include <pcsx5/runtime/guest_memory_backing.h>
#include "host_memory_provider.h"
#include <cstdio>
#include <exception>
#include <new>
#include <utility>

namespace rt = pcsx5::runtime;
namespace core = pcsx5::core;
namespace {
struct observations { unsigned destructors{}; unsigned releases{}; unsigned closed_queries{}; } observed;
// Instrument delegation only; all mapping/commit/copy/release operations use
// the real selected host provider. No synthetic successful native release.
class observed_reservation final : public rt::memory_reservation {
public:
    explicit observed_reservation(std::unique_ptr<rt::memory_reservation> owner) noexcept
        : owner_(std::move(owner)) {}
    ~observed_reservation() override {
        ++observed.destructors;
        if (!release()) std::terminate();
    }
    observed_reservation(const observed_reservation&) = delete;
    observed_reservation& operator=(const observed_reservation&) = delete;
    observed_reservation(observed_reservation&&) = delete;
    observed_reservation& operator=(observed_reservation&&) = delete;
    rt::memory_geometry geometry() const noexcept override { return owner_->geometry(); }
    std::uint64_t size() const noexcept override { return owner_->size(); }
    rt::memory_result<void> commit(std::uint64_t offset, std::uint64_t size, rt::memory_access access) noexcept override {
        return owner_->commit(offset, size, access);
    }
    rt::memory_result<void> protect(std::uint64_t offset, std::uint64_t size, rt::memory_access access) noexcept override {
        return owner_->protect(offset, size, access);
    }
    rt::memory_result<void> decommit(std::uint64_t offset, std::uint64_t size) noexcept override {
        return owner_->decommit(offset, size);
    }
    rt::memory_result<rt::page_info> query(std::uint64_t offset) const noexcept override { return owner_->query(offset); }
    rt::memory_result<void> read(std::uint64_t offset, std::span<std::byte> out) const noexcept override {
        return owner_->read(offset, out);
    }
    rt::memory_result<void> write(std::uint64_t offset, std::span<const std::byte> in) noexcept override {
        return owner_->write(offset, in);
    }
    rt::memory_result<void> release() noexcept override {
        const auto result = owner_->release();
        if (result) {
            ++observed.releases;
            const auto closed = owner_->query(0);
            if (!closed && closed.error() == rt::memory_error::invalid_state) ++observed.closed_queries;
        }
        return result;
    }
private:
    std::unique_ptr<rt::memory_reservation> owner_;
};
rt::memory_result<std::unique_ptr<rt::memory_reservation>> reserve(std::uint64_t size) noexcept {
    auto owner = test_host::reserve_memory(size);
    if (!owner) return std::unexpected(owner.error());
    try { return std::make_unique<observed_reservation>(std::move(*owner)); }
    catch (const std::bad_alloc&) { return std::unexpected(rt::memory_error::out_of_memory); }
}
}
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (false)
int main() {
    const auto geometry = test_host::memory_geometry(); CHECK(geometry);
    {
        core::guest_memory guest;
        auto backing = rt::make_guest_memory_backing(geometry->page_size(), reserve);
        CHECK(backing && *backing);
        CHECK(guest.map(0x10000, geometry->page_size(), core::guest_memory_access::read_write, *backing));
        CHECK(!*backing);
        CHECK(observed.destructors == 0 && observed.releases == 0);
    }
    CHECK(observed.destructors == 1 && observed.releases == 1 && observed.closed_queries == 1);
    std::puts("guest-memory native destruction delegation PASS");
}
