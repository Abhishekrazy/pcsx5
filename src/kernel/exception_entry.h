#pragma once

#include <windows.h>
#include <cstddef>
#include <vector>
#include <string>

namespace HLE { struct TraceEntry; }

namespace Kernel::Legacy {

// Legacy Windows adapter only. Returns the same handler registered by
// Kernel::Initialize; exposing its address adds no alternative routing logic.
PVECTORED_EXCEPTION_HANDLER ExceptionEntry();

// Diagnostic-only seam: null uses the original HLE import trace. A reader may
// supply diagnostic history, but cannot alter fault routing or guest state.
// Both reader setters are unsynchronized configuration: install/reset only
// while guest execution is stopped and the exception handler is uninstalled.
using ExceptionTraceReader = std::vector<HLE::TraceEntry> (*)(std::size_t);
void SetExceptionTraceReader(ExceptionTraceReader reader);
using ExceptionTimelineReader = std::vector<std::string> (*)();
void SetExceptionTimelineReader(ExceptionTimelineReader reader);

}
