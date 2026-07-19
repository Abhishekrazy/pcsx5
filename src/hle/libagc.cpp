// libSceAgc / libSceAgcDriver HLE — Phase 5 Milestone 0.
//
// Ported semantics from SharpEmu (sharpemu_clone/src/SharpEmu.Libs/Agc/):
//   - sceAgcCreateShader: header validation (magic 0x34333231, version 0x18),
//     embedded-pointer relocation, SPI/COMPUTE_PGM_LO/HI patching against the
//     real code address, shader handle written to *dest and returned.
//   - sceAgcGetRegisterDefaults2(/Internal): real Gen5 primary/internal
//     register-default tables (Kyty-derived values via SharpEmu's
//     AgcPrimaryRegisterDefaults.cs), served from a persistent guest blob
//     with the SDK layout (cx/sh/uc table pointers @0/8/0x10, types @0x30,
//     group count @0x38).
//   - sceAgcDcb*/sceAgcCb* command-buffer builders emitting PM4 packets into
//     the guest-provided command buffer (cursor @0x10/0x18, reserved @0x30).
//   - sceAgcDriverSubmitDcb/SubmitMultiDcbs/SubmitAcb: PM4 packet-stream
//     walker maintaining Cx/Sh/Uc register shadow state; draw/dispatch/flip
//     packets are logged at Info; RFlip packets are forwarded to the
//     libSceVideoOut flip path (VideoOutSubmitFlipFromAgc). No rendering yet.
#include "hle.h"
#include "../common/log.h"
#include "../memory/memory.h"
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace HLE {

namespace {

// ---------------------------------------------------------------------------
// PM4 packet encoding (SharpEmu AgcExports.cs Pm4/Pm4Length).
// ---------------------------------------------------------------------------
constexpr u32 kItNop               = 0x10;
constexpr u32 kItSetBase           = 0x11;
constexpr u32 kItIndexBufferSize   = 0x13;
constexpr u32 kItDispatchDirect    = 0x15;
constexpr u32 kItDispatchIndirect  = 0x16;
constexpr u32 kItDrawIndirect      = 0x24;
constexpr u32 kItDrawIndexIndirect = 0x25;
constexpr u32 kItIndexBase         = 0x26;
constexpr u32 kItDrawIndex2        = 0x27;
constexpr u32 kItIndexType         = 0x2A;
constexpr u32 kItDrawIndexAuto     = 0x2D;
constexpr u32 kItNumInstances      = 0x2F;
constexpr u32 kItDrawIndexMultiAuto= 0x30;
constexpr u32 kItDrawIndexOffset2  = 0x35;
constexpr u32 kItWriteData         = 0x37;
constexpr u32 kItWaitRegMem        = 0x3C;
constexpr u32 kItEventWrite        = 0x46;
constexpr u32 kItReleaseMem        = 0x49;
constexpr u32 kItDmaData           = 0x50;
constexpr u32 kItSetContextReg     = 0x69;
constexpr u32 kItSetShReg          = 0x76;
constexpr u32 kItSetUconfigReg     = 0x79;

// AGC sub-opcodes carried in the register field of IT_NOP packets.
constexpr u32 kRZero            = 0x00;
constexpr u32 kRDrawIndexAuto   = 0x04;
constexpr u32 kRDrawReset       = 0x05;
constexpr u32 kRWaitFlipDone    = 0x06;
constexpr u32 kRAcbReset        = 0x09;
constexpr u32 kRWaitMem32       = 0x0A;
constexpr u32 kRPushMarker      = 0x0B;
constexpr u32 kRPopMarker       = 0x0C;
constexpr u32 kRShRegsIndirect  = 0x11;
constexpr u32 kRCxRegsIndirect  = 0x12;
constexpr u32 kRUcRegsIndirect  = 0x13;
constexpr u32 kRAcquireMem      = 0x14;
constexpr u32 kRWriteData       = 0x15;
constexpr u32 kRWaitMem64       = 0x16;
constexpr u32 kRIndexCount      = 0x1C;
constexpr u32 kRFlip            = 0x17;
constexpr u32 kRReleaseMem      = 0x18;
constexpr u32 kRDmaData         = 0x19;

// Gen2 result codes (SharpEmu OrbisGen2Result.cs), sign-extended into RAX.
constexpr s32 kAgcErrorInvalidArgument = static_cast<s32>(0x80020003);
constexpr s32 kAgcErrorMemoryFault     = static_cast<s32>(0x80020101);

u32 Pm4(u32 length_dwords, u32 op, u32 reg) {
    return 0xC0000000u | (((length_dwords - 2u) & 0x3FFFu) << 16) |
           ((op & 0xFFu) << 8) | ((reg & 0x3Fu) << 2);
}
u32 Pm4Length(u32 header) { return ((header >> 16) & 0x3FFFu) + 2u; }

// ---------------------------------------------------------------------------
// Shader header layout (AgcExports.cs Shader*Offset constants).
// ---------------------------------------------------------------------------
constexpr u32 kShaderFileMagic   = 0x34333231;
constexpr u32 kShaderVersion     = 0x18;
constexpr u64 kShUserDataOff     = 0x08;
constexpr u64 kShCodeOff         = 0x10;
constexpr u64 kShCxRegsOff       = 0x18;
constexpr u64 kShShRegsOff       = 0x20;
constexpr u64 kShSpecialsOff     = 0x28;
constexpr u64 kShInSemanticsOff  = 0x30;
constexpr u64 kShOutSemanticsOff = 0x38;
constexpr u64 kShNumInOff        = 0x50;
constexpr u64 kShNumOutOff       = 0x56;
constexpr u64 kShTypeOff         = 0x5A;
constexpr u64 kShNumShRegsOff    = 0x5C;

// Shader-specials struct field offsets (AgcExports.cs ShaderSpecial*Offset).
constexpr u64 kSpecialGeCntlOff             = 0x00; // GE_CNTL
constexpr u64 kSpecialVgtShaderStagesEnOff  = 0x08; // VGT_SHADER_STAGES_EN
constexpr u64 kSpecialVgtGsOutPrimTypeOff   = 0x20; // VGT_GS_OUT_PRIM_TYPE
constexpr u64 kSpecialGeUserVgprEnOff       = 0x28; // GE_USER_VGPR_EN

// VGT_PRIMITIVE_TYPE uconfig register written by sceAgcCreatePrimState.
constexpr u32 kVgtPrimitiveType = 0x242;

// SPI_PS_INPUT_CNTL_0 context register base (AgcExports.cs SpiPsInputCntl0);
// the interpolant-mapping builder emits 32 consecutive registers from here.
constexpr u32 kSpiPsInputCntl0 = 0x191;

// Marker dword emitted ahead of sceAgcCbSetShRegisterRangeDirect packets.
constexpr u32 kCbSetShRegisterRangeMarker = 0x6875000D;

// PGM_LO/HI register offsets per shader type (AgcExports.cs ~:79-97).
constexpr u32 kSpiShaderPgmLoPs = 0x008, kSpiShaderPgmHiPs = 0x009;
constexpr u32 kSpiShaderPgmLoEs = 0x0C8, kSpiShaderPgmHiEs = 0x0C9;
constexpr u32 kSpiShaderPgmLoLs = 0x148, kSpiShaderPgmHiLs = 0x149;
constexpr u32 kSpiShaderPgmLoGs = 0x08A, kSpiShaderPgmHiGs = 0x08B;
constexpr u32 kComputePgmLo     = 0x20C, kComputePgmHi     = 0x20D;

// Guest command-buffer struct layout (AgcExports.cs CommandBuffer*Offset).
constexpr u64 kCbCursorUpOff   = 0x10;
constexpr u64 kCbCursorDownOff = 0x18;
constexpr u64 kCbReservedDwOff = 0x30;

// code address -> shader header, for future milestones (M1 shader decode).
std::unordered_map<u64, u64> g_agc_shaders_by_code;

// ---------------------------------------------------------------------------
// Gen5 register-default tables (AgcPrimaryRegisterDefaults.cs — exact Kyty
// values; hashes dropped, they are metadata only).
// ---------------------------------------------------------------------------
struct AgcRegDefault { u32 offset; u32 value; };
struct AgcRegDefaultGroup { u32 space; u32 index; std::vector<AgcRegDefault> regs; };

const std::vector<AgcRegDefaultGroup>& PrimaryRegisterDefaults() {
    static const std::vector<AgcRegDefaultGroup> kGroups = {
        // context register groups
        {0u, 0u, {{0x202u, 0x00CC0010u}}}, // CB_COLOR_CONTROL
        {0u, 1u, {{0x109u, 0x00000000u}}}, // CB_DCC_CONTROL
        {0u, 2u, {{0x104u, 0x00000000u}}}, // CB_RMI_GL2_CACHE_CONTROL
        {0u, 3u, {{0x08Fu, 0x00000000u}}}, // CB_SHADER_MASK
        {0u, 4u, {{0x08Eu, 0x0000000Fu}}}, // CB_TARGET_MASK
        {0u, 5u, {{0x2DCu, 0x0000AA00u}}}, // DB_ALPHA_TO_MASK
        {0u, 6u, {{0x001u, 0x00000000u}}}, // DB_COUNT_CONTROL
        {0u, 7u, {{0x200u, 0x00000000u}}}, // DB_DEPTH_CONTROL
        {0u, 8u, {{0x201u, 0x00000000u}}}, // DB_EQAA
        {0u, 9u, {{0x000u, 0x00000000u}}}, // DB_RENDER_CONTROL
        {0u, 10u, {{0x006u, 0x00000000u}}}, // PS_SHADER_SAMPLE_EXCLUSION_MASK
        {0u, 11u, {{0x01Fu, 0x00000000u}}}, // DB_RMI_L2_CACHE_CONTROL
        {0u, 12u, {{0x203u, 0x00000000u}}}, // DB_SHADER_CONTROL
        {0u, 13u, {{0x2B0u, 0x00000000u}}}, // DB_SRESULTS_COMPARE_STATE0
        {0u, 14u, {{0x2B1u, 0x00000000u}}}, // DB_SRESULTS_COMPARE_STATE1
        {0u, 15u, {{0x10Cu, 0x00000000u}}}, // DB_STENCILREFMASK
        {0u, 16u, {{0x10Du, 0x00000000u}}}, // DB_STENCILREFMASK_BF
        {0u, 17u, {{0x10Bu, 0x00000000u}}}, // DB_STENCIL_CONTROL
        {0u, 18u, {{0x1FFu, 0x00000000u}}}, // GE_MAX_OUTPUT_PER_SUBGROUP
        {0u, 19u, {{0x204u, 0x00000000u}}}, // PA_CL_CLIP_CNTL
        {0u, 20u, {{0x20Du, 0x00000000u}}}, // PA_CL_OBJPRIM_ID_CNTL
        {0u, 21u, {{0x206u, 0x0000043Fu}}}, // PA_CL_VTE_CNTL
        {0u, 22u, {{0x2F8u, 0x00000000u}}}, // PA_SC_AA_CONFIG
        {0u, 23u, {{0x083u, 0x0000FFFFu}}}, // PA_SC_CLIPRECT_RULE
        {0u, 24u, {{0x313u, 0x00000000u}}}, // PA_SC_CONSERVATIVE_RASTERIZATION_CNTL
        {0u, 25u, {{0x800003FEu, 0x00000000u}}}, // PA_SC_FSR_ENABLE
        {0u, 26u, {{0x0EAu, 0x00000000u}}}, // PA_SC_HORIZ_GRID
        {0u, 27u, {{0x0E9u, 0x00000000u}}}, // PA_SC_LEFT_VERT_GRID
        {0u, 28u, {{0x292u, 0x00000002u}}}, // PA_SC_MODE_CNTL_0
        {0u, 29u, {{0x293u, 0x00000000u}}}, // PA_SC_MODE_CNTL_1
        {0u, 30u, {{0x0E8u, 0x00000000u}}}, // PA_SC_RIGHT_VERT_GRID
        {0u, 31u, {{0x080u, 0x00000000u}}}, // PA_SC_WINDOW_OFFSET
        {0u, 32u, {{0x211u, 0x00000000u}}}, // PA_STATE_STEREO_X
        {0u, 33u, {{0x210u, 0x00000000u}}}, // PA_STEREO_CNTL
        {0u, 34u, {{0x08Du, 0x00000000u}}}, // PA_SU_HARDWARE_SCREEN_OFFSET
        {0u, 35u, {{0x282u, 0x00000008u}}}, // PA_SU_LINE_CNTL
        {0u, 36u, {{0x281u, 0xFFFF0000u}}}, // PA_SU_POINT_MINMAX
        {0u, 37u, {{0x280u, 0x00080008u}}}, // PA_SU_POINT_SIZE
        {0u, 38u, {{0x2DFu, 0x00000000u}}}, // PA_SU_POLY_OFFSET_CLAMP
        {0u, 39u, {{0x2DEu, 0x000001E9u}}}, // PA_SU_POLY_OFFSET_DB_FMT_CNTL
        {0u, 40u, {{0x205u, 0x00000240u}}}, // PA_SU_SC_MODE_CNTL
        {0u, 41u, {{0x20Cu, 0x00000001u}}}, // PA_SU_SMALL_PRIM_FILTER_CNTL
        {0u, 42u, {{0x2F9u, 0x0000002Du}}}, // PA_SU_VTX_CNTL
        {0u, 43u, {{0x1BAu, 0x00000000u}}}, // SPI_TMPRING_SIZE
        {0u, 44u, {{0x2A6u, 0x00000000u}}}, // VGT_DRAW_PAYLOAD_CNTL
        {0u, 45u, {{0x2CEu, 0x00000400u}}}, // VGT_GS_MAX_VERT_OUT
        {0u, 46u, {{0x29Bu, 0x00000002u}}}, // VGT_GS_OUT_PRIM_TYPE
        {0u, 47u, {{0x2D6u, 0x00000000u}}}, // VGT_LS_HS_CONFIG
        {0u, 48u, {{0x2A3u, 0xFFFFFFFFu}}}, // VGT_PRIMITIVEID_RESET
        {0u, 49u, {{0x2A1u, 0x00000000u}}}, // VGT_PRIMITIVEID_EN
        {0u, 50u, {{0x2ADu, 0x00000000u}}}, // VGT_REUSE_OFF
        {0u, 51u, {{0x2D5u, 0x00000000u}}}, // VGT_SHADER_STAGES_EN
        {0u, 52u, {{0x2D4u, 0x88101000u}}}, // VGT_TESS_DISTRIBUTION
        {0u, 53u, {{0x2DBu, 0x00000000u}}}, // VGT_TF_PARAM
        {0u, 54u, {{0x2F5u, 0x00000000u}, {0x2F6u, 0x00000000u}}}, // PA_SC_CENTROID_PRIORITY_0/1
        {0u, 55u, {{0x2FEu, 0x00000000u}}}, // PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y0_0
        {0u, 56u, {{0x30Eu, 0xFFFFFFFFu}, {0x30Fu, 0xFFFFFFFFu}}}, // PA_SC_AA_MASK_X0Y0_X1Y0 / X0Y1_X1Y1
        {0u, 57u, {{0x311u, 0x00000002u}, {0x312u, 0x03FF0080u}}}, // PA_SC_BINNER_CNTL_0/1
        {0u, 58u, {{0x105u, 0x00000000u}, {0x107u, 0x00000000u}, {0x106u, 0x00000000u}, {0x108u, 0x00000000u}}}, // CB_BLEND_RED/BLUE/GREEN/ALPHA
        {0u, 59u, {{0x1E0u, 0x20010001u}}}, // CB_BLEND0_CONTROL
        {0u, 60u, {{0x020u, 0x00000000u}, {0x021u, 0x00000000u}}}, // TA_BC_BASE_ADDR/HI
        {0u, 61u, {{0x084u, 0x00000000u}, {0x085u, 0x20002000u}}}, // PA_SC_CLIPRECT_0_TL/BR
        {0u, 62u, {{0x800003FFu, 0x00000000u}}}, // CX_NOP
        {0u, 63u, {{0x008u, 0x00000000u}, {0x009u, 0x00000000u}}}, // DB_DEPTH_BOUNDS_MIN/MAX
        {0u, 64u, {{0x010u, 0x80000000u}, {0x011u, 0x20000000u}, {0x012u, 0x00000000u}, {0x013u, 0x00000000u},
                   {0x014u, 0x00000000u}, {0x015u, 0x00000000u}, {0x01Au, 0x00000000u}, {0x01Bu, 0x00000000u},
                   {0x01Cu, 0x00000000u}, {0x01Du, 0x00000000u}, {0x01Eu, 0x00000000u}, {0x002u, 0x00000000u},
                   {0x005u, 0x00000000u}, {0x007u, 0x00000000u}, {0x00Bu, 0x00000000u}, {0x00Au, 0x00000000u}}}, // DB_Z_INFO..DB_STENCIL_CLEAR
        {0u, 65u, {{0x0EBu, 0xFF00FF00u}, {0x0ECu, 0x00000000u}}}, // PA_SC_FOV_WINDOW_LR/TB
        {0u, 66u, {{0x800003FCu, 0x00000000u}, {0x800003FDu, 0x00000000u}}}, // FSR_RECURSIONS0/1
        {0u, 67u, {{0x090u, 0x80000000u}, {0x091u, 0x40004000u}}}, // PA_SC_GENERIC_SCISSOR_TL/BR
        {0u, 68u, {{0x2FAu, 0x4E7E0000u}, {0x2FBu, 0x4E7E0000u}, {0x2FCu, 0x4E7E0000u}, {0x2FDu, 0x4E7E0000u}}}, // PA_CL_GB_*_ADJ
        {0u, 69u, {{0x2E2u, 0x00000000u}, {0x2E3u, 0x00000000u}}}, // PA_SU_POLY_OFFSET_BACK_*
        {0u, 70u, {{0x2E0u, 0x00000000u}, {0x2E1u, 0x00000000u}}}, // PA_SU_POLY_OFFSET_FRONT_*
        {0u, 71u, {{0x003u, 0x00000000u}, {0x004u, 0x00000000u}}}, // DB_RENDER_OVERRIDE/2
        {0u, 72u, {{0x318u, 0x00000000u}, {0x31Bu, 0x00000000u}, {0x31Cu, 0x00000000u}, {0x31Du, 0x00000000u},
                   {0x31Eu, 0x00000048u}, {0x31Fu, 0x00000000u}, {0x321u, 0x00000000u}, {0x323u, 0x00000000u},
                   {0x324u, 0x00000000u}, {0x325u, 0x00000000u}, {0x390u, 0x00000000u}, {0x398u, 0x00000000u},
                   {0x3A0u, 0x00000000u}, {0x3A8u, 0x00000000u}, {0x3B0u, 0x00000000u}, {0x3B8u, 0x0006C000u}}}, // CB_COLOR0_*
        {0u, 73u, {{0x00Cu, 0x00000000u}, {0x00Du, 0x40004000u}}}, // PA_SC_SCREEN_SCISSOR_TL/BR
        {0u, 74u, {{0x191u, 0x00000000u}}}, // SPI_PS_INPUT_CNTL_0
        {0u, 75u, {{0x16Fu, 0x00000000u}, {0x170u, 0x00000000u}, {0x171u, 0x00000000u}, {0x172u, 0x00000000u}}}, // PA_CL_UCP_0_*
        {0u, 76u, {{0x10Fu, 0x4E7E0000u}, {0x111u, 0x4E7E0000u}, {0x113u, 0x4E7E0000u}, {0x110u, 0x00000000u},
                   {0x112u, 0x00000000u}, {0x114u, 0x00000000u}, {0x094u, 0x80000000u}, {0x095u, 0x40004000u},
                   {0x0B4u, 0x00000000u}, {0x0B5u, 0x00000000u}}}, // PA_CL_VPORT_* / PA_SC_VPORT_*
        {0u, 77u, {{0x081u, 0x80000000u}, {0x082u, 0x40004000u}}}, // PA_SC_WINDOW_SCISSOR_TL/BR
        // shader register groups
        {1u, 0u, {{0x212u, 0x00000000u}}}, // COMPUTE_PGM_RSRC1
        {1u, 1u, {{0x213u, 0x00000000u}}}, // COMPUTE_PGM_RSRC2
        {1u, 2u, {{0x228u, 0x00000000u}}}, // COMPUTE_PGM_RSRC3
        {1u, 3u, {{0x215u, 0x00000000u}}}, // COMPUTE_RESOURCE_LIMITS
        {1u, 4u, {{0x218u, 0x00000000u}}}, // COMPUTE_TMPRING_SIZE
        {1u, 5u, {{0x08Au, 0x00000000u}}}, // SPI_SHADER_PGM_RSRC1_GS
        {1u, 6u, {{0x10Au, 0x00000000u}}}, // SPI_SHADER_PGM_RSRC1_HS
        {1u, 7u, {{0x00Au, 0x00000000u}}}, // SPI_SHADER_PGM_RSRC1_PS
        {1u, 8u, {{0x08Bu, 0x00000000u}}}, // SPI_SHADER_PGM_RSRC2_GS
        {1u, 9u, {{0x10Bu, 0x00000000u}}}, // SPI_SHADER_PGM_RSRC2_HS
        {1u, 10u, {{0x00Bu, 0x00000000u}}}, // SPI_SHADER_PGM_RSRC2_PS
        {1u, 11u, {{0x224u, 0x00000000u}}}, // COMPUTE_USER_ACCUM_0
        {1u, 12u, {{0x107u, 0x00000000u}, {0x087u, 0x00000000u}, {0x007u, 0x00000000u}}}, // SPI_SHADER_PGM_RSRC3_HS/GS/PS
        {1u, 13u, {{0x20Cu, 0x00000000u}, {0x20Du, 0x00000000u}}}, // COMPUTE_PGM_LO/HI
        {1u, 14u, {{0x0C8u, 0x00000000u}, {0x0C9u, 0x00000000u}}}, // SPI_SHADER_PGM_LO/HI_ES
        {1u, 15u, {{0x088u, 0x00000000u}, {0x089u, 0x00000000u}}}, // SPI_SHADER_PGM_LO/HI_GS
        {1u, 16u, {{0x108u, 0x00000000u}, {0x109u, 0x00000000u}}}, // SPI_SHADER_PGM_LO/HI_HS
        {1u, 17u, {{0x148u, 0x00000000u}, {0x149u, 0x00000000u}}}, // SPI_SHADER_PGM_LO/HI_LS
        {1u, 18u, {{0x008u, 0x00000000u}, {0x009u, 0x00000000u}}}, // SPI_SHADER_PGM_LO/HI_PS
        {1u, 19u, {{0x800002FFu, 0x00000000u}}}, // SH_NOP
        {1u, 20u, {{0x0B2u, 0x00000000u}}}, // SPI_SHADER_USER_ACCUM_ESGS_0
        {1u, 21u, {{0x132u, 0x00000000u}}}, // SPI_SHADER_USER_ACCUM_LSHS_0
        {1u, 22u, {{0x032u, 0x00000000u}}}, // SPI_SHADER_USER_ACCUM_PS_0
        {1u, 23u, {{0x082u, 0x00000000u}, {0x083u, 0x00000000u}}}, // SPI_SHADER_USER_DATA_ADDR_LO/HI_GS
        {1u, 24u, {{0x102u, 0x00000000u}, {0x103u, 0x00000000u}}}, // SPI_SHADER_USER_DATA_ADDR_LO/HI_HS
        {1u, 25u, {{0x240u, 0x00000000u}}}, // COMPUTE_USER_DATA_0
        {1u, 26u, {{0x08Cu, 0x00000000u}}}, // SPI_SHADER_USER_DATA_GS_0
        {1u, 27u, {{0x10Cu, 0x00000000u}}}, // SPI_SHADER_USER_DATA_HS_0
        {1u, 28u, {{0x00Cu, 0x00000000u}}}, // SPI_SHADER_USER_DATA_PS_0
        // uconfig register groups
        {2u, 0u, {{0x41Fu, 0x00000000u}}}, // GDS_OA_ADDRESS
        {2u, 1u, {{0x41Du, 0x00000000u}}}, // GDS_OA_CNTL
        {2u, 2u, {{0x41Eu, 0x00000000u}}}, // GDS_OA_COUNTER
        {2u, 3u, {{0x25Bu, 0x00000000u}}}, // GE_CNTL
        {2u, 4u, {{0x24Au, 0x00000000u}}}, // GE_INDX_OFFSET
        {2u, 5u, {{0x24Bu, 0x00000000u}}}, // GE_MULTI_PRIM_IB_RESET_EN
        {2u, 6u, {{0x25Fu, 0x00000000u}}}, // GE_STEREO_CNTL
        {2u, 7u, {{0x262u, 0x00000000u}}}, // GE_USER_VGPR_EN
        {2u, 8u, {{0x80003FF4u, 0x00000000u}}}, // FSR_EXTEND_SUBPIXEL_ROUNDING
        {2u, 9u, {{0x80003FFDu, 0x00000000u}}}, // TEXTURE_GRADIENT_CONTROL
        {2u, 10u, {{0x382u, 0x40000040u}}}, // TEXTURE_GRADIENT_FACTORS
        {2u, 11u, {{0x248u, 0x00000000u}}}, // VGT_OBJECT_ID
        {2u, 12u, {{0x242u, 0x00000000u}}}, // VGT_PRIMITIVE_TYPE
        {2u, 13u, {{0x380u, 0x00000000u}, {0x381u, 0x00000000u}}}, // TA_CS_BC_BASE_ADDR/HI
        {2u, 14u, {{0x80003FF5u, 0x00000000u}, {0x80003FF6u, 0x00000000u}}}, // FSR_ALPHA_VALUE0/1
        {2u, 15u, {{0x80003FF7u, 0x00000000u}, {0x80003FF8u, 0x00000000u}, {0x80003FF9u, 0x00000000u}, {0x80003FFAu, 0x00000000u}}}, // FSR_CONTROL_POINT0-3
        {2u, 16u, {{0x80003FFBu, 0x00000000u}, {0x80003FFCu, 0x00000000u}}}, // FSR_WINDOW0/1
        {2u, 17u, {{0x80003FFEu, 0x00000000u}}}, // MEMORY_MAPPING_MASK
        {2u, 18u, {{0x80003FFFu, 0x00000000u}}}, // UC_NOP
        {2u, 19u, {{0x25Cu, 0x00000000u}}}, // GE_USER_VGPR1
    };
    return kGroups;
}

const std::vector<AgcRegDefaultGroup>& InternalRegisterDefaults() {
    static const std::vector<AgcRegDefaultGroup> kGroups = {
        {0u, 0u, {{0x00Eu, 0u}}},
        {0u, 1u, {{0x2AFu, 0u}}},
        {0u, 2u, {{0x314u, 0u}}},
        {0u, 3u, {{0x1B5u, 0u}}},
        {1u, 0u, {{0x216u, 0u}}},
        {1u, 1u, {{0x217u, 0u}}},
        {1u, 2u, {{0x219u, 0u}}},
        {1u, 3u, {{0x21Au, 0u}}},
        {1u, 4u, {{0x27Du, 0u}}},
        {1u, 5u, {{0x22Au, 0u}}},
        {1u, 6u, {{0x204u, 0u}}},
        {1u, 7u, {{0x205u, 0u}}},
        {1u, 8u, {{0x206u, 0u}}},
        {1u, 9u, {{0x080u, 0u}}},
        {1u, 10u, {{0x100u, 0u}}},
        {1u, 11u, {{0x006u, 0u}}},
        {1u, 12u, {{0x081u, 0u}}},
        {1u, 13u, {{0x101u, 0u}}},
        {1u, 14u, {{0x001u, 0u}}},
        {2u, 0u, {{0x24Fu, 0u}}},
        {2u, 1u, {{0x80003FFFu, 0u}}},
        {2u, 2u, {{0x250u, 0u}}},
    };
    return kGroups;
}

u32 AlignUp32(u32 v, u32 a) { return (v + a - 1u) & ~(a - 1u); }

// Builds the SDK defaults blob (AgcExports.cs TryBuildRegisterDefaults):
// header 0x40 bytes (cx table ptr @0, sh @8, uc @0x10, types ptr @0x30, group
// count @0x38), then per-space tables of block pointers, the per-group type
// entries, and finally one 128-byte register block per group.
guest_addr_t BuildRegisterDefaultsBlob(const std::vector<AgcRegDefaultGroup>& groups,
                                       u32 cx_len, u32 sh_len, u32 uc_len) {
    constexpr u32 kHeaderSize = 0x40;
    constexpr u32 kBlockSize = 16 * 8;
    const u32 cx_off = AlignUp32(kHeaderSize, 8);
    const u32 sh_off = cx_off + cx_len * 8;
    const u32 uc_off = sh_off + sh_len * 8;
    const u32 types_off = AlignUp32(uc_off + uc_len * 8, 4);
    const u32 blocks_off = AlignUp32(types_off + static_cast<u32>(groups.size()) * 3 * 4, 8);
    const u32 blob_len = blocks_off + static_cast<u32>(groups.size()) * kBlockSize;

    guest_addr_t base = 0;
    if (Memory::Map(0, AlignUp32(blob_len, 0x1000),
                    Memory::PROT_READ | Memory::PROT_WRITE, &base) != Memory::Status::Ok) {
        LOG_ERROR(HLE, "AGC register defaults: failed to map %u-byte blob", blob_len);
        return 0;
    }

    std::vector<u8> blob(blob_len, 0);
    auto w64 = [&](u32 off, u64 v) { std::memcpy(blob.data() + off, &v, 8); };
    auto w32 = [&](u32 off, u32 v) { std::memcpy(blob.data() + off, &v, 4); };
    w64(0x00, base + cx_off);
    w64(0x08, base + sh_off);
    w64(0x10, base + uc_off);
    w64(0x30, base + types_off);
    w32(0x38, static_cast<u32>(groups.size()));

    for (size_t gi = 0; gi < groups.size(); ++gi) {
        const auto& g = groups[gi];
        const u32 table_off = g.space == 0 ? cx_off : g.space == 1 ? sh_off : uc_off;
        const u32 block_off = blocks_off + static_cast<u32>(gi) * kBlockSize;
        w64(table_off + g.index * 8, base + block_off);
        const u32 type_off = types_off + static_cast<u32>(gi) * 12;
        w32(type_off, g.space);                 // type
        w32(type_off + 4, g.index * 4 + g.space); // encoded index
        for (size_t ri = 0; ri < g.regs.size() && ri < 16; ++ri) {
            w32(block_off + static_cast<u32>(ri) * 8, g.regs[ri].offset);
            w32(block_off + static_cast<u32>(ri) * 8 + 4, g.regs[ri].value);
        }
    }

    Memory::WriteBuffer(base, blob.data(), blob.size());
    return base;
}

guest_addr_t GetDefaultsBlob(bool internal) {
    static guest_addr_t s_primary = 0;
    static guest_addr_t s_internal = 0;
    static std::mutex s_defaults_mutex;
    std::lock_guard<std::mutex> lk(s_defaults_mutex);
    if (!s_primary) {
        s_primary = BuildRegisterDefaultsBlob(PrimaryRegisterDefaults(), 78, 29, 20);
    }
    if (!s_internal) {
        s_internal = BuildRegisterDefaultsBlob(InternalRegisterDefaults(), 4, 15, 3);
    }
    return internal ? s_internal : s_primary;
}

// ---------------------------------------------------------------------------
// sceAgcCreateShader helpers.
// ---------------------------------------------------------------------------
void RelocatePointerField(guest_addr_t field_addr) {
    const u64 rel = Memory::Read<u64>(field_addr);
    if (rel != 0) {
        Memory::Write<u64>(field_addr, field_addr + rel);
    }
}

// Patches the PGM_LO/HI entries of the shader's Sh-register table to point at
// the real code address (AgcExports.cs PatchShaderProgramRegisters).
bool PatchShaderProgramRegisters(guest_addr_t header, guest_addr_t code) {
    const u64 sh_regs = Memory::Read<u64>(header + kShShRegsOff);
    const u8 shader_type = Memory::Read<u8>(header + kShTypeOff);
    const u8 reg_count = Memory::Read<u8>(header + kShNumShRegsOff);
    if (sh_regs == 0 || reg_count < 2) {
        LOG_WARN(HLE, "sceAgcCreateShader: no Sh register table (shRegs=0x%llx count=%u)",
                 sh_regs, reg_count);
        return false;
    }

    u32 expected_lo = 0, expected_hi = 0;
    switch (shader_type) {
        case 0: expected_lo = kComputePgmLo;     expected_hi = kComputePgmHi;     break;
        case 1: expected_lo = kSpiShaderPgmLoPs; expected_hi = kSpiShaderPgmHiPs; break;
        case 2:
        case 6: expected_lo = kSpiShaderPgmLoEs; expected_hi = kSpiShaderPgmHiEs; break;
        case 4: expected_lo = kSpiShaderPgmLoGs; expected_hi = kSpiShaderPgmHiGs; break;
        case 7: expected_lo = kSpiShaderPgmLoLs; expected_hi = kSpiShaderPgmHiLs; break;
        default: break;
    }
    const u32 lo_reg = Memory::Read<u32>(sh_regs);
    const u32 hi_reg = Memory::Read<u32>(sh_regs + 8);
    if (expected_lo == 0 || lo_reg != expected_lo || hi_reg != expected_hi) {
        LOG_WARN(HLE, "sceAgcCreateShader: unexpected PGM regs type=%u lo=0x%X hi=0x%X",
                 shader_type, lo_reg, hi_reg);
        return false;
    }

    Memory::Write<u32>(sh_regs + 4, static_cast<u32>((code >> 8) & 0xFFFFFFFFull));
    Memory::Write<u32>(sh_regs + 12, static_cast<u32>((code >> 40) & 0xFFull));
    return true;
}

// ---------------------------------------------------------------------------
// Command-buffer dword allocation (AgcExports.cs TryAllocateCommandDwords,
// minus the buffer-full guest callback which our dispatcher cannot invoke).
// ---------------------------------------------------------------------------
guest_addr_t AllocCommandDwords(guest_addr_t cb, u32 dwords) {
    if (cb == 0 || dwords == 0) return 0;
    const u64 up = Memory::Read<u64>(cb + kCbCursorUpOff);
    const u64 down = Memory::Read<u64>(cb + kCbCursorDownOff);
    const u32 reserved = Memory::Read<u32>(cb + kCbReservedDwOff);
    if (up == 0 || down < up) {
        LOG_WARN(HLE, "AGC DCB builder: bad cursors (up=0x%llx down=0x%llx)", up, down);
        return 0;
    }
    const u64 available = (down - up) / sizeof(u32);
    const u64 remaining = available > reserved ? available - reserved : 0;
    if (dwords > remaining) {
        LOG_WARN(HLE, "AGC DCB builder: buffer full (need %u dwords, %llu remaining)",
                 dwords, remaining);
        return 0;
    }
    Memory::Write<u64>(cb + kCbCursorUpOff, up + static_cast<u64>(dwords) * sizeof(u32));
    return up;
}

// ---------------------------------------------------------------------------
// Submitted-DCB walker: PM4 packet stream + register shadow state
// (AgcExports.cs ParseSubmittedDcbCore / ApplySubmittedRegisters /
//  TryReadSubmittedDrawCount, minus GPU translation).
// ---------------------------------------------------------------------------
struct AgcSubmitShadow {
    std::unordered_map<u32, u32> cx, sh, uc;
    u64 index_addr = 0;
    u32 index_count = 0;
    u32 index_size = 0;
    u32 instances = 1;
    u64 indirect_args = 0;
    u64 total_draws = 0;
    u64 total_dispatches = 0;
    u64 total_flips = 0;
};

std::mutex g_agc_submit_mutex;
AgcSubmitShadow g_agc_graphics_shadow;
std::unordered_map<u32, AgcSubmitShadow> g_agc_compute_shadows;

void ApplySubmittedRegisters(AgcSubmitShadow& st, guest_addr_t packet,
                             u32 length, u32 op, u32 reg) {
    if (op == kItSetShReg || op == kItSetContextReg || op == kItSetUconfigReg) {
        if (length < 3) return;
        const u32 start = Memory::Read<u32>(packet + 4);
        auto& dst = op == kItSetShReg ? st.sh : op == kItSetContextReg ? st.cx : st.uc;
        for (u32 i = 0; i < length - 2; ++i) {
            dst[start + i] = Memory::Read<u32>(packet + 8 + static_cast<u64>(i) * 4);
        }
        return;
    }
    if (op != kItNop ||
        (reg != kRCxRegsIndirect && reg != kRShRegsIndirect && reg != kRUcRegsIndirect) ||
        length < 4) {
        return;
    }
    const u32 count = Memory::Read<u32>(packet + 4);
    const u64 regs_addr = Memory::Read<u64>(packet + 8);
    if (regs_addr == 0 || count > 0x10000) return;
    auto& dst = reg == kRCxRegsIndirect ? st.cx : reg == kRShRegsIndirect ? st.sh : st.uc;
    for (u32 i = 0; i < count; ++i) {
        const u64 entry = regs_addr + static_cast<u64>(i) * 8;
        const u32 reg_off = Memory::Read<u32>(entry);
        const u32 value = Memory::Read<u32>(entry + 4);
        dst[reg_off] = value;
    }
}

// Returns the draw count for draw opcodes, 0 when not a (counted) draw.
u32 SubmittedDrawCount(AgcSubmitShadow& st, guest_addr_t packet, u32 length, u32 op) {
    switch (op) {
        case kItDrawIndexAuto:
            return length >= 3 ? Memory::Read<u32>(packet + 4) : 0;
        case kItDrawIndex2:
            return length >= 6 ? Memory::Read<u32>(packet + 16) : 0;
        case kItDrawIndexOffset2:
            return length >= 5 ? Memory::Read<u32>(packet + 12) : 0;
        case kItDrawIndexMultiAuto:
            return length >= 4 ? (Memory::Read<u32>(packet + 12) >> 21) & 0x7FFu : 0;
        case kItDrawIndirect:
        case kItDrawIndexIndirect:
            if (length >= 5 && st.indirect_args != 0) {
                const u32 data_off = Memory::Read<u32>(packet + 4);
                return Memory::Read<u32>(st.indirect_args + data_off);
            }
            return 0;
        default:
            return 0;
    }
}

// ---------------------------------------------------------------------------
// CP DMA fill/copy execution (Phase 5 M1, SharpEmu ApplySubmittedDmaData).
//
// AGC games clear their render targets with CP DMA fills into unified guest
// memory instead of GPU clear packets.  Since our presentation path uploads
// display buffers straight from guest memory, executing the fill/copy here
// is exactly what makes per-frame DMA clears visible (a GPU-side clear would
// additionally race flip ordering, so the guest-memory fill is the M1 clear
// path; VkPresentClearColor exists for a future render-target model).
//
// Packet layouts (NOP sub-opcode kRDmaData / IT_DMA_DATA):
//   compact (length 7): dst@4,  src@12, byteCount@20 — fill when
//                       dst >= 0x10000 && src <= 0xFFFFFFFF (src = fill value)
//   standard (length 8): byteCount@12, dst@16, src@24 — copy from src
// ---------------------------------------------------------------------------
void ApplySubmittedDmaData(guest_addr_t packet, u32 length, const char* queue_name) {
    const bool compact = (length == 7);
    const u64 byte_count_off = compact ? 20 : 12;
    const u64 dst_off        = compact ? 4  : 16;
    const u64 src_off        = compact ? 12 : 24;

    const u32 byte_count = Memory::Read<u32>(packet + byte_count_off);
    const u64 dst        = Memory::Read<u64>(packet + dst_off);
    const u64 src        = Memory::Read<u64>(packet + src_off);

    const bool fill = compact && dst >= 0x10000 && src <= 0xFFFFFFFFull;
    if (byte_count == 0 || byte_count > 256u * 1024u * 1024u || dst == 0) {
        LOG_DEBUG(HLE, "AGC %s: dma_data skipped (dst=0x%llx bytes=%u)", queue_name, dst, byte_count);
        return;
    }

    if (fill) {
        const u32 value = static_cast<u32>(src);
        LOG_INFO(HLE, "AGC %s: dma fill dst=0x%llx value=0x%08X bytes=%u",
                 queue_name, dst, value, byte_count);
        u8* p = reinterpret_cast<u8*>(dst);
        __try {
            for (u32 i = 0; i + 4 <= byte_count; i += 4) {
                std::memcpy(p + i, &value, 4);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            LOG_WARN(HLE, "AGC %s: dma fill faulted at dst=0x%llx", queue_name, dst);
        }
        return;
    }

    if (src == 0) return;
    LOG_INFO(HLE, "AGC %s: dma copy dst=0x%llx src=0x%llx bytes=%u",
             queue_name, dst, src, byte_count);
    __try {
        std::memcpy(reinterpret_cast<void*>(dst), reinterpret_cast<const void*>(src), byte_count);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARN(HLE, "AGC %s: dma copy faulted (dst=0x%llx src=0x%llx)", queue_name, dst, src);
    }
}

void WalkCommandBuffer(guest_addr_t addr, u32 dword_count,
                       AgcSubmitShadow& st, const char* queue_name) {    if (addr == 0 || dword_count == 0) return;
    if (dword_count > 0x100000) {
        LOG_WARN(HLE, "AGC %s: implausible dword count %u — clamped", queue_name, dword_count);
        dword_count = 0x100000;
    }

    u32 draws = 0, dispatches = 0, flips = 0;
    u32 offset = 0;
    while (offset < dword_count) {
        const guest_addr_t packet = addr + static_cast<u64>(offset) * 4;
        const u32 header = Memory::Read<u32>(packet);
        const u32 packet_type = header >> 30;
        if (packet_type == 2) { // type-2: single filler dword
            offset++;
            continue;
        }
        if (packet_type != 3) {
            LOG_WARN(HLE, "AGC %s: bad packet type %u at dword %u (header 0x%X) — stop",
                     queue_name, packet_type, offset, header);
            break;
        }
        const u32 length = Pm4Length(header);
        if (length == 0 || offset + length > dword_count) {
            LOG_WARN(HLE, "AGC %s: bad packet length %u at dword %u (of %u) — stop",
                     queue_name, length, offset, dword_count);
            break;
        }
        const u32 op = (header >> 8) & 0xFFu;
        const u32 reg = (header >> 2) & 0x3Fu;

        if (op == kItNop && (reg == kRDrawReset || reg == kRAcbReset) && length >= 2) {
            st.cx.clear(); st.sh.clear(); st.uc.clear();
            st.index_addr = 0; st.index_count = 0; st.index_size = 0;
            st.instances = 1; st.indirect_args = 0;
        }

        ApplySubmittedRegisters(st, packet, length, op, reg);

        if (op == kItSetBase && length >= 4 && Memory::Read<u32>(packet + 4) == 1) {
            st.indirect_args = Memory::Read<u64>(packet + 8);
        }
        if (op == kItIndexBase && length >= 3) {
            st.index_addr = Memory::Read<u64>(packet + 4);
        }
        if (op == kItIndexBufferSize && length >= 2) {
            st.index_count = Memory::Read<u32>(packet + 4);
        }
        if (op == kItNop && reg == kRIndexCount && length >= 2) {
            st.index_count = Memory::Read<u32>(packet + 4);
        }
        if (op == kItIndexType && length >= 2) {
            st.index_size = Memory::Read<u32>(packet + 4) & 0x3u;
        }
        if (op == kItNumInstances && length >= 2) {
            st.instances = Memory::Read<u32>(packet + 4);
            if (st.instances == 0) st.instances = 1;
        }

        // Draw opcodes.
        const u32 draw_count = SubmittedDrawCount(st, packet, length, op);
        if (draw_count != 0) {
            ++draws;
            ++st.total_draws;
            LOG_INFO(HLE, "AGC %s: draw op=0x%02X count=%u instances=%u (frame draw #%u)",
                     queue_name, op, draw_count, st.instances, draws);
        }
        if (op == kItNop && reg == kRDrawIndexAuto && length >= 2) {
            const u32 auto_count = Memory::Read<u32>(packet + 4);
            if (auto_count != 0) {
                ++draws;
                ++st.total_draws;
                LOG_INFO(HLE, "AGC %s: draw auto count=%u instances=%u (frame draw #%u)",
                         queue_name, auto_count, st.instances, draws);
            }
        }

        // Compute dispatches.
        if (op == kItDispatchDirect && length >= 5) {
            ++dispatches;
            ++st.total_dispatches;
            LOG_INFO(HLE, "AGC %s: dispatch groups=(%u,%u,%u)",
                     queue_name, Memory::Read<u32>(packet + 4),
                     Memory::Read<u32>(packet + 8), Memory::Read<u32>(packet + 12));
        }
        if (op == kItDispatchIndirect && length >= 3) {
            ++dispatches;
            ++st.total_dispatches;
            LOG_INFO(HLE, "AGC %s: dispatch indirect offset=0x%X",
                     queue_name, Memory::Read<u32>(packet + 4));
        }

        if (op == kItNop && reg == kRWaitFlipDone && length >= 3) {
            LOG_DEBUG(HLE, "AGC %s: wait-flip-done handle=%u index=%u",
                      queue_name, Memory::Read<u32>(packet + 4),
                      Memory::Read<u32>(packet + 8));
        }
        if (op == kItEventWrite && length >= 2) {
            LOG_DEBUG(HLE, "AGC %s: event-write type=0x%02X",
                      queue_name, Memory::Read<u32>(packet + 4) & 0x3Fu);
        }

        // CP DMA fill/copy: execute into guest memory (per-frame DMA clears
        // of the display buffer become visible at the next flip).
        if (op == kItNop && reg == kRDmaData && length >= 7) {
            ApplySubmittedDmaData(packet, length, queue_name);
        }
        if (op == kItDmaData && length >= 7) {
            ApplySubmittedDmaData(packet, length, queue_name);
        }

        // Flip: forward to the videoout flip path (SharpEmu SubmitFlipFromAgc).
        if (op == kItNop && reg == kRFlip && length >= 6) {
            const u32 handle = Memory::Read<u32>(packet + 4);
            const s32 buf_index = static_cast<s32>(Memory::Read<u32>(packet + 8));
            const u32 flip_mode = Memory::Read<u32>(packet + 12);
            const s64 flip_arg = static_cast<s64>(Memory::Read<u64>(packet + 16));
            ++flips;
            ++st.total_flips;
            LOG_INFO(HLE, "AGC %s: flip handle=0x%X index=%d mode=%u arg=%lld",
                     queue_name, handle, buf_index, flip_mode, flip_arg);
            const u64 rc = VideoOutSubmitFlipFromAgc(handle, buf_index, flip_mode, flip_arg);
            if (rc != 0) {
                LOG_DEBUG(HLE, "AGC %s: flip forward -> 0x%llX", queue_name, rc);
            }
        }

        offset += length;
    }

    if (draws || dispatches || flips) {
        LOG_INFO(HLE, "AGC %s: walked %u dwords — %u draws, %u dispatches, %u flips",
                 queue_name, offset, draws, dispatches, flips);
    } else {
        LOG_DEBUG(HLE, "AGC %s: walked %u dwords (no draw/dispatch/flip)",
                  queue_name, offset);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Test/introspection hooks (declared in hle.h).
// ---------------------------------------------------------------------------
u64 AgcGetSubmittedStats(u32 which) {
    std::lock_guard<std::mutex> lk(g_agc_submit_mutex);
    switch (which) {
        case 0: return g_agc_graphics_shadow.total_draws;
        case 1: return g_agc_graphics_shadow.total_dispatches;
        case 2: return g_agc_graphics_shadow.total_flips;
        default: return 0;
    }
}

bool AgcGetShadowRegister(u32 space, u32 reg, u32* value_out) {
    if (!value_out) return false;
    std::lock_guard<std::mutex> lk(g_agc_submit_mutex);
    const auto& map = space == 0 ? g_agc_graphics_shadow.cx
                    : space == 1 ? g_agc_graphics_shadow.sh
                                 : g_agc_graphics_shadow.uc;
    const auto it = map.find(reg);
    if (it == map.end()) return false;
    *value_out = it->second;
    return true;
}

void RegisterLibAgc() {
    LOG_INFO(HLE, "Registering libSceAgc / libSceAgcDriver HLE symbols (Phase 5 M0)...");

    // ------------------------------------------------------------------
    // Core library init/context entry points (kept from the stub phase).
    // ------------------------------------------------------------------
    RegisterSymbol("libSceAgc", "sceAgcInitialize", [](const GuestArgs& args) -> u64 {
        (void)args;
        LOG_INFO(HLE, "sceAgcInitialize() called");
        return 0;
    });

    RegisterSymbol("libSceAgc", "sceAgcCreateContext", [](const GuestArgs& args) -> u64 {
        guest_addr_t ctx_out = args.arg1;
        LOG_INFO(HLE, "sceAgcCreateContext() called");
        if (ctx_out) {
            Memory::Write<u64>(ctx_out, 0x5000); // Mock context handle
        }
        return 0;
    });

    RegisterSymbol("libSceAgc", "sceAgcRegisterContext", [](const GuestArgs& args) -> u64 {
        u64 ctx = args.arg1;
        LOG_INFO(HLE, "sceAgcRegisterContext(context: 0x%llx) called", ctx);
        return 0;
    });

    // sceAgcRegisterDisplay / sceAgcRegisterConfiguration — not present in
    // SharpEmu; plausible success contract: accept and remember nothing.
    auto AgcRegisterDisplay = [](const GuestArgs& args) -> u64 {
        LOG_INFO(HLE, "sceAgcRegisterDisplay(0x%llx, 0x%llx, 0x%llx) -> 0",
                 args.arg1, args.arg2, args.arg3);
        return 0;
    };
    RegisterSymbol("libSceAgc", "sceAgcRegisterDisplay", AgcRegisterDisplay);
    RegisterSymbol("libSceAgc", "-VVn74ZyhEs", AgcRegisterDisplay);

    auto AgcRegisterConfiguration = [](const GuestArgs& args) -> u64 {
        LOG_INFO(HLE, "sceAgcRegisterConfiguration(0x%llx, 0x%llx, 0x%llx) -> 0",
                 args.arg1, args.arg2, args.arg3);
        return 0;
    };
    RegisterSymbol("libSceAgc", "sceAgcRegisterConfiguration", AgcRegisterConfiguration);
    RegisterSymbol("libSceAgc", "2sWzhYqFH4E", AgcRegisterConfiguration);

    // GNM-era submit entry point: walk the buffer like a DCB.  `size` is
    // treated as a dword count (the AGC convention everywhere else).
    auto AgcSubmitCommandBuffer = [](const GuestArgs& args) -> u64 {
        u64 ctx = args.arg1;
        guest_addr_t cmd_buf = args.arg2;
        u32 dwords = static_cast<u32>(args.arg3);
        LOG_INFO(HLE, "sceAgcSubmitCommandBuffer(context: 0x%llx, cmd_buf: 0x%llx, dwords: %u)",
                 ctx, cmd_buf, dwords);
        std::lock_guard<std::mutex> lk(g_agc_submit_mutex);
        WalkCommandBuffer(cmd_buf, dwords, g_agc_graphics_shadow, "submit.cb");
        return 0;
    };
    RegisterSymbol("libSceAgc", "sceAgcSubmitCommandBuffer", AgcSubmitCommandBuffer);

    // ------------------------------------------------------------------
    // Register defaults (real Gen5 tables).
    // ------------------------------------------------------------------
    auto AgcGetRegisterDefaults2 = [](const GuestArgs& args) -> u64 {
        const u32 version = static_cast<u32>(args.arg1);
        guest_addr_t blob = GetDefaultsBlob(false);
        LOG_INFO(HLE, "sceAgcGetRegisterDefaults2(version: %u) -> 0x%llx", version, blob);
        return blob;
    };
    RegisterSymbol("libSceAgc", "sceAgcGetRegisterDefaults2", AgcGetRegisterDefaults2);
    RegisterSymbol("libSceAgc", "2JtWUUiYBXs#A#B", AgcGetRegisterDefaults2);
    RegisterSymbol("libSceAgc", "2JtWUUiYBXs", AgcGetRegisterDefaults2);

    auto AgcGetRegisterDefaults2Internal = [](const GuestArgs& args) -> u64 {
        const u32 version = static_cast<u32>(args.arg1);
        guest_addr_t blob = GetDefaultsBlob(true);
        LOG_INFO(HLE, "sceAgcGetRegisterDefaults2Internal(version: %u) -> 0x%llx", version, blob);
        return blob;
    };
    RegisterSymbol("libSceAgc", "sceAgcGetRegisterDefaults2Internal", AgcGetRegisterDefaults2Internal);
    RegisterSymbol("libSceAgc", "wRbq6ZjNop4", AgcGetRegisterDefaults2Internal);

    // 23LRUSvYu1M — upstream NID for sceAgcInit(state, version); success
    // contract only, no outputs.  (Was previously aliased to the defaults
    // query; SharpEmu's table shows it is the init entry point.)
    auto AgcInit = [](const GuestArgs& args) -> u64 {
        const u64 state = args.arg1;
        const u32 version = static_cast<u32>(args.arg2);
        LOG_INFO(HLE, "sceAgcInit(state: 0x%llx, version: %u) -> 0", state, version);
        if (state == 0) {
            LOG_WARN(HLE, "sceAgcInit: null state pointer");
        }
        if (version != 7 && version != 8 && version != 10 && version != 13) {
            LOG_WARN(HLE, "sceAgcInit: unsupported version %u (accepted anyway)", version);
        }
        return 0;
    };
    RegisterSymbol("libSceAgc", "sceAgcInit", AgcInit);
    RegisterSymbol("libSceAgc", "23LRUSvYu1M#A#B", AgcInit);
    RegisterSymbol("libSceAgc", "23LRUSvYu1M", AgcInit);

    // ------------------------------------------------------------------
    // sceAgcCreateShader (f3dg2CSgRKY) — real shader ABI.
    // ABI: rdi = dest (u64* out), rsi = shader header, rdx = code address.
    // Returns the header pointer as the shader handle; *dest = header too.
    // ------------------------------------------------------------------
    auto AgcCreateShader = [](const GuestArgs& args) -> u64 {
        const guest_addr_t dest = args.arg1;
        const guest_addr_t header = args.arg2;
        const guest_addr_t code = args.arg3;
        if (header == 0 || code == 0) {
            LOG_WARN(HLE, "sceAgcCreateShader: null header/code (header=0x%llx code=0x%llx)",
                     header, code);
            return 0;
        }
        const u32 magic = Memory::Read<u32>(header);
        const u32 version = Memory::Read<u32>(header + 4);
        if (magic != kShaderFileMagic || version != kShaderVersion) {
            LOG_WARN(HLE, "sceAgcCreateShader: bad header magic=0x%X version=0x%X",
                     magic, version);
            return 0;
        }

        RelocatePointerField(header + kShCxRegsOff);
        RelocatePointerField(header + kShShRegsOff);
        RelocatePointerField(header + kShUserDataOff);
        RelocatePointerField(header + kShSpecialsOff);
        RelocatePointerField(header + kShInSemanticsOff);
        RelocatePointerField(header + kShOutSemanticsOff);
        Memory::Write<u64>(header + kShCodeOff, code);

        const u64 user_data = Memory::Read<u64>(header + kShUserDataOff);
        if (user_data != 0) {
            RelocatePointerField(user_data);
            RelocatePointerField(user_data + 0x08);
            RelocatePointerField(user_data + 0x10);
            RelocatePointerField(user_data + 0x18);
            RelocatePointerField(user_data + 0x20);
        }

        if (!PatchShaderProgramRegisters(header, code)) {
            LOG_WARN(HLE, "sceAgcCreateShader: PGM register patch failed (header=0x%llx)",
                     header);
            return 0;
        }

        if (dest != 0) {
            Memory::Write<u64>(dest, header);
        }
        g_agc_shaders_by_code[code] = header;

        LOG_INFO(HLE, "sceAgcCreateShader(dest: 0x%llx, header: 0x%llx, code: 0x%llx) -> 0x%llx",
                 dest, header, code, header);
        return header;
    };
    RegisterSymbol("libSceAgc", "sceAgcCreateShader", AgcCreateShader);
    RegisterSymbol("libSceAgc", "f3dg2CSgRKY", AgcCreateShader);

    // ------------------------------------------------------------------
    // DCB/CB command-buffer builders (PM4 emitters).
    // ------------------------------------------------------------------
    auto DcbResetQueue = [](const GuestArgs& args) -> u64 {
        const guest_addr_t cb = args.arg1;
        const u32 op = static_cast<u32>(args.arg2);
        const u32 state = static_cast<u32>(args.arg3);
        if (cb == 0 || op != 0x3FF || state != 0) return 0;
        const guest_addr_t cmd = AllocCommandDwords(cb, 2);
        if (!cmd) return 0;
        Memory::Write<u32>(cmd, Pm4(2, kItNop, kRDrawReset));
        Memory::Write<u32>(cmd + 4, 0);
        return cmd;
    };
    RegisterSymbol("libSceAgc", "sceAgcDcbResetQueue", DcbResetQueue);
    RegisterSymbol("libSceAgc", "TRO721eVt4g", DcbResetQueue);

    auto AcbResetQueue = [](const GuestArgs& args) -> u64 {
        const guest_addr_t cb = args.arg1;
        const guest_addr_t cmd = AllocCommandDwords(cb, 2);
        if (!cmd) return 0;
        Memory::Write<u32>(cmd, Pm4(2, kItNop, kRAcbReset));
        Memory::Write<u32>(cmd + 4, 0);
        return cmd;
    };
    RegisterSymbol("libSceAgc", "sceAgcAcbResetQueue", AcbResetQueue);
    RegisterSymbol("libSceAgc", "JrtiDtKeS38", AcbResetQueue);

    auto CbNop = [](const GuestArgs& args) -> u64 {
        const guest_addr_t cb = args.arg1;
        const u32 dwords = static_cast<u32>(args.arg2);
        if (cb == 0 || dwords < 2 || dwords > 0x4001) return 0;
        const guest_addr_t cmd = AllocCommandDwords(cb, dwords);
        if (!cmd) return 0;
        Memory::Write<u32>(cmd, Pm4(dwords, kItNop, kRZero));
        for (u32 i = 1; i < dwords; ++i) {
            Memory::Write<u32>(cmd + static_cast<u64>(i) * 4, 0);
        }
        return cmd;
    };
    RegisterSymbol("libSceAgc", "sceAgcCbNop", CbNop);
    RegisterSymbol("libSceAgc", "LtTouSCZjHM", CbNop);

    auto DcbSetRegistersIndirect = [](const GuestArgs& args, u32 packet_reg) -> u64 {
        const guest_addr_t cb = args.arg1;
        const u64 regs_addr = args.arg2;
        const u32 count = static_cast<u32>(args.arg3);
        if (cb == 0) return 0;
        const guest_addr_t cmd = AllocCommandDwords(cb, 4);
        if (!cmd) return 0;
        Memory::Write<u32>(cmd, Pm4(4, kItNop, packet_reg));
        Memory::Write<u32>(cmd + 4, count);
        Memory::Write<u32>(cmd + 8, static_cast<u32>(regs_addr & 0xFFFFFFFFull));
        Memory::Write<u32>(cmd + 12, static_cast<u32>(regs_addr >> 32));
        return cmd;
    };
    auto DcbSetCxRegistersIndirect = [DcbSetRegistersIndirect](const GuestArgs& args) -> u64 {
        return DcbSetRegistersIndirect(args, kRCxRegsIndirect);
    };
    auto DcbSetShRegistersIndirect = [DcbSetRegistersIndirect](const GuestArgs& args) -> u64 {
        return DcbSetRegistersIndirect(args, kRShRegsIndirect);
    };
    auto DcbSetUcRegistersIndirect = [DcbSetRegistersIndirect](const GuestArgs& args) -> u64 {
        return DcbSetRegistersIndirect(args, kRUcRegsIndirect);
    };
    RegisterSymbol("libSceAgc", "sceAgcDcbSetCxRegistersIndirect", DcbSetCxRegistersIndirect);
    RegisterSymbol("libSceAgc", "ZvwO9euwYzc", DcbSetCxRegistersIndirect);
    RegisterSymbol("libSceAgc", "sceAgcDcbSetShRegistersIndirect", DcbSetShRegistersIndirect);
    RegisterSymbol("libSceAgc", "-HOOCn0JY48", DcbSetShRegistersIndirect);
    RegisterSymbol("libSceAgc", "sceAgcDcbSetUcRegistersIndirect", DcbSetUcRegistersIndirect);
    RegisterSymbol("libSceAgc", "hvUfkUIQcOE", DcbSetUcRegistersIndirect);

    auto CbSetShRegistersDirect = [](const GuestArgs& args) -> u64 {
        const guest_addr_t cb = args.arg1;
        const guest_addr_t regs_addr = args.arg2;
        const u32 count = static_cast<u32>(args.arg3);
        if (cb == 0 || regs_addr == 0 || count == 0 || count > 4096) return 0;

        std::vector<AgcRegDefault> regs(count);
        for (u32 i = 0; i < count; ++i) {
            const u64 entry = regs_addr + static_cast<u64>(i) * 8;
            regs[i].offset = Memory::Read<u32>(entry);
            regs[i].value = Memory::Read<u32>(entry + 4);
        }
        std::sort(regs.begin(), regs.end(),
                  [](const AgcRegDefault& a, const AgcRegDefault& b) { return a.offset < b.offset; });

        u64 first_cmd = 0;
        u32 start = 0;
        while (start < count) {
            u32 end = start + 1;
            while (end < count && regs[end].offset == regs[end - 1].offset + 1) ++end;
            const u32 values = end - start;
            const u32 packet_dwords = values + 2;
            const guest_addr_t cmd = AllocCommandDwords(cb, packet_dwords);
            if (!cmd) return 0;
            Memory::Write<u32>(cmd, Pm4(packet_dwords, kItSetShReg, 0));
            Memory::Write<u32>(cmd + 4, regs[start].offset & 0xFFFFu);
            for (u32 i = start; i < end; ++i) {
                Memory::Write<u32>(cmd + 8 + static_cast<u64>(i - start) * 4, regs[i].value);
            }
            if (first_cmd == 0) first_cmd = cmd;
            start = end;
        }
        return first_cmd;
    };
    RegisterSymbol("libSceAgc", "sceAgcCbSetShRegistersDirect", CbSetShRegistersDirect);
    RegisterSymbol("libSceAgc", "UZbQjYAwwXM", CbSetShRegistersDirect);

    // Gen2-style error return: sign-extended s32 in RAX (AgcExports.cs SetReturn).
    auto AgcError = [](s32 code) -> u64 {
        return static_cast<u64>(static_cast<s64>(code));
    };

    // ------------------------------------------------------------------
    // sceAgcCreatePrimState (AgcExports.cs:765-803) — no allocation; fills
    // the caller-provided cx/uc register-pair buffers from the geometry
    // shader's specials struct.  cx: 16 bytes, uc: 24 bytes.
    // ------------------------------------------------------------------
    auto CreatePrimState = [AgcError](const GuestArgs& args) -> u64 {
        const guest_addr_t cx_regs = args.arg1;
        const guest_addr_t uc_regs = args.arg2;
        const guest_addr_t hull    = args.arg3;
        const guest_addr_t geom    = args.arg4;
        const u32 prim_type = static_cast<u32>(args.arg5);
        if (cx_regs == 0 || uc_regs == 0 || hull != 0 || geom == 0) {
            return AgcError(kAgcErrorInvalidArgument);
        }
        const u32 shader_type = Memory::Read<u8>(geom + kShTypeOff);
        if (shader_type != 2 && shader_type != 6) { // ES-geometry shader types
            return AgcError(kAgcErrorInvalidArgument);
        }
        const guest_addr_t specials = Memory::Read<u64>(geom + kShSpecialsOff);
        if (specials == 0) {
            return AgcError(kAgcErrorInvalidArgument);
        }
        if (!Memory::IsWritable(cx_regs, 16) || !Memory::IsWritable(uc_regs, 24) ||
            !Memory::IsReadable(specials, 0x30)) {
            return AgcError(kAgcErrorMemoryFault);
        }
        auto copy_pair = [](guest_addr_t dst, guest_addr_t src) {
            Memory::Write<u32>(dst, Memory::Read<u32>(src));
            Memory::Write<u32>(dst + 4, Memory::Read<u32>(src + 4));
        };
        copy_pair(cx_regs + 0, specials + kSpecialVgtShaderStagesEnOff);
        copy_pair(cx_regs + 8, specials + kSpecialVgtGsOutPrimTypeOff);
        copy_pair(uc_regs + 0, specials + kSpecialGeCntlOff);
        copy_pair(uc_regs + 8, specials + kSpecialGeUserVgprEnOff);
        Memory::Write<u32>(uc_regs + 16, kVgtPrimitiveType);
        Memory::Write<u32>(uc_regs + 20, prim_type);
        return 0;
    };
    RegisterSymbol("libSceAgc", "sceAgcCreatePrimState", CreatePrimState);
    RegisterSymbol("libSceAgc", "D9sr1xGUriE", CreatePrimState);

    // ------------------------------------------------------------------
    // HV4j+E0MBHE (AgcExports.cs:805-862) — interpolant-mapping builder.
    // The true export name is unpublished (newer-SDK libSceAgc split);
    // SharpEmu/Kyty both RE'd this NID from shipped titles and bind it to
    // placeholder names.  Fills 32 {offset,value} register pairs for
    // SPI_PS_INPUT_CNTL_0..31 from the producing shader's output
    // semantics; bit 0x400 marks flat-shaded PS inputs.
    //   rdi = out RegPair[32] (256 bytes)
    //   rsi = geometry/producing shader (required)
    //   rdx = pixel shader (nullable; supplies flat-shading flags)
    // ------------------------------------------------------------------
    auto CreateInterpolantMappingNid = [AgcError](const GuestArgs& args) -> u64 {
        const guest_addr_t regs = args.arg1;
        const guest_addr_t gs   = args.arg2;
        const guest_addr_t ps   = args.arg3;
        if (regs == 0 || gs == 0) return AgcError(kAgcErrorInvalidArgument);
        if (!Memory::IsReadable(gs + kShOutSemanticsOff, 8) ||
            !Memory::IsReadable(gs + kShNumOutOff, 4)) {
            return AgcError(kAgcErrorMemoryFault);
        }
        const guest_addr_t out_sem = Memory::Read<u64>(gs + kShOutSemanticsOff);
        const u32 out_count        = Memory::Read<u32>(gs + kShNumOutOff);
        guest_addr_t in_sem = 0;
        if (ps != 0) {
            if (!Memory::IsReadable(ps + kShInSemanticsOff, 8) ||
                !Memory::IsReadable(ps + kShNumInOff, 4)) {
                return AgcError(kAgcErrorMemoryFault);
            }
            in_sem = Memory::Read<u64>(ps + kShInSemanticsOff);
        }
        if (!Memory::IsWritable(regs, 32 * 8)) return AgcError(kAgcErrorMemoryFault);
        for (u32 i = 0; i < 32; ++i) {
            u32 value = 0;
            if (i < out_count && out_sem != 0) {
                bool flat = false;
                if (ps != 0 && in_sem != 0 && Memory::IsReadable(in_sem + i * 4, 4)) {
                    const u32 input_sem = Memory::Read<u32>(in_sem + i * 4);
                    flat = ((input_sem >> 22) & 0x1) != 0;
                }
                value = i | (flat ? 0x400u : 0u);
            }
            Memory::Write<u32>(regs + i * 8, kSpiPsInputCntl0 + i);
            Memory::Write<u32>(regs + i * 8 + 4, value);
        }
        return 0;
    };
    RegisterSymbol("libSceAgc", "HV4j+E0MBHE", CreateInterpolantMappingNid);

    // ------------------------------------------------------------------
    // V++UgBtQhn0 (AgcExports.cs:866-905) — data-packet payload-address
    // helper; true export name likewise unpublished.  Games use it to
    // find the writable payload of a packet they just emitted (e.g. the
    // resource tables Dreaming Sarah builds right after
    // sceAgcCbSetShRegisterRangeDirect — its stub returning 0 left the
    // table pointer null and crashed the store at eboot 0x5f0f).
    //   rdi = out u64 (payload address)
    //   rsi = command packet address
    //   edx = type: 0 => payload after header (NULL when the header
    //         count field is all-ones), nonzero => cmd + 8
    // ------------------------------------------------------------------
    auto GetDataPacketPayloadAddressNid = [AgcError](const GuestArgs& args) -> u64 {
        const guest_addr_t out = args.arg1;
        const guest_addr_t cmd = args.arg2;
        const s32 type = static_cast<s32>(args.arg3);
        if (out == 0 || cmd == 0) return AgcError(kAgcErrorInvalidArgument);
        u64 payload = cmd + 8;
        if (type == 0) {
            if (!Memory::IsReadable(cmd, 4)) return AgcError(kAgcErrorMemoryFault);
            const u32 header = Memory::Read<u32>(cmd);
            payload = (header & 0x3FFF0000u) == 0x3FFF0000u ? 0 : cmd + 4;
        }
        if (!Memory::IsWritable(out, 8)) return AgcError(kAgcErrorMemoryFault);
        Memory::Write<u64>(out, payload);
        return 0;
    };
    RegisterSymbol("libSceAgc", "V++UgBtQhn0", GetDataPacketPayloadAddressNid);

    // ------------------------------------------------------------------
    // RegIndirectPatch family (AgcExports.cs SetIndirectPatchAddress /
    // AddIndirectPatchRegisters): patches an already-emitted 4-dword
    // indirect-registers packet in place (count @+4, address @+8/+12).
    // ------------------------------------------------------------------
    auto SetIndirectPatchAddress = [AgcError](const GuestArgs& args) -> u64 {
        const guest_addr_t cmd = args.arg1;
        const u64 regs_addr = args.arg2;
        if (cmd == 0 || regs_addr == 0) return AgcError(kAgcErrorInvalidArgument);
        if (!Memory::IsWritable(cmd, 16)) return AgcError(kAgcErrorMemoryFault);
        Memory::Write<u32>(cmd + 8, static_cast<u32>(regs_addr & 0xFFFFFFFFull));
        Memory::Write<u32>(cmd + 12, static_cast<u32>(regs_addr >> 32));
        return 0;
    };
    auto AddIndirectPatchRegisters = [AgcError](const GuestArgs& args) -> u64 {
        const guest_addr_t cmd = args.arg1;
        const u32 count = static_cast<u32>(args.arg2);
        if (cmd == 0) return AgcError(kAgcErrorInvalidArgument);
        if (!Memory::IsWritable(cmd, 8)) return AgcError(kAgcErrorMemoryFault);
        Memory::Write<u32>(cmd + 4, Memory::Read<u32>(cmd + 4) + count);
        return 0;
    };
    RegisterSymbol("libSceAgc", "sceAgcSetCxRegIndirectPatchSetAddress", SetIndirectPatchAddress);
    RegisterSymbol("libSceAgc", "vcmNN+AAXnY", SetIndirectPatchAddress);
    RegisterSymbol("libSceAgc", "sceAgcSetShRegIndirectPatchSetAddress", SetIndirectPatchAddress);
    RegisterSymbol("libSceAgc", "Qrj4c+61z4A", SetIndirectPatchAddress);
    RegisterSymbol("libSceAgc", "sceAgcSetUcRegIndirectPatchSetAddress", SetIndirectPatchAddress);
    RegisterSymbol("libSceAgc", "6lNcCp+fxi4", SetIndirectPatchAddress);
    RegisterSymbol("libSceAgc", "sceAgcSetCxRegIndirectPatchAddRegisters", AddIndirectPatchRegisters);
    RegisterSymbol("libSceAgc", "d-6uF9sZDIU", AddIndirectPatchRegisters);
    RegisterSymbol("libSceAgc", "sceAgcSetShRegIndirectPatchAddRegisters", AddIndirectPatchRegisters);
    RegisterSymbol("libSceAgc", "z2duB-hHQSM", AddIndirectPatchRegisters);
    RegisterSymbol("libSceAgc", "sceAgcSetUcRegIndirectPatchAddRegisters", AddIndirectPatchRegisters);
    RegisterSymbol("libSceAgc", "vRoArM9zaIk", AddIndirectPatchRegisters);

    // ------------------------------------------------------------------
    // sceAgcCbSetShRegisterRangeDirect (AgcExports.cs:1228-1270): emits a
    // marker NOP then one SET_SH_REG packet covering a contiguous register
    // range; returns the packet (second allocation) address or 0.
    // ------------------------------------------------------------------
    auto CbSetShRegisterRangeDirect = [](const GuestArgs& args) -> u64 {
        const guest_addr_t cb = args.arg1;
        const u32 offset = static_cast<u32>(args.arg2);
        const guest_addr_t values_addr = args.arg3;
        const u32 count = static_cast<u32>(args.arg4);
        // count > 0x3FFF would overflow the PM4 length field.
        if (cb == 0 || offset == 0 || offset > 0x3FF || count == 0 || count > 0x3FFF) return 0;

        const guest_addr_t marker = AllocCommandDwords(cb, 2);
        if (!marker) return 0;
        Memory::Write<u32>(marker, Pm4(2, kItNop, kRZero));
        Memory::Write<u32>(marker + 4, kCbSetShRegisterRangeMarker);

        const guest_addr_t cmd = AllocCommandDwords(cb, count + 2);
        if (!cmd) return 0;
        Memory::Write<u32>(cmd, Pm4(count + 2, kItSetShReg, 0));
        Memory::Write<u32>(cmd + 4, offset);
        for (u32 i = 0; i < count; ++i) {
            const u32 value = values_addr != 0
                ? Memory::Read<u32>(values_addr + static_cast<u64>(i) * 4) : 0;
            Memory::Write<u32>(cmd + 8 + static_cast<u64>(i) * 4, value);
        }
        return cmd;
    };
    RegisterSymbol("libSceAgc", "sceAgcCbSetShRegisterRangeDirect", CbSetShRegisterRangeDirect);
    RegisterSymbol("libSceAgc", "n2fD4A+pb+g", CbSetShRegisterRangeDirect);

    // ------------------------------------------------------------------
    // sceAgcDcbWaitRegMem (AgcExports.cs:1778-1862) — three packet shapes;
    // reference/mask/pollCycles arrive as SysV stack arguments.
    // ------------------------------------------------------------------
    auto DcbWaitRegMem = [](const GuestArgs& args) -> u64 {
        const guest_addr_t cb = args.arg1;
        const u32 size         = static_cast<u32>(args.arg2) & 0xFF;
        const u32 compare_func = static_cast<u32>(args.arg3) & 0xFF;
        const u32 operation    = static_cast<u32>(args.arg4) & 0xFF;
        const u32 cache_policy = static_cast<u32>(args.arg5) & 0xFF; // read, not emitted
        const u64 address = args.arg6;
        if (cb == 0 || size > 1 || compare_func > 7 || operation > 4 || cache_policy > 3) {
            return 0;
        }
        const u64 reference   = Memory::Read<u64>(args.stack_args + 0);
        const u64 mask        = Memory::Read<u64>(args.stack_args + 8);
        const u32 poll_cycles = Memory::Read<u32>(args.stack_args + 16);

        if (operation == 2 || operation == 3) {
            // Standard IT_WAIT_REG_MEM (7 dwords).
            const guest_addr_t cmd = AllocCommandDwords(cb, 7);
            if (!cmd) return 0;
            Memory::Write<u32>(cmd + 0, Pm4(7, kItWaitRegMem, 0));
            Memory::Write<u32>(cmd + 4, compare_func | ((operation & 1) << 8));
            Memory::Write<u32>(cmd + 8, static_cast<u32>(address & 0xFFFFFFFFull));
            Memory::Write<u32>(cmd + 12, static_cast<u32>(address >> 32));
            Memory::Write<u32>(cmd + 16, static_cast<u32>(reference & 0xFFFFFFFFull));
            Memory::Write<u32>(cmd + 20, static_cast<u32>(mask & 0xFFFFFFFFull));
            Memory::Write<u32>(cmd + 24, poll_cycles / 40);
            return cmd;
        }
        if (size == 0) {
            // AGC 32-bit wait (6 dwords, NOP/RWaitMem32).
            const guest_addr_t cmd = AllocCommandDwords(cb, 6);
            if (!cmd) return 0;
            Memory::Write<u32>(cmd + 0, Pm4(6, kItNop, kRWaitMem32));
            Memory::Write<u32>(cmd + 4, static_cast<u32>(address & 0xFFFFFFFFull));
            Memory::Write<u32>(cmd + 8, static_cast<u32>(address >> 32));
            Memory::Write<u32>(cmd + 12, static_cast<u32>(mask & 0xFFFFFFFFull));
            Memory::Write<u32>(cmd + 16, compare_func | (operation << 8));
            Memory::Write<u32>(cmd + 20, static_cast<u32>(reference & 0xFFFFFFFFull));
            return cmd;
        }
        // AGC 64-bit wait (9 dwords, NOP/RWaitMem64).
        const guest_addr_t cmd = AllocCommandDwords(cb, 9);
        if (!cmd) return 0;
        Memory::Write<u32>(cmd + 0, Pm4(9, kItNop, kRWaitMem64));
        Memory::Write<u32>(cmd + 4, static_cast<u32>(address & 0xFFFFFFFFull));
        Memory::Write<u32>(cmd + 8, static_cast<u32>(address >> 32));
        Memory::Write<u32>(cmd + 12, static_cast<u32>(mask & 0xFFFFFFFFull));
        Memory::Write<u32>(cmd + 16, static_cast<u32>(mask >> 32));
        Memory::Write<u32>(cmd + 20, static_cast<u32>(reference & 0xFFFFFFFFull));
        Memory::Write<u32>(cmd + 24, static_cast<u32>(reference >> 32));
        Memory::Write<u32>(cmd + 28, compare_func | (operation << 8));
        Memory::Write<u32>(cmd + 32, poll_cycles / 40);
        return cmd;
    };
    RegisterSymbol("libSceAgc", "sceAgcDcbWaitRegMem", DcbWaitRegMem);
    RegisterSymbol("libSceAgc", "VmW0Tdpy420", DcbWaitRegMem);

    // ------------------------------------------------------------------
    // sceAgcDcbAcquireMem (AgcExports.cs:1668-1713) — 8-dword NOP/RAcquireMem;
    // pollCycles is a SysV stack argument.
    // ------------------------------------------------------------------
    auto DcbAcquireMem = [](const GuestArgs& args) -> u64 {
        const guest_addr_t cb = args.arg1;
        const u32 engine      = static_cast<u32>(args.arg2) & 0xFF;
        const u32 cb_db_op    = static_cast<u32>(args.arg3);
        const u32 gcr_control = static_cast<u32>(args.arg4);
        const u64 base_addr   = args.arg5;
        const u64 size_bytes  = args.arg6;
        const bool no_size = (size_bytes == ~0ull);
        if (cb == 0 || engine > 1 ||
            (!no_size && (size_bytes & 0xFF) != 0) || (!no_size && (size_bytes >> 40) != 0) ||
            (base_addr & 0xFF) != 0 || (base_addr >> 40) != 0) {
            return 0;
        }
        const u32 poll_cycles = Memory::Read<u32>(args.stack_args + 0);
        const guest_addr_t cmd = AllocCommandDwords(cb, 8);
        if (!cmd) return 0;
        Memory::Write<u32>(cmd + 0, Pm4(8, kItNop, kRAcquireMem));
        Memory::Write<u32>(cmd + 4, (engine << 31) | cb_db_op);
        Memory::Write<u32>(cmd + 8, no_size ? 0u : static_cast<u32>(size_bytes >> 8));
        Memory::Write<u32>(cmd + 12, 0);
        Memory::Write<u32>(cmd + 16, static_cast<u32>(base_addr >> 8));
        Memory::Write<u32>(cmd + 20, 0);
        Memory::Write<u32>(cmd + 24, poll_cycles / 40);
        Memory::Write<u32>(cmd + 28, gcr_control);
        return cmd;
    };
    RegisterSymbol("libSceAgc", "sceAgcDcbAcquireMem", DcbAcquireMem);
    RegisterSymbol("libSceAgc", "57labkp+rSQ", DcbAcquireMem);

    // ------------------------------------------------------------------
    // sceAgcDcbDmaData (AgcExports.cs:1903-1961) — 8-dword NOP/RDmaData;
    // control4/sourceAddress/byteCount/control7-9 are SysV stack arguments.
    // Matches the length-8 layout consumed by ApplySubmittedDmaData.
    // ------------------------------------------------------------------
    auto DcbDmaData = [](const GuestArgs& args) -> u64 {
        const guest_addr_t cb = args.arg1;
        const u32 dst       = static_cast<u32>(args.arg2) & 0xFF;
        const u32 dst_cache = static_cast<u32>(args.arg3) & 0xFF;
        const u32 src       = static_cast<u32>(args.arg4) & 0xFF;
        const u64 dst_addr  = args.arg5;
        const u32 src_cache = static_cast<u32>(args.arg6) & 0xFF;
        const u64 control4  = Memory::Read<u64>(args.stack_args + 0) & 0xFF;
        const u64 src_addr  = Memory::Read<u64>(args.stack_args + 8);
        const u32 byte_count = Memory::Read<u32>(args.stack_args + 16);
        const u64 control7  = Memory::Read<u64>(args.stack_args + 24) & 0xFF;
        const u64 control8  = Memory::Read<u64>(args.stack_args + 32) & 0xFF;
        const u64 control9  = Memory::Read<u64>(args.stack_args + 40) & 0xFF;
        if (cb == 0 || byte_count == 0 || (byte_count & 3) != 0) return 0;
        const guest_addr_t cmd = AllocCommandDwords(cb, 8);
        if (!cmd) return 0;
        Memory::Write<u32>(cmd + 0, Pm4(8, kItNop, kRDmaData));
        Memory::Write<u32>(cmd + 4, dst | (dst_cache << 8) | (src << 16) | (src_cache << 24));
        Memory::Write<u32>(cmd + 8, static_cast<u32>(control4 | (control7 << 8) |
                                                     (control8 << 16) | (control9 << 24)));
        Memory::Write<u32>(cmd + 12, byte_count);
        Memory::Write<u32>(cmd + 16, static_cast<u32>(dst_addr & 0xFFFFFFFFull));
        Memory::Write<u32>(cmd + 20, static_cast<u32>(dst_addr >> 32));
        Memory::Write<u32>(cmd + 24, static_cast<u32>(src_addr & 0xFFFFFFFFull));
        Memory::Write<u32>(cmd + 28, static_cast<u32>(src_addr >> 32));
        return cmd;
    };
    RegisterSymbol("libSceAgc", "sceAgcDcbDmaData", DcbDmaData);
    RegisterSymbol("libSceAgc", "WmAc2MEj6Io", DcbDmaData);

    auto DcbDmaDataGetSize = [](const GuestArgs& args) -> u64 {
        (void)args;
        return 8u * sizeof(u32);
    };
    RegisterSymbol("libSceAgc", "sceAgcDcbDmaDataGetSize", DcbDmaDataGetSize);
    RegisterSymbol("libSceAgc", "2ccJz9LQI+w", DcbDmaDataGetSize);

    // ------------------------------------------------------------------
    // sceAgcCbReleaseMem (AgcExports.cs:1272-1331) — 8-dword NOP/RReleaseMem
    // (the NOP form, not IT_RELEASE_MEM); six SysV stack arguments.
    // ------------------------------------------------------------------
    auto CbReleaseMem = [](const GuestArgs& args) -> u64 {
        const guest_addr_t cb = args.arg1;
        const u32 action       = static_cast<u32>(args.arg2) & 0xFF;
        const u32 gcr_control  = static_cast<u32>(args.arg3) & 0xFFFF;
        const u32 destination  = static_cast<u32>(args.arg4) & 0xFF;
        const u32 cache_policy = static_cast<u32>(args.arg5) & 0xFF;
        const u64 dst_addr     = args.arg6;
        const u64 data_selection = Memory::Read<u64>(args.stack_args + 0) & 0xFF;
        const u64 data           = Memory::Read<u64>(args.stack_args + 8);
        const u64 gds_offset     = Memory::Read<u64>(args.stack_args + 16) & 0xFFFF;
        const u64 gds_size       = Memory::Read<u64>(args.stack_args + 24) & 0xFFFF;
        const u64 interrupt      = Memory::Read<u64>(args.stack_args + 32) & 0xFF;
        const u64 interrupt_ctx  = Memory::Read<u64>(args.stack_args + 40);
        if (cb == 0 || destination > 1 || data_selection > 3 || gds_offset != 0 ||
            gds_size > 2 || interrupt > 3) {
            return 0;
        }
        const guest_addr_t cmd = AllocCommandDwords(cb, 8);
        if (!cmd) return 0;
        Memory::Write<u32>(cmd + 0, Pm4(8, kItNop, kRReleaseMem));
        Memory::Write<u32>(cmd + 4, action | (cache_policy << 8));
        Memory::Write<u32>(cmd + 8, static_cast<u32>(gcr_control | (data_selection << 16) |
                                                     (interrupt << 24)));
        Memory::Write<u32>(cmd + 12, static_cast<u32>(dst_addr & 0xFFFFFFFFull));
        Memory::Write<u32>(cmd + 16, static_cast<u32>(dst_addr >> 32));
        Memory::Write<u32>(cmd + 20, static_cast<u32>(data & 0xFFFFFFFFull));
        Memory::Write<u32>(cmd + 24, static_cast<u32>(data >> 32));
        Memory::Write<u32>(cmd + 28, static_cast<u32>(interrupt_ctx & 0xFFFFFFFFull));
        return cmd;
    };
    RegisterSymbol("libSceAgc", "sceAgcCbReleaseMem", CbReleaseMem);
    RegisterSymbol("libSceAgc", "wr23dPKyWc0", CbReleaseMem);

    auto DcbSetIndexSize = [](const GuestArgs& args) -> u64 {
        const guest_addr_t cb = args.arg1;
        const u32 index_size = static_cast<u32>(args.arg2) & 0xFF;
        const u32 cache_policy = static_cast<u32>(args.arg3) & 0xFF;
        if (cb == 0 || cache_policy != 0) return 0;
        const guest_addr_t cmd = AllocCommandDwords(cb, 2);
        if (!cmd) return 0;
        Memory::Write<u32>(cmd, Pm4(2, kItIndexType, 0));
        Memory::Write<u32>(cmd + 4, index_size);
        return cmd;
    };
    RegisterSymbol("libSceAgc", "sceAgcDcbSetIndexSize", DcbSetIndexSize);
    RegisterSymbol("libSceAgc", "GIIW2J37e70", DcbSetIndexSize);

    auto DcbSetIndexCount = [](const GuestArgs& args) -> u64 {
        const guest_addr_t cb = args.arg1;
        const u32 count = static_cast<u32>(args.arg2);
        if (cb == 0) return 0;
        const guest_addr_t cmd = AllocCommandDwords(cb, 2);
        if (!cmd) return 0;
        Memory::Write<u32>(cmd, Pm4(2, kItNop, kRIndexCount));
        Memory::Write<u32>(cmd + 4, count);
        return cmd;
    };
    RegisterSymbol("libSceAgc", "sceAgcDcbSetIndexCount", DcbSetIndexCount);
    RegisterSymbol("libSceAgc", "8N2tmT3jmC8", DcbSetIndexCount);

    auto DcbSetIndexCountGetSize = [](const GuestArgs& args) -> u64 {
        (void)args;
        return 7u * sizeof(u32);
    };
    RegisterSymbol("libSceAgc", "sceAgcDcbSetIndexCountGetSize", DcbSetIndexCountGetSize);
    RegisterSymbol("libSceAgc", "mljzuGDZRQ4", DcbSetIndexCountGetSize);

    auto DcbSetNumInstances = [](const GuestArgs& args) -> u64 {
        const guest_addr_t cb = args.arg1;
        const u32 count = static_cast<u32>(args.arg2);
        if (cb == 0) return 0;
        const guest_addr_t cmd = AllocCommandDwords(cb, 2);
        if (!cmd) return 0;
        Memory::Write<u32>(cmd, Pm4(2, kItNumInstances, 0));
        Memory::Write<u32>(cmd + 4, count);
        return cmd;
    };
    RegisterSymbol("libSceAgc", "sceAgcDcbSetNumInstances", DcbSetNumInstances);
    RegisterSymbol("libSceAgc", "tSBxhAPyytQ", DcbSetNumInstances);

    // Emits INDEX_BASE + INDEX_BUFFER_SIZE (5 dwords); shared by
    // DcbSetIndexBuffer and the first half of DcbDrawIndex.
    auto EmitIndexBaseAndSize = [](guest_addr_t cb, u64 ib_addr, u32 count) -> guest_addr_t {
        const guest_addr_t cmd = AllocCommandDwords(cb, 5);
        if (!cmd) return 0;
        Memory::Write<u32>(cmd, Pm4(3, kItIndexBase, 0));
        Memory::Write<u32>(cmd + 4, static_cast<u32>(ib_addr & 0xFFFFFFFFull));
        Memory::Write<u32>(cmd + 8, static_cast<u32>(ib_addr >> 32));
        Memory::Write<u32>(cmd + 12, Pm4(2, kItIndexBufferSize, 0));
        Memory::Write<u32>(cmd + 16, count);
        return cmd;
    };

    auto DcbSetIndexBuffer = [EmitIndexBaseAndSize](const GuestArgs& args) -> u64 {
        const guest_addr_t cb = args.arg1;
        if (cb == 0) return 0;
        return EmitIndexBaseAndSize(cb, args.arg2, static_cast<u32>(args.arg3));
    };
    RegisterSymbol("libSceAgc", "sceAgcDcbSetIndexBuffer", DcbSetIndexBuffer);
    RegisterSymbol("libSceAgc", "l4fM9K-Lyks", DcbSetIndexBuffer);

    auto DcbDrawIndex = [EmitIndexBaseAndSize](const GuestArgs& args) -> u64 {
        const guest_addr_t cb = args.arg1;
        const u32 count = static_cast<u32>(args.arg2);
        const u64 index_addr = args.arg3;
        const u32 modifier = static_cast<u32>(args.arg4);
        if (cb == 0 || modifier != 0x40000000u) return 0;
        if (!EmitIndexBaseAndSize(cb, index_addr, count)) return 0;
        // DRAW_INDEX_2: header, max count, 64-bit index base, count, initiator.
        const guest_addr_t cmd = AllocCommandDwords(cb, 6);
        if (!cmd) return 0;
        Memory::Write<u32>(cmd, Pm4(6, kItDrawIndex2, 0));
        Memory::Write<u32>(cmd + 4, count);
        Memory::Write<u32>(cmd + 8, static_cast<u32>(index_addr & 0xFFFFFFFFull));
        Memory::Write<u32>(cmd + 12, static_cast<u32>(index_addr >> 32));
        Memory::Write<u32>(cmd + 16, count);
        Memory::Write<u32>(cmd + 20, 0);
        return cmd;
    };
    RegisterSymbol("libSceAgc", "sceAgcDcbDrawIndex", DcbDrawIndex);
    RegisterSymbol("libSceAgc", "q88lQ+GP5Yk", DcbDrawIndex);

    auto DcbDrawIndexAuto = [](const GuestArgs& args) -> u64 {
        const guest_addr_t cb = args.arg1;
        const u32 count = static_cast<u32>(args.arg2);
        const u64 modifier = args.arg3;
        if (cb == 0 || modifier != 0x40000000ull) return 0;
        const guest_addr_t cmd = AllocCommandDwords(cb, 7);
        if (!cmd) return 0;
        Memory::Write<u32>(cmd, Pm4(7, kItNop, kRDrawIndexAuto));
        for (u32 i = 0; i < 6; ++i) {
            Memory::Write<u32>(cmd + 4 + static_cast<u64>(i) * 4, i == 0 ? count : 0);
        }
        return cmd;
    };
    RegisterSymbol("libSceAgc", "sceAgcDcbDrawIndexAuto", DcbDrawIndexAuto);
    RegisterSymbol("libSceAgc", "Yw0jKSqop+E", DcbDrawIndexAuto);

    auto DcbDrawIndexOffset = [](const GuestArgs& args) -> u64 {
        const guest_addr_t cb = args.arg1;
        const u32 index_offset = static_cast<u32>(args.arg2);
        const u32 count = static_cast<u32>(args.arg3);
        const u32 flags = static_cast<u32>(args.arg4);
        if (cb == 0) return 0;
        const guest_addr_t cmd = AllocCommandDwords(cb, 5);
        if (!cmd) return 0;
        Memory::Write<u32>(cmd, Pm4(5, kItDrawIndexOffset2, 0));
        Memory::Write<u32>(cmd + 4, count);
        Memory::Write<u32>(cmd + 8, index_offset);
        Memory::Write<u32>(cmd + 12, count);
        Memory::Write<u32>(cmd + 16, flags & 0xE0000001u);
        return cmd;
    };
    RegisterSymbol("libSceAgc", "sceAgcDcbDrawIndexOffset", DcbDrawIndexOffset);
    RegisterSymbol("libSceAgc", "B+aG9DUnTKA", DcbDrawIndexOffset);

    auto DcbDrawIndexIndirect = [](const GuestArgs& args) -> u64 {
        const guest_addr_t cb = args.arg1;
        const u32 data_offset = static_cast<u32>(args.arg2);
        const u32 modifier = static_cast<u32>(args.arg3);
        if (cb == 0) return 0;
        const guest_addr_t cmd = AllocCommandDwords(cb, 5);
        if (!cmd) return 0;
        Memory::Write<u32>(cmd, Pm4(5, kItDrawIndexIndirect, 0));
        Memory::Write<u32>(cmd + 4, data_offset);
        Memory::Write<u32>(cmd + 8, 0);
        Memory::Write<u32>(cmd + 12, 0);
        Memory::Write<u32>(cmd + 16, modifier);
        return cmd;
    };
    RegisterSymbol("libSceAgc", "sceAgcDcbDrawIndexIndirect", DcbDrawIndexIndirect);
    RegisterSymbol("libSceAgc", "t1vNu082-jM", DcbDrawIndexIndirect);

    auto DcbDrawIndexIndirectGetSize = [](const GuestArgs& args) -> u64 {
        (void)args;
        return 5u * sizeof(u32);
    };
    RegisterSymbol("libSceAgc", "sceAgcDcbDrawIndexIndirectGetSize", DcbDrawIndexIndirectGetSize);
    RegisterSymbol("libSceAgc", "mStuvI0zOtc", DcbDrawIndexIndirectGetSize);

    auto DcbEventWrite = [](const GuestArgs& args) -> u64 {
        const guest_addr_t cb = args.arg1;
        const u32 event_type = static_cast<u32>(args.arg2) & 0xFF;
        const u64 event_addr = args.arg3;
        if (cb == 0 || event_type > 0x3F || event_addr != 0) return 0;
        const guest_addr_t cmd = AllocCommandDwords(cb, 2);
        if (!cmd) return 0;
        Memory::Write<u32>(cmd, Pm4(2, kItEventWrite, 0));
        Memory::Write<u32>(cmd + 4, event_type);
        return cmd;
    };
    RegisterSymbol("libSceAgc", "sceAgcDcbEventWrite", DcbEventWrite);
    RegisterSymbol("libSceAgc", "aJf+j5yntiU", DcbEventWrite);
    RegisterSymbol("libSceAgc", "sceAgcAcbEventWrite", DcbEventWrite);
    RegisterSymbol("libSceAgc", "cFazmnXpJOE", DcbEventWrite);

    auto CbDispatch = [](const GuestArgs& args) -> u64 {
        const guest_addr_t cb = args.arg1;
        const u32 gx = static_cast<u32>(args.arg2);
        const u32 gy = static_cast<u32>(args.arg3);
        const u32 gz = static_cast<u32>(args.arg4);
        const u32 modifier = static_cast<u32>(args.arg5);
        if (cb == 0) return 0;
        const guest_addr_t cmd = AllocCommandDwords(cb, 5);
        if (!cmd) return 0;
        Memory::Write<u32>(cmd, Pm4(5, kItDispatchDirect, 0));
        Memory::Write<u32>(cmd + 4, gx);
        Memory::Write<u32>(cmd + 8, gy);
        Memory::Write<u32>(cmd + 12, gz);
        Memory::Write<u32>(cmd + 16, (modifier & 0xA038u) | 0x41u);
        return cmd;
    };
    RegisterSymbol("libSceAgc", "sceAgcCbDispatch", CbDispatch);
    RegisterSymbol("libSceAgc", "k3GhuSNmBLU", CbDispatch);

    auto DcbDispatchIndirect = [](const GuestArgs& args) -> u64 {
        const guest_addr_t cb = args.arg1;
        const u32 data_offset = static_cast<u32>(args.arg2);
        const u32 modifier = static_cast<u32>(args.arg3);
        if (cb == 0) return 0;
        const guest_addr_t cmd = AllocCommandDwords(cb, 3);
        if (!cmd) return 0;
        Memory::Write<u32>(cmd, Pm4(3, kItDispatchIndirect, 0));
        Memory::Write<u32>(cmd + 4, data_offset);
        Memory::Write<u32>(cmd + 8, (modifier & 0xA038u) | 0x41u);
        return cmd;
    };
    RegisterSymbol("libSceAgc", "sceAgcDcbDispatchIndirect", DcbDispatchIndirect);
    RegisterSymbol("libSceAgc", "CtB+A9-VxO0", DcbDispatchIndirect);
    RegisterSymbol("libSceAgc", "sceAgcAcbDispatchIndirect", DcbDispatchIndirect);
    RegisterSymbol("libSceAgc", "j3EtxFkSIhQ", DcbDispatchIndirect);

    auto DcbSetBaseIndirectArgs = [](const GuestArgs& args) -> u64 {
        const guest_addr_t cb = args.arg1;
        const u32 base_index = static_cast<u32>(args.arg2);
        const u64 addr = args.arg3;
        if (cb == 0) return 0;
        const guest_addr_t cmd = AllocCommandDwords(cb, 4);
        if (!cmd) return 0;
        Memory::Write<u32>(cmd, Pm4(4, kItSetBase, 0) | (base_index << 1));
        Memory::Write<u32>(cmd + 4, 1);
        Memory::Write<u32>(cmd + 8, static_cast<u32>(addr) & ~7u);
        Memory::Write<u32>(cmd + 12, static_cast<u32>(addr >> 32));
        return cmd;
    };
    RegisterSymbol("libSceAgc", "sceAgcDcbSetBaseIndirectArgs", DcbSetBaseIndirectArgs);
    RegisterSymbol("libSceAgc", "RmaJwLtc8rY", DcbSetBaseIndirectArgs);

    auto DcbPushMarker = [](const GuestArgs& args) -> u64 {
        const guest_addr_t cb = args.arg1;
        const guest_addr_t marker_addr = args.arg2;
        if (cb == 0 || marker_addr == 0) return 0;
        // Read the guest C string (bounded).
        u32 len = 0;
        while (len < 4095 && Memory::Read<u8>(marker_addr + len) != 0) ++len;
        const u32 payload = (len + 4) / 4 > 1 ? (len + 4) / 4 : 1;
        const u32 packet_dwords = payload + 1;
        const guest_addr_t cmd = AllocCommandDwords(cb, packet_dwords);
        if (!cmd) return 0;
        Memory::Write<u32>(cmd, Pm4(packet_dwords, kItNop, kRPushMarker));
        for (u32 i = 0; i < payload; ++i) {
            u32 v = 0;
            for (u32 b = 0; b < 4; ++b) {
                const u32 mi = i * 4 + b;
                if (mi < len) v |= static_cast<u32>(Memory::Read<u8>(marker_addr + mi)) << (b * 8);
            }
            Memory::Write<u32>(cmd + 4 + static_cast<u64>(i) * 4, v);
        }
        return cmd;
    };
    RegisterSymbol("libSceAgc", "sceAgcDcbPushMarker", DcbPushMarker);
    RegisterSymbol("libSceAgc", "+kSrjIVxKFE", DcbPushMarker);
    RegisterSymbol("libSceAgc", "sceAgcAcbPushMarker", DcbPushMarker);
    RegisterSymbol("libSceAgc", "cpCILPya5Zk", DcbPushMarker);

    auto DcbPopMarker = [](const GuestArgs& args) -> u64 {
        const guest_addr_t cb = args.arg1;
        if (cb == 0) return 0;
        const guest_addr_t cmd = AllocCommandDwords(cb, 2);
        if (!cmd) return 0;
        Memory::Write<u32>(cmd, Pm4(2, kItNop, kRPopMarker));
        Memory::Write<u32>(cmd + 4, 0);
        return cmd;
    };
    RegisterSymbol("libSceAgc", "sceAgcDcbPopMarker", DcbPopMarker);
    RegisterSymbol("libSceAgc", "H7uZqCoNuWk", DcbPopMarker);
    RegisterSymbol("libSceAgc", "sceAgcAcbPopMarker", DcbPopMarker);
    RegisterSymbol("libSceAgc", "6mFxkVqdmbQ", DcbPopMarker);

    auto DcbWaitUntilSafeForRendering = [](const GuestArgs& args) -> u64 {
        const guest_addr_t cb = args.arg1;
        const u32 handle = static_cast<u32>(args.arg2);
        const u32 buf_index = static_cast<u32>(args.arg3);
        if (cb == 0) return 0;
        const guest_addr_t cmd = AllocCommandDwords(cb, 7);
        if (!cmd) return 0;
        Memory::Write<u32>(cmd, Pm4(7, kItNop, kRWaitFlipDone));
        Memory::Write<u32>(cmd + 4, handle);
        Memory::Write<u32>(cmd + 8, buf_index);
        for (u32 i = 2; i < 6; ++i) Memory::Write<u32>(cmd + static_cast<u64>(i) * 4, 0);
        return cmd;
    };
    RegisterSymbol("libSceAgc", "sceAgcDcbWaitUntilSafeForRendering", DcbWaitUntilSafeForRendering);
    RegisterSymbol("libSceAgc", "MWiElSNE8j8", DcbWaitUntilSafeForRendering);

    // Emits a well-formed no-op: there is no independent hardware command
    // processor to stall, but the packet keeps cursors/addresses coherent.
    auto DcbStallCommandBufferParser = [](const GuestArgs& args) -> u64 {
        const guest_addr_t cb = args.arg1;
        if (cb == 0) return 0;
        const guest_addr_t cmd = AllocCommandDwords(cb, 2);
        if (!cmd) return 0;
        Memory::Write<u32>(cmd, Pm4(2, kItNop, kRZero));
        Memory::Write<u32>(cmd + 4, 0);
        return cmd;
    };
    RegisterSymbol("libSceAgc", "sceAgcDcbStallCommandBufferParser", DcbStallCommandBufferParser);
    RegisterSymbol("libSceAgc", "u2T2DiA5hRI", DcbStallCommandBufferParser);

    auto DcbStallCommandBufferParserGetSize = [](const GuestArgs& args) -> u64 {
        (void)args;
        return 2u * sizeof(u32);
    };
    RegisterSymbol("libSceAgc", "sceAgcDcbStallCommandBufferParserGetSize",
                   DcbStallCommandBufferParserGetSize);
    RegisterSymbol("libSceAgc", "+u6dKSLWM2o", DcbStallCommandBufferParserGetSize);

    auto DcbSetFlip = [](const GuestArgs& args) -> u64 {
        const guest_addr_t cb = args.arg1;
        const u32 handle = static_cast<u32>(args.arg2);
        const s32 buf_index = static_cast<s32>(args.arg3);
        const u32 flip_mode = static_cast<u32>(args.arg4);
        const u64 flip_arg = args.arg5;
        if (cb == 0) return 0;
        const guest_addr_t cmd = AllocCommandDwords(cb, 6);
        if (!cmd) return 0;
        Memory::Write<u32>(cmd, Pm4(6, kItNop, kRFlip));
        Memory::Write<u32>(cmd + 4, handle);
        Memory::Write<u32>(cmd + 8, static_cast<u32>(buf_index));
        Memory::Write<u32>(cmd + 12, flip_mode);
        Memory::Write<u32>(cmd + 16, static_cast<u32>(flip_arg & 0xFFFFFFFFull));
        Memory::Write<u32>(cmd + 20, static_cast<u32>(flip_arg >> 32));
        return cmd;
    };
    RegisterSymbol("libSceAgc", "sceAgcDcbSetFlip", DcbSetFlip);
    RegisterSymbol("libSceAgc", "YUeqkyT7mEQ", DcbSetFlip);

    // ------------------------------------------------------------------
    // Driver: submit entry points (packet-stream walker).
    // ------------------------------------------------------------------
    auto DriverSubmitDcb = [](const GuestArgs& args) -> u64 {
        const guest_addr_t packet_ptr = args.arg1;
        if (packet_ptr == 0) return 0;
        const u64 cmd_addr = Memory::Read<u64>(packet_ptr);
        const u32 dwords = Memory::Read<u32>(packet_ptr + 8);
        LOG_DEBUG(HLE, "sceAgcDriverSubmitDcb(addr: 0x%llx, dwords: %u)", cmd_addr, dwords);
        std::lock_guard<std::mutex> lk(g_agc_submit_mutex);
        WalkCommandBuffer(cmd_addr, dwords, g_agc_graphics_shadow, "dcb.graphics");
        return 0;
    };
    RegisterSymbol("libSceAgcDriver", "sceAgcDriverSubmitDcb", DriverSubmitDcb);
    RegisterSymbol("libSceAgcDriver", "UglJIZjGssM", DriverSubmitDcb);

    auto DriverSubmitAcb = [](const GuestArgs& args) -> u64 {
        const u32 owner = static_cast<u32>(args.arg1);
        const guest_addr_t packet_ptr = args.arg2;
        if (packet_ptr == 0) return 0;
        const u64 cmd_addr = Memory::Read<u64>(packet_ptr);
        const u32 dwords = Memory::Read<u32>(packet_ptr + 8);
        LOG_DEBUG(HLE, "sceAgcDriverSubmitAcb(owner: %u, addr: 0x%llx, dwords: %u)",
                  owner, cmd_addr, dwords);
        std::lock_guard<std::mutex> lk(g_agc_submit_mutex);
        WalkCommandBuffer(cmd_addr, dwords, g_agc_compute_shadows[owner], "acb.compute");
        return 0;
    };
    RegisterSymbol("libSceAgcDriver", "sceAgcDriverSubmitAcb", DriverSubmitAcb);
    RegisterSymbol("libSceAgcDriver", "gSRnr79F8tQ", DriverSubmitAcb);

    // ABI (reversed from Quake by SharpEmu): rdi = u64 address array,
    // rsi = u32 dword-count array, rdx = buffer count.
    auto DriverSubmitMultiDcbs = [](const GuestArgs& args) -> u64 {
        const guest_addr_t addr_array = args.arg1;
        const guest_addr_t size_array = args.arg2;
        const u32 count = static_cast<u32>(args.arg3);
        if (addr_array == 0 || size_array == 0 || count == 0 || count > 4096) return 0;
        std::lock_guard<std::mutex> lk(g_agc_submit_mutex);
        for (u32 i = 0; i < count; ++i) {
            const u64 cmd_addr = Memory::Read<u64>(addr_array + static_cast<u64>(i) * 8);
            const u32 dwords = Memory::Read<u32>(size_array + static_cast<u64>(i) * 4);
            if (cmd_addr == 0 || dwords == 0) continue;
            WalkCommandBuffer(cmd_addr, dwords, g_agc_graphics_shadow, "dcb.graphics");
        }
        return 0;
    };
    RegisterSymbol("libSceAgcDriver", "sceAgcDriverSubmitMultiDcbs", DriverSubmitMultiDcbs);
    RegisterSymbol("libSceAgcDriver", "6UzEidRZwkg", DriverSubmitMultiDcbs);

    // ------------------------------------------------------------------
    // Driver lifecycle (kept stubs / new success contracts).
    // ------------------------------------------------------------------
    auto AgcDriverInitialize = [](const GuestArgs& args) -> u64 {
        (void)args;
        LOG_INFO(HLE, "sceAgcDriverInitialize() called");
        return 0;
    };
    RegisterSymbol("libSceAgcDriver", "sceAgcDriverInitialize", AgcDriverInitialize);
    RegisterSymbol("libSceAgcDriver", "1kZFcktOm+s", AgcDriverInitialize);
    RegisterSymbol("libSceAgc", "sceAgcDriverInitialize", AgcDriverInitialize);
    RegisterSymbol("libSceAgc", "1kZFcktOm+s", AgcDriverInitialize);

    auto AgcDriverUninitialize = [](const GuestArgs& args) -> u64 {
        (void)args;
        LOG_INFO(HLE, "sceAgcDriverUninitialize() -> 0");
        return 0;
    };
    RegisterSymbol("libSceAgc", "sceAgcDriverUninitialize", AgcDriverUninitialize);
    RegisterSymbol("libSceAgc", "-L+-8F0+gBc", AgcDriverUninitialize);
    RegisterSymbol("libSceAgcDriver", "sceAgcDriverUninitialize", AgcDriverUninitialize);
    RegisterSymbol("libSceAgcDriver", "-L+-8F0+gBc", AgcDriverUninitialize);

    RegisterSymbol("libSceAgcDriver", "sceAgcDriverCreateDevice", [](const GuestArgs& args) -> u64 {
        guest_addr_t dev_out = args.arg1;
        LOG_INFO(HLE, "sceAgcDriverCreateDevice() called");
        if (dev_out) {
            Memory::Write<u64>(dev_out, 0x6000); // Mock device handle
        }
        return 0;
    });

    auto AgcDriverMapMemoryImpl = [](const GuestArgs& args) -> u64 {
        u64 dev = args.arg1;
        guest_addr_t addr = args.arg2;
        u64 size = args.arg3;
        u32 type = static_cast<u32>(args.arg4);
        LOG_DEBUG(HLE, "sceAgcDriverMapMemory(device: 0x%llx, addr: 0x%llx, size: %llu, type: %u)",
                  dev, addr, size, type);
        return 0;
    };
    RegisterSymbol("libSceAgcDriver", "sceAgcDriverMapMemory", AgcDriverMapMemoryImpl);
    RegisterSymbol("libSceAgcDriver", "9UK1vLZQft4#y#J", AgcDriverMapMemoryImpl);

    RegisterSymbol("libSceAgcDriver", "tn3VlD0hG60#k#N", [](const GuestArgs& args) -> u64 {
        u64 device = args.arg1;
        u64 addr = args.arg2;
        u64 host_ptr = args.arg3;
        LOG_DEBUG(HLE, "tn3VlD0hG60#k#N called: device=0x%llx, addr=0x%llx, host_ptr=0x%llx",
                  device, addr, host_ptr);

        // LOST EPIC treats the return value as a mapped host pointer and
        // writes through it immediately; returning 0 caused a null write.
        // Back it with a persistent 1 MB guest-visible buffer.
        static guest_addr_t s_agc_host_buffer = 0;
        if (!s_agc_host_buffer) {
            if (Memory::Map(0, 1 * 1024 * 1024,
                            Memory::PROT_READ | Memory::PROT_WRITE,
                            &s_agc_host_buffer) != Memory::Status::Ok) {
                LOG_ERROR(HLE, "tn3VlD0hG60#k#N: failed to map host buffer!");
                return 0;
            }
            LOG_INFO(HLE, "tn3VlD0hG60#k#N: mapped host buffer at 0x%llx", s_agc_host_buffer);
        }
        return s_agc_host_buffer;
    });

    RegisterSymbol("libSceAgcDriver", "Ujf3KzMvRmI#j#j", [](const GuestArgs& args) -> u64 {
        u64 align = args.arg1;
        u64 size = args.arg2;
        u64 ctx = args.arg3;
        LOG_DEBUG(HLE, "Ujf3KzMvRmI#j#j (allocator) called: align=%llu, size=%llu, ctx=0x%llx",
                  align, size, ctx);

        // Allocate guest virtual memory for the request
        guest_addr_t allocated_addr = 0;
        u64 rounded_size = (size + 0x3FFF) & ~0x3FFFull; // round up to page size (16KB)
        if (Memory::Map(0, rounded_size, Memory::PROT_READ | Memory::PROT_WRITE, &allocated_addr) == Memory::Status::Ok) {
            LOG_DEBUG(HLE, "  Allocated memory at 0x%llx (size=%llu)", allocated_addr, rounded_size);
            return allocated_addr;
        }
        LOG_ERROR(HLE, "  Failed to allocate memory!");
        return 0;
    });
}

} // namespace HLE
