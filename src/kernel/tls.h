#pragma once

#include "../common/types.h"

namespace Kernel {

    // Describes a guest TLS allocation using the FreeBSD-style variant-II layout:
    // the thread pointer lies inside the allocation so negative TLS offsets are valid.
    class GuestTlsContext {
    public:
        static constexpr u64 kDefaultAllocationSize = 128 * 1024;
        static constexpr u64 kDefaultThreadPointerOffset = kDefaultAllocationSize / 2;

        bool Configure(guest_addr_t allocation_base, u64 allocation_size,
                       u64 thread_pointer_offset = kDefaultThreadPointerOffset);
        void Reset();

        [[nodiscard]] guest_addr_t ThreadPointer() const { return thread_pointer_; }
        [[nodiscard]] guest_addr_t AllocationBase() const { return allocation_base_; }
        [[nodiscard]] u64 AllocationSize() const { return allocation_size_; }

        // Converts an FS displacement into a checked guest address. The displacement
        // is signed because variant-II TLS accesses commonly use negative offsets.
        [[nodiscard]] bool Translate(s64 displacement, u64 width, guest_addr_t& address) const;

    private:
        guest_addr_t allocation_base_ = 0;
        guest_addr_t thread_pointer_ = 0;
        u64 allocation_size_ = 0;
    };
}
