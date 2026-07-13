#include "tls.h"

#include <limits>

namespace Kernel {

    bool GuestTlsContext::Configure(guest_addr_t allocation_base, u64 allocation_size,
                                     u64 thread_pointer_offset) {
        if (allocation_base == 0 || allocation_size == 0 ||
            thread_pointer_offset >= allocation_size ||
            allocation_base > std::numeric_limits<guest_addr_t>::max() - allocation_size) {
            Reset();
            return false;
        }

        allocation_base_ = allocation_base;
        allocation_size_ = allocation_size;
        thread_pointer_ = allocation_base + thread_pointer_offset;
        return true;
    }

    void GuestTlsContext::Reset() {
        allocation_base_ = 0;
        thread_pointer_ = 0;
        allocation_size_ = 0;
    }

    bool GuestTlsContext::Translate(s64 displacement, u64 width, guest_addr_t& address) const {
        if (thread_pointer_ == 0 || width == 0 || width > allocation_size_) {
            return false;
        }

        const s64 base = static_cast<s64>(thread_pointer_ - allocation_base_);
        if ((displacement > 0 && base > std::numeric_limits<s64>::max() - displacement) ||
            (displacement < 0 && base < std::numeric_limits<s64>::min() - displacement)) {
            return false;
        }

        const s64 offset = base + displacement;
        if (offset < 0 || static_cast<u64>(offset) > allocation_size_ - width) {
            return false;
        }

        address = allocation_base_ + static_cast<u64>(offset);
        return true;
    }
}
