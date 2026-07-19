// See guest_printf.h for the design notes.
#include "guest_printf.h"
#include "../memory/memory.h"
#include <cstdio>
#include <cstring>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace HLE {

u64 GetNextVaListArg(SysVAmd64VaList& valist) {
    u64 val = 0;
    if (valist.gp_offset < 48) {
        val = Memory::Read<u64>(valist.reg_save_area + valist.gp_offset);
        valist.gp_offset += 8;
    } else {
        val = Memory::Read<u64>(valist.overflow_arg_area);
        valist.overflow_arg_area += 8;
    }
    return val;
}

double GetNextVaListDouble(SysVAmd64VaList& valist) {
    if (valist.fp_offset < 48 + 128) {
        // FP slots live at reg_save_area + 48 .. 48+128 (xmm0-7, 16 bytes each).
        const double val = Memory::Read<double>(valist.reg_save_area + valist.fp_offset);
        valist.fp_offset += 16;
        return val;
    }
    // FP overflow goes through the overflow arg area (8-byte slots).
    const u64 bits = Memory::Read<u64>(valist.overflow_arg_area);
    valist.overflow_arg_area += 8;
    double val;
    std::memcpy(&val, &bits, sizeof(val));
    return val;
}

bool SafeReadCharacter(u64 addr, u8& out_ch) {
    __try {
        out_ch = *reinterpret_cast<const u8*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string ReadGuestCString(guest_addr_t ptr, u64 max_len) {
    std::string out;
    if (!ptr) return out;
    for (u64 i = 0; i < max_len; ++i) {
        u8 ch = 0;
        if (!SafeReadCharacter(ptr + i, ch)) {
            out += "(invalid)";
            break;
        }
        if (ch == 0) break;
        out += static_cast<char>(ch);
    }
    return out;
}

namespace {

enum class Length { None, HH, H, L, LL, Z, T };

// Appends snprintf-style formatted output for one integer conversion.
// The host format is rebuilt from validated components only (no guest text is
// ever passed to the host CRT as a format string).
void AppendInteger(std::string& out, char type, const std::string& flags,
                   int width, int precision, Length len, u64 raw) {
    char fmt[32];
    char* p = fmt;
    *p++ = '%';
    for (char f : flags) *p++ = f;
    if (width >= 0) p += std::snprintf(p, 8, "%d", width);
    if (precision >= 0) p += std::snprintf(p, 8, ".%d", precision);
    *p++ = 'l';
    *p++ = 'l';
    *p++ = type;
    *p = '\0';

    char buf[1024];
    if (type == 'd' || type == 'i') {
        // Sign-extend per the declared length modifier.
        long long v;
        switch (len) {
        case Length::HH:   v = static_cast<signed char>(raw); break;
        case Length::H:    v = static_cast<short>(raw); break;
        case Length::L:
        case Length::LL:
        case Length::Z:
        case Length::T:    v = static_cast<long long>(raw); break;
        case Length::None:
        default:           v = static_cast<int>(raw); break;
        }
        std::snprintf(buf, sizeof(buf), fmt, v);
    } else {
        unsigned long long v;
        switch (len) {
        case Length::HH:   v = static_cast<unsigned char>(raw); break;
        case Length::H:    v = static_cast<unsigned short>(raw); break;
        case Length::L:
        case Length::LL:
        case Length::Z:
        case Length::T:    v = static_cast<unsigned long long>(raw); break;
        case Length::None:
        default:           v = static_cast<unsigned int>(raw); break;
        }
        std::snprintf(buf, sizeof(buf), fmt, v);
    }
    out += buf;
}

// Writes a 32-bit value to guest memory with SEH fault recovery (%n support).
// Separate function: __try cannot live in a function requiring unwinding.
void SafeWriteS32(u64 addr, s32 value) {
    __try {
        Memory::Write<s32>(addr, value);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

} // namespace

std::string FormatGuestString(const std::string& fmt, SysVAmd64VaList& valist) {
    std::string result;
    size_t i = 0;
    while (i < fmt.size()) {
        if (fmt[i] != '%' || i + 1 >= fmt.size()) {
            result += fmt[i];
            i++;
            continue;
        }
        const size_t spec_start = i;
        i++; // skip '%'
        if (fmt[i] == '%') {
            result += '%';
            i++;
            continue;
        }

        // Flags.
        std::string flags;
        while (i < fmt.size() && std::strchr("-+ #0", fmt[i])) {
            flags += fmt[i];
            i++;
        }
        // Width ('*' pulls an int vararg).
        int width = -1;
        if (i < fmt.size() && fmt[i] == '*') {
            width = static_cast<int>(GetNextVaListArg(valist));
            i++;
            if (width < 0) { flags += '-'; width = -width; }
            if (width > 512) width = 512;
        } else {
            while (i < fmt.size() && fmt[i] >= '0' && fmt[i] <= '9') {
                if (width < 0) width = 0;
                width = width * 10 + (fmt[i] - '0');
                if (width > 512) width = 512;
                i++;
            }
        }
        // Precision ('.*' pulls an int vararg).
        int precision = -1;
        if (i < fmt.size() && fmt[i] == '.') {
            i++;
            precision = 0;
            if (i < fmt.size() && fmt[i] == '*') {
                precision = static_cast<int>(GetNextVaListArg(valist));
                i++;
                if (precision < 0) precision = -1;
                if (precision > 512) precision = 512;
            } else {
                while (i < fmt.size() && fmt[i] >= '0' && fmt[i] <= '9') {
                    precision = precision * 10 + (fmt[i] - '0');
                    if (precision > 512) precision = 512;
                    i++;
                }
            }
        }
        // Length modifier.
        Length len = Length::None;
        if (i < fmt.size() && fmt[i] == 'h') {
            if (i + 1 < fmt.size() && fmt[i + 1] == 'h') { len = Length::HH; i += 2; }
            else { len = Length::H; i++; }
        } else if (i < fmt.size() && fmt[i] == 'l') {
            if (i + 1 < fmt.size() && fmt[i + 1] == 'l') { len = Length::LL; i += 2; }
            else { len = Length::L; i++; }
        } else if (i < fmt.size() && fmt[i] == 'z') {
            len = Length::Z; i++;
        } else if (i < fmt.size() && (fmt[i] == 't' || fmt[i] == 'j')) {
            len = Length::T; i++;
        }

        if (i >= fmt.size()) {
            result += fmt.substr(spec_start);
            break;
        }
        const char type = fmt[i];
        i++;

        switch (type) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
            AppendInteger(result, type, flags, width, precision, len,
                          GetNextVaListArg(valist));
            break;
        }
        case 'p': {
            const u64 v = GetNextVaListArg(valist);
            char buf[32];
            std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(v));
            result += buf;
            break;
        }
        case 'c': {
            const char ch = static_cast<char>(GetNextVaListArg(valist));
            std::string pad(width > 1 ? width - 1 : 0, ' ');
            if (!flags.empty() && flags.find('-') != std::string::npos) {
                result += ch; result += pad;
            } else {
                result += pad; result += ch;
            }
            break;
        }
        case 's': {
            const guest_addr_t str_ptr = static_cast<guest_addr_t>(GetNextVaListArg(valist));
            std::string str_val = str_ptr
                ? ReadGuestCString(str_ptr, precision >= 0 ? static_cast<u64>(precision) : 4096)
                : std::string("(null)");
            std::string pad(width > (int)str_val.size() ? width - (int)str_val.size() : 0, ' ');
            if (!flags.empty() && flags.find('-') != std::string::npos) {
                result += str_val; result += pad;
            } else {
                result += pad; result += str_val;
            }
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            // Only reachable for real guest va_lists (vsnprintf/vsprintf);
            // register-captured varargs synthesize fp_offset past the FP area
            // so this pulls a stack slot as a last resort.
            const double v = GetNextVaListDouble(valist);
            char hfmt[32];
            char* p = hfmt;
            *p++ = '%';
            for (char f : flags) *p++ = f;
            if (width >= 0) p += std::snprintf(p, 8, "%d", width);
            if (precision >= 0) p += std::snprintf(p, 8, ".%d", precision);
            *p++ = type;
            *p = '\0';
            char buf[1024];
            std::snprintf(buf, sizeof(buf), hfmt, v);
            result += buf;
            break;
        }
        case 'n': {
            // %n writes the count so far through a pointer; honored for
            // completeness (some libc paths use it internally).
            const guest_addr_t out_ptr = static_cast<guest_addr_t>(GetNextVaListArg(valist));
            if (out_ptr) {
                SafeWriteS32(out_ptr, static_cast<s32>(result.size()));
            }
            break;
        }
        default:
            // Unknown conversion: emit the spec verbatim.
            result += fmt.substr(spec_start, i - spec_start);
            break;
        }
    }
    return result;
}

std::string FormatGuestStringFromRegs(const std::string& fmt, const GuestArgs& args,
                                      u32 first_vararg) {
    // Synthesize a va_list over the six dispatcher-captured integer registers.
    // Guest VA == host VA, so a host stack buffer is a valid "reg save area".
    u64 reg_save_area[22] = {}; // 6 gp slots + 16 fp slots (left zeroed)
    reg_save_area[0] = args.arg1;
    reg_save_area[1] = args.arg2;
    reg_save_area[2] = args.arg3;
    reg_save_area[3] = args.arg4;
    reg_save_area[4] = args.arg5;
    reg_save_area[5] = args.arg6;

    SysVAmd64VaList valist;
    valist.gp_offset = first_vararg * 8;
    // No xmm registers were captured: point fp_offset past the FP save area so
    // any %f falls through to the (zeroed) upper slots instead of desyncing gp.
    valist.fp_offset = 48 + 128;
    // Overflow args beyond r9 live on the guest stack.  If the dispatcher did
    // not supply the guest stack pointer, drain from a zeroed dummy instead of
    // dereferencing address 0.
    static u64 g_zero_overflow[16] = {};
    valist.overflow_arg_area = args.stack_args
        ? args.stack_args
        : reinterpret_cast<u64>(&g_zero_overflow[0]);
    valist.reg_save_area = reinterpret_cast<u64>(&reg_save_area[0]);

    return FormatGuestString(fmt, valist);
}

// ---------------------------------------------------------------------------
// sprintf-family HLE handlers (registered by liblibc.cpp / libkernel.cpp).
// ---------------------------------------------------------------------------

namespace {

// Writes `formatted` to `dest` like the *nprintf family: at most `size` bytes
// including the NUL (size==0 = unbounded, i.e. sprintf semantics).  Returns
// the number of characters that would have been written excluding the NUL.
u64 WriteFormatted(guest_addr_t dest, u64 size, const std::string& formatted) {
    if (dest) {
        const u64 copy_len = (size == 0)
            ? formatted.size()
            : (formatted.size() < size - 1 ? formatted.size() : size - 1);
        if (copy_len > 0) {
            Memory::WriteBuffer(dest, formatted.data(), copy_len);
        }
        if (size != 0 || formatted.size() == copy_len) {
            Memory::Write<u8>(dest + copy_len, 0);
        }
    }
    return formatted.size();
}

} // namespace

u64 GuestSprintf(const GuestArgs& args) {
    const guest_addr_t dest = args.arg1;
    const guest_addr_t fmt_ptr = args.arg2;
    if (!fmt_ptr) return 0;
    const std::string fmt = ReadGuestCString(fmt_ptr, 2048);
    const std::string formatted = FormatGuestStringFromRegs(fmt, args, 2);
    return WriteFormatted(dest, 0, formatted);
}

u64 GuestSnprintf(const GuestArgs& args) {
    const guest_addr_t dest = args.arg1;
    const u64 size = args.arg2;
    const guest_addr_t fmt_ptr = args.arg3;
    if (!fmt_ptr) return 0;
    const std::string fmt = ReadGuestCString(fmt_ptr, 2048);
    const std::string formatted = FormatGuestStringFromRegs(fmt, args, 3);
    return WriteFormatted(dest, size, formatted);
}

u64 GuestVsprintf(const GuestArgs& args) {
    const guest_addr_t dest = args.arg1;
    const guest_addr_t fmt_ptr = args.arg2;
    const guest_addr_t valist_ptr = args.arg3;
    if (!fmt_ptr || !valist_ptr) return 0;
    const std::string fmt = ReadGuestCString(fmt_ptr, 2048);
    SysVAmd64VaList valist;
    Memory::ReadBuffer(valist_ptr, &valist, sizeof(valist));
    const std::string formatted = FormatGuestString(fmt, valist);
    return WriteFormatted(dest, 0, formatted);
}

u64 GuestVsnprintf(const GuestArgs& args) {
    const guest_addr_t dest = args.arg1;
    const u64 size = args.arg2;
    const guest_addr_t fmt_ptr = args.arg3;
    const guest_addr_t valist_ptr = args.arg4;
    if (!fmt_ptr || !valist_ptr) return 0;
    const std::string fmt = ReadGuestCString(fmt_ptr, 2048);
    SysVAmd64VaList valist;
    Memory::ReadBuffer(valist_ptr, &valist, sizeof(valist));
    const std::string formatted = FormatGuestString(fmt, valist);
    return WriteFormatted(dest, size, formatted);
}

} // namespace HLE
