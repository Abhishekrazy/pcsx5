// Original synthetic state values; no guest binary or operating-system context
// is executed. Each conversion direction has independently seeded expectations.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "cpu/cpu.h"
#include <array>
#include <cstdio>
#include <cstring>

namespace {
int failures = 0;
void Check(bool condition, const char* direction, const char* field) {
    std::printf("%s %s %s\n", condition ? "PASS" : "FAIL", direction, field);
    if (!condition) ++failures;
}

struct RegisterMapping {
    const char* name;
    u64 CPUState::* guest;
    DWORD64 CONTEXT::* host;
};
constexpr std::array<RegisterMapping, 17> mappings{{
    {"rax", &CPUState::rax, &CONTEXT::Rax},
    {"rbx", &CPUState::rbx, &CONTEXT::Rbx},
    {"rcx", &CPUState::rcx, &CONTEXT::Rcx},
    {"rdx", &CPUState::rdx, &CONTEXT::Rdx},
    {"rsi", &CPUState::rsi, &CONTEXT::Rsi},
    {"rdi", &CPUState::rdi, &CONTEXT::Rdi},
    {"rsp", &CPUState::rsp, &CONTEXT::Rsp},
    {"rbp", &CPUState::rbp, &CONTEXT::Rbp},
    {"r8", &CPUState::r8, &CONTEXT::R8},
    {"r9", &CPUState::r9, &CONTEXT::R9},
    {"r10", &CPUState::r10, &CONTEXT::R10},
    {"r11", &CPUState::r11, &CONTEXT::R11},
    {"r12", &CPUState::r12, &CONTEXT::R12},
    {"r13", &CPUState::r13, &CONTEXT::R13},
    {"r14", &CPUState::r14, &CONTEXT::R14},
    {"r15", &CPUState::r15, &CONTEXT::R15},
    {"rip", &CPUState::rip, &CONTEXT::Rip},
}};
// Explicit named members avoid trusting the production pointer-arithmetic layout.
constexpr std::array<M128A CONTEXT::*, 16> xmm_members{
    &CONTEXT::Xmm0, &CONTEXT::Xmm1, &CONTEXT::Xmm2, &CONTEXT::Xmm3,
    &CONTEXT::Xmm4, &CONTEXT::Xmm5, &CONTEXT::Xmm6, &CONTEXT::Xmm7,
    &CONTEXT::Xmm8, &CONTEXT::Xmm9, &CONTEXT::Xmm10, &CONTEXT::Xmm11,
    &CONTEXT::Xmm12, &CONTEXT::Xmm13, &CONTEXT::Xmm14, &CONTEXT::Xmm15};

void CheckXmm(bool condition, const char* direction, std::size_t index, const char* half) {
    char field[32]{};
    std::snprintf(field, sizeof(field), "xmm%zu.%s", index, half);
    Check(condition, direction, field);
}

void FromContextCharacterization() {
    CONTEXT source;
    std::memset(&source, 0xa5, sizeof(source));
    for (std::size_t i = 0; i < mappings.size(); ++i) {
        source.*(mappings[i].host) = 0x8001020304050607ULL + i * 0x0101010101010101ULL;
    }
    for (std::size_t i = 0; i < xmm_members.size(); ++i) {
        (source.*xmm_members[i]).Low = 0x1021324354657687ULL + i;
        (source.*xmm_members[i]).High = static_cast<LONGLONG>(0xfedcba9876543210ULL - i);
    }
    source.EFlags = 0xdeadbeef;
    source.MxCsr = 0x1f80;
    source.FltSave.MxCsr = 0x3f80;
    std::array<unsigned char, sizeof(CONTEXT)> unchanged{};
    std::memcpy(unchanged.data(), &source, sizeof(source));

    CPUState result;
    result.FromContext(&source);
    for (std::size_t i = 0; i < mappings.size(); ++i) {
        const auto& field = mappings[i];
        Check(result.*(field.guest) == 0x8001020304050607ULL + i * 0x0101010101010101ULL,
              "FromContext", field.name);
    }
    for (std::size_t i = 0; i < xmm_members.size(); ++i) {
        CheckXmm(result.xmm[i].lo == 0x1021324354657687ULL + i, "FromContext", i, "lo");
        CheckXmm(result.xmm[i].hi == 0xfedcba9876543210ULL - i, "FromContext", i, "hi");
    }
    Check(result.rflags == 0x00000000deadbeefULL, "FromContext", "rflags zero-extended from DWORD");
    Check(result.mxcsr == 0x3f80, "FromContext", "mxcsr reads FltSave.MxCsr");
    Check(result.mxcsr != source.MxCsr, "FromContext",
          "LEGACY GAP: conflicting top-level MXCSR copy is ignored");
    Check(std::memcmp(&source, unchanged.data(), sizeof(source)) == 0,
          "FromContext", "source context unchanged byte-for-byte");
}

void ToContextCharacterization() {
    // Do not derive this state by FromContext: cancelling conversion mistakes
    // would otherwise allow a round-trip-only test to pass.
    CPUState source;
    for (std::size_t i = 0; i < mappings.size(); ++i) {
        source.*(mappings[i].guest) = 0x7654321089abcdefULL - i * 0x0101010101010101ULL;
    }
    for (std::size_t i = 0; i < xmm_members.size(); ++i) {
        source.xmm[i].lo = 0x8796a5b4c3d2e1f0ULL - i;
        source.xmm[i].hi = 0x8123456789abcdefULL + i;
    }
    source.rflags = 0x12345678cafebabeULL;
    source.mxcsr = 0x5f80;

    CONTEXT result;
    std::memset(&result, 0x5a, sizeof(result));
    result.MxCsr = 0x1f80;
    // Build expected bytes from the destination's original representation so
    // padding, segments, debug/x87/extended state, ContextFlags and other
    // unmodeled fields are required to remain untouched.
    CONTEXT expected;
    std::memcpy(&expected, &result, sizeof(expected));
    for (std::size_t i = 0; i < mappings.size(); ++i) {
        expected.*(mappings[i].host) = 0x7654321089abcdefULL - i * 0x0101010101010101ULL;
    }
    for (std::size_t i = 0; i < xmm_members.size(); ++i) {
        (expected.*xmm_members[i]).Low = 0x8796a5b4c3d2e1f0ULL - i;
        (expected.*xmm_members[i]).High = static_cast<LONGLONG>(0x8123456789abcdefULL + i);
    }
    expected.EFlags = 0xcafebabe;
    expected.FltSave.MxCsr = 0x5f80;

    source.ToContext(&result);
    for (std::size_t i = 0; i < mappings.size(); ++i) {
        const auto& field = mappings[i];
        Check(result.*(field.host) == 0x7654321089abcdefULL - i * 0x0101010101010101ULL,
              "ToContext", field.name);
    }
    for (std::size_t i = 0; i < xmm_members.size(); ++i) {
        CheckXmm((result.*xmm_members[i]).Low == 0x8796a5b4c3d2e1f0ULL - i,
                 "ToContext", i, "lo");
        CheckXmm(static_cast<u64>((result.*xmm_members[i]).High) == 0x8123456789abcdefULL + i,
                 "ToContext", i, "hi");
    }
    Check(result.EFlags == 0xcafebabe, "ToContext", "rflags truncated to DWORD");
    Check(result.FltSave.MxCsr == 0x5f80, "ToContext", "mxcsr writes FltSave.MxCsr");
    Check(result.MxCsr == 0x1f80 && result.MxCsr != result.FltSave.MxCsr,
          "ToContext", "LEGACY GAP: top-level MXCSR copy remains inconsistent");
    Check(std::memcmp(&result, &expected, sizeof(result)) == 0,
          "ToContext", "exact output including preservation of all unmodeled bytes");
}
}

int main() {
    FromContextCharacterization();
    ToContextCharacterization();
    std::printf("register context characterization: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
