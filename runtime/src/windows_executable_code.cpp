#include <pcsx5/runtime/executable_code.h>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <cstring>
#include <exception>
#include <new>
namespace pcsx5::runtime {
namespace {
class code_owner final : public executable_code {
public:
    code_owner(std::size_t size,std::size_t page) noexcept : size_(size),page_(page) {}
    ~code_owner() override { if (!release()) std::terminate(); }
    std::size_t size() const noexcept override { return size_; }
    std::size_t page_size() const noexcept override { return page_; }
    code_result<void> invoke(void* argument) noexcept override {
        if (!address_ || !argument || !sealed_) return std::unexpected(code_error::invalid);
        reinterpret_cast<void(*)(void*)>(address_)(argument); return {};
    }
    code_result<void> release() noexcept override {
        if (!address_) return {};
        if (!VirtualFree(address_,0,MEM_RELEASE)) return std::unexpected(code_error::host_failure);
        address_=nullptr; sealed_=false; return {};
    }
    std::size_t size_,page_;
    void* address_{};
    bool sealed_{};
};
}
code_result<std::unique_ptr<executable_code>> create_windows_code(std::span<const std::byte> bytes,code_isa isa) noexcept {
    if (bytes.empty() || bytes.size()>maximum_code_bytes || (isa!=code_isa::x64 && isa!=code_isa::arm64) ||
        (isa==code_isa::arm64 && bytes.size()%4!=0)) return std::unexpected(code_error::invalid);
#if defined(_M_ARM64)
    if (isa!=code_isa::arm64) return std::unexpected(code_error::unavailable);
#elif defined(_M_X64)
    if (isa!=code_isa::x64) return std::unexpected(code_error::unavailable);
#else
    return std::unexpected(code_error::unavailable);
#endif
    SYSTEM_INFO info{}; GetSystemInfo(&info);
    if (!info.dwPageSize) return std::unexpected(code_error::host_failure);
    try {
        auto owner=std::make_unique<code_owner>(bytes.size(),info.dwPageSize);
        owner->address_=VirtualAlloc(nullptr,bytes.size(),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
        if (!owner->address_) return std::unexpected(GetLastError()==ERROR_NOT_ENOUGH_MEMORY ? code_error::out_of_memory : code_error::host_failure);
        std::memcpy(owner->address_,bytes.data(),bytes.size());
        DWORD old{};
        if (!VirtualProtect(owner->address_,bytes.size(),PAGE_EXECUTE_READ,&old) ||
            !FlushInstructionCache(GetCurrentProcess(),owner->address_,bytes.size())) return std::unexpected(code_error::host_failure);
        owner->sealed_=true;
        return std::unique_ptr<executable_code>(std::move(owner));
    } catch (const std::bad_alloc&) { return std::unexpected(code_error::out_of_memory); }
}
} // namespace pcsx5::runtime
