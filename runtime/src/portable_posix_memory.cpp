#include <pcsx5/runtime/posix.h>
#include <sys/mman.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/personality.h>
#endif
#include <cerrno>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>
namespace pcsx5::runtime {
namespace {
memory_error native_error() noexcept {
    return errno == ENOMEM ? memory_error::out_of_memory : memory_error::host_failure;
}
memory_result<int> native_access(memory_access access) noexcept {
    switch (access) {
    case memory_access::none: return PROT_NONE;
    case memory_access::read_only: return PROT_READ;
    case memory_access::read_write: return PROT_READ | PROT_WRITE;
    }
    return std::unexpected(memory_error::unsupported);
}
struct tracked_page { page_info info{page_state::reserved,memory_access::none}; bool certain{true}; };
class reservation final : public memory_reservation {
public:
    reservation(memory_geometry geometry, std::uint64_t size)
        : geometry_(geometry), size_(size), pages_(static_cast<std::size_t>(size/geometry.page_size())) {}
    ~reservation() override { if (!release()) std::terminate(); }
    memory_result<void> initialize() noexcept {
        base_=mmap(nullptr,static_cast<std::size_t>(size_),PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if (base_==MAP_FAILED) return std::unexpected(native_error());
        live_=true; return {};
    }
    memory_geometry geometry() const noexcept override { return geometry_; }
    std::uint64_t size() const noexcept override { return size_; }
    memory_result<page_info> query(std::uint64_t offset) const noexcept override {
        if (!live_) return std::unexpected(memory_error::invalid_state);
        if (offset>=size_) return std::unexpected(memory_error::invalid_range);
        const auto& page=pages_[static_cast<std::size_t>(offset/geometry_.page_size())];
        if (!page.certain) return std::unexpected(memory_error::host_failure);
        return page.info;
    }
    memory_result<void> commit(std::uint64_t offset,std::uint64_t count,memory_access access) noexcept override {
        auto valid=validate(offset,count,page_state::reserved); if (!valid) return valid;
        auto protection=native_access(access); if (!protection) return std::unexpected(protection.error());
        // Explicit zeroing avoids relying on Darwin/Linux discard differences.
        if (mprotect(address(offset),static_cast<std::size_t>(count),PROT_READ|PROT_WRITE)!=0) return uncertain(offset,count);
        std::memset(address(offset),0,static_cast<std::size_t>(count));
        if (mprotect(address(offset),static_cast<std::size_t>(count),*protection)!=0) return uncertain(offset,count);
        record(offset,count,{page_state::committed,access}); return {};
    }
    memory_result<void> protect(std::uint64_t offset,std::uint64_t count,memory_access access) noexcept override {
        auto valid=validate(offset,count,page_state::committed); if (!valid) return valid;
        auto protection=native_access(access); if (!protection) return std::unexpected(protection.error());
        if (mprotect(address(offset),static_cast<std::size_t>(count),*protection)!=0) return uncertain(offset,count);
        record(offset,count,{page_state::committed,access}); return {};
    }
    memory_result<void> decommit(std::uint64_t offset,std::uint64_t count) noexcept override {
        auto valid=validate(offset,count,page_state::committed); if (!valid) return valid;
        if (mprotect(address(offset),static_cast<std::size_t>(count),PROT_NONE)!=0) return uncertain(offset,count);
        // Logical decommit. Pages may remain resident; next commit always zeros.
        record(offset,count,{page_state::reserved,memory_access::none}); return {};
    }
    memory_result<void> read(std::uint64_t offset,std::span<std::byte> output) const noexcept override {
        auto valid=copyable(offset,output.size(),false); if (!valid) return valid;
        std::memcpy(output.data(),address(offset),output.size()); return {};
    }
    memory_result<void> write(std::uint64_t offset,std::span<const std::byte> input) noexcept override {
        auto valid=copyable(offset,input.size(),true); if (!valid) return valid;
        std::memcpy(address(offset),input.data(),input.size()); return {};
    }
    memory_result<void> release() noexcept override {
        if (!live_) return {};
        if (munmap(base_,static_cast<std::size_t>(size_))!=0) return std::unexpected(native_error());
        live_=false; base_=nullptr; return {};
    }
private:
    void* address(std::uint64_t offset) const noexcept {
        return reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(base_)+offset);
    }
    memory_result<void> validate(std::uint64_t offset,std::uint64_t count,page_state state) const noexcept {
        if (!live_) return std::unexpected(memory_error::invalid_state);
        if (!page_range::make(geometry_,size_,offset,count)) return std::unexpected(memory_error::invalid_range);
        for (auto cursor=offset;cursor<offset+count;cursor+=geometry_.page_size()) {
            auto page=query(cursor); if (!page) return std::unexpected(page.error());
            if (page->state!=state) return std::unexpected(memory_error::invalid_state);
        }
        return {};
    }
    void record(std::uint64_t offset,std::uint64_t count,page_info info) noexcept {
        for (auto cursor=offset;cursor<offset+count;cursor+=geometry_.page_size())
            pages_[static_cast<std::size_t>(cursor/geometry_.page_size())]={info,true};
    }
    memory_result<void> uncertain(std::uint64_t offset,std::uint64_t count) noexcept {
        const auto error=native_error();
        for (auto cursor=offset;cursor<offset+count;cursor+=geometry_.page_size())
            pages_[static_cast<std::size_t>(cursor/geometry_.page_size())].certain=false;
        return std::unexpected(error);
    }
    memory_result<void> copyable(std::uint64_t offset,std::uint64_t count,bool write) const noexcept {
        if (!live_) return std::unexpected(memory_error::invalid_state);
        if (!count || offset>size_ || count>size_-offset) return std::unexpected(memory_error::invalid_range);
        for (auto cursor=offset-offset%geometry_.page_size();cursor<offset+count;cursor+=geometry_.page_size()) {
            auto page=query(cursor); if (!page) return std::unexpected(page.error());
            if (page->state!=page_state::committed) return std::unexpected(memory_error::invalid_state);
            if (page->access==memory_access::none || (write && page->access!=memory_access::read_write))
                return std::unexpected(memory_error::access_denied);
        }
        return {};
    }
    memory_geometry geometry_;
    std::uint64_t size_;
    std::vector<tracked_page> pages_;
    void* base_{};
    bool live_{};
};
}
memory_result<memory_geometry> posix_memory_geometry() noexcept {
    const long page=sysconf(_SC_PAGESIZE);
    if (page<=0) return std::unexpected(memory_error::host_failure);
    auto geometry=memory_geometry::make(static_cast<std::uint64_t>(page),static_cast<std::uint64_t>(page));
    if (!geometry) return std::unexpected(memory_error::unsupported);
    return *geometry;
}
memory_result<std::unique_ptr<memory_reservation>> reserve_posix_memory(std::uint64_t size) noexcept {
    auto geometry=posix_memory_geometry(); if (!geometry) return std::unexpected(geometry.error());
    if (!page_range::make(*geometry,size,0,size) || size>static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max()))
        return std::unexpected(memory_error::invalid_range);
#if defined(__linux__)
    const int flags=personality(0xffffffffUL);
    if (flags<0 || (flags & READ_IMPLIES_EXEC)!=0) return std::unexpected(memory_error::unsupported);
#endif
    try {
        auto owner=std::make_unique<reservation>(*geometry,size);
        auto initialized=owner->initialize(); if (!initialized) return std::unexpected(initialized.error());
        return std::unique_ptr<memory_reservation>(std::move(owner));
    } catch (const std::bad_alloc&) { return std::unexpected(memory_error::out_of_memory); }
      catch (const std::length_error&) { return std::unexpected(memory_error::out_of_memory); }
}
}
