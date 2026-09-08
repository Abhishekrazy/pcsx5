#pragma once
#include "windows_memory_api.h"
#include <pcsx5/runtime/fault.h>

namespace pcsx5::runtime::detail {
// Caller holds owner alive and externally excludes release/mapping changes.
// Record must be a valid native exception record. No allocation, locks, native
// query, callbacks, context modification or demand commit occurs here.
[[nodiscard]] fault_observation observe_windows_fault(
    const memory_reservation& owner, const EXCEPTION_RECORD& record) noexcept;
// Adapter for a scoped SEH filter. Always continues search, even when owned.
// It only stores the normalized observation into caller-owned output.
[[nodiscard]] LONG windows_fault_filter(const memory_reservation& owner,
    const EXCEPTION_RECORD& record, fault_observation& output) noexcept;
} // namespace pcsx5::runtime::detail
