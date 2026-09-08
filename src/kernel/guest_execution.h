#pragma once
#include "../common/types.h"

// Legacy Windows execution seam, not a portable core contract.
namespace Kernel {
bool StartGuestCaptured(guest_addr_t entry_point, guest_addr_t sp, u32* out_exit_code);
}
