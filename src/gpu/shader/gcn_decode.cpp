// RDNA2 (GCN Gen5 / gfx10) shader instruction stream decoder.
// Guided transliteration of SharpEmu's Gen5ShaderTranslator decode half —
// see gcn_decode.h for the contract.

#include "gcn_decode.h"

#include <cstdio>
#include <iterator>

namespace GPU::Shader {

namespace {

struct OpcodeName {
    u32         op;
    const char* name;
};

const char* Lookup(const OpcodeName* table, size_t count, u32 op) {
    for (size_t i = 0; i < count; ++i) {
        if (table[i].op == op) {
            return table[i].name;
        }
    }
    return nullptr;
}

std::string RawName(const char* prefix, u32 op, int width) {
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%sRaw0x%0*X", prefix, width, op);
    return buffer;
}

s32 SignExtend(u32 value, int bits) {
    const int shift = 32 - bits;
    return static_cast<s32>(value << shift) >> shift;
}

// ---------------------------------------------------------------------------
// Opcode tables — ported from Gen5ShaderTranslator.cs DecodeSop1..DecodeVintrp.
// ---------------------------------------------------------------------------

constexpr OpcodeName kSop1Names[] = {
    {0x03, "SMovB32"},           {0x04, "SMovB64"},
    {0x07, "SNotB32"},           {0x08, "SNotB64"},
    {0x0A, "SWqmB64"},           {0x0B, "SBrevB32"},
    {0x0F, "SBcnt1I32B32"},      {0x13, "SFF1I32B32"},
    {0x1D, "SBitset1B32"},       {0x1F, "SGetpcB64"},
    {0x20, "SSetpcB64"},         {0x21, "SSwappcB64"},
    {0x24, "SAndSaveexecB64"},   {0x25, "SOrSaveexecB64"},
    {0x26, "SXorSaveexecB64"},   {0x27, "SAndn2SaveexecB64"},
    {0x28, "SOrn2SaveexecB64"},  {0x29, "SNandSaveexecB64"},
    {0x2A, "SNorSaveexecB64"},   {0x2B, "SXnorSaveexecB64"},
    {0x37, "SAndn1SaveexecB64"}, {0x38, "SOrn1SaveexecB64"},
    {0x3C, "SAndSaveexecB32"},   {0x3D, "SOrSaveexecB32"},
    {0x3E, "SXorSaveexecB32"},   {0x3F, "SAndn2SaveexecB32"},
    {0x40, "SOrn2SaveexecB32"},  {0x41, "SNandSaveexecB32"},
    {0x42, "SNorSaveexecB32"},   {0x43, "SXnorSaveexecB32"},
    {0x44, "SAndn1SaveexecB32"}, {0x45, "SOrn1SaveexecB32"},
};

constexpr OpcodeName kSop2Names[] = {
    {0x00, "SAddU32"},       {0x01, "SSubU32"},
    {0x02, "SAddI32"},       {0x03, "SSubI32"},
    {0x04, "SAddcU32"},      {0x05, "SSubbU32"},
    {0x06, "SMinI32"},       {0x07, "SMinU32"},
    {0x08, "SMaxI32"},       {0x09, "SMaxU32"},
    {0x0A, "SCselectB32"},   {0x0B, "SCselectB64"},
    {0x0E, "SAndB32"},       {0x0F, "SAndB64"},
    {0x10, "SOrB32"},        {0x11, "SOrB64"},
    {0x12, "SXorB32"},       {0x13, "SXorB64"},
    {0x14, "SAndn2B32"},     {0x15, "SAndn2B64"},
    {0x16, "SOrn2B32"},      {0x17, "SOrn2B64"},
    {0x18, "SNandB32"},      {0x19, "SNandB64"},
    {0x1A, "SNorB32"},       {0x1B, "SNorB64"},
    {0x1C, "SXnorB32"},      {0x1D, "SXnorB64"},
    {0x1E, "SLshlB32"},      {0x1F, "SLshlB64"},
    {0x20, "SLshrB32"},      {0x21, "SLshrB64"},
    {0x22, "SAshrI32"},      {0x23, "SAshrI64"},
    {0x24, "SBfmB32"},       {0x25, "SBfmB64"},
    {0x26, "SMulI32"},       {0x27, "SBfeU32"},
    {0x28, "SBfeI32"},       {0x29, "SBfeU64"},
    {0x2A, "SBfeI64"},       {0x2D, "SAbsdiffI32"},
    {0x2E, "SLshl1AddU32"},  {0x2F, "SLshl2AddU32"},
    {0x30, "SLshl3AddU32"},  {0x31, "SLshl4AddU32"},
    {0x32, "SPackLlB32B16"}, {0x33, "SPackLhB32B16"},
    {0x34, "SPackHhB32B16"}, {0x35, "SMulHiU32"},
    {0x36, "SMulHiI32"},
};

constexpr OpcodeName kSopcNames[] = {
    {0x00, "SCmpEqI32"},   {0x01, "SCmpLgI32"},
    {0x02, "SCmpGtI32"},   {0x03, "SCmpGeI32"},
    {0x04, "SCmpLtI32"},   {0x05, "SCmpLeI32"},
    {0x06, "SCmpEqU32"},   {0x07, "SCmpLgU32"},
    {0x08, "SCmpGtU32"},   {0x09, "SCmpGeU32"},
    {0x0A, "SCmpLtU32"},   {0x0B, "SCmpLeU32"},
    {0x0C, "SBitcmp0B32"}, {0x0D, "SBitcmp1B32"},
    {0x0E, "SBitcmp0B64"}, {0x0F, "SBitcmp1B64"},
};

constexpr OpcodeName kSoppNames[] = {
    {0x00, "SNop"},            {0x01, "SEndpgm"},
    {0x02, "SBranch"},         {0x04, "SCbranchScc0"},
    {0x05, "SCbranchScc1"},    {0x06, "SCbranchVccz"},
    {0x07, "SCbranchVccnz"},   {0x08, "SCbranchExecz"},
    {0x09, "SCbranchExecnz"},  {0x0A, "SBarrier"},
    {0x0C, "SWaitcnt"},        {0x10, "SSendmsg"},
    {0x16, "STtraceData"},     {0x20, "SInstPrefetch"},
    {0x21, "SClause"},         {0x23, "SWaitcntDepctr"},
};

constexpr OpcodeName kSopkNames[] = {
    {0x00, "SMovkI32"},    {0x03, "SCmpkEqI32"},
    {0x04, "SCmpkLgI32"},  {0x05, "SCmpkGtI32"},
    {0x06, "SCmpkGeI32"},  {0x07, "SCmpkLtI32"},
    {0x08, "SCmpkLeI32"},  {0x09, "SCmpkEqU32"},
    {0x0A, "SCmpkLgU32"},  {0x0B, "SCmpkGtU32"},
    {0x0C, "SCmpkGeU32"},  {0x0D, "SCmpkLtU32"},
    {0x0E, "SCmpkLeU32"},  {0x0F, "SAddkI32"},
    {0x10, "SMulkI32"},
};

constexpr OpcodeName kVop1Names[] = {
    {0x00, "VNop"},           {0x01, "VMovB32"},
    {0x02, "VReadfirstlaneB32"},
    {0x05, "VCvtF32I32"},     {0x06, "VCvtF32U32"},
    {0x07, "VCvtU32F32"},     {0x08, "VCvtI32F32"},
    {0x0A, "VCvtF16F32"},     {0x0B, "VCvtF32F16"},
    {0x0C, "VCvtRpiI32F32"},  {0x0D, "VCvtFlrI32F32"},
    {0x0E, "VCvtOffF32I4"},   {0x11, "VCvtF32Ubyte0"},
    {0x12, "VCvtF32Ubyte1"},  {0x13, "VCvtF32Ubyte2"},
    {0x14, "VCvtF32Ubyte3"},  {0x20, "VFractF32"},
    {0x21, "VTruncF32"},      {0x22, "VCeilF32"},
    {0x23, "VRndneF32"},      {0x24, "VFloorF32"},
    {0x25, "VExpF32"},        {0x27, "VLogF32"},
    {0x2A, "VRcpF32"},        {0x2B, "VRcpIflagF32"},
    {0x2E, "VRsqF32"},        {0x33, "VSqrtF32"},
    {0x35, "VSinF32"},        {0x36, "VCosF32"},
    {0x37, "VNotB32"},        {0x38, "VBfrevB32"},
    {0x3A, "VFfblB32"},       {0x42, "VMovreldB32"},
    {0x43, "VMovrelsB32"},    {0x44, "VMovrelsdB32"},
};

constexpr OpcodeName kVop2Names[] = {
    {0x01, "VCndmaskB32"},    {0x02, "VDot2cF32F16"},
    {0x03, "VAddF32"},        {0x04, "VSubF32"},
    {0x05, "VSubrevF32"},     {0x08, "VMulF32"},
    {0x0B, "VMulU32U24"},     {0x0C, "VMulHiU32U24"},
    {0x0F, "VMinF32"},        {0x10, "VMaxF32"},
    {0x11, "VMinI32"},        {0x12, "VMaxI32"},
    {0x13, "VMinU32"},        {0x14, "VMaxU32"},
    {0x15, "VLshrB32"},       {0x16, "VLshrrevB32"},
    {0x17, "VAshrI32"},       {0x18, "VAshrrevI32"},
    {0x19, "VLshlB32"},       {0x1A, "VLshlrevB32"},
    {0x1B, "VAndB32"},        {0x1C, "VOrB32"},
    {0x1D, "VXorB32"},        {0x1E, "VXnorB32"},
    {0x1F, "VMacF32"},        {0x20, "VMadMkF32"},
    {0x21, "VMadAkF32"},      {0x22, "VBcntU32B32"},
    {0x23, "VMbcntLoU32B32"}, {0x24, "VMbcntHiU32B32"},
    {0x25, "VAddI32"},        {0x26, "VSubI32"},
    {0x27, "VSubrevI32"},     {0x28, "VAddcU32"},
    {0x29, "VSubbU32"},       {0x2A, "VSubbrevU32"},
    {0x2B, "VFmacF32"},       {0x2C, "VFmaMkF32"},
    {0x2D, "VFmaAkF32"},      {0x2F, "VCvtPkrtzF16F32"},
    {0x30, "VCvtPkU16U32"},   {0x31, "VCvtPkI16I32"},
};

// VOPC float conditions (op 0x00-0x1F, +0x80/0xC0 integer sets share the
// condition suffix).
constexpr const char* kVopcConditions[16] = {
    "F", "Lt", "Eq", "Le", "Gt", "Lg", "Ge", "O",
    "U", "Nge", "Nlg", "Ngt", "Nle", "Neq", "Nlt", "Tru",
};

constexpr OpcodeName kVop3bNames[] = {
    {0x128, "VAddCoCiU32"}, {0x30F, "VAddCoU32"},
    {0x310, "VSubCoU32"},   {0x319, "VSubrevCoU32"},
    {0x176, "VMadU64U32"},
};

constexpr OpcodeName kVop3aNames[] = {
    {0x101, "VCndmaskB32"},    {0x103, "VAddF32"},
    {0x104, "VSubF32"},        {0x108, "VMulF32"},
    {0x10F, "VMinF32"},        {0x110, "VMaxF32"},
    {0x11F, "VMacF32"},        {0x12B, "VFmacF32"},
    {0x12F, "VCvtPkrtzF16F32"},{0x141, "VMadF32"},
    {0x143, "VMadU32U24"},     {0x144, "VCubeidF32"},
    {0x145, "VCubescF32"},     {0x146, "VCubetcF32"},
    {0x147, "VCubemaF32"},     {0x14A, "VBfiB32"},
    {0x14B, "VFmaF32"},        {0x151, "VMin3F32"},
    {0x152, "VMin3I32"},       {0x153, "VMin3U32"},
    {0x154, "VMax3F32"},       {0x155, "VMax3I32"},
    {0x156, "VMax3U32"},       {0x157, "VMed3F32"},
    {0x158, "VMed3I32"},       {0x159, "VMed3U32"},
    {0x15A, "VSadU8"},         {0x15B, "VSadHiU8"},
    {0x15C, "VSadU16"},        {0x15D, "VSadU32"},
    {0x15E, "VCvtPkU8F32"},    {0x148, "VBfeU32"},
    {0x169, "VMulLoU32"},      {0x16A, "VMulHiU32"},
    {0x16B, "VMulLoI32"},      {0x16C, "VMulHiI32"},
    {0x360, "VReadlaneB32"},   {0x361, "VWritelaneB32"},
    {0x362, "VLdexpF32"},      {0x363, "VBfmB32"},
    {0x364, "VBcntU32B32"},    {0x365, "VMbcntLoU32B32"},
    {0x366, "VMbcntHiU32B32"}, {0x368, "VCvtPknormI16F32"},
    {0x369, "VCvtPknormU16F32"},{0x373, "VMadU32U16"},
    {0x346, "VLshlAddU32"},    {0x347, "VAddLshlU32"},
    {0x36D, "VAdd3U32"},       {0x36F, "VLshlOrU32"},
    {0x371, "VAndOrB32"},      {0x372, "VOr3U32"},
    {0x377, "VPermlane16B32"}, {0x378, "VPermlanex16B32"},
};

// gfx9/gfx10 VOP3P packed 16-bit ops (LLVM AMDGPU VOP3PInstructions.td);
// SharpEmu carries 0x0E-0x12, the 0x00-0x0D integer-packed forms follow the
// same numbering.
constexpr OpcodeName kVop3pNames[] = {
    {0x00, "VPkMadI16"},     {0x01, "VPkMulLoU16"},
    {0x02, "VPkAddI16"},     {0x03, "VPkSubI16"},
    {0x04, "VPkLshlrevB16"}, {0x05, "VPkLshrrevB16"},
    {0x06, "VPkAshrrevI16"}, {0x07, "VPkMaxI16"},
    {0x08, "VPkMinI16"},     {0x09, "VPkMadU16"},
    {0x0A, "VPkAddU16"},     {0x0B, "VPkSubU16"},
    {0x0C, "VPkMaxU16"},     {0x0D, "VPkMinU16"},
    {0x0E, "VPkFmaF16"},     {0x0F, "VPkAddF16"},
    {0x10, "VPkMulF16"},     {0x11, "VPkMinF16"},
    {0x12, "VPkMaxF16"},
};

constexpr OpcodeName kDsNames[] = {
    {0x00, "DsAddU32"},        {0x01, "DsSubU32"},
    {0x03, "DsIncU32"},        {0x04, "DsDecU32"},
    {0x05, "DsMinI32"},        {0x06, "DsMaxI32"},
    {0x07, "DsMinU32"},        {0x08, "DsMaxU32"},
    {0x09, "DsAndB32"},        {0x0A, "DsOrB32"},
    {0x0B, "DsXorB32"},        {0x0D, "DsWriteB32"},
    {0x0E, "DsWrite2B32"},     {0x0F, "DsWrite2St64B32"},
    {0x10, "DsCmpstB32"},      {0x20, "DsAddRtnU32"},
    {0x21, "DsSubRtnU32"},     {0x23, "DsIncRtnU32"},
    {0x24, "DsDecRtnU32"},     {0x25, "DsMinRtnI32"},
    {0x26, "DsMaxRtnI32"},     {0x27, "DsMinRtnU32"},
    {0x28, "DsMaxRtnU32"},     {0x29, "DsAndRtnB32"},
    {0x2A, "DsOrRtnB32"},      {0x2B, "DsXorRtnB32"},
    {0x2D, "DsWrxchgRtnB32"},  {0x30, "DsCmpstRtnB32"},
    {0x35, "DsSwizzleB32"},    {0x36, "DsReadB32"},
    {0x37, "DsRead2B32"},      {0x38, "DsRead2St64B32"},
    {0x4D, "DsWriteB64"},      {0xDE, "DsWriteB96"},
    {0xDF, "DsWriteB128"},     {0xFE, "DsReadB96"},
    {0xFF, "DsReadB128"},
};

constexpr OpcodeName kMtbufNames[] = {
    {0x00, "TBufferLoadFormatX"},    {0x01, "TBufferLoadFormatXy"},
    {0x02, "TBufferLoadFormatXyz"},  {0x03, "TBufferLoadFormatXyzw"},
    {0x04, "TBufferStoreFormatX"},   {0x05, "TBufferStoreFormatXy"},
    {0x06, "TBufferStoreFormatXyz"}, {0x07, "TBufferStoreFormatXyzw"},
};

constexpr OpcodeName kMubufNames[] = {
    {0x00, "BufferLoadFormatX"},      {0x01, "BufferLoadFormatXy"},
    {0x02, "BufferLoadFormatXyz"},    {0x03, "BufferLoadFormatXyzw"},
    {0x04, "BufferStoreFormatX"},     {0x05, "BufferStoreFormatXy"},
    {0x06, "BufferStoreFormatXyz"},   {0x07, "BufferStoreFormatXyzw"},
    {0x08, "BufferLoadUbyte"},        {0x09, "BufferLoadSbyte"},
    {0x0A, "BufferLoadUshort"},       {0x0B, "BufferLoadSshort"},
    {0x0C, "BufferLoadDword"},        {0x0D, "BufferLoadDwordx2"},
    {0x0E, "BufferLoadDwordx4"},      {0x0F, "BufferLoadDwordx3"},
    {0x18, "BufferStoreByte"},        {0x19, "BufferStoreByteD16Hi"},
    {0x1A, "BufferStoreShort"},       {0x1B, "BufferStoreShortD16Hi"},
    {0x1C, "BufferStoreDword"},       {0x1D, "BufferStoreDwordx2"},
    {0x1E, "BufferStoreDwordx4"},     {0x1F, "BufferStoreDwordx3"},
    {0x20, "BufferLoadUbyteD16"},     {0x21, "BufferLoadUbyteD16Hi"},
    {0x22, "BufferLoadSbyteD16"},     {0x23, "BufferLoadSbyteD16Hi"},
    {0x24, "BufferLoadShortD16"},     {0x25, "BufferLoadShortD16Hi"},
    {0x30, "BufferAtomicSwap"},       {0x31, "BufferAtomicCmpswap"},
    {0x32, "BufferAtomicAdd"},        {0x33, "BufferAtomicSub"},
    {0x35, "BufferAtomicSmin"},       {0x36, "BufferAtomicUmin"},
    {0x37, "BufferAtomicSmax"},       {0x38, "BufferAtomicUmax"},
    {0x39, "BufferAtomicAnd"},        {0x3A, "BufferAtomicOr"},
    {0x3B, "BufferAtomicXor"},        {0x3C, "BufferAtomicInc"},
    {0x3D, "BufferAtomicDec"},
};

// Shared FLAT-segment opcode suffixes (SharpEmu decodes segment 2 "Global";
// the same numbering applies to flat (0) and scratch (1) segments).
constexpr OpcodeName kFlatNames[] = {
    {0x08, "LoadUbyte"},        {0x09, "LoadSbyte"},
    {0x0A, "LoadUshort"},       {0x0B, "LoadSshort"},
    {0x0C, "LoadDword"},        {0x0D, "LoadDwordx2"},
    {0x0E, "LoadDwordx4"},      {0x0F, "LoadDwordx3"},
    {0x18, "StoreByte"},        {0x19, "StoreByteD16Hi"},
    {0x1A, "StoreShort"},       {0x1B, "StoreShortD16Hi"},
    {0x1C, "StoreDword"},       {0x1D, "StoreDwordx2"},
    {0x1E, "StoreDwordx4"},     {0x1F, "StoreDwordx3"},
    {0x20, "LoadUbyteD16"},     {0x21, "LoadUbyteD16Hi"},
    {0x22, "LoadSbyteD16"},     {0x23, "LoadSbyteD16Hi"},
    {0x24, "LoadShortD16"},     {0x25, "LoadShortD16Hi"},
    {0x32, "AtomicAdd"},        {0x38, "AtomicUMax"},
};

constexpr OpcodeName kSmrdNames[] = {
    {0x00, "SLoadDword"},        {0x01, "SLoadDwordx2"},
    {0x02, "SLoadDwordx4"},      {0x03, "SLoadDwordx8"},
    {0x04, "SLoadDwordx16"},     {0x08, "SBufferLoadDword"},
    {0x09, "SBufferLoadDwordx2"},{0x0A, "SBufferLoadDwordx4"},
    {0x0B, "SBufferLoadDwordx8"},{0x0C, "SBufferLoadDwordx16"},
};

constexpr OpcodeName kMimgNames[] = {
    {0x00, "ImageLoad"},          {0x01, "ImageLoadMip"},
    {0x08, "ImageStore"},         {0x09, "ImageStoreMip"},
    {0x0E, "ImageGetResinfo"},    {0x0F, "ImageAtomicSwap"},
    {0x10, "ImageAtomicCmpswap"}, {0x11, "ImageAtomicAdd"},
    {0x12, "ImageAtomicSub"},     {0x14, "ImageAtomicSmin"},
    {0x15, "ImageAtomicUmin"},    {0x16, "ImageAtomicSmax"},
    {0x17, "ImageAtomicUmax"},    {0x18, "ImageAtomicAnd"},
    {0x19, "ImageAtomicOr"},      {0x1A, "ImageAtomicXor"},
    {0x1B, "ImageAtomicInc"},     {0x1C, "ImageAtomicDec"},
    {0x20, "ImageSample"},        {0x22, "ImageSampleD"},
    {0x24, "ImageSampleL"},       {0x25, "ImageSampleB"},
    {0x27, "ImageSampleLz"},      {0x2F, "ImageSampleCLz"},
    {0x30, "ImageSampleO"},       {0x34, "ImageSampleLO"},
    {0x37, "ImageSampleLzO"},     {0x40, "ImageGather4"},
    {0x47, "ImageGather4Lz"},     {0x48, "ImageGather4C"},
    {0x4E, "ImageGather4CBCl"},   {0x57, "ImageGather4LzO"},
    {0x5F, "ImageGather4CLzO"},
};

constexpr OpcodeName kVintrpNames[] = {
    {0x00, "VInterpP1F32"},
    {0x01, "VInterpP2F32"},
    {0x02, "VInterpMovF32"},
};

bool IsVop3BOpcode(u32 opcode) {
    return opcode == 0x128 || opcode == 0x16D || opcode == 0x16E ||
           opcode == 0x176 || opcode == 0x177 || opcode == 0x30F ||
           opcode == 0x310 || opcode == 0x319;
}

bool IsDataShareAtomic(const std::string& name) {
    static constexpr const char* kAtomics[] = {
        "DsAddU32",       "DsSubU32",      "DsIncU32",      "DsDecU32",
        "DsMinI32",       "DsMaxI32",      "DsMinU32",      "DsMaxU32",
        "DsAndB32",       "DsOrB32",       "DsXorB32",      "DsCmpstB32",
        "DsAddRtnU32",    "DsSubRtnU32",   "DsIncRtnU32",   "DsDecRtnU32",
        "DsMinRtnI32",    "DsMaxRtnI32",   "DsMinRtnU32",   "DsMaxRtnU32",
        "DsAndRtnB32",    "DsOrRtnB32",    "DsXorRtnB32",
        "DsWrxchgRtnB32", "DsCmpstRtnB32",
    };
    for (const char* atomic : kAtomics) {
        if (name == atomic) {
            return true;
        }
    }
    return false;
}

int MinimumEncodingDwords(GcnEncoding encoding) {
    switch (encoding) {
    case GcnEncoding::Vop3:
    case GcnEncoding::Smem:
    case GcnEncoding::Mubuf:
    case GcnEncoding::Mtbuf:
    case GcnEncoding::Ds:
    case GcnEncoding::Flat:
    case GcnEncoding::Vop3p:
    case GcnEncoding::Mimg:
    case GcnEncoding::Exp:
        return 2;
    default:
        return 1;
    }
}

u32 ScalarLoadDwordCount(const std::string& opcode) {
    if (opcode == "SLoadDword" || opcode == "SBufferLoadDword") return 1;
    if (opcode == "SLoadDwordx2" || opcode == "SBufferLoadDwordx2") return 2;
    if (opcode == "SLoadDwordx4" || opcode == "SBufferLoadDwordx4") return 4;
    if (opcode == "SLoadDwordx8" || opcode == "SBufferLoadDwordx8") return 8;
    if (opcode == "SLoadDwordx16" || opcode == "SBufferLoadDwordx16") return 16;
    return 0;
}

GcnDppControl CreateDppControl(u32 word) {
    GcnDppControl control;
    control.control        = (word >> 8) & 0x1FF;
    control.fetch_inactive = ((word >> 18) & 1) != 0;
    control.bound_control  = ((word >> 19) & 1) != 0;
    control.absolute_mask  = ((word >> 21) & 1) | (((word >> 23) & 1) << 1);
    control.negate_mask    = ((word >> 20) & 1) | (((word >> 22) & 1) << 1);
    control.bank_mask      = (word >> 24) & 0xF;
    control.row_mask       = (word >> 28) & 0xF;
    return control;
}

GcnSdwaControl CreateSdwaControl(u32 word, bool is_compare, bool has_source1) {
    GcnSdwaControl control;
    std::optional<u32> scalar_destination;
    if (is_compare) {
        scalar_destination = ((word >> 15) & 1) != 0 ? (word >> 8) & 0x7Fu : 106u;
    }
    control.destination_select = is_compare ? 6u : (word >> 8) & 0x7u;
    control.destination_unused = is_compare ? 0u : (word >> 11) & 0x3u;
    control.source0_select     = (word >> 16) & 0x7u;
    control.source1_select     = has_source1 ? (word >> 24) & 0x7u : 6u;
    control.source0_sign_extend = ((word >> 19) & 1) != 0;
    control.source1_sign_extend = has_source1 && ((word >> 27) & 1) != 0;
    control.absolute_mask =
        ((word >> 21) & 1) | (has_source1 ? ((word >> 29) & 1) << 1 : 0u);
    control.negate_mask =
        ((word >> 20) & 1) | (has_source1 ? ((word >> 28) & 1) << 1 : 0u);
    control.output_modifier    = is_compare ? 0u : (word >> 14) & 0x3u;
    control.clamp              = !is_compare && ((word >> 13) & 1) != 0;
    control.scalar_destination = scalar_destination;
    return control;
}

} // namespace

GcnOperand GcnOperand::Scalar(u32 index) {
    return GcnOperand{GcnOperandKind::ScalarRegister, index};
}

GcnOperand GcnOperand::Vector(u32 index) {
    return GcnOperand{GcnOperandKind::VectorRegister, index};
}

GcnOperand GcnOperand::Source(u32 encoded, std::optional<u32> literal) {
    if (encoded >= 256) {
        return Vector(encoded - 256);
    }
    if ((encoded == 249 || encoded == 255) && literal.has_value()) {
        return GcnOperand{GcnOperandKind::LiteralConstant, *literal};
    }
    if (encoded <= 105 || encoded == 106 || encoded == 107 ||
        encoded == 124 || encoded == 126 || encoded == 127) {
        return Scalar(encoded);
    }
    return GcnOperand{GcnOperandKind::EncodedConstant, encoded};
}

std::string GcnOperand::ToString() const {
    char buffer[24];
    switch (kind) {
    case GcnOperandKind::ScalarRegister:
        std::snprintf(buffer, sizeof(buffer), "s%u", value);
        break;
    case GcnOperandKind::VectorRegister:
        std::snprintf(buffer, sizeof(buffer), "v%u", value);
        break;
    case GcnOperandKind::LiteralConstant:
        std::snprintf(buffer, sizeof(buffer), "0x%08X", value);
        break;
    default:
        std::snprintf(buffer, sizeof(buffer), "src[%u]", value);
        break;
    }
    return buffer;
}

const char* GcnEncodingName(GcnEncoding encoding) {
    switch (encoding) {
    case GcnEncoding::Sop1:   return "SOP1";
    case GcnEncoding::Sop2:   return "SOP2";
    case GcnEncoding::Sopc:   return "SOPC";
    case GcnEncoding::Sopp:   return "SOPP";
    case GcnEncoding::Sopk:   return "SOPK";
    case GcnEncoding::Smrd:   return "SMRD";
    case GcnEncoding::Smem:   return "SMEM";
    case GcnEncoding::Mubuf:  return "MUBUF";
    case GcnEncoding::Mtbuf:  return "MTBUF";
    case GcnEncoding::Vop1:   return "VOP1";
    case GcnEncoding::Vop2:   return "VOP2";
    case GcnEncoding::Vopc:   return "VOPC";
    case GcnEncoding::Vop3:   return "VOP3";
    case GcnEncoding::Vintrp: return "VINTRP";
    case GcnEncoding::Ds:     return "DS";
    case GcnEncoding::Flat:   return "FLAT";
    case GcnEncoding::Vop3p:  return "VOP3P";
    case GcnEncoding::Mimg:   return "MIMG";
    case GcnEncoding::Exp:    return "EXP";
    }
    return "?";
}

const char* GcnClassifyInstruction(const std::string& opcode_name) {
    if (opcode_name.rfind("Image", 0) == 0) return "image";
    if (opcode_name.rfind("Global", 0) == 0) return "global_memory";
    if (opcode_name.rfind("VInterp", 0) == 0) return "interp";
    if (opcode_name == "Exp") return "export";
    if (opcode_name.rfind("SLoad", 0) == 0 || opcode_name.rfind("SBufferLoad", 0) == 0) {
        return "scalar_load";
    }
    if (!opcode_name.empty() && opcode_name[0] == 'V') return "valu";
    if (!opcode_name.empty() && opcode_name[0] == 'S') return "salu";
    return "other";
}

namespace {

// ---------------------------------------------------------------------------
// Name resolution per encoding (ported from DecodeSop1..DecodeVintrp).
// Sets `known=false` and a "<Fam>Raw0x.." name when the opcode is not in the
// tables — the instruction still decodes structurally.
// ---------------------------------------------------------------------------

struct DecodeInfo {
    GcnEncoding encoding = GcnEncoding::Vop2;
    std::string name;
    bool        known = true;
    u32         size_dwords = 1;
};

bool IsVopSrc0Extended(u32 src0) {
    return src0 == 0xE9 || src0 == 0xEA || src0 == 0xF9 ||
           src0 == 0xFA || src0 == 0xFF;
}

std::string VopcName(u32 opcode, bool& known) {
    known = true;
    const u32 cond = opcode & 0xF;
    std::string base;
    if (opcode < 0x10) {
        base = "VCmp";
    } else if (opcode < 0x20) {
        base = "VCmpx";
    } else if (opcode >= 0x80 && opcode < 0x88) {
        static constexpr const char* kI32[8] = {
            "F", "Lt", "Eq", "Le", "Gt", "Ne", "Ge", "T",
        };
        return std::string("VCmp") + kI32[opcode - 0x80] + "I32";
    } else if (opcode == 0x88) {
        return "VCmpClassF32";
    } else if (opcode >= 0x90 && opcode < 0x98) {
        static constexpr const char* kI32[8] = {
            "F", "Lt", "Eq", "Le", "Gt", "Ne", "Ge", "T",
        };
        return std::string("VCmpx") + kI32[opcode - 0x90] + "I32";
    } else if (opcode >= 0xC0 && opcode < 0xC8) {
        static constexpr const char* kU32[8] = {
            "F", "Lt", "Eq", "Le", "Gt", "Ne", "Ge", "T",
        };
        return std::string("VCmp") + kU32[opcode - 0xC0] + "U32";
    } else if (opcode >= 0xD0 && opcode < 0xD8) {
        static constexpr const char* kU32[8] = {
            "F", "Lt", "Eq", "Le", "Gt", "Ne", "Ge", "T",
        };
        return std::string("VCmpx") + kU32[opcode - 0xD0] + "U32";
    } else {
        known = false;
        return RawName("Vopc", opcode, 2);
    }
    return base + kVopcConditions[cond] + "F32";
}

// First pass: identify encoding, opcode name and instruction size.  Mirrors
// TryDecodeInstruction; returns false only for reserved top-level encodings.
bool ClassifyWord(u32 word, const u32* code, size_t words_available, u32 index,
                  DecodeInfo& info, std::string& error) {
    if ((word & 0x80000000u) == 0) {
        const u32 vop_opcode = (word >> 25) & 0x3F;
        const u32 src0       = word & 0x1FF;
        if (vop_opcode == 0x3E) {
            info.encoding = GcnEncoding::Vopc;
            info.name     = VopcName((word >> 17) & 0xFF, info.known);
            info.size_dwords = IsVopSrc0Extended(src0) ? 2 : 1;
            return true;
        }
        if (vop_opcode == 0x3F) {
            info.encoding = GcnEncoding::Vop1;
            const u32 op = (word >> 9) & 0xFF;
            if (const char* name = Lookup(kVop1Names, std::size(kVop1Names), op)) {
                info.name = name;
            } else {
                info.name  = RawName("Vop1", op, 2);
                info.known = false;
            }
            info.size_dwords = IsVopSrc0Extended(src0) ? 2 : 1;
            return true;
        }
        info.encoding = GcnEncoding::Vop2;
        if (const char* name = Lookup(kVop2Names, std::size(kVop2Names), vop_opcode)) {
            info.name = name;
        } else {
            info.name  = RawName("Vop2", vop_opcode, 2);
            info.known = false;
        }
        info.size_dwords =
            vop_opcode == 0x20 || vop_opcode == 0x21 || vop_opcode == 0x2C ||
                    vop_opcode == 0x2D || IsVopSrc0Extended(src0)
                ? 2
                : 1;
        return true;
    }

    if ((word & 0xF8000000u) == 0xC0000000u) {
        info.encoding = GcnEncoding::Smrd;
        const u32 op = (word >> 22) & 0x1F;
        if (const char* name = Lookup(kSmrdNames, std::size(kSmrdNames), op)) {
            info.name = name;
        } else {
            info.name  = RawName("Smrd", op, 2);
            info.known = false;
        }
        const u32  offset          = word & 0xFF;
        const bool immediate_offset = ((word >> 8) & 1) != 0;
        info.size_dwords = !immediate_offset && offset == 0xFF ? 2 : 1;
        return true;
    }

    if ((word & 0xC0000000u) == 0x80000000u) {
        const u32 sop_opcode = (word >> 23) & 0x7F;
        const u32 src0       = word & 0xFF;
        const u32 src1       = (word >> 8) & 0xFF;
        if (sop_opcode == 0x7D) {
            info.encoding = GcnEncoding::Sop1;
            const u32 op = (word >> 8) & 0xFF;
            if (const char* name = Lookup(kSop1Names, std::size(kSop1Names), op)) {
                info.name = name;
            } else {
                info.name  = RawName("Sop1", op, 2);
                info.known = false;
            }
            info.size_dwords = src0 == 0xFF ? 2 : 1;
            return true;
        }
        if (sop_opcode == 0x7E) {
            info.encoding = GcnEncoding::Sopc;
            const u32 op = (word >> 16) & 0x7F;
            if (const char* name = Lookup(kSopcNames, std::size(kSopcNames), op)) {
                info.name = name;
            } else {
                info.name  = RawName("Sopc", op, 2);
                info.known = false;
            }
            info.size_dwords = src0 == 0xFF || src1 == 0xFF ? 2 : 1;
            return true;
        }
        if (sop_opcode == 0x7F) {
            info.encoding = GcnEncoding::Sopp;
            const u32 op = (word >> 16) & 0x7F;
            if (const char* name = Lookup(kSoppNames, std::size(kSoppNames), op)) {
                info.name = name;
            } else {
                info.name  = RawName("Sopp", op, 2);
                info.known = false;
            }
            info.size_dwords = 1;
            return true;
        }
        if (sop_opcode >= 0x60) {
            info.encoding = GcnEncoding::Sopk;
            const u32 op = sop_opcode - 0x60;
            if (const char* name = Lookup(kSopkNames, std::size(kSopkNames), op)) {
                info.name = name;
            } else {
                info.name  = RawName("Sopk", op, 2);
                info.known = false;
            }
            info.size_dwords = 1;
            return true;
        }
        info.encoding = GcnEncoding::Sop2;
        if (const char* name =
                Lookup(kSop2Names, std::size(kSop2Names), sop_opcode)) {
            info.name = name;
        } else {
            info.name  = RawName("Sop2", sop_opcode, 2);
            info.known = false;
        }
        info.size_dwords = src0 == 0xFF || src1 == 0xFF ? 2 : 1;
        return true;
    }

    // gfx10 VOP3P (packed 16-bit math) prefix, before the coarse switch.
    if ((word & 0xFF800000u) == 0xCC000000u) {
        info.encoding = GcnEncoding::Vop3p;
        const u32 op = (word >> 16) & 0x7F;
        if (const char* name = Lookup(kVop3pNames, std::size(kVop3pNames), op)) {
            info.name = name;
        } else {
            info.name  = RawName("Vop3p", op, 2);
            info.known = false;
        }
        info.size_dwords = 2;
        if (index + 1 < words_available) {
            const u32 extra = code[index + 1];
            if ((extra & 0x1FF) == 0xFF || ((extra >> 9) & 0x1FF) == 0xFF ||
                ((extra >> 18) & 0x1FF) == 0xFF) {
                info.size_dwords = 3;
            }
        }
        return true;
    }

    switch (word >> 26) {
    case 0x33:
    case 0x3D: {
        info.encoding = GcnEncoding::Smem;
        const u32 op = (word >> 18) & 0xFF;
        if (const char* name = Lookup(kSmrdNames, std::size(kSmrdNames), op)) {
            info.name = name;
        } else {
            info.name  = RawName("Smem", op, 2);
            info.known = false;
        }
        info.size_dwords = 2;
        return true;
    }
    case 0x32: {
        info.encoding = GcnEncoding::Vintrp;
        const u32 op = (word >> 16) & 0x3;
        if (const char* name = Lookup(kVintrpNames, std::size(kVintrpNames), op)) {
            info.name = name;
        } else {
            info.name  = RawName("Vintrp", op, 1);
            info.known = false;
        }
        info.size_dwords = 1;
        return true;
    }
    case 0x34:
    case 0x35: {
        info.encoding  = GcnEncoding::Vop3;
        const u32 op   = (word >> 16) & 0x3FF;
        const bool is_b = IsVop3BOpcode(op);
        const char* name = is_b
            ? Lookup(kVop3bNames, std::size(kVop3bNames), op)
            : Lookup(kVop3aNames, std::size(kVop3aNames), op);
        if (name) {
            info.name = name;
        } else {
            info.name  = RawName(is_b ? "Vop3b" : "Vop3", op, 3);
            info.known = false;
        }
        info.size_dwords = 2;
        if (index + 1 < words_available) {
            const u32 extra = code[index + 1];
            if ((extra & 0x1FF) == 0xFF || ((extra >> 9) & 0x1FF) == 0xFF ||
                ((extra >> 18) & 0x1FF) == 0xFF) {
                info.size_dwords = 3;
            }
        }
        return true;
    }
    case 0x36: {
        info.encoding = GcnEncoding::Ds;
        const u32 op = (word >> 18) & 0xFF;
        if (const char* name = Lookup(kDsNames, std::size(kDsNames), op)) {
            info.name = name;
        } else {
            info.name  = RawName("Ds", op, 2);
            info.known = false;
        }
        info.size_dwords = 2;
        return true;
    }
    case 0x37: {
        info.encoding     = GcnEncoding::Flat;
        const u32 segment = (word >> 14) & 0x3;
        const u32 op      = (word >> 18) & 0x7F;
        const char* prefix =
            segment == 0 ? "Flat" : segment == 1 ? "Scratch" : "Global";
        if (segment <= 2) {
            if (const char* suffix =
                    Lookup(kFlatNames, std::size(kFlatNames), op)) {
                info.name = std::string(prefix) + suffix;
            } else {
                info.name  = RawName(prefix, op, 2);
                info.known = false;
            }
        } else {
            info.name  = RawName("FlatSeg3", op, 2);
            info.known = false;
        }
        info.size_dwords = 2;
        return true;
    }
    case 0x38: {
        info.encoding = GcnEncoding::Mubuf;
        const u32 op = (word >> 18) & 0x7F;
        if (const char* name = Lookup(kMubufNames, std::size(kMubufNames), op)) {
            info.name = name;
        } else {
            info.name  = RawName("Mubuf", op, 2);
            info.known = false;
        }
        info.size_dwords = 2;
        if (index + 1 < words_available && (code[index + 1] >> 24) == 0xFF) {
            info.size_dwords = 3;
        }
        return true;
    }
    case 0x3A: {
        info.encoding = GcnEncoding::Mtbuf;
        const u32 op = (word >> 16) & 0x7;
        if (const char* name = Lookup(kMtbufNames, std::size(kMtbufNames), op)) {
            info.name = name;
        } else {
            info.name  = RawName("Mtbuf", op, 1);
            info.known = false;
        }
        info.size_dwords = 2;
        if (index + 1 < words_available && (code[index + 1] >> 24) == 0xFF) {
            info.size_dwords = 3;
        }
        return true;
    }
    case 0x3C: {
        info.encoding = GcnEncoding::Mimg;
        const u32 op = (word >> 18) & 0x7F;
        if (const char* name = Lookup(kMimgNames, std::size(kMimgNames), op)) {
            info.name = name;
        } else {
            info.name  = RawName("Mimg", op, 2);
            info.known = false;
        }
        info.size_dwords = 2 + ((word >> 1) & 0x3);
        return true;
    }
    case 0x3E:
        info.encoding    = GcnEncoding::Exp;
        info.name        = "Exp";
        info.size_dwords = 2;
        return true;
    case 0x3F:
        info.encoding    = GcnEncoding::Vop3p;
        info.name        = RawName("Vop3p", word >> 24, 2);
        info.known       = false;
        info.size_dwords = 2;
        return true;
    default: {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "unknown-top word=0x%08X", word);
        error = buffer;
        return false;
    }
    }
}

u32 FlatDwordCount(const std::string& name) {
    // `name` carries a Flat/Scratch/Global prefix; count by the Load/Store
    // width suffix.
    if (name.find("Dwordx2") != std::string::npos) return 2;
    if (name.find("Dwordx3") != std::string::npos) return 3;
    if (name.find("Dwordx4") != std::string::npos) return 4;
    if (name.find("LoadDword") != std::string::npos) return 1;
    if (name.find("StoreDword") != std::string::npos) return 1;
    if (name.find("Ubyte") != std::string::npos ||
        name.find("Sbyte") != std::string::npos ||
        name.find("Ushort") != std::string::npos ||
        name.find("Sshort") != std::string::npos ||
        name.find("Byte") != std::string::npos ||
        name.find("Short") != std::string::npos ||
        name.find("Atomic") != std::string::npos) {
        return 1;
    }
    return 0;
}

u32 BufferDwordCount(const std::string& name) {
    // SharpEmu only assigns data widths to typed-buffer loads; TBuffer stores
    // (and unknown Raw forms) carry no vector data registers.
    if (name.rfind("TBufferStore", 0) == 0) return 0;
    if (name.find("FormatXyzw") != std::string::npos) return 4;
    if (name.find("FormatXyz") != std::string::npos) return 3;
    if (name.find("FormatXy") != std::string::npos) return 2;
    if (name.find("FormatX") != std::string::npos) return 1;
    if (name.find("Dwordx2") != std::string::npos) return 2;
    if (name.find("Dwordx3") != std::string::npos) return 3;
    if (name.find("Dwordx4") != std::string::npos) return 4;
    if (name.find("Dword") != std::string::npos) return 1;
    if (name == "BufferAtomicCmpswap") return 2;
    if (name.rfind("BufferAtomic", 0) == 0) return 1;
    if (name.find("byte") != std::string::npos ||
        name.find("Byte") != std::string::npos ||
        name.find("hort") != std::string::npos) {
        return 1;
    }
    return 0;
}

// Second pass: operand/control extraction — port of CreateInstruction.
void BuildInstruction(u32 pc, const DecodeInfo& info, const u32* words,
                      size_t word_count, GcnInstruction& out) {
    const u32 word = words[0];
    out.pc          = pc;
    out.encoding    = info.encoding;
    out.opcode      = info.name;
    out.opcode_known = info.known;
    out.words.assign(words, words + word_count);

    const bool is_vop12c = info.encoding == GcnEncoding::Vop1 ||
                           info.encoding == GcnEncoding::Vop2 ||
                           info.encoding == GcnEncoding::Vopc;
    const bool is_sdwa = is_vop12c && (word & 0x1FF) == 0xF9;
    const bool is_dpp  = is_vop12c && (word & 0x1FF) == 0xFA;
    const bool is_dpp8 =
        is_vop12c && ((word & 0x1FF) == 0xE9 || (word & 0x1FF) == 0xEA);

    std::optional<u32> literal;
    if (!is_sdwa && !is_dpp && !is_dpp8 &&
        word_count > static_cast<size_t>(MinimumEncodingDwords(info.encoding))) {
        literal = words[word_count - 1];
    }

    auto& srcs = out.sources;
    auto& dsts = out.destinations;

    switch (info.encoding) {
    case GcnEncoding::Sop1:
        srcs.push_back(GcnOperand::Source(word & 0xFF, literal));
        dsts.push_back(GcnOperand::Scalar((word >> 16) & 0x7F));
        break;
    case GcnEncoding::Sop2:
        srcs.push_back(GcnOperand::Source(word & 0xFF, literal));
        srcs.push_back(GcnOperand::Source((word >> 8) & 0xFF, literal));
        dsts.push_back(GcnOperand::Scalar((word >> 16) & 0x7F));
        break;
    case GcnEncoding::Sopc:
        srcs.push_back(GcnOperand::Source(word & 0xFF, literal));
        srcs.push_back(GcnOperand::Source((word >> 8) & 0xFF, literal));
        break;
    case GcnEncoding::Sopk:
        srcs.push_back(
            GcnOperand{GcnOperandKind::EncodedConstant, word & 0xFFFF});
        dsts.push_back(GcnOperand::Scalar((word >> 16) & 0x7F));
        break;
    case GcnEncoding::Smrd: {
        const u32  scalar_base        = ((word >> 9) & 0x3F) * 2;
        const u32  scalar_destination = (word >> 15) & 0x7F;
        const bool immediate          = ((word >> 8) & 1) != 0;
        const u32  offset             = word & 0xFF;
        const u32  count              = ScalarLoadDwordCount(info.name);
        std::optional<u32> dynamic_offset_register;
        s32 immediate_offset_bytes = 0;
        if (immediate) {
            immediate_offset_bytes = static_cast<s32>(offset * sizeof(u32));
        } else if (offset == 0xFF && literal.has_value()) {
            immediate_offset_bytes = static_cast<s32>(*literal);
        } else {
            dynamic_offset_register = offset;
        }

        srcs.push_back(GcnOperand::Scalar(scalar_base));
        if (dynamic_offset_register.has_value()) {
            srcs.push_back(GcnOperand::Scalar(*dynamic_offset_register));
        }
        for (u32 i = 0; i < count; ++i) {
            dsts.push_back(GcnOperand::Scalar(scalar_destination + i));
        }
        out.control = GcnScalarMemoryControl{
            count, immediate_offset_bytes, dynamic_offset_register};
        break;
    }
    case GcnEncoding::Smem: {
        const u32 extra              = words[1];
        const u32 scalar_base        = (word & 0x3F) * 2;
        const u32 scalar_destination = (word >> 6) & 0x7F;
        const u32 scalar_offset      = (extra >> 25) & 0x7F;
        const s32 offset             = SignExtend(extra & 0x1FFFFF, 21);
        const u32 count              = ScalarLoadDwordCount(info.name);
        const GcnOperand scalar_offset_operand = GcnOperand::Source(scalar_offset);
        std::optional<u32> dynamic_offset_register;
        if (scalar_offset_operand.kind == GcnOperandKind::ScalarRegister) {
            dynamic_offset_register = scalar_offset_operand.value;
        }
        srcs.push_back(GcnOperand::Scalar(scalar_base));
        srcs.push_back(scalar_offset_operand);
        for (u32 i = 0; i < count; ++i) {
            dsts.push_back(GcnOperand::Scalar(scalar_destination + i));
        }
        out.control = GcnScalarMemoryControl{
            count, offset, dynamic_offset_register};
        break;
    }
    case GcnEncoding::Vop1:
        if (is_dpp8) {
            const u32 extra = words[1];
            srcs.push_back(GcnOperand::Vector(extra & 0xFF));
            out.control = GcnDpp8Control{extra >> 8, (word & 0x1FF) == 0xEA};
        } else if (is_dpp) {
            const u32 extra = words[1];
            srcs.push_back(GcnOperand::Vector(extra & 0xFF));
            out.control = CreateDppControl(extra);
        } else if (is_sdwa) {
            const u32 extra = words[1];
            const u32 source0 =
                (extra & 0xFF) + ((((extra >> 23) & 1) == 0) ? 256u : 0u);
            srcs.push_back(GcnOperand::Source(source0));
            out.control = CreateSdwaControl(extra, false, false);
        } else {
            srcs.push_back(GcnOperand::Source(word & 0x1FF, literal));
        }
        // V_READFIRSTLANE_B32 names an SGPR destination; everything else a VGPR.
        if (info.name == "VReadfirstlaneB32") {
            dsts.push_back(GcnOperand::Scalar((word >> 17) & 0x7F));
        } else {
            dsts.push_back(GcnOperand::Vector((word >> 17) & 0xFF));
        }
        break;
    case GcnEncoding::Vop2:
        if (is_dpp8) {
            const u32 extra = words[1];
            srcs.push_back(GcnOperand::Vector(extra & 0xFF));
            srcs.push_back(GcnOperand::Vector((word >> 9) & 0xFF));
            out.control = GcnDpp8Control{extra >> 8, (word & 0x1FF) == 0xEA};
        } else if (is_dpp) {
            const u32 extra = words[1];
            srcs.push_back(GcnOperand::Vector(extra & 0xFF));
            srcs.push_back(GcnOperand::Vector((word >> 9) & 0xFF));
            out.control = CreateDppControl(extra);
        } else if (is_sdwa) {
            const u32 extra = words[1];
            const u32 source0 =
                (extra & 0xFF) + ((((extra >> 23) & 1) == 0) ? 256u : 0u);
            const u32 source1 =
                ((word >> 9) & 0xFF) + ((((extra >> 31) & 1) == 0) ? 256u : 0u);
            srcs.push_back(GcnOperand::Source(source0));
            srcs.push_back(GcnOperand::Source(source1));
            out.control = CreateSdwaControl(extra, false, true);
        } else {
            srcs.push_back(GcnOperand::Source(word & 0x1FF, literal));
            srcs.push_back(GcnOperand::Vector((word >> 9) & 0xFF));
            if ((info.name == "VMadMkF32" || info.name == "VFmaMkF32") &&
                literal.has_value()) {
                srcs = {
                    srcs[0],
                    GcnOperand{GcnOperandKind::LiteralConstant, *literal},
                    srcs[1],
                };
            } else if ((info.name == "VMadAkF32" || info.name == "VFmaAkF32") &&
                       literal.has_value()) {
                srcs.push_back(
                    GcnOperand{GcnOperandKind::LiteralConstant, *literal});
            }
        }
        dsts.push_back(GcnOperand::Vector((word >> 17) & 0xFF));
        break;
    case GcnEncoding::Vopc:
        if (is_dpp8) {
            const u32 extra = words[1];
            srcs.push_back(GcnOperand::Vector(extra & 0xFF));
            srcs.push_back(GcnOperand::Vector((word >> 9) & 0xFF));
            out.control = GcnDpp8Control{extra >> 8, (word & 0x1FF) == 0xEA};
        } else if (is_dpp) {
            const u32 extra = words[1];
            srcs.push_back(GcnOperand::Vector(extra & 0xFF));
            srcs.push_back(GcnOperand::Vector((word >> 9) & 0xFF));
            out.control = CreateDppControl(extra);
        } else if (is_sdwa) {
            const u32 extra = words[1];
            const u32 source0 =
                (extra & 0xFF) + ((((extra >> 23) & 1) == 0) ? 256u : 0u);
            const u32 source1 =
                ((word >> 9) & 0xFF) + ((((extra >> 31) & 1) == 0) ? 256u : 0u);
            srcs.push_back(GcnOperand::Source(source0));
            srcs.push_back(GcnOperand::Source(source1));
            const auto sdwa = CreateSdwaControl(extra, true, true);
            if (sdwa.scalar_destination.has_value() &&
                *sdwa.scalar_destination != 106) {
                dsts.push_back(GcnOperand::Scalar(*sdwa.scalar_destination));
            }
            out.control = sdwa;
        } else {
            srcs.push_back(GcnOperand::Source(word & 0x1FF, literal));
            srcs.push_back(GcnOperand::Vector((word >> 9) & 0xFF));
        }
        break;
    case GcnEncoding::Vop3: {
        const u32 extra = words[1];
        srcs.push_back(GcnOperand::Source(extra & 0x1FF, literal));
        srcs.push_back(GcnOperand::Source((extra >> 9) & 0x1FF, literal));
        srcs.push_back(GcnOperand::Source((extra >> 18) & 0x1FF, literal));
        if (info.name == "VReadlaneB32") {
            // V_READLANE uses the VOP3A vdst byte even though the destination
            // register is scalar.
            dsts.push_back(GcnOperand::Scalar(word & 0xFF));
        } else {
            dsts.push_back(GcnOperand::Vector(word & 0xFF));
        }
        const bool is_vop3b = IsVop3BOpcode((word >> 16) & 0x3FF);
        GcnVop3Control control;
        control.absolute_mask  = is_vop3b ? 0 : (word >> 8) & 0x7;
        control.negate_mask    = (extra >> 29) & 0x7;
        control.output_modifier = (extra >> 27) & 0x3;
        control.clamp          = ((word >> 15) & 1) != 0;
        control.operand_select = is_vop3b ? 0 : (word >> 11) & 0xF;
        if (is_vop3b) {
            control.scalar_destination = (word >> 8) & 0x7F;
        }
        out.control = control;
        break;
    }
    case GcnEncoding::Vop3p: {
        if (word_count < 2) {
            break;
        }
        const u32 extra = words[1];
        srcs.push_back(GcnOperand::Source(extra & 0x1FF, literal));
        srcs.push_back(GcnOperand::Source((extra >> 9) & 0x1FF, literal));
        srcs.push_back(GcnOperand::Source((extra >> 18) & 0x1FF, literal));
        dsts.push_back(GcnOperand::Vector(word & 0xFF));
        // op_sel_hi is split across both dwords: bits [1:0] live in word1
        // [28:27], bit [2] in word0 [14].
        const u32 op_sel_hi =
            ((extra >> 27) & 0x3) | (((word >> 14) & 0x1) << 2);
        out.control = GcnVop3pControl{
            (word >> 11) & 0x7,
            op_sel_hi,
            (extra >> 29) & 0x7,
            (word >> 8) & 0x7,
            ((word >> 15) & 1) != 0,
        };
        break;
    }
    case GcnEncoding::Ds: {
        const u32 extra              = words[1];
        const u32 vector_address     = extra & 0xFF;
        const u32 vector_data0       = (extra >> 8) & 0xFF;
        const u32 vector_data1       = (extra >> 16) & 0xFF;
        const u32 vector_destination = (extra >> 24) & 0xFF;
        out.control = GcnDataShareControl{
            word & 0xFF, (word >> 8) & 0xFF, ((word >> 17) & 1) != 0};
        const std::string& op = info.name;
        if (op == "DsWriteB32") {
            srcs = {GcnOperand::Vector(vector_address),
                    GcnOperand::Vector(vector_data0)};
        } else if (op == "DsWriteB64") {
            srcs = {GcnOperand::Vector(vector_address),
                    GcnOperand::Vector(vector_data0),
                    GcnOperand::Vector(vector_data0 + 1)};
        } else if (op == "DsWriteB96") {
            srcs = {GcnOperand::Vector(vector_address),
                    GcnOperand::Vector(vector_data0),
                    GcnOperand::Vector(vector_data0 + 1),
                    GcnOperand::Vector(vector_data0 + 2)};
        } else if (op == "DsWriteB128") {
            srcs = {GcnOperand::Vector(vector_address),
                    GcnOperand::Vector(vector_data0),
                    GcnOperand::Vector(vector_data0 + 1),
                    GcnOperand::Vector(vector_data0 + 2),
                    GcnOperand::Vector(vector_data0 + 3)};
        } else if (op == "DsWrite2B32" || op == "DsWrite2St64B32") {
            srcs = {GcnOperand::Vector(vector_address),
                    GcnOperand::Vector(vector_data0),
                    GcnOperand::Vector(vector_data1)};
        } else if (op == "DsSwizzleB32") {
            srcs = {GcnOperand::Vector(vector_data0)};
        } else if (op == "DsCmpstB32" || op == "DsCmpstRtnB32") {
            // DS_CMPST operand order is reversed vs buffer/image cmpswap:
            // DATA0 holds the comparator, DATA1 the new value.
            srcs = {GcnOperand::Vector(vector_address),
                    GcnOperand::Vector(vector_data0),
                    GcnOperand::Vector(vector_data1)};
        } else if (IsDataShareAtomic(op)) {
            srcs = {GcnOperand::Vector(vector_address),
                    GcnOperand::Vector(vector_data0)};
        } else {
            srcs = {GcnOperand::Vector(vector_address)};
        }
        if (op == "DsReadB32" || op == "DsSwizzleB32") {
            dsts = {GcnOperand::Vector(vector_destination)};
        } else if (op == "DsRead2B32" || op == "DsRead2St64B32") {
            dsts = {GcnOperand::Vector(vector_destination),
                    GcnOperand::Vector(vector_destination + 1)};
        } else if (op == "DsReadB96") {
            dsts = {GcnOperand::Vector(vector_destination),
                    GcnOperand::Vector(vector_destination + 1),
                    GcnOperand::Vector(vector_destination + 2)};
        } else if (op == "DsReadB128") {
            dsts = {GcnOperand::Vector(vector_destination),
                    GcnOperand::Vector(vector_destination + 1),
                    GcnOperand::Vector(vector_destination + 2),
                    GcnOperand::Vector(vector_destination + 3)};
        } else if (IsDataShareAtomic(op) &&
                   op.find("Rtn") != std::string::npos) {
            dsts = {GcnOperand::Vector(vector_destination)};
        }
        break;
    }
    case GcnEncoding::Vintrp: {
        srcs.push_back(GcnOperand::Vector(word & 0xFF));
        dsts.push_back(GcnOperand::Vector((word >> 18) & 0xFF));
        out.control = GcnInterpolationControl{(word >> 10) & 0x3F,
                                              (word >> 8) & 0x3};
        break;
    }
    case GcnEncoding::Flat: {
        const u32 extra          = words[1];
        const u32 vector_address = extra & 0xFF;
        const u32 vector_data    = (extra >> 8) & 0xFF;
        const u32 scalar_address = (extra >> 16) & 0x7F;
        const u32 dword_count    = FlatDwordCount(info.name);
        const bool is_load       = info.name.find("Load") != std::string::npos;
        srcs.push_back(GcnOperand::Vector(vector_address));
        srcs.push_back(GcnOperand::Scalar(scalar_address));
        if (is_load) {
            for (u32 i = 0; i < dword_count; ++i) {
                dsts.push_back(GcnOperand::Vector(vector_data + i));
            }
        }
        out.control = GcnGlobalMemoryControl{
            dword_count,
            vector_address,
            vector_data,
            scalar_address,
            SignExtend(word & 0x1FFF, 13),
            ((word >> 16) & 1) != 0,
            ((word >> 17) & 1) != 0,
        };
        break;
    }
    case GcnEncoding::Mubuf:
    case GcnEncoding::Mtbuf: {
        const u32 extra           = words[1];
        const u32 vector_address  = extra & 0xFF;
        const u32 vector_data     = (extra >> 8) & 0xFF;
        const u32 scalar_resource = ((extra >> 16) & 0x1F) * 4;
        const u32 scalar_offset   = (extra >> 24) & 0xFF;
        const u32 dword_count     = BufferDwordCount(info.name);
        srcs.push_back(GcnOperand::Vector(vector_address));
        srcs.push_back(GcnOperand::Scalar(scalar_resource));
        srcs.push_back(GcnOperand::Source(scalar_offset, literal));
        for (u32 i = 0; i < dword_count; ++i) {
            dsts.push_back(GcnOperand::Vector(vector_data + i));
        }
        out.control = GcnBufferMemoryControl{
            dword_count,
            vector_address,
            vector_data,
            scalar_resource,
            static_cast<s32>(word & 0xFFF),
            ((word >> 13) & 1) != 0,
            ((word >> 12) & 1) != 0,
            ((word >> 14) & 1) != 0,
            ((extra >> 22) & 1) != 0,
        };
        break;
    }
    case GcnEncoding::Mimg: {
        const u32 extra           = words[1];
        const u32 vector_address  = extra & 0xFF;
        const u32 vector_data     = (extra >> 8) & 0xFF;
        const u32 scalar_resource = ((extra >> 16) & 0x1F) * 4;
        const u32 scalar_sampler  = ((extra >> 21) & 0x1F) * 4;
        std::vector<u32> address_registers;
        address_registers.push_back(vector_address);
        for (size_t word_index = 2; word_index < word_count; ++word_index) {
            for (u32 shift = 0; shift < 32; shift += 8) {
                address_registers.push_back((words[word_index] >> shift) & 0xFF);
            }
        }
        for (u32 address_register : address_registers) {
            srcs.push_back(GcnOperand::Vector(address_register));
        }
        srcs.push_back(GcnOperand::Scalar(scalar_resource));
        srcs.push_back(GcnOperand::Scalar(scalar_sampler));
        if (info.name.rfind("ImageStore", 0) != 0) {
            dsts.push_back(GcnOperand::Vector(vector_data));
        }
        const u32 dimension = (word >> 3) & 0x7;
        GcnImageControl control;
        control.dmask           = (word >> 8) & 0xF;
        control.vector_address  = vector_address;
        control.address_registers = address_registers;
        control.vector_data     = vector_data;
        control.scalar_resource = scalar_resource;
        control.scalar_sampler  = scalar_sampler;
        control.dimension       = dimension;
        control.is_array = dimension == 4 || dimension == 5 || dimension == 7;
        control.glc = ((word >> 13) & 1) != 0;
        control.slc = ((word >> 25) & 1) != 0;
        control.a16 = ((extra >> 30) & 1) != 0;
        control.d16 = ((extra >> 31) & 1) != 0;
        out.control = control;
        break;
    }
    case GcnEncoding::Exp: {
        const u32 extra = words[1];
        srcs.push_back(GcnOperand::Vector(extra & 0xFF));
        srcs.push_back(GcnOperand::Vector((extra >> 8) & 0xFF));
        srcs.push_back(GcnOperand::Vector((extra >> 16) & 0xFF));
        srcs.push_back(GcnOperand::Vector((extra >> 24) & 0xFF));
        out.control = GcnExportControl{
            (word >> 4) & 0x3F,
            word & 0xF,
            ((word >> 10) & 1) != 0,
            ((word >> 11) & 1) != 0,
            ((word >> 12) & 1) != 0,
        };
        break;
    }
    }
}

} // namespace

bool GcnDecodeInstruction(
    const u32*      code,
    size_t          words_available,
    u32             pc,
    GcnInstruction& out,
    u32&            size_dwords,
    std::string&    error) {
    error.clear();
    const u32 index = pc / sizeof(u32);
    if (index >= words_available) {
        error = "read-failed";
        return false;
    }

    DecodeInfo info;
    if (!ClassifyWord(code[index], code, words_available, index, info, error)) {
        return false;
    }
    if (static_cast<size_t>(index) + info.size_dwords > words_available) {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "read-failed pc=0x%X", pc);
        error = buffer;
        return false;
    }

    BuildInstruction(pc, info, code + index, info.size_dwords, out);
    size_dwords = info.size_dwords;
    return true;
}

bool GcnDecodeProgram(
    const u32*  code,
    size_t      dword_count,
    GcnProgram& out,
    std::string& error) {
    error.clear();
    out.instructions.clear();

    constexpr size_t kMaxInstructions = 65536;
    for (u32 pc = 0; out.instructions.size() < kMaxInstructions;) {
        const u32 index = pc / sizeof(u32);
        if (index >= dword_count) {
            error = "unterminated";
            return false;
        }

        // On-disk .shader_text sections are zero-padded after S_ENDPGM.
        if (code[index] == 0) {
            bool padding_only = true;
            for (size_t i = index; i < dword_count; ++i) {
                if (code[i] != 0) {
                    padding_only = false;
                    break;
                }
            }
            if (padding_only && !out.instructions.empty()) {
                return true;
            }
        }

        GcnInstruction instruction;
        u32            size_dwords = 0;
        if (!GcnDecodeInstruction(
                code, dword_count, pc, instruction, size_dwords, error)) {
            return false;
        }

        const bool is_endpgm = instruction.opcode == "SEndpgm";
        out.instructions.push_back(std::move(instruction));
        pc += size_dwords * sizeof(u32);
        if (is_endpgm) {
            return true;
        }
    }

    error = "instruction-limit";
    return false;
}

} // namespace GPU::Shader
