#include <pcsx5/runtime/executable_code.h>
#include <pthread.h>
#include <libkern/OSCacheControl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <exception>
#include <mutex>
#include <new>
namespace pcsx5::runtime {
namespace {
// One MAP_JIT region per process, as required by the hardened-runtime policy.
// The mutex serializes publication, execution and logical release across threads.
struct arena {
    std::mutex mutex;
    void* address{};
    std::size_t mapped{},page{};
    bool occupied{};
    ~arena() { if (address && munmap(address,mapped)!=0) std::terminate(); }
};
arena storage;
class code_owner final : public executable_code {
public:
    explicit code_owner(std::size_t size) noexcept : size_(size) {}
    ~code_owner() override { if (!release()) std::terminate(); }
    std::size_t size() const noexcept override { return size_; }
    std::size_t page_size() const noexcept override { return storage.page; }
    code_result<void> invoke(void* argument) noexcept override {
        std::lock_guard lock(storage.mutex);
        if (!live_ || !argument) return std::unexpected(code_error::invalid);
        pthread_jit_write_protect_np(1);
        reinterpret_cast<void(*)(void*)>(storage.address)(argument);
        return {};
    }
    code_result<void> release() noexcept override {
        if (!live_) return {};
        std::lock_guard lock(storage.mutex);
        live_=false; storage.occupied=false; return {};
    }
    void publish() noexcept { live_=true; }
private:
    std::size_t size_;
    bool live_{};
};
}
code_result<std::unique_ptr<executable_code>> create_darwin_code(std::span<const std::byte> bytes,code_isa isa) noexcept {
    if (bytes.empty() || bytes.size()>maximum_code_bytes || (isa!=code_isa::x64 && isa!=code_isa::arm64) ||
        (isa==code_isa::arm64 && bytes.size()%4!=0)) return std::unexpected(code_error::invalid);
#if !defined(__aarch64__)
    return std::unexpected(code_error::unavailable);
#else
    if (isa!=code_isa::arm64 || !pthread_jit_write_protect_supported_np()) return std::unexpected(code_error::unavailable);
    try {
        // Allocate before the arena lock; a failed unpublished owner is inert.
        auto owner=std::make_unique<code_owner>(bytes.size());
        std::lock_guard lock(storage.mutex);
        if (storage.occupied) return std::unexpected(code_error::unavailable);
        if (!storage.address) {
            const long page=sysconf(_SC_PAGESIZE);
            if (page<=0) return std::unexpected(code_error::host_failure);
            storage.page=static_cast<std::size_t>(page);
            storage.mapped=(maximum_code_bytes/storage.page+(maximum_code_bytes%storage.page!=0))*storage.page;
            void* mapped=mmap(nullptr,storage.mapped,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS|MAP_JIT,-1,0);
            if (mapped==MAP_FAILED) return std::unexpected(code_error::unavailable);
            storage.address=mapped;
        }
        // No callbacks, allocations or throwing operations in the writable window.
        pthread_jit_write_protect_np(0);
        std::memcpy(storage.address,bytes.data(),bytes.size());
        pthread_jit_write_protect_np(1);
        sys_icache_invalidate(storage.address,bytes.size());
        storage.occupied=true; owner->publish();
        return std::unique_ptr<executable_code>(std::move(owner));
    } catch (const std::bad_alloc&) { return std::unexpected(code_error::out_of_memory); }
}
}
