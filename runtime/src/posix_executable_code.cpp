#include <pcsx5/runtime/executable_code.h>
#include <cerrno>
#include <cstring>
#include <exception>
#include <new>
#include <sys/mman.h>
#include <sys/personality.h>
#include <unistd.h>
namespace pcsx5::runtime {
namespace {
class code_owner final : public executable_code {
public:
    code_owner(std::size_t size,std::size_t mapped,std::size_t page) noexcept
        : size_(size),mapped_(mapped),page_(page) {}
    ~code_owner() override { if (!release()) std::terminate(); }
    std::size_t size() const noexcept override { return size_; }
    std::size_t page_size() const noexcept override { return page_; }
    code_result<void> invoke(void* argument) noexcept override {
        if (!address_ || !argument || !sealed_) return std::unexpected(code_error::invalid);
        reinterpret_cast<void(*)(void*)>(address_)(argument); return {};
    }
    code_result<void> release() noexcept override {
        if (!address_) return {};
        if (::munmap(address_,mapped_)!=0) return std::unexpected(code_error::host_failure);
        address_=nullptr; sealed_=false; return {};
    }
    std::size_t size_,mapped_,page_;
    void* address_{};
    bool sealed_{};
};
}
code_result<std::unique_ptr<executable_code>> create_posix_code(std::span<const std::byte> bytes,code_isa isa) noexcept {
    if (bytes.empty() || bytes.size()>maximum_code_bytes || (isa!=code_isa::x64 && isa!=code_isa::arm64) ||
        (isa==code_isa::arm64 && bytes.size()%4!=0)) return std::unexpected(code_error::invalid);
#if defined(__aarch64__)
    if (isa!=code_isa::arm64) return std::unexpected(code_error::unavailable);
#elif defined(__x86_64__)
    if (isa!=code_isa::x64) return std::unexpected(code_error::unavailable);
#else
    return std::unexpected(code_error::unavailable);
#endif
    const int personality=::personality(0xffffffffUL);
    if (personality<0 || (personality & READ_IMPLIES_EXEC)!=0) return std::unexpected(code_error::unavailable);
    const long native_page=::sysconf(_SC_PAGESIZE);
    if (native_page<=0) return std::unexpected(code_error::host_failure);
    const auto page=static_cast<std::size_t>(native_page);
    const auto pages=bytes.size()/page+(bytes.size()%page!=0 ? 1U : 0U);
    if (pages>static_cast<std::size_t>(-1)/page) return std::unexpected(code_error::invalid);
    try {
        auto owner=std::make_unique<code_owner>(bytes.size(),pages*page,page);
        owner->address_=::mmap(nullptr,owner->mapped_,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if (owner->address_==MAP_FAILED) {
            owner->address_=nullptr;
            return std::unexpected(errno==ENOMEM ? code_error::out_of_memory : code_error::host_failure);
        }
        std::memcpy(owner->address_,bytes.data(),bytes.size());
        __builtin___clear_cache(static_cast<char*>(owner->address_),static_cast<char*>(owner->address_)+bytes.size());
        if (::mprotect(owner->address_,owner->mapped_,PROT_READ|PROT_EXEC)!=0)
            return std::unexpected(errno==EACCES || errno==EPERM ? code_error::unavailable : code_error::host_failure);
        owner->sealed_=true;
        return std::unique_ptr<executable_code>(std::move(owner));
    } catch (const std::bad_alloc&) { return std::unexpected(code_error::out_of_memory); }
}
} // namespace pcsx5::runtime
