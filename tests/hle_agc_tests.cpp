// Unit tests for the Phase 5 Milestone-0 AGC HLE: sceAgcCreateShader shader
// ABI (header validation, pointer relocation, PGM_LO/HI patch, handle
// contract), the Gen5 register-defaults blobs, the sceAgcDcb* PM4 builders,
// and the sceAgcDriverSubmitDcb packet-stream walker (register shadow state,
// draw/flip accounting).
//
// Build target: hle_agc_tests (see CMakeLists.txt).

#include "hle/hle.h"
#include "memory/memory.h"
#include "common/log.h"

#include <cstdio>
#include <cstring>
#include <string>

extern "C" u64 HleDispatch(u64, u64, u64, u64, u64, u64, u64, u64, u64);

namespace {

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

u64 SymbolId(const char* module, const char* name) {
    return ReadSymbolIdFromThunk(HLE::Resolve(module, name));
}

int g_failures = 0;

#define EXPECT(cond, msg)                                                        \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, msg); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

#define EXPECT_EQ(a, b, msg)                                                                   \
    do {                                                                                       \
        auto _lhs = (a);                                                                       \
        auto _rhs = (b);                                                                       \
        if (!(_lhs == _rhs)) {                                                                 \
            std::fprintf(stderr, "[FAIL] %s:%d: %s  (lhs=%lld rhs=%lld)\n",                    \
                         __FILE__, __LINE__, msg,                                              \
                         (long long)_lhs, (long long)_rhs);                                    \
            ++g_failures;                                                                      \
        }                                                                                      \
    } while (0)

// PM4 header encoding (must match src/hle/libagc.cpp).
u32 Pm4(u32 length_dwords, u32 op, u32 reg) {
    return 0xC0000000u | (((length_dwords - 2u) & 0x3FFFu) << 16) |
           ((op & 0xFFu) << 8) | ((reg & 0x3Fu) << 2);
}

guest_addr_t MapPages(u64 size) {
    guest_addr_t addr = 0;
    if (Memory::Map(0, size, Memory::PROT_READ | Memory::PROT_WRITE, &addr) != Memory::Status::Ok) {
        return 0;
    }
    return addr;
}

// Sets up a guest AGC command buffer: struct at `cb`, data area at cb+0x100,
// capacity up to cb+size.
void InitCommandBuffer(guest_addr_t cb, u64 size) {
    std::memset(reinterpret_cast<void*>(cb), 0, size);
    Memory::Write<u64>(cb + 0x10, cb + 0x100); // cursorUp
    Memory::Write<u64>(cb + 0x18, cb + size);  // cursorDown
    Memory::Write<u32>(cb + 0x30, 0);          // reserved dwords
}

// ---------------------------------------------------------------------------
// sceAgcCreateShader — relocation, PGM patch, return contract.
// ---------------------------------------------------------------------------
void TestCreateShader() {
    std::fprintf(stdout, "[TEST] sceAgcCreateShader\n");

    const u64 create_id = SymbolId("libSceAgc", "sceAgcCreateShader");
    const u64 create_nid = ReadSymbolIdFromThunk(HLE::ResolveAny("f3dg2CSgRKY"));
    EXPECT(create_id != 0, "sceAgcCreateShader resolves");
    EXPECT(create_nid != 0, "sceAgcCreateShader NID f3dg2CSgRKY resolves");

    guest_addr_t header = MapPages(0x1000);
    guest_addr_t code = MapPages(0x1000);
    EXPECT(header && code, "mapped shader header + code pages");
    std::memset(reinterpret_cast<void*>(header), 0, 0x1000);

    // Header: magic/version, Sh-register table at header+0x100 (rel ptr @0x20),
    // user-data block at header+0x180 (rel ptr @0x08).  Build is a lambda
    // because relocation is destructive: each dispatch needs a fresh header.
    const guest_addr_t sh_regs = header + 0x100;
    const guest_addr_t user_data = header + 0x180;
    const guest_addr_t dest = header + 0x200;
    auto build_header = [&]() {
        std::memset(reinterpret_cast<void*>(header), 0, 0x1000);
        Memory::Write<u32>(header + 0x00, 0x34333231);
        Memory::Write<u32>(header + 0x04, 0x18);
        Memory::Write<u64>(header + 0x08, 0x180 - 0x08); // userData rel
        Memory::Write<u64>(header + 0x20, 0x100 - 0x20); // shRegs rel
        Memory::Write<u32>(sh_regs + 0, 0x008);  // SPI_SHADER_PGM_LO_PS
        Memory::Write<u32>(sh_regs + 4, 0);
        Memory::Write<u32>(sh_regs + 8, 0x009);  // SPI_SHADER_PGM_HI_PS
        Memory::Write<u32>(sh_regs + 12, 0);
        Memory::Write<u64>(user_data, 0x10);     // rel pointer inside user data
        Memory::Write<u8>(header + 0x5A, 1);     // type = PS
        Memory::Write<u8>(header + 0x5C, 2);     // numShRegisters
    };
    build_header();

    // The NID form dispatches to the same implementation (same behavior).
    EXPECT_EQ(HleDispatch(create_nid, dest, header, code, 0, 0, 0, 0x7003, 0), (u64)header,
              "CreateShader via NID returns the header as the handle");

    build_header();
    const u64 ret = HleDispatch(create_id, dest, header, code, 0, 0, 0, 0x7000, 0);
    EXPECT_EQ(ret, (u64)header, "CreateShader returns the header as the handle");
    EXPECT_EQ(Memory::Read<u64>(dest), (u64)header, "CreateShader writes *dest = header");

    // Relocated pointers.
    EXPECT_EQ(Memory::Read<u64>(header + 0x20), (u64)sh_regs, "shRegs relocated");
    EXPECT_EQ(Memory::Read<u64>(header + 0x08), (u64)user_data, "userData relocated");
    EXPECT_EQ(Memory::Read<u64>(user_data), (u64)(user_data + 0x10), "userData[0] relocated");
    EXPECT_EQ(Memory::Read<u64>(header + 0x10), (u64)code, "code pointer stored @0x10");

    // PGM_LO/HI patched to the real code address (lo = code>>8, hi = code>>40).
    EXPECT_EQ(Memory::Read<u32>(sh_regs + 4), (u32)((code >> 8) & 0xFFFFFFFFull),
              "PGM_LO patched");
    EXPECT_EQ(Memory::Read<u32>(sh_regs + 12), (u32)((code >> 40) & 0xFFull),
              "PGM_HI patched");

    // Reject a bad magic.
    Memory::Write<u32>(header + 0x00, 0xDEADBEEF);
    EXPECT_EQ(HleDispatch(create_id, dest, header, code, 0, 0, 0, 0x7001, 0), (u64)0,
              "CreateShader(bad magic) -> 0");
    // Null header/code rejected.
    EXPECT_EQ(HleDispatch(create_id, dest, 0, code, 0, 0, 0, 0x7002, 0), (u64)0,
              "CreateShader(null header) -> 0");

    Memory::Unmap(header, 0x1000);
    Memory::Unmap(code, 0x1000);
}

// ---------------------------------------------------------------------------
// sceAgcGetRegisterDefaults2(/Internal) — real Gen5 tables, SDK blob layout.
// ---------------------------------------------------------------------------
void TestRegisterDefaults() {
    std::fprintf(stdout, "[TEST] sceAgcGetRegisterDefaults2 tables\n");

    const u64 defaults_id = SymbolId("libSceAgc", "sceAgcGetRegisterDefaults2");
    EXPECT(defaults_id != 0, "sceAgcGetRegisterDefaults2 resolves");

    const u64 blob = HleDispatch(defaults_id, 8, 0, 0, 0, 0, 0, 0x7100, 0);
    EXPECT(blob != 0, "defaults blob returned");
    EXPECT_EQ(Memory::Read<u32>(blob + 0x38), (u32)127, "primary group count == 127");
    const u64 cx_table = Memory::Read<u64>(blob + 0x00);
    const u64 sh_table = Memory::Read<u64>(blob + 0x08);
    const u64 uc_table = Memory::Read<u64>(blob + 0x10);
    const u64 types = Memory::Read<u64>(blob + 0x30);
    EXPECT(cx_table && sh_table && uc_table && types, "table pointers populated");

    // Group 0 == CB_COLOR_CONTROL {0x202, 0x00CC0010}.
    const u64 block0 = Memory::Read<u64>(cx_table + 0);
    EXPECT_EQ(Memory::Read<u32>(block0 + 0), (u32)0x202, "group0 reg offset CB_COLOR_CONTROL");
    EXPECT_EQ(Memory::Read<u32>(block0 + 4), (u32)0x00CC0010, "group0 value");

    // Group 40 == PA_SU_SC_MODE_CNTL {0x205, 0x240} (transcription-order sanity).
    const u64 block40 = Memory::Read<u64>(cx_table + 40 * 8);
    EXPECT_EQ(Memory::Read<u32>(block40 + 0), (u32)0x205, "group40 reg offset");
    EXPECT_EQ(Memory::Read<u32>(block40 + 4), (u32)0x240, "group40 value");

    // Sh group 18 == SPI_SHADER_PGM_LO/HI_PS.
    const u64 shblock18 = Memory::Read<u64>(sh_table + 18 * 8);
    EXPECT_EQ(Memory::Read<u32>(shblock18 + 0), (u32)0x008, "sh group18 reg0");
    EXPECT_EQ(Memory::Read<u32>(shblock18 + 8), (u32)0x009, "sh group18 reg1");

    // Internal table: 22 groups.
    const u64 internal_id = SymbolId("libSceAgc", "sceAgcGetRegisterDefaults2Internal");
    EXPECT(internal_id != 0, "sceAgcGetRegisterDefaults2Internal resolves");
    const u64 iblob = HleDispatch(internal_id, 8, 0, 0, 0, 0, 0, 0x7101, 0);
    EXPECT(iblob != 0, "internal defaults blob returned");
    EXPECT_EQ(Memory::Read<u32>(iblob + 0x38), (u32)22, "internal group count == 22");

    // The legacy NID aliases still resolve and dispatch to the same paths.
    const u64 legacy_id = ReadSymbolIdFromThunk(HLE::ResolveAny("2JtWUUiYBXs#A#B"));
    EXPECT(legacy_id != 0, "2JtWUUiYBXs#A#B resolves");
    EXPECT_EQ(HleDispatch(legacy_id, 8, 0, 0, 0, 0, 0, 0x7103, 0), blob,
              "2JtWUUiYBXs#A#B returns the same defaults blob");
    EXPECT_EQ(HleDispatch(ReadSymbolIdFromThunk(HLE::ResolveAny("23LRUSvYu1M#A#B")),
                          0x1000, 8, 0, 0, 0, 0, 0x7102, 0),
              (u64)0, "23LRUSvYu1M (sceAgcInit) -> 0");
}

// ---------------------------------------------------------------------------
// DCB builders — PM4 dword emission into the guest command buffer.
// ---------------------------------------------------------------------------
void TestDcbBuilders() {
    std::fprintf(stdout, "[TEST] sceAgcDcb* PM4 builders\n");

    guest_addr_t cb = MapPages(0x2000);
    EXPECT(cb != 0, "mapped command buffer");
    InitCommandBuffer(cb, 0x2000);
    const guest_addr_t data = cb + 0x100;

    // sceAgcDcbSetFlip: 6 dwords, IT_NOP/RFlip.
    const u64 flip_id = SymbolId("libSceAgc", "sceAgcDcbSetFlip");
    EXPECT(flip_id != 0, "sceAgcDcbSetFlip resolves");
    const u64 cmd = HleDispatch(flip_id, cb, 0x1234, 2, 1, 0xAABBCCDD, 0, 0x7200, 0);
    EXPECT_EQ(cmd, (u64)data, "SetFlip returns command address");
    EXPECT_EQ(Memory::Read<u32>(cmd + 0), (u32)Pm4(6, 0x10, 0x17), "SetFlip header");
    EXPECT_EQ(Memory::Read<u32>(cmd + 4), (u32)0x1234, "SetFlip handle");
    EXPECT_EQ(Memory::Read<u32>(cmd + 8), (u32)2, "SetFlip buffer index");
    EXPECT_EQ(Memory::Read<u32>(cmd + 12), (u32)1, "SetFlip mode");
    EXPECT_EQ(Memory::Read<u32>(cmd + 16), (u32)0xAABBCCDD, "SetFlip arg lo");
    EXPECT_EQ(Memory::Read<u32>(cmd + 20), (u32)0, "SetFlip arg hi");
    EXPECT_EQ(Memory::Read<u64>(cb + 0x10), (u64)(data + 24), "cursor advanced by 6 dwords");

    // sceAgcDcbSetShRegistersIndirect: 4 dwords, IT_NOP/RShRegsIndirect.
    const u64 shind_id = SymbolId("libSceAgc", "sceAgcDcbSetShRegistersIndirect");
    EXPECT(shind_id != 0, "sceAgcDcbSetShRegistersIndirect resolves");
    const u64 cmd2 = HleDispatch(shind_id, cb, 0xABCDE000, 3, 0, 0, 0, 0x7201, 0);
    EXPECT_EQ(cmd2, (u64)(data + 24), "SetShRegistersIndirect returns command address");
    EXPECT_EQ(Memory::Read<u32>(cmd2 + 0), (u32)Pm4(4, 0x10, 0x11), "ShIndirect header");
    EXPECT_EQ(Memory::Read<u32>(cmd2 + 4), (u32)3, "ShIndirect count");
    EXPECT_EQ(Memory::Read<u64>(cmd2 + 8), (u64)0xABCDE000, "ShIndirect address");

    // sceAgcDcbDrawIndexAuto: 7 dwords, IT_NOP/RDrawIndexAuto.
    const u64 drawauto_id = SymbolId("libSceAgc", "sceAgcDcbDrawIndexAuto");
    EXPECT(drawauto_id != 0, "sceAgcDcbDrawIndexAuto resolves");
    const u64 cmd3 = HleDispatch(drawauto_id, cb, 99, 0x40000000ull, 0, 0, 0, 0x7202, 0);
    EXPECT(cmd3 != 0, "DrawIndexAuto returns command address");
    EXPECT_EQ(Memory::Read<u32>(cmd3 + 0), (u32)Pm4(7, 0x10, 0x04), "DrawIndexAuto header");
    EXPECT_EQ(Memory::Read<u32>(cmd3 + 4), (u32)99, "DrawIndexAuto count");

    // Modifier validation: wrong modifier is rejected.
    EXPECT_EQ(HleDispatch(drawauto_id, cb, 99, 0, 0, 0, 0, 0x7203, 0), (u64)0,
              "DrawIndexAuto(bad modifier) -> 0");

    // sceAgcDcbDrawIndex: INDEX_BASE/SIZE (5 dwords) + DRAW_INDEX_2 (6 dwords).
    const u64 draw_id = SymbolId("libSceAgc", "sceAgcDcbDrawIndex");
    EXPECT(draw_id != 0, "sceAgcDcbDrawIndex resolves");
    const u64 cmd4 = HleDispatch(draw_id, cb, 36, 0xBEEF0000, 0x40000000ull, 0, 0, 0x7204, 0);
    EXPECT(cmd4 != 0, "DrawIndex returns command address");
    const u64 base_cmd = cmd4 - 20;
    EXPECT_EQ(Memory::Read<u32>(base_cmd + 0), (u32)Pm4(3, 0x26, 0), "IndexBase header");
    EXPECT_EQ(Memory::Read<u32>(base_cmd + 12), (u32)Pm4(2, 0x13, 0), "IndexBufferSize header");
    EXPECT_EQ(Memory::Read<u32>(cmd4 + 0), (u32)Pm4(6, 0x27, 0), "DrawIndex2 header");
    EXPECT_EQ(Memory::Read<u32>(cmd4 + 4), (u32)36, "DrawIndex2 max count");
    EXPECT_EQ(Memory::Read<u32>(cmd4 + 16), (u32)36, "DrawIndex2 count");

    // sceAgcCbSetShRegistersDirect: sorts and coalesces contiguous registers.
    const guest_addr_t regs = cb + 0x800;
    Memory::Write<u32>(regs + 0, 0x101); Memory::Write<u32>(regs + 4, 0xBBBB);
    Memory::Write<u32>(regs + 8, 0x100); Memory::Write<u32>(regs + 12, 0xAAAA);
    Memory::Write<u32>(regs + 16, 0x200); Memory::Write<u32>(regs + 20, 0xCCCC);
    const u64 shdir_id = SymbolId("libSceAgc", "sceAgcCbSetShRegistersDirect");
    EXPECT(shdir_id != 0, "sceAgcCbSetShRegistersDirect resolves");
    const u64 cmd5 = HleDispatch(shdir_id, cb, regs, 3, 0, 0, 0, 0x7205, 0);
    EXPECT(cmd5 != 0, "SetShRegistersDirect returns command address");
    EXPECT_EQ(Memory::Read<u32>(cmd5 + 0), (u32)Pm4(4, 0x76, 0), "SetShReg header");
    EXPECT_EQ(Memory::Read<u32>(cmd5 + 4), (u32)0x100, "SetShReg start register");
    EXPECT_EQ(Memory::Read<u32>(cmd5 + 8), (u32)0xAAAA, "SetShReg value[0]");
    EXPECT_EQ(Memory::Read<u32>(cmd5 + 12), (u32)0xBBBB, "SetShReg value[1]");
    // Second (non-contiguous) packet follows immediately.
    const u64 cmd6 = cmd5 + 16;
    EXPECT_EQ(Memory::Read<u32>(cmd6 + 0), (u32)Pm4(3, 0x76, 0), "SetShReg #2 header");
    EXPECT_EQ(Memory::Read<u32>(cmd6 + 4), (u32)0x200, "SetShReg #2 start register");
    EXPECT_EQ(Memory::Read<u32>(cmd6 + 8), (u32)0xCCCC, "SetShReg #2 value");

    Memory::Unmap(cb, 0x2000);
}

// ---------------------------------------------------------------------------
// sceAgcDriverSubmitDcb — packet walker: shadow state + draw/flip accounting.
// ---------------------------------------------------------------------------
void TestSubmitWalker() {
    std::fprintf(stdout, "[TEST] sceAgcDriverSubmitDcb walker\n");

    guest_addr_t cb = MapPages(0x2000);
    guest_addr_t pkt = MapPages(0x1000);
    EXPECT(cb && pkt, "mapped command buffer + submit packet");
    InitCommandBuffer(cb, 0x2000);
    const guest_addr_t data = cb + 0x100;

    // Build: SetShRegistersIndirect(2 entries) + DrawIndexAuto(33) + SetFlip.
    const guest_addr_t reg_table = cb + 0x800;
    Memory::Write<u32>(reg_table + 0, 0x123);
    Memory::Write<u32>(reg_table + 4, 0xDEADBEEF);
    Memory::Write<u32>(reg_table + 8, 0x008);
    Memory::Write<u32>(reg_table + 12, 0x00404040);

    HleDispatch(SymbolId("libSceAgc", "sceAgcDcbSetShRegistersIndirect"),
                cb, reg_table, 2, 0, 0, 0, 0x7300, 0);
    HleDispatch(SymbolId("libSceAgc", "sceAgcDcbDrawIndexAuto"),
                cb, 33, 0x40000000ull, 0, 0, 0, 0x7301, 0);
    HleDispatch(SymbolId("libSceAgc", "sceAgcDcbSetFlip"),
                cb, 0x7777, 0, 1, 55, 0, 0x7302, 0);

    const u32 dwords = static_cast<u32>((Memory::Read<u64>(cb + 0x10) - data) / 4);
    EXPECT(dwords == 4 + 7 + 6, "builder emitted expected dword count");

    const u64 draws_before = HLE::AgcGetSubmittedStats(0);
    const u64 flips_before = HLE::AgcGetSubmittedStats(2);

    Memory::Write<u64>(pkt + 0, data);
    Memory::Write<u32>(pkt + 8, dwords);
    const u64 submit_id = SymbolId("libSceAgcDriver", "sceAgcDriverSubmitDcb");
    EXPECT(submit_id != 0, "sceAgcDriverSubmitDcb resolves");
    EXPECT_EQ(HleDispatch(submit_id, pkt, 0, 0, 0, 0, 0, 0x7303, 0), (u64)0, "SubmitDcb -> 0");

    EXPECT_EQ(HLE::AgcGetSubmittedStats(0), draws_before + 1, "walker counted the draw");
    EXPECT_EQ(HLE::AgcGetSubmittedStats(2), flips_before + 1, "walker counted the flip");

    u32 value = 0;
    EXPECT(HLE::AgcGetShadowRegister(1, 0x123, &value), "sh shadow register present");
    EXPECT_EQ(value, (u32)0xDEADBEEF, "sh shadow register value");
    EXPECT(HLE::AgcGetShadowRegister(1, 0x008, &value), "sh shadow reg 0x008 present");
    EXPECT_EQ(value, (u32)0x00404040, "sh shadow reg 0x008 value");
    EXPECT(!HLE::AgcGetShadowRegister(0, 0x123, &value), "cx shadow untouched");

    // Multi-submit: two buffers, one counted draw total.
    guest_addr_t arrays = pkt + 0x100;
    Memory::Write<u64>(arrays + 0, data);            // buffer 0: full stream
    Memory::Write<u64>(arrays + 8, data + 4 * 4);    // buffer 1: draw+flip tail
    Memory::Write<u32>(arrays + 16, dwords);
    Memory::Write<u32>(arrays + 20, 7 + 6);
    const u64 multi_id = SymbolId("libSceAgcDriver", "sceAgcDriverSubmitMultiDcbs");
    EXPECT(multi_id != 0, "sceAgcDriverSubmitMultiDcbs resolves");
    const u64 draws_before2 = HLE::AgcGetSubmittedStats(0);
    EXPECT_EQ(HleDispatch(multi_id, arrays, arrays + 16, 2, 0, 0, 0, 0x7304, 0), (u64)0,
              "SubmitMultiDcbs -> 0");
    EXPECT_EQ(HLE::AgcGetSubmittedStats(0), draws_before2 + 2, "multi-submit counted both draws");

    Memory::Unmap(cb, 0x2000);
    Memory::Unmap(pkt, 0x1000);
}

// ---------------------------------------------------------------------------
// sceAgcCreatePrimState — fills caller cx/uc buffers from the specials struct.
// ---------------------------------------------------------------------------
void TestCreatePrimState() {
    std::fprintf(stdout, "[TEST] sceAgcCreatePrimState\n");

    const u64 prim_id = SymbolId("libSceAgc", "sceAgcCreatePrimState");
    const u64 prim_nid = ReadSymbolIdFromThunk(HLE::ResolveAny("D9sr1xGUriE"));
    EXPECT(prim_id != 0, "sceAgcCreatePrimState resolves");
    EXPECT(prim_nid != 0, "sceAgcCreatePrimState NID D9sr1xGUriE resolves");

    guest_addr_t page = MapPages(0x2000);
    EXPECT(page != 0, "mapped prim-state page");
    std::memset(reinterpret_cast<void*>(page), 0, 0x2000);
    const guest_addr_t geom     = page;         // shader header
    const guest_addr_t specials = page + 0x100; // specials struct
    const guest_addr_t cx       = page + 0x200; // 16 bytes
    const guest_addr_t uc       = page + 0x300; // 24 bytes

    Memory::Write<u8>(geom + 0x5A, 6);              // shader type: ES-geometry
    Memory::Write<u64>(geom + 0x28, specials);      // specials pointer
    Memory::Write<u64>(specials + 0x00, 0x1111111122222222ull); // GE_CNTL pair
    Memory::Write<u64>(specials + 0x08, 0x3333333344444444ull); // VGT_SHADER_STAGES_EN
    Memory::Write<u64>(specials + 0x20, 0x5555555566666666ull); // VGT_GS_OUT_PRIM_TYPE
    Memory::Write<u64>(specials + 0x28, 0x7777777788888888ull); // GE_USER_VGPR_EN

    // Happy path: returns 0 and copies the expected register pairs.
    EXPECT_EQ(HleDispatch(prim_id, cx, uc, 0, geom, 9, 0, 0x7400, 0), (u64)0,
              "CreatePrimState -> 0");
    EXPECT_EQ(Memory::Read<u64>(cx + 0), (u64)0x3333333344444444ull, "cx[0] = VGT_SHADER_STAGES_EN");
    EXPECT_EQ(Memory::Read<u64>(cx + 8), (u64)0x5555555566666666ull, "cx[1] = VGT_GS_OUT_PRIM_TYPE");
    EXPECT_EQ(Memory::Read<u64>(uc + 0), (u64)0x1111111122222222ull, "uc[0] = GE_CNTL");
    EXPECT_EQ(Memory::Read<u64>(uc + 8), (u64)0x7777777788888888ull, "uc[1] = GE_USER_VGPR_EN");
    EXPECT_EQ(Memory::Read<u32>(uc + 16), (u32)0x242, "uc[2].offset = VGT_PRIMITIVE_TYPE");
    EXPECT_EQ(Memory::Read<u32>(uc + 20), (u32)9, "uc[2].value = primitiveType");

    // The NID form dispatches to the same implementation.
    EXPECT_EQ(HleDispatch(prim_nid, cx, uc, 0, geom, 4, 0, 0x7401, 0), (u64)0,
              "CreatePrimState via NID -> 0");
    EXPECT_EQ(Memory::Read<u32>(uc + 20), (u32)4, "NID form wrote primitiveType");

    // Validation failures -> sign-extended 0x80020003.
    const u64 kInvalid = 0xFFFFFFFF80020003ull;
    EXPECT_EQ(HleDispatch(prim_id, 0, uc, 0, geom, 9, 0, 0x7402, 0), kInvalid,
              "CreatePrimState(null cx) -> INVALID_ARGUMENT");
    EXPECT_EQ(HleDispatch(prim_id, cx, 0, 0, geom, 9, 0, 0x7403, 0), kInvalid,
              "CreatePrimState(null uc) -> INVALID_ARGUMENT");
    EXPECT_EQ(HleDispatch(prim_id, cx, uc, geom, geom, 9, 0, 0x7404, 0), kInvalid,
              "CreatePrimState(hull != 0) -> INVALID_ARGUMENT");
    EXPECT_EQ(HleDispatch(prim_id, cx, uc, 0, 0, 9, 0, 0x7405, 0), kInvalid,
              "CreatePrimState(null geom) -> INVALID_ARGUMENT");
    Memory::Write<u8>(geom + 0x5A, 1); // not an ES-geometry type
    EXPECT_EQ(HleDispatch(prim_id, cx, uc, 0, geom, 9, 0, 0x7406, 0), kInvalid,
              "CreatePrimState(bad shader type) -> INVALID_ARGUMENT");
    Memory::Write<u8>(geom + 0x5A, 2);
    Memory::Write<u64>(geom + 0x28, 0); // no specials
    EXPECT_EQ(HleDispatch(prim_id, cx, uc, 0, geom, 9, 0, 0x7407, 0), kInvalid,
              "CreatePrimState(null specials) -> INVALID_ARGUMENT");

    Memory::Unmap(page, 0x2000);
}

// ---------------------------------------------------------------------------
// RegIndirectPatch family — in-place patch of indirect-registers packets.
// ---------------------------------------------------------------------------
void TestRegIndirectPatch() {
    std::fprintf(stdout, "[TEST] sceAgcSet*RegIndirectPatch*\n");

    guest_addr_t page = MapPages(0x1000);
    EXPECT(page != 0, "mapped patch page");
    std::memset(reinterpret_cast<void*>(page), 0, 0x1000);
    const guest_addr_t cmd = page;

    const u64 set_id = SymbolId("libSceAgc", "sceAgcSetShRegIndirectPatchSetAddress");
    const u64 add_id = SymbolId("libSceAgc", "sceAgcSetCxRegIndirectPatchAddRegisters");
    EXPECT(set_id != 0, "SetShRegIndirectPatchSetAddress resolves");
    EXPECT(add_id != 0, "SetCxRegIndirectPatchAddRegisters resolves");

    // SetAddress writes the 64-bit registers address at cmd+8/+12.
    EXPECT_EQ(HleDispatch(set_id, cmd, 0x1122334455667788ull, 0, 0, 0, 0, 0x7500, 0), (u64)0,
              "PatchSetAddress -> 0");
    EXPECT_EQ(Memory::Read<u32>(cmd + 8), (u32)0x55667788, "patched address lo");
    EXPECT_EQ(Memory::Read<u32>(cmd + 12), (u32)0x11223344, "patched address hi");

    // AddRegisters read-modify-writes the count at cmd+4.
    Memory::Write<u32>(cmd + 4, 3);
    EXPECT_EQ(HleDispatch(add_id, cmd, 5, 0, 0, 0, 0, 0x7501, 0), (u64)0,
              "PatchAddRegisters -> 0");
    EXPECT_EQ(Memory::Read<u32>(cmd + 4), (u32)8, "count incremented 3 -> 8");

    // Validation -> sign-extended 0x80020003.
    const u64 kInvalid = 0xFFFFFFFF80020003ull;
    EXPECT_EQ(HleDispatch(set_id, 0, 0x1000, 0, 0, 0, 0, 0x7502, 0), kInvalid,
              "PatchSetAddress(null cmd) -> INVALID_ARGUMENT");
    EXPECT_EQ(HleDispatch(set_id, cmd, 0, 0, 0, 0, 0, 0x7503, 0), kInvalid,
              "PatchSetAddress(null regs) -> INVALID_ARGUMENT");
    EXPECT_EQ(HleDispatch(add_id, 0, 5, 0, 0, 0, 0, 0x7504, 0), kInvalid,
              "PatchAddRegisters(null cmd) -> INVALID_ARGUMENT");

    Memory::Unmap(page, 0x1000);
}

// ---------------------------------------------------------------------------
// HV4j+E0MBHE — interpolant-mapping builder (unpublished export name;
// semantics RE'd by SharpEmu/Kyty: SPI_PS_INPUT_CNTL_0..31 register pairs).
// ---------------------------------------------------------------------------
void TestCreateInterpolantMappingNid() {
    std::fprintf(stdout, "[TEST] HV4j+E0MBHE interpolant mapping\n");

    const u64 map_id = ReadSymbolIdFromThunk(HLE::ResolveAny("HV4j+E0MBHE"));
    EXPECT(map_id != 0, "HV4j+E0MBHE resolves");

    guest_addr_t page = MapPages(0x2000);
    EXPECT(page != 0, "mapped interp page");
    std::memset(reinterpret_cast<void*>(page), 0, 0x2000);
    const guest_addr_t regs    = page + 0x400; // 32 * 8 bytes
    const guest_addr_t gs      = page;         // producing shader header
    const guest_addr_t ps      = page + 0x100; // pixel shader header
    const guest_addr_t out_sem = page + 0x800;
    const guest_addr_t in_sem  = page + 0x900;

    Memory::Write<u64>(gs + 0x38, out_sem);  // OutSemantics
    Memory::Write<u32>(gs + 0x56, 3);        // NumOut = 3
    Memory::Write<u64>(ps + 0x30, in_sem);   // InSemantics
    Memory::Write<u32>(ps + 0x50, 3);        // NumIn
    // Input semantics: index 1 has the flat-shading bit (bit 22) set.
    Memory::Write<u32>(in_sem + 0, 0);
    Memory::Write<u32>(in_sem + 4, 1u << 22);
    Memory::Write<u32>(in_sem + 8, 0);

    EXPECT_EQ(HleDispatch(map_id, regs, gs, ps, 0, 0, 0, 0x7600, 0), (u64)0,
              "interp mapping -> 0");
    for (u32 i = 0; i < 32; ++i) {
        EXPECT_EQ(Memory::Read<u32>(regs + i * 8), 0x191u + i, "register offset = SPI_PS_INPUT_CNTL_0+i");
    }
    EXPECT_EQ(Memory::Read<u32>(regs + 0 * 8 + 4), 0u, "entry 0: enabled, not flat");
    EXPECT_EQ(Memory::Read<u32>(regs + 1 * 8 + 4), 1u | 0x400u, "entry 1: flat-shaded");
    EXPECT_EQ(Memory::Read<u32>(regs + 2 * 8 + 4), 2u, "entry 2: enabled, not flat");
    EXPECT_EQ(Memory::Read<u32>(regs + 3 * 8 + 4), 0u, "entry 3+: beyond NumOut -> 0");

    // Null pixel shader is allowed.
    std::memset(reinterpret_cast<void*>(regs), 0, 32 * 8);
    EXPECT_EQ(HleDispatch(map_id, regs, gs, 0, 0, 0, 0, 0x7601, 0), (u64)0,
              "interp mapping (null ps) -> 0");
    EXPECT_EQ(Memory::Read<u32>(regs + 1 * 8 + 4), 1u, "null ps: no flat flag");

    // Validation -> sign-extended 0x80020003.
    const u64 kInvalid = 0xFFFFFFFF80020003ull;
    EXPECT_EQ(HleDispatch(map_id, 0, gs, ps, 0, 0, 0, 0x7602, 0), kInvalid,
              "interp mapping(null out) -> INVALID_ARGUMENT");
    EXPECT_EQ(HleDispatch(map_id, regs, 0, ps, 0, 0, 0, 0x7603, 0), kInvalid,
              "interp mapping(null gs) -> INVALID_ARGUMENT");

    Memory::Unmap(page, 0x2000);
}

// ---------------------------------------------------------------------------
// V++UgBtQhn0 — data-packet payload-address helper (unpublished export name).
// ---------------------------------------------------------------------------
void TestGetDataPacketPayloadAddressNid() {
    std::fprintf(stdout, "[TEST] V++UgBtQhn0 payload address\n");

    const u64 pay_id = ReadSymbolIdFromThunk(HLE::ResolveAny("V++UgBtQhn0"));
    EXPECT(pay_id != 0, "V++UgBtQhn0 resolves");

    guest_addr_t page = MapPages(0x1000);
    EXPECT(page != 0, "mapped payload page");
    std::memset(reinterpret_cast<void*>(page), 0, 0x1000);
    const guest_addr_t out = page;
    const guest_addr_t cmd = page + 0x100;

    // type != 0 -> payload = cmd + 8.
    EXPECT_EQ(HleDispatch(pay_id, out, cmd, 1, 0, 0, 0, 0x7700, 0), (u64)0,
              "payload (type 1) -> 0");
    EXPECT_EQ(Memory::Read<u64>(out), cmd + 8, "type 1 payload = cmd+8");

    // type == 0, normal header -> payload = cmd + 4.
    Memory::Write<u32>(cmd, 0xC0000000u);
    EXPECT_EQ(HleDispatch(pay_id, out, cmd, 0, 0, 0, 0, 0x7701, 0), (u64)0,
              "payload (type 0) -> 0");
    EXPECT_EQ(Memory::Read<u64>(out), cmd + 4, "type 0 payload = cmd+4");

    // type == 0, all-ones count field -> payload = NULL.
    Memory::Write<u32>(cmd, 0x3FFF0000u);
    EXPECT_EQ(HleDispatch(pay_id, out, cmd, 0, 0, 0, 0, 0x7702, 0), (u64)0,
              "payload (type 0, full) -> 0");
    EXPECT_EQ(Memory::Read<u64>(out), (u64)0, "all-ones count -> NULL payload");

    // Validation -> sign-extended 0x80020003.
    const u64 kInvalid = 0xFFFFFFFF80020003ull;
    EXPECT_EQ(HleDispatch(pay_id, 0, cmd, 1, 0, 0, 0, 0x7703, 0), kInvalid,
              "payload(null out) -> INVALID_ARGUMENT");
    EXPECT_EQ(HleDispatch(pay_id, out, 0, 1, 0, 0, 0, 0x7704, 0), kInvalid,
              "payload(null cmd) -> INVALID_ARGUMENT");

    Memory::Unmap(page, 0x1000);
}

// ---------------------------------------------------------------------------
// sceAgcCbSetShRegisterRangeDirect — marker + SET_SH_REG range packet.
// ---------------------------------------------------------------------------
void TestSetShRegisterRangeDirect() {
    std::fprintf(stdout, "[TEST] sceAgcCbSetShRegisterRangeDirect\n");

    guest_addr_t cb = MapPages(0x2000);
    EXPECT(cb != 0, "mapped command buffer");
    InitCommandBuffer(cb, 0x2000);
    const guest_addr_t data = cb + 0x100;

    const u64 range_id = SymbolId("libSceAgc", "sceAgcCbSetShRegisterRangeDirect");
    EXPECT(range_id != 0, "sceAgcCbSetShRegisterRangeDirect resolves");

    const guest_addr_t values = cb + 0x800;
    Memory::Write<u32>(values + 0, 0xAAAA);
    Memory::Write<u32>(values + 4, 0xBBBB);
    Memory::Write<u32>(values + 8, 0xCCCC);

    // Returns the second allocation (the SET_SH_REG packet); the 2-dword
    // marker sits immediately before it.
    const u64 cmd = HleDispatch(range_id, cb, 0x100, values, 3, 0, 0, 0x7600, 0);
    EXPECT_EQ(cmd, (u64)(data + 8), "returns packet address (after marker)");
    EXPECT_EQ(Memory::Read<u32>(data + 0), (u32)Pm4(2, 0x10, 0x00), "marker header");
    EXPECT_EQ(Memory::Read<u32>(data + 4), (u32)0x6875000D, "marker dword");
    EXPECT_EQ(Memory::Read<u32>(cmd + 0), (u32)Pm4(5, 0x76, 0), "SET_SH_REG header");
    EXPECT_EQ(Memory::Read<u32>(cmd + 4), (u32)0x100, "range start offset");
    EXPECT_EQ(Memory::Read<u32>(cmd + 8), (u32)0xAAAA, "value[0]");
    EXPECT_EQ(Memory::Read<u32>(cmd + 12), (u32)0xBBBB, "value[1]");
    EXPECT_EQ(Memory::Read<u32>(cmd + 16), (u32)0xCCCC, "value[2]");
    EXPECT_EQ(Memory::Read<u64>(cb + 0x10), (u64)(data + 28), "cursor advanced 2+5 dwords");

    // Null values pointer emits zeroed values.
    const u64 cmd2 = HleDispatch(range_id, cb, 0x200, 0, 2, 0, 0, 0x7601, 0);
    EXPECT(cmd2 != 0, "null values pointer still emits");
    EXPECT_EQ(Memory::Read<u32>(cmd2 + 8), (u32)0, "zeroed value[0]");
    EXPECT_EQ(Memory::Read<u32>(cmd2 + 12), (u32)0, "zeroed value[1]");

    // Validation failures -> 0.
    EXPECT_EQ(HleDispatch(range_id, cb, 0, values, 3, 0, 0, 0x7602, 0), (u64)0,
              "offset 0 -> 0");
    EXPECT_EQ(HleDispatch(range_id, cb, 0x400, values, 3, 0, 0, 0x7603, 0), (u64)0,
              "offset > 0x3FF -> 0");
    EXPECT_EQ(HleDispatch(range_id, cb, 0x100, values, 0, 0, 0, 0x7604, 0), (u64)0,
              "count 0 -> 0");

    // Buffer full -> 0 (games treat 0 as "buffer full").
    Memory::Write<u64>(cb + 0x18, Memory::Read<u64>(cb + 0x10)); // cursorDown = cursorUp
    EXPECT_EQ(HleDispatch(range_id, cb, 0x100, values, 3, 0, 0, 0x7605, 0), (u64)0,
              "buffer full -> 0");

    Memory::Unmap(cb, 0x2000);
}

// ---------------------------------------------------------------------------
// sceAgcDcbWaitRegMem — three packet shapes; reference/mask/pollCycles are
// SysV stack arguments (GuestArgs::stack_args = guest_rsp + 8).
// ---------------------------------------------------------------------------
void TestDcbWaitRegMem() {
    std::fprintf(stdout, "[TEST] sceAgcDcbWaitRegMem\n");

    guest_addr_t cb = MapPages(0x2000);
    guest_addr_t stack = MapPages(0x1000);
    EXPECT(cb && stack, "mapped command buffer + stack");
    InitCommandBuffer(cb, 0x2000);
    const guest_addr_t data = cb + 0x100;
    std::memset(reinterpret_cast<void*>(stack), 0, 0x1000);

    const u64 wait_id = SymbolId("libSceAgc", "sceAgcDcbWaitRegMem");
    EXPECT(wait_id != 0, "sceAgcDcbWaitRegMem resolves");

    // stack_args = rsp + 8: reference @+8, mask @+16, pollCycles @+24.
    const guest_addr_t rsp = stack;
    Memory::Write<u64>(rsp + 8, 0x1111111122222222ull);  // reference
    Memory::Write<u64>(rsp + 16, 0x3333333344444444ull); // mask
    Memory::Write<u32>(rsp + 24, 4000);                  // pollCycles

    const u64 addr = 0xAABBCCDD00112233ull;

    // 32-bit shape (size=0, operation not 2/3): 6 dwords, NOP/RWaitMem32.
    const u64 cmd = HleDispatch(wait_id, cb, 0, 3, 1, 0, addr, 0x7700, rsp);
    EXPECT_EQ(cmd, (u64)data, "WaitRegMem32 returns command address");
    EXPECT_EQ(Memory::Read<u32>(cmd + 0), (u32)Pm4(6, 0x10, 0x0A), "WaitMem32 header");
    EXPECT_EQ(Memory::Read<u32>(cmd + 4), (u32)0x00112233, "WaitMem32 addr lo");
    EXPECT_EQ(Memory::Read<u32>(cmd + 8), (u32)0xAABBCCDD, "WaitMem32 addr hi");
    EXPECT_EQ(Memory::Read<u32>(cmd + 12), (u32)0x44444444, "WaitMem32 mask lo (stack arg)");
    EXPECT_EQ(Memory::Read<u32>(cmd + 16), (u32)(3 | (1 << 8)), "WaitMem32 cmp|op");
    EXPECT_EQ(Memory::Read<u32>(cmd + 20), (u32)0x22222222, "WaitMem32 ref lo (stack arg)");

    // Standard shape (operation==2): 7 dwords, IT_WAIT_REG_MEM.
    const u64 cmd2 = HleDispatch(wait_id, cb, 0, 5, 2, 0, addr, 0x7701, rsp);
    EXPECT_EQ(cmd2, (u64)(data + 24), "WaitRegMem standard returns command address");
    EXPECT_EQ(Memory::Read<u32>(cmd2 + 0), (u32)Pm4(7, 0x3C, 0), "standard header");
    EXPECT_EQ(Memory::Read<u32>(cmd2 + 4), (u32)5, "standard cmp|(op&1)<<8");
    EXPECT_EQ(Memory::Read<u32>(cmd2 + 8), (u32)0x00112233, "standard addr lo");
    EXPECT_EQ(Memory::Read<u32>(cmd2 + 16), (u32)0x22222222, "standard ref lo");
    EXPECT_EQ(Memory::Read<u32>(cmd2 + 20), (u32)0x44444444, "standard mask lo");
    EXPECT_EQ(Memory::Read<u32>(cmd2 + 24), (u32)100, "standard pollCycles/40");

    // 64-bit shape (size=1): 9 dwords, NOP/RWaitMem64.
    const u64 cmd3 = HleDispatch(wait_id, cb, 1, 2, 0, 0, addr, 0x7702, rsp);
    EXPECT_EQ(cmd3, (u64)(data + 24 + 28), "WaitRegMem64 returns command address");
    EXPECT_EQ(Memory::Read<u32>(cmd3 + 0), (u32)Pm4(9, 0x10, 0x16), "WaitMem64 header");
    EXPECT_EQ(Memory::Read<u32>(cmd3 + 12), (u32)0x44444444, "WaitMem64 mask lo");
    EXPECT_EQ(Memory::Read<u32>(cmd3 + 16), (u32)0x33333333, "WaitMem64 mask hi");
    EXPECT_EQ(Memory::Read<u32>(cmd3 + 20), (u32)0x22222222, "WaitMem64 ref lo");
    EXPECT_EQ(Memory::Read<u32>(cmd3 + 24), (u32)0x11111111, "WaitMem64 ref hi");
    EXPECT_EQ(Memory::Read<u32>(cmd3 + 28), (u32)2, "WaitMem64 cmp|op");
    EXPECT_EQ(Memory::Read<u32>(cmd3 + 32), (u32)100, "WaitMem64 pollCycles/40");

    // Range validation -> 0.
    EXPECT_EQ(HleDispatch(wait_id, cb, 2, 3, 1, 0, addr, 0x7703, rsp), (u64)0,
              "size > 1 -> 0");
    EXPECT_EQ(HleDispatch(wait_id, cb, 0, 8, 1, 0, addr, 0x7704, rsp), (u64)0,
              "compareFunction > 7 -> 0");
    EXPECT_EQ(HleDispatch(wait_id, cb, 0, 3, 5, 0, addr, 0x7705, rsp), (u64)0,
              "operation > 4 -> 0");

    Memory::Unmap(cb, 0x2000);
    Memory::Unmap(stack, 0x1000);
}

// ---------------------------------------------------------------------------
// sceAgcDcbDmaData (+GetSize) — 8-dword NOP/RDmaData with six stack args.
// ---------------------------------------------------------------------------
void TestDcbDmaData() {
    std::fprintf(stdout, "[TEST] sceAgcDcbDmaData\n");

    guest_addr_t cb = MapPages(0x2000);
    guest_addr_t stack = MapPages(0x1000);
    EXPECT(cb && stack, "mapped command buffer + stack");
    InitCommandBuffer(cb, 0x2000);
    const guest_addr_t data = cb + 0x100;
    std::memset(reinterpret_cast<void*>(stack), 0, 0x1000);

    const u64 dma_id = SymbolId("libSceAgc", "sceAgcDcbDmaData");
    const u64 size_id = SymbolId("libSceAgc", "sceAgcDcbDmaDataGetSize");
    EXPECT(dma_id != 0, "sceAgcDcbDmaData resolves");
    EXPECT(size_id != 0, "sceAgcDcbDmaDataGetSize resolves");
    EXPECT_EQ(HleDispatch(size_id, 0, 0, 0, 0, 0, 0, 0x7800, 0), (u64)32,
              "DmaDataGetSize -> 32");

    // stack_args = rsp + 8: control4 @+8, sourceAddress @+16, byteCount @+24,
    // control7 @+32, control8 @+40, control9 @+48.
    const guest_addr_t rsp = stack;
    Memory::Write<u64>(rsp + 8, 0xC4);
    Memory::Write<u64>(rsp + 16, 0x5555666677778888ull); // sourceAddress
    Memory::Write<u32>(rsp + 24, 0x1000);                // byteCount
    Memory::Write<u64>(rsp + 32, 0xC7);
    Memory::Write<u64>(rsp + 40, 0xC8);
    Memory::Write<u64>(rsp + 48, 0xC9);

    const u64 dst_addr = 0x1111222233334444ull;
    const u64 cmd = HleDispatch(dma_id, cb, 1, 2, 3, dst_addr, 4, 0x7801, rsp);
    EXPECT_EQ(cmd, (u64)data, "DmaData returns command address");
    EXPECT_EQ(Memory::Read<u32>(cmd + 0), (u32)Pm4(8, 0x10, 0x19), "DmaData header");
    EXPECT_EQ(Memory::Read<u32>(cmd + 4), (u32)(1 | (2 << 8) | (3 << 16) | (4u << 24)),
              "DmaData dst/src selection");
    EXPECT_EQ(Memory::Read<u32>(cmd + 8), (u32)(0xC4 | (0xC7 << 8) | (0xC8 << 16) | (0xC9u << 24)),
              "DmaData control bytes (stack args)");
    EXPECT_EQ(Memory::Read<u32>(cmd + 12), (u32)0x1000, "DmaData byteCount (stack arg)");
    EXPECT_EQ(Memory::Read<u64>(cmd + 16), dst_addr, "DmaData destination address");
    EXPECT_EQ(Memory::Read<u64>(cmd + 24), (u64)0x5555666677778888ull,
              "DmaData source address (stack arg)");

    // byteCount validation -> 0.
    Memory::Write<u32>(rsp + 24, 0);
    EXPECT_EQ(HleDispatch(dma_id, cb, 1, 2, 3, dst_addr, 4, 0x7803, rsp), (u64)0,
              "byteCount 0 -> 0");
    Memory::Write<u32>(rsp + 24, 6);
    EXPECT_EQ(HleDispatch(dma_id, cb, 1, 2, 3, dst_addr, 4, 0x7804, rsp), (u64)0,
              "byteCount not dword-aligned -> 0");

    Memory::Unmap(cb, 0x2000);
    Memory::Unmap(stack, 0x1000);
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

    TestCreateShader();
    TestRegisterDefaults();
    TestDcbBuilders();
    TestSubmitWalker();
    TestCreatePrimState();
    TestCreateInterpolantMappingNid();
    TestGetDataPacketPayloadAddressNid();
    TestRegIndirectPatch();
    TestSetShRegisterRangeDirect();
    TestDcbWaitRegMem();
    TestDcbDmaData();

    HLE::Shutdown();
    Memory::Shutdown();

    if (g_failures == 0) {
        std::fprintf(stdout, "HLE AGC (Phase 5 M0) tests passed.\n");
        return 0;
    }
    std::fprintf(stderr, "HLE AGC (Phase 5 M0) tests: %d failure(s).\n", g_failures);
    return 1;
}
