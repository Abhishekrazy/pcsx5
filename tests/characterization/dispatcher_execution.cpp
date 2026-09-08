// Original synthetic characterization; tests current code, not PS5 accuracy.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>

extern "C" {
std::uint64_t observed[70]{};
std::uint64_t input_args[6]{0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
std::uint64_t saved_wrapper_rsp{};
std::uint64_t shadow_write{};
std::uint64_t xmm_input[16]{};
void RunDispatcherFixture(std::uint64_t mode, void* stack_top);
void RunHleFixture();
void CaptureStart();
}

namespace {
int failures{};
std::array<std::uint64_t, 16> incoming_xmm{};
void Check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
void CheckSavedRegisters(bool expected_equal) {
    bool equal = true;
    for (std::size_t i = 0; i != 6; ++i) equal &= observed[12+i] == observed[18+i];
    Check(equal == expected_equal, "six shared nonvolatile GPRs / shadow-slot observation");
}
}

extern "C" void FixtureSetIncomingXmmBlock(const void* block) {
    std::memcpy(incoming_xmm.data(), block, sizeof(incoming_xmm));
}
extern "C" void SetIncomingXmm0(std::uint64_t) {
    Check(false, "unexpected obsolete XMM callback");
}
extern "C" std::uint64_t HleDispatch(std::uint64_t symbol, std::uint64_t a1,
    std::uint64_t a2, std::uint64_t a3, std::uint64_t a4, std::uint64_t a5,
    std::uint64_t a6, std::uint64_t rip, std::uint64_t rsp) {
    Check(symbol == 0x77, "HLE symbol ID");
    const std::array args{a1, a2, a3, a4, a5, a6};
    for (std::size_t i = 0; i != args.size(); ++i)
        Check(args[i] == input_args[i], "HLE argument order");
    Check(rsp == observed[35], "HLE captured guest stack pointer");
    Check(rip == observed[36], "HLE captured guest return address");
    Check(*reinterpret_cast<const std::uint64_t*>(rsp) == rip, "HLE return slot");
    return 0x1234;
}

int main(int argc, char** argv) {
    alignas(16) std::array<std::byte, 64 * 1024> guest_stack{};
    auto* top = guest_stack.data() + guest_stack.size();
    for (std::uint64_t mode = 0; mode != 3; ++mode) {
        for (std::uint64_t writes = 0; writes != 2; ++writes) {
            std::memset(observed, 0, sizeof(observed));
            shadow_write = writes;
            RunDispatcherFixture(mode, top);
            for (std::size_t i = 0; i != 6; ++i) {
                const auto count = mode == 0 ? 3u : mode == 1 ? 6u : 1u;
                Check(observed[i] == (i < count ? input_args[i] : 0), "guest argument mapping");
            }
            Check((observed[6] & 15) == 8, "guest entry stack alignment");
            Check(observed[7] == 0x1234, "guest return value");
            Check(observed[10] != 0, "host stack callback called");
            Check(observed[11] == (mode == 2 ? 0u : 8u), "observed host callback alignment");
            if (mode == 2)
                Check(observed[6] == reinterpret_cast<std::uintptr_t>(top)-8, "alternate guest stack");
            // Deliberately pin legacy defects, never label these as desired ABI behavior.
            Check(observed[8] == 0xd1 && observed[9] == 0xd2, "KNOWN DEFECT: host RDI/RSI leaked");
            Check(observed[24] == 0, "KNOWN DEFECT: host XMM6 leaked");
            CheckSavedRegisters(!(mode == 2 && writes));
        }
    }
    std::memset(observed, 0, sizeof(observed));
    shadow_write = 0;
    RunDispatcherFixture(3, top);
    Check(observed[0] == reinterpret_cast<std::uintptr_t>(top), "StartGuest entry parameter pointer");
    Check(observed[1] != 0 && observed[34] == 1, "StartGuest atexit stub returns");
    Check(observed[6] == reinterpret_cast<std::uintptr_t>(top)-8, "StartGuest stack position");
    for (std::size_t i = 2; i != 6; ++i) Check(observed[i] == 0, "StartGuest cleared argument register");
    for (std::size_t i = 25; i != 33; ++i) Check(observed[i] == 0, "StartGuest cleared GPR");
    Check(observed[33] == reinterpret_cast<std::uintptr_t>(&CaptureStart), "StartGuest retains entry in RBX");
    Check(observed[11] == 0, "KNOWN DEFECT: StartGuest callback stack misaligned");

    std::memset(observed, 0, sizeof(observed));
    for (std::size_t i = 0; i != 16; ++i)
        xmm_input[i] = 0xaabbccdd00000000ULL + i;
    RunHleFixture();
    Check(observed[7] == 0x1234, "HLE integer return");
    CheckSavedRegisters(true);
    for (std::size_t i = 0; i != incoming_xmm.size(); ++i)
        Check(incoming_xmm[i] == xmm_input[i], "incoming XMM register and lane order");
    Check(observed[38] == 0x1234 && observed[39] == 0, "HLE XMM0 mirrors integer bits");
    for (std::size_t i = 40; i != 54; ++i)
        Check(observed[i] == xmm_input[i-38], "HLE XMM1-7 restored");

    if (failures) return 1;
    // Post-assertion semantic summary, not an instruction trace/replay oracle.
    // Never expose ASLR pointers or treat defects as requirements.
    if (argc == 2) {
        std::ofstream trace(argv[1], std::ios::binary);
        trace << R"({"schema_version":1,"fixture":"dispatcher_execution","events":[
{"sequence":0,"kind":"enter","subject":"invoke_guest","value":"guest-code+0x0000000000000000"},
{"sequence":1,"kind":"register","subject":"guest_rax","value":"0x0000000000001234"},
{"sequence":2,"kind":"check","subject":"legacy_host_nonvolatile_preservation","value":"fail"},
{"sequence":3,"kind":"check","subject":"legacy_on_stack_shadow_space","value":"fail"},
{"sequence":4,"kind":"check","subject":"legacy_start_callback_alignment","value":"fail"},
{"sequence":5,"kind":"check","subject":"hle_argument_roundtrip","value":"pass"},
{"sequence":6,"kind":"exit","subject":"synthetic_fixture","value":"pass"}]}
)";
        trace.close();
        if (!trace) { std::fprintf(stderr, "cannot write trace\n"); return 1; }
    }
    std::puts("PASS: dispatcher characterization (includes known legacy ABI defects; not production conformance)");
    return 0;
}
