#include <pcsx5/runtime/fault.h>
#include <pcsx5/runtime/worker.h>
#include <cstdio>
#include <limits>
using namespace pcsx5::runtime;
static_assert(!make_fault_record(0, 0, fault_access::read, fault_cause::access_violation));
int main() {
    for (std::uint64_t size = 0; size < 32; ++size)
        for (std::uint64_t offset = 0; offset < 40; ++offset)
            for (unsigned access = 0; access < 5; ++access)
                for (unsigned cause = 0; cause < 4; ++cause) {
                    const auto result = make_fault_record(size, offset,
                        static_cast<fault_access>(access), static_cast<fault_cause>(cause));
                    if (result.has_value() != (offset < size && access < 3 && cause < 2) ||
                        (result && (result->offset != offset ||
                         result->access != static_cast<fault_access>(access) ||
                         result->cause != static_cast<fault_cause>(cause)))) return 1;
                }
    const auto max = std::numeric_limits<std::uint64_t>::max();
    if (!make_fault_record(max, max - 1, fault_access::execute, fault_cause::backing_store_error) ||
        make_fault_record(max, max, fault_access::read, fault_cause::access_violation)) return 1;
    std::puts("fault record normalization checks passed");
}
