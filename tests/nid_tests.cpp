// PS5 NID (Name ID) encoder/decoder tests.
//
// Verifies:
//   - Encode/Decode round-trip is exact for the sample inputs.
//   - Decode rejects wrong-length / out-of-alphabet strings.
//   - ParseNidString splits the 4-char type tag correctly (the
//     "#T#T", "#A#B", "#S#N", "#B#C" forms).
//   - LookupNidName hits the small known-name table for the top three
//     NIDs observed in PPSA02929 (pZ9WXcClPO8, byV+FWlAnB4, 9ByRMdo7ywg).
//
// Self-contained: no Memory / Loader / HLE dependencies.

#include "common/nid.h"
#include "common/log.h"
#include "common/types.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace {

int g_failures = 0;
int g_checks   = 0;

#define EXPECT(cond, msg) do {                                     \
    ++g_checks;                                                    \
    if (!(cond)) {                                                 \
        ++g_failures;                                              \
        std::fprintf(stderr, "FAIL [%s:%d] %s\n",                  \
                     __FILE__, __LINE__, (msg));                   \
    }                                                              \
} while (0)

void TestRoundTrip(std::string_view s) {
    auto decoded = Common::DecodeNid(s);
    if (!decoded) {
        EXPECT(false, "DecodeNid of valid string returned nullopt");
        return;
    }
    auto re_encoded = Common::EncodeNid(*decoded);
    if (re_encoded != s) {
        std::fprintf(stderr, "  round-trip: in='%.*s' out='%.*s' (len %zu vs %zu)\n",
                     (int)s.size(), s.data(),
                     (int)re_encoded.size(), re_encoded.data(),
                     s.size(), re_encoded.size());
        ++g_checks;
        ++g_failures;
    } else {
        ++g_checks;
    }
}

void TestRoundTripFails(std::string_view s) {
    auto decoded = Common::DecodeNid(s);
    EXPECT(!decoded.has_value(), "DecodeNid of invalid string returned value");
}

void TestTypeTag(std::string_view full, Common::Ps5NidType expected) {
    auto parsed = Common::ParseNidString(full);
    if (!parsed) {
        EXPECT(false, "ParseNidString rejected valid input");
        return;
    }
    if (parsed->type != expected) {
        std::fprintf(stderr, "  type: in='%.*s' got=%d want=%d\n",
                     (int)full.size(), full.data(),
                     (int)parsed->type, (int)expected);
        ++g_checks;
        ++g_failures;
    } else {
        ++g_checks;
    }
}

void TestNameLookup(std::string_view nid11, std::string_view expected_name) {
    auto nid = Common::DecodeNid(nid11);
    EXPECT(nid.has_value(), "DecodeNid of table sample failed");
    if (!nid) return;
    auto name = Common::LookupNidName(*nid);
    EXPECT(name.has_value(), "LookupNidName missed a known NID");
    if (name) {
        EXPECT(*name == expected_name, "name string mismatch");
    }
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    LogConfig::SetLevel(LogCategory::Loader, LogLevel::Warn);
    LogConfig::SetLevel(LogCategory::Memory, LogLevel::Warn);

    std::fprintf(stdout, "=== nid_tests ===\n");

    // 1. Round-trip: the three top PPSA02929 NIDs and a handful of
    //    data/object/block samples from the same import list.
    //
    //    The 11th base64 char carries 2 bits of padding; valid Sony
    //    NIDs always have these bits as zero.  We only test inputs
    //    that satisfy that constraint, since the encoder drops them.
    TestRoundTrip("pZ9WXcClPO8");
    TestRoundTrip("byV+FWlAnB4");
    TestRoundTrip("9ByRMdo7ywg");
    TestRoundTrip("+P6FRGH4LfA");   // function
    TestRoundTrip("12wOHk8ywb0");   // object
    TestRoundTrip("+T8Xo6LtFJI");   // block
    TestRoundTrip("AAAAAAAAAAA");   // all-zero bytes
    TestRoundTrip("00000000000");   // all 0x34
    TestRoundTrip("zzzzzzzzzzw");   // last char 'w' = index 48, low 2 bits = 0
    TestRoundTrip("M3aXkQY3e9A");   // mixed, last char 'A' (low 2 bits = 0)

    // 2. Decode failures: wrong length, out-of-alphabet.
    TestRoundTripFails("pZ9WXcClPO");          // 10 chars
    TestRoundTripFails("pZ9WXcClPO89");        // 12 chars
    TestRoundTripFails("pZ9WXcClPO.");         // '.' not in Sony alphabet
    TestRoundTripFails("");                    // empty

    // 3. Type tag parsing (4-char suffix).
    TestTypeTag("pZ9WXcClPO8#T#T", Common::Ps5NidType::Function);
    TestTypeTag("+P6FRGH4LfA#A#B", Common::Ps5NidType::Data);
    TestTypeTag("12wOHk8ywb0#S#N", Common::Ps5NidType::Object);
    TestTypeTag("+T8Xo6LtFJI#B#C", Common::Ps5NidType::Block);

    // 4. Known-name lookup.  The table is small; we hit the three
    //    top PPSA02929 NIDs and a couple of libSceAgc entries.
    TestNameLookup("pZ9WXcClPO8", "sceKernelMapDirectMemory");
    TestNameLookup("byV+FWlAnB4", "sceKernelMapFlexibleMemory");
    TestNameLookup("9ByRMdo7ywg", "sceKernelAllocateFlexibleMemory");
    TestNameLookup("+P6FRGH4LfA", "memmove");
    TestNameLookup("1kZFcktOm+s", "sceAgcDriverInitialize");

    // 5. Unknown NID: round-trip still works, lookup misses.
    {
        auto nid = Common::DecodeNid("pZ9WXcClPO8");  // a known one
        EXPECT(nid.has_value(), "decode of test nid failed");
        if (nid) {
            // Pick an obviously fake NID that is not in the table by
            // mutating one character.  The encoder drops the last 2
            // bits, so flipping a high-bit char is the safest way to
            // guarantee a different value.
            std::string fake = Common::EncodeNid(*nid);
            fake[0] = (fake[0] == 'A') ? 'B' : 'A';
            auto fnid = Common::DecodeNid(fake);
            EXPECT(fnid.has_value(), "decode of fake nid failed");
            if (fnid) {
                auto miss = Common::LookupNidName(*fnid);
                EXPECT(!miss.has_value(), "lookup of fake nid unexpectedly hit");
            }
        }
    }

    // 6. NidType round-trip: every type tag has a stable string form.
    EXPECT(Common::NidTypeToString(Common::Ps5NidType::Function) == "#T#T",
           "Function tag string");
    EXPECT(Common::NidTypeToString(Common::Ps5NidType::Data) == "#A#B",
           "Data tag string");
    EXPECT(Common::NidTypeToString(Common::Ps5NidType::Object) == "#S#N",
           "Object tag string");
    EXPECT(Common::NidTypeToString(Common::Ps5NidType::Block) == "#B#C",
           "Block tag string");

    // 7. Type from string: malformed tags are rejected.
    EXPECT(!Common::NidTypeFromString("T#T#").has_value(),
           "3-char tag rejected");
    EXPECT(!Common::NidTypeFromString("").has_value(),
           "empty tag rejected");
    EXPECT(!Common::NidTypeFromString("X#T#T").has_value(),
           "missing leading '#' rejected");
    EXPECT(!Common::NidTypeFromString("#T#T#").has_value(),
           "5-char tag rejected");

    std::fprintf(stdout, "  %d/%d checks passed\n",
                 g_checks - g_failures, g_checks);
    if (g_failures != 0) {
        std::fprintf(stderr, "  %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::fprintf(stdout, "OK\n");
    return 0;
}
