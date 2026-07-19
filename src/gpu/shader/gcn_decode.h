// RDNA2 (GCN Gen5 / gfx10) shader instruction stream decoder (Phase 5 M2).
//
// Guided transliteration of SharpEmu's Gen5ShaderTranslator decode half
// (sharpemu_clone/src/SharpEmu.ShaderCompiler/Gen5ShaderTranslator.cs,
// TryDecodeInstruction/CreateInstruction and the per-encoding tables) into
// table-driven C++.  Decode is complete across every gfx10 encoding family;
// opcodes that have no name in the tables still decode structurally (operands,
// size, control fields) and are flagged via GcnInstruction::opcode_known ==
// false so corpus tooling can list them instead of crashing.
#pragma once

#include "../../common/types.h"

#include <cstddef>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace GPU::Shader {

// Mirrors SharpEmu's Gen5ShaderEncoding.
enum class GcnEncoding {
    Sop1,
    Sop2,
    Sopc,
    Sopp,
    Sopk,
    Smrd,
    Smem,
    Mubuf,
    Mtbuf,
    Vop1,
    Vop2,
    Vopc,
    Vop3,
    Vintrp,
    Ds,
    Flat,
    Vop3p,
    Mimg,
    Exp,
};

const char* GcnEncodingName(GcnEncoding encoding);

// Mirrors SharpEmu's Gen5OperandKind.
enum class GcnOperandKind {
    ScalarRegister,
    VectorRegister,
    EncodedConstant,
    LiteralConstant,
};

struct GcnOperand {
    GcnOperandKind kind  = GcnOperandKind::EncodedConstant;
    u32           value = 0;

    static GcnOperand Scalar(u32 index);
    static GcnOperand Vector(u32 index);
    // Classifies a raw encoded source operand (9-bit VOP / 8-bit SOP form).
    // `literal` is the trailing literal dword when the encoding carries one.
    static GcnOperand Source(u32 encoded, std::optional<u32> literal = std::nullopt);

    std::string ToString() const;
};

// ---- Instruction control records (mirror SharpEmu's Gen5*Control) ---------

struct GcnImageControl {
    u32              dmask          = 0;
    u32              vector_address = 0;
    std::vector<u32> address_registers;
    u32              vector_data    = 0;
    u32              scalar_resource = 0;
    u32              scalar_sampler  = 0;
    u32              dimension       = 0;
    bool             is_array = false;
    bool             glc      = false;
    bool             slc      = false;
    bool             a16      = false;
    bool             d16      = false;
};

struct GcnGlobalMemoryControl {
    u32 dword_count    = 0;
    u32 vector_address = 0;
    u32 vector_data    = 0;
    u32 scalar_address = 0;
    s32 offset_bytes   = 0;
    bool glc = false;
    bool slc = false;
};

struct GcnBufferMemoryControl {
    u32 dword_count     = 0;
    u32 vector_address  = 0;
    u32 vector_data     = 0;
    u32 scalar_resource = 0;
    s32 offset_bytes    = 0;
    bool index_enabled  = false;
    bool offset_enabled = false;
    bool glc            = false;
    bool slc            = false;
};

struct GcnExportControl {
    u32  target     = 0;
    u32  enable_mask = 0;
    bool compressed = false;
    bool done       = false;
    bool valid_mask = false;
};

struct GcnInterpolationControl {
    u32 attribute = 0;
    u32 channel   = 0;
};

struct GcnVop3Control {
    u32                absolute_mask = 0;
    u32                negate_mask   = 0;
    u32                output_modifier = 0;
    bool               clamp         = false;
    u32                operand_select = 0;
    std::optional<u32> scalar_destination;
};

struct GcnSdwaControl {
    u32                destination_select = 0;
    u32                destination_unused = 0;
    u32                source0_select = 0;
    u32                source1_select = 0;
    bool               source0_sign_extend = false;
    bool               source1_sign_extend = false;
    u32                absolute_mask  = 0;
    u32                negate_mask    = 0;
    u32                output_modifier = 0;
    bool               clamp          = false;
    std::optional<u32> scalar_destination;
};

struct GcnVop3pControl {
    u32  op_sel_mask    = 0;
    u32  op_sel_hi_mask = 0;
    u32  neg_lo_mask    = 0;
    u32  neg_hi_mask    = 0;
    bool clamp          = false;
};

struct GcnDppControl {
    u32  control        = 0;
    bool fetch_inactive = false;
    bool bound_control  = false;
    u32  absolute_mask  = 0;
    u32  negate_mask    = 0;
    u32  bank_mask      = 0;
    u32  row_mask       = 0;
};

struct GcnDpp8Control {
    u32  lane_selectors = 0;
    bool fetch_inactive = false;
};

struct GcnScalarMemoryControl {
    u32                destination_count      = 0;
    s32                immediate_offset_bytes = 0;
    std::optional<u32> dynamic_offset_register;
};

struct GcnDataShareControl {
    u32  offset0 = 0;
    u32  offset1 = 0;
    bool gds     = false;
};

using GcnControl = std::variant<
    std::monostate,
    GcnImageControl,
    GcnGlobalMemoryControl,
    GcnBufferMemoryControl,
    GcnExportControl,
    GcnInterpolationControl,
    GcnVop3Control,
    GcnSdwaControl,
    GcnVop3pControl,
    GcnDppControl,
    GcnDpp8Control,
    GcnScalarMemoryControl,
    GcnDataShareControl>;

// Mirrors SharpEmu's Gen5ShaderInstruction.
struct GcnInstruction {
    u32                     pc = 0;
    GcnEncoding             encoding = GcnEncoding::Vop2;
    std::string             opcode;
    // False when the opcode number is not in the decode tables.  The
    // instruction is still structurally decoded (size/operands/control);
    // opcode is then "Unknown<Encoding>0x..".
    bool                    opcode_known = true;
    std::vector<u32>        words;
    std::vector<GcnOperand> sources;
    std::vector<GcnOperand> destinations;
    GcnControl              control;
};

struct GcnProgram {
    std::vector<GcnInstruction> instructions;
};

// Decodes a single instruction at `pc` (byte offset) from the dword stream.
// `words_available` counts dwords readable from `code`.  On success returns
// true and sets `size_dwords` (1..5).  Returns false only when the encoding
// itself is reserved (never for a merely-unknown opcode).
bool GcnDecodeInstruction(
    const u32*  code,
    size_t      words_available,
    u32         pc,
    GcnInstruction& out,
    u32&        size_dwords,
    std::string& error);

// Linear decode of a shader program, stopping at S_ENDPGM (SharpEmu's
// TryDecodeProgram contract).  Trailing zero padding after S_ENDPGM is
// ignored; if the stream ends before S_ENDPGM but the undecoded tail is all
// zero dwords, decode still succeeds (on-disk .shader_text sections are
// padded).  Fails only on read overrun of a multi-dword instruction or a
// reserved encoding.
bool GcnDecodeProgram(
    const u32*  code,
    size_t      dword_count,
    GcnProgram& out,
    std::string& error);

// Category used for corpus histograms (SharpEmu's ClassifyInstruction).
const char* GcnClassifyInstruction(const std::string& opcode_name);

} // namespace GPU::Shader
