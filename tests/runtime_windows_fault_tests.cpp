#include "../runtime/src/windows_fault.h"
#include <cstdio>
#include <cstring>
using namespace pcsx5::runtime;
namespace {
void* capture(void* context, void* address, SIZE_T size, DWORD kind, DWORD protection) noexcept {
    const auto result = VirtualAlloc(address, size, kind, protection);
    if (kind == MEM_RESERVE) *static_cast<void**>(context) = result;
    return result;
}
BOOL free_memory(void*, void* address, SIZE_T size, DWORD kind) noexcept {
    return VirtualFree(address, size, kind);
}
BOOL protect(void*, void* address, SIZE_T size, DWORD access, DWORD* old) noexcept {
    return VirtualProtect(address, size, access, old);
}
SIZE_T query(void*, const void* address, MEMORY_BASIC_INFORMATION* info, SIZE_T size) noexcept {
    return VirtualQuery(address, info, size);
}
// No objects requiring C++ unwinding in the deliberately faulting test frame.
__declspec(noinline) bool actual_fault(const memory_reservation& owner, void* address,
    bool writing, fault_observation& observation) {
    __try {
        __try {
            if (writing) *static_cast<volatile unsigned char*>(address) = 42;
            else { const volatile auto value = *static_cast<volatile unsigned char*>(address); (void)value; }
        } __except(detail::windows_fault_filter(owner, *GetExceptionInformation()->ExceptionRecord, observation)) {
            return false; // Production filter must never consume the fault.
        }
    } __except(GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return true;
    }
    return false;
}
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while(false)
}
int main() {
    const auto geometry = windows_memory_geometry();
    CHECK(geometry);
    void* base{};
    auto owner = detail::reserve_windows_memory_with_api(geometry->page_size(),
        {&base, capture, free_memory, protect, query});
    CHECK(owner && *owner && base);
    fault_observation observation{fault_route::unsupported, {}};
    CHECK(actual_fault(**owner, base, false, observation));
    CHECK(observation.route == fault_route::owned_memory && observation.record &&
        observation.record->offset == 0 && observation.record->access == fault_access::read);
    CHECK(actual_fault(**owner, base, true, observation));
    CHECK(observation.record && observation.record->access == fault_access::write);
    void* other_base{};
    auto other = detail::reserve_windows_memory_with_api(geometry->page_size(),
        {&other_base, capture, free_memory, protect, query});
    CHECK(other && *other);
    CHECK(actual_fault(**owner, other_base, false, observation));
    CHECK(observation.route == fault_route::unowned && !observation.record);
    EXCEPTION_RECORD record{};
    record.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    record.NumberParameters = 2;
    record.ExceptionInformation[1] = reinterpret_cast<ULONG_PTR>(base);
    for (const ULONG_PTR access : {ULONG_PTR{0}, ULONG_PTR{1}, ULONG_PTR{8}}) {
        record.ExceptionInformation[0] = access;
        const auto copy = record;
        CHECK(detail::windows_fault_filter(**owner, record, observation) == EXCEPTION_CONTINUE_SEARCH);
        CHECK(observation.route == fault_route::owned_memory && observation.record);
        CHECK(observation.record->access == (access == 0 ? fault_access::read :
            access == 1 ? fault_access::write : fault_access::execute));
        CHECK(std::memcmp(&copy, &record, sizeof(record)) == 0);
    }
    record.ExceptionInformation[0] = 7;
    CHECK(detail::observe_windows_fault(**owner, record).route == fault_route::unsupported);
    record.ExceptionInformation[0] = 0;
    record.NumberParameters = 1;
    CHECK(detail::observe_windows_fault(**owner, record).route == fault_route::unsupported);
    record.NumberParameters = EXCEPTION_MAXIMUM_PARAMETERS + 1;
    CHECK(detail::observe_windows_fault(**owner, record).route == fault_route::unsupported);
    record.NumberParameters = 3;
    record.ExceptionCode = EXCEPTION_IN_PAGE_ERROR;
    observation = detail::observe_windows_fault(**owner, record);
    CHECK(observation.record && observation.record->cause == fault_cause::backing_store_error);
    record.ExceptionCode = EXCEPTION_BREAKPOINT;
    CHECK(detail::observe_windows_fault(**owner, record).route == fault_route::unsupported);
    record.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    record.ExceptionInformation[1] += geometry->page_size();
    CHECK(detail::observe_windows_fault(**owner, record).route == fault_route::unowned);
    record.ExceptionInformation[1] = reinterpret_cast<ULONG_PTR>(base);
    CHECK((*owner)->release());
    CHECK(detail::observe_windows_fault(**owner, record).route == fault_route::unowned);
    std::puts("real owned/unowned fault forwarding and synthetic normalization passed");
}
