// Unit tests for the guest printf family (src/hle/guest_printf.{h,cpp}) and
// the libc memcmp registration (src/hle/liblibc.cpp).
//
// Covered:
//   - FormatGuestString over a synthesized SysV va_list (%s %d %i %u %x %X %o
//     %p %c %%, flags, width, precision, h/hh/l/ll/z length modifiers, '*')
//   - FormatGuestStringFromRegs: dispatcher-captured register varargs plus
//     overflow args read from the guest stack (SysV AMD64 ABI)
//   - GuestSprintf / GuestSnprintf / GuestVsnprintf handlers (incl. truncation)
//   - The registered libc::sprintf / libc::memcmp symbols via HleDispatch
//
// Build target: guest_printf_tests (see CMakeLists.txt).

#include "hle/hle.h"
#include "hle/guest_printf.h"
#include "memory/memory.h"

#include <cstdio>
#include <cstring>
#include <string>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

extern "C" u64 HleDispatch(u64, u64, u64, u64, u64, u64, u64, u64, u64);

namespace {

int g_failures = 0;

#define EXPECT(cond, msg)                                                        \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, msg); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

void ExpectStr(const std::string& got, const char* want, const char* msg) {
    if (got != want) {
        std::fprintf(stderr, "[FAIL] %s:%d: %s  (got='%s' want='%s')\n",
                     __FILE__, __LINE__, msg, got.c_str(), want);
        ++g_failures;
    }
}

// Builds a SysV va_list whose reg_save_area holds `regs` (up to 6) and whose
// overflow area is `stack`.  gp_offset starts at `first` (in slots).
HLE::SysVAmd64VaList MakeVaList(u64* regs, u32 first, u64* stack) {
    HLE::SysVAmd64VaList vl;
    vl.gp_offset = first * 8;
    vl.fp_offset = 48 + 128;
    vl.overflow_arg_area = reinterpret_cast<u64>(stack);
    vl.reg_save_area = reinterpret_cast<u64>(regs);
    return vl;
}

// Read the symbol_id encoded in a thunk (see tests/hle_import_report.cpp).
u64 ReadSymbolIdFromThunk(guest_addr_t thunk_addr) {
    if (!thunk_addr) return 0;
    u8 buf[10] = {};
    std::memcpy(buf, reinterpret_cast<const void*>(thunk_addr), sizeof(buf));
    u64 id = 0;
    for (int i = 0; i < 8; ++i) {
        id |= static_cast<u64>(buf[2 + i]) << (8 * i);
    }
    return id;
}

void TestFormatBasics() {
    u64 regs[6] = {};
    u64 stack[8] = {};

    const char* str = "world";
    // %s / %d / %x / %u / %% in one format.
    regs[0] = reinterpret_cast<u64>(str);
    regs[1] = static_cast<u64>(static_cast<s64>(-42));
    regs[2] = 0xDEADBEEF;
    regs[3] = 4000000000u;
    auto vl = MakeVaList(regs, 0, stack);
    ExpectStr(HLE::FormatGuestString("hello %s %d %x %u 100%%", vl),
              "hello world -42 deadbeef 4000000000 100%", "basic conversions");

    // %i, %X, %o, %c, %p.
    regs[0] = 77;
    regs[1] = 255;
    regs[2] = 8;
    regs[3] = 'A';
    regs[4] = 0x1234;
    vl = MakeVaList(regs, 0, stack);
    ExpectStr(HLE::FormatGuestString("%i %X %o %c %p", vl),
              "77 FF 10 A 0x1234", "i/X/o/c/p conversions");

    // Signed vs unsigned interpretation of the same bits.
    regs[0] = 0xFFFFFFFFull;
    regs[1] = 0xFFFFFFFFull;
    vl = MakeVaList(regs, 0, stack);
    ExpectStr(HLE::FormatGuestString("%d %u", vl), "-1 4294967295", "32-bit sign extension");

    // Length modifiers.
    regs[0] = 0xFFFFFFFFFFFFFFFFull;
    regs[1] = 0xFF;
    regs[2] = 0x1234;
    regs[3] = 0xFFFFFFFFFFFFFFFFull;
    vl = MakeVaList(regs, 0, stack);
    ExpectStr(HLE::FormatGuestString("%lld %hhd %hd %zu", vl),
              "-1 -1 4660 18446744073709551615", "length modifiers");
}

void TestFormatWidthPrecision() {
    u64 regs[6] = {};
    u64 stack[8] = {};

    regs[0] = 42;
    regs[1] = 42;
    regs[2] = 42;
    regs[3] = 7;
    auto vl = MakeVaList(regs, 0, stack);
    ExpectStr(HLE::FormatGuestString("[%5d][%-5d][%05d][%.3d]", vl),
              "[   42][42   ][00042][007]", "integer width/flags/precision");

    const char* str = "truncate";
    regs[0] = reinterpret_cast<u64>(str);
    regs[1] = reinterpret_cast<u64>(str);
    regs[2] = reinterpret_cast<u64>(str);
    vl = MakeVaList(regs, 0, stack);
    ExpectStr(HLE::FormatGuestString("[%.3s][%10s][%-10s]", vl),
              "[tru][  truncate][truncate  ]", "string precision/width");

    // '*' width and precision pull int varargs.
    regs[0] = 8;    // width
    regs[1] = 3;    // precision
    regs[2] = 42;
    vl = MakeVaList(regs, 0, stack);
    ExpectStr(HLE::FormatGuestString("[%*.*d]", vl), "[     042]", "star width/precision");

    // %x with # flag and zero padding.
    regs[0] = 0xAB;
    regs[1] = 0xAB;
    vl = MakeVaList(regs, 0, stack);
    ExpectStr(HLE::FormatGuestString("%#x %#X", vl), "0xab 0XAB", "alternate form");
}

void TestFormatFromRegs() {
    // Simulates sprintf(dst, fmt, a, b, c, d, e, f): named args consume slots
    // 0-1, varargs a..d land in rdx..r9, e/f overflow to the guest stack.
    HLE::GuestArgs args = {};
    args.arg1 = 0; // dst (unused by the formatter)
    args.arg2 = 0; // fmt (unused by the formatter)
    args.arg3 = 1;
    args.arg4 = 2;
    args.arg5 = 3;
    args.arg6 = 4;
    u64 guest_stack[4] = {5, 6, 0, 0};
    args.stack_args = reinterpret_cast<u64>(&guest_stack[0]);

    ExpectStr(HLE::FormatGuestStringFromRegs("%d %d %d %d %d %d", args, 2),
              "1 2 3 4 5 6", "register + stack overflow varargs");

    // snprintf-style: three named args, varargs start at slot 3.
    args.arg3 = 0; // fmt (unused)
    args.arg4 = 10;
    args.arg5 = 20;
    args.arg6 = 30;
    guest_stack[0] = 40;
    ExpectStr(HLE::FormatGuestStringFromRegs("%d %d %d %d", args, 3),
              "10 20 30 40", "snprintf vararg offset");
}

void TestHandlers() {
    char dst[256];
    u64 stack[4] = {};

    // GuestSprintf: dst=arg1, fmt=arg2, varargs arg3+.
    const char* fmt = "%s=%d";
    const char* name = "level";
    HLE::GuestArgs args = {};
    args.arg1 = reinterpret_cast<u64>(dst);
    args.arg2 = reinterpret_cast<u64>(fmt);
    args.arg3 = reinterpret_cast<u64>(name);
    args.arg4 = 3;
    args.stack_args = reinterpret_cast<u64>(&stack[0]);
    std::memset(dst, 0xAA, sizeof(dst));
    EXPECT(HLE::GuestSprintf(args) == 7, "sprintf returns length");
    ExpectStr(dst, "level=3", "sprintf writes formatted string");

    // GuestSnprintf truncation: size 4 -> 3 chars + NUL, returns full length.
    args.arg2 = 4;
    args.arg3 = reinterpret_cast<u64>(fmt);
    args.arg4 = reinterpret_cast<u64>(name);
    args.arg5 = 3;
    std::memset(dst, 0xAA, sizeof(dst));
    EXPECT(HLE::GuestSnprintf(args) == 7, "snprintf returns would-be length");
    ExpectStr(dst, "lev", "snprintf truncates with NUL");
    EXPECT(dst[3] == '\0', "snprintf NUL-terminates");

    // GuestVsnprintf with a real va_list.
    u64 reg_area[22] = {};
    reg_area[0] = 99; // gp slot 0
    HLE::SysVAmd64VaList vl;
    vl.gp_offset = 0;
    vl.fp_offset = 48 + 128;
    vl.overflow_arg_area = reinterpret_cast<u64>(&stack[0]);
    vl.reg_save_area = reinterpret_cast<u64>(&reg_area[0]);
    const char* vfmt = "val=%d";
    args.arg1 = reinterpret_cast<u64>(dst);
    args.arg2 = sizeof(dst);
    args.arg3 = reinterpret_cast<u64>(vfmt);
    args.arg4 = reinterpret_cast<u64>(&vl);
    std::memset(dst, 0, sizeof(dst));
    EXPECT(HLE::GuestVsnprintf(args) == 6, "vsnprintf returns length");
    ExpectStr(dst, "val=99", "vsnprintf walks guest va_list");
}

void TestRegisteredSymbols() {
    char dst[256];
    u64 stack[4] = {};

    const u64 sprintf_id = ReadSymbolIdFromThunk(HLE::Resolve("libc", "sprintf"));
    EXPECT(sprintf_id != 0, "libc::sprintf resolves to a real symbol");

    const char* fmt = "/app0/%s";
    const char* file = "save.dat";
    std::memset(dst, 0, sizeof(dst));
    const u64 len = HleDispatch(sprintf_id,
                                reinterpret_cast<u64>(dst),
                                reinterpret_cast<u64>(fmt),
                                reinterpret_cast<u64>(file),
                                0, 0, 0, 0x9000, reinterpret_cast<u64>(&stack[-1]));
    EXPECT(len == 14, "registered sprintf returns length");
    ExpectStr(dst, "/app0/save.dat", "registered sprintf formats path");

    const u64 memcmp_id = ReadSymbolIdFromThunk(HLE::Resolve("libc", "memcmp"));
    EXPECT(memcmp_id != 0, "libc::memcmp resolves to a real symbol");
    const char* a = "abcdef";
    const char* b = "abdcef";
    const s64 cmp = static_cast<s64>(HleDispatch(
        memcmp_id, reinterpret_cast<u64>(a), reinterpret_cast<u64>(b), 6, 0, 0, 0, 0x9001, 0));
    EXPECT(cmp < 0, "memcmp(abc, abd) < 0");
    const s64 cmp_eq = static_cast<s64>(HleDispatch(
        memcmp_id, reinterpret_cast<u64>(a), reinterpret_cast<u64>(a), 6, 0, 0, 0, 0x9002, 0));
    EXPECT(cmp_eq == 0, "memcmp(a, a) == 0");

    // The auto-stub warning path must be gone: snprintf/vsprintf/vsnprintf
    // resolve as real symbols too.
    EXPECT(ReadSymbolIdFromThunk(HLE::Resolve("libc", "snprintf")) != 0, "libc::snprintf resolves");
    EXPECT(ReadSymbolIdFromThunk(HLE::Resolve("libc", "vsprintf")) != 0, "libc::vsprintf resolves");
    EXPECT(ReadSymbolIdFromThunk(HLE::Resolve("libc", "vsnprintf")) != 0, "libc::vsnprintf resolves");

    // libc::_Getpctype returns a 256-entry u16 classification table (MSVC
    // flags); spot-check the entries the game CRT indexes directly.
    const u64 pctype_id = ReadSymbolIdFromThunk(HLE::Resolve("libc", "_Getpctype"));
    EXPECT(pctype_id != 0, "libc::_Getpctype resolves to a real symbol");
    const guest_addr_t table = HleDispatch(pctype_id, 0, 0, 0, 0, 0, 0, 0x9003, 0);
    EXPECT(table != 0, "_Getpctype returns a table pointer");
    if (table) {
        const u16* t = reinterpret_cast<const u16*>(table);
        EXPECT((t['A'] & 0x101) == 0x101, "table['A'] is UPPER|ALPHA");
        EXPECT((t['a'] & 0x102) == 0x102, "table['a'] is LOWER|ALPHA");
        EXPECT((t['5'] & 0x84) == 0x84, "table['5'] is DIGIT|HEX");
        EXPECT((t[' '] & 0x08) == 0x08, "table[' '] is SPACE");
        EXPECT((t['\n'] & 0x08) == 0x08, "table['\\n'] is SPACE");
    }
}

void TestVariadicFloatFormatting() {
    // Tests variadic float formatting with multiple %f / %g / %e args passed via xmm_args
    HLE::GuestArgs args = {};
    args.arg1 = 0; // dst
    args.arg2 = 0; // fmt

    // Pack double representations into xmm_args[0..2]
    const double val0 = 3.14159;
    const double val1 = -12.5;
    const double val2 = 0.00042;
    std::memcpy(&args.xmm_args[0], &val0, sizeof(val0));
    std::memcpy(&args.xmm_args[1], &val1, sizeof(val1));
    std::memcpy(&args.xmm_args[2], &val2, sizeof(val2));

    ExpectStr(HLE::FormatGuestStringFromRegs("%.2f, %.1f, %g", args, 2),
              "3.14, -12.5, 0.00042", "variadic float formatting from xmm_args");
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    if (!Memory::Initialize()) {
        std::fprintf(stderr, "FATAL: Memory::Initialize failed\n");
        return 2;
    }
    if (!HLE::Initialize()) {
        std::fprintf(stderr, "FATAL: HLE::Initialize failed\n");
        Memory::Shutdown();
        return 2;
    }
    HLE::SetStrictImportMode(false);

    TestFormatBasics();
    TestFormatWidthPrecision();
    TestFormatFromRegs();
    TestHandlers();
    TestRegisteredSymbols();
    TestVariadicFloatFormatting();

    HLE::Shutdown();
    Memory::Shutdown();

    if (g_failures == 0) {
        std::fprintf(stdout, "Guest printf tests passed.\n");
        return 0;
    }
    std::fprintf(stderr, "Guest printf tests: %d failure(s).\n", g_failures);
    return 1;
}
