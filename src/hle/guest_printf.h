// Shared guest printf-family helpers.
//
// Implements the SysV AMD64 varargs conventions for the HLE sprintf family:
//   - vsnprintf/vsprintf receive a real guest va_list (struct with gp_offset /
//     fp_offset / overflow_arg_area / reg_save_area) and walk it directly.
//   - sprintf/snprintf receive their varargs in argument registers rdi..r9
//     (captured by the dispatcher into GuestArgs) with any overflow on the
//     guest stack (GuestArgs::stack_args).  We synthesize a va_list over the
//     captured registers + stack base so one formatter serves both paths.
//
// Supported conversions: %d %i %u %x %X %o %p %c %s %% with flags -+ #0,
// width/precision (incl. '*'), and length modifiers h/hh/l/ll/z/t.
// Floating-point (%f/%e/%g) is only supported when walking a real guest
// va_list (the FP reg-save area exists); for register-captured varargs the
// dispatcher does not capture xmm0-7, so a "(float)" placeholder is emitted.
#pragma once
#include "../common/types.h"
#include "hle.h"
#include <string>

namespace HLE {

// SysV AMD64 va_list layout (as built by guest clang __builtin_va_start).
struct SysVAmd64VaList {
    u32 gp_offset;
    u32 fp_offset;
    u64 overflow_arg_area;
    u64 reg_save_area;
};

// Fetch the next integer-class vararg from a guest va_list.
u64 GetNextVaListArg(SysVAmd64VaList& valist);

// Fetch the next SSE-class (double) vararg from a guest va_list.  Only valid
// for a real guest va_list whose reg_save_area holds the xmm save slots.
double GetNextVaListDouble(SysVAmd64VaList& valist);

// Reads one byte from guest memory with SEH fault recovery (returns false on
// an unmapped address instead of crashing the host).
bool SafeReadCharacter(u64 addr, u8& out_ch);

// Core formatter.  Walks `fmt`, consuming varargs from `valist`.
std::string FormatGuestString(const std::string& fmt, SysVAmd64VaList& valist);

// Formatter entry point for register-dispatched variadic calls (sprintf,
// snprintf).  `args` holds the six SysV integer argument registers;
// `first_vararg` is the 0-based index of the first variadic argument
// (2 for sprintf(dst, fmt, ...), 3 for snprintf(dst, size, fmt, ...)).
// Overflow args are read from args.stack_args (0 = unavailable).
std::string FormatGuestStringFromRegs(const std::string& fmt, const GuestArgs& args,
                                      u32 first_vararg);

// Reads a NUL-terminated guest C string (bounded by `max_len`).
std::string ReadGuestCString(guest_addr_t ptr, u64 max_len = 4096);

// Ready-to-register HLE handlers for the sprintf family.  Each returns the
// guest-style result (characters written, excluding the NUL).
u64 GuestSprintf(const GuestArgs& args);
u64 GuestSnprintf(const GuestArgs& args);
u64 GuestVsprintf(const GuestArgs& args);
u64 GuestVsnprintf(const GuestArgs& args);

} // namespace HLE
