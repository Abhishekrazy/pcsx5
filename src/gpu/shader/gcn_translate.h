// RDNA2 (GCN Gen5) -> SPIR-V translator (Phase 5 M2, slice 2).
//
// Guided transliteration of SharpEmu's Gen5SpirvTranslator.cs /
// Gen5SpirvTranslator.Alu.cs (sharpemu_clone/src/SharpEmu.ShaderCompiler.Vulkan)
// onto our M2.1 decoder (gcn_decode.h) and SPIR-V module builder
// (spirv_builder.h).
//
// Translation model (identical to the C# original):
//   * One SPIR-V invocation emulates one guest wave lane.  VCC/EXEC/SCC
//     collapse to per-lane booleans; the guest-visible scalar pairs
//     s106:s107 (VCC) and s126:s127 (EXEC) are kept in sync by StoreS.
//   * SGPRs and VGPRs are Private u32 arrays (256 each) holding raw
//     dwords; floats are bitcast on use.  Vector stores are predicated
//     by EXEC via OpSelect.
//   * Control flow runs through a PC dispatcher: the guest program is
//     split into basic blocks, an OpSwitch selects the block for the
//     current index, and each block terminator stores the next block
//     index into a Private program-counter variable.  No structural
//     analysis is required for arbitrary guest branches.
//   * Guest memory resources arrive as caller-supplied bindings
//     (SharpEmu computes them per-draw with the scalar evaluator; M3
//     will drive ours from the AGC submit walker).  Scalar/buffer loads
//     read u32 words from a StorageBuffer array-of-blocks with
//     bounds-checked access; image instructions bind one
//     UniformConstant image per instruction PC.
#pragma once

#include "../../common/types.h"
#include "gcn_decode.h"
#include "metadata.h"

#include <array>
#include <vector>

namespace GPU::Shader {

enum class GcnSpirvStage {
    Vertex,
    Pixel,
};

// Pixel render-target kinds (mirrors SharpEmu's Gen5PixelOutputKind).
enum class GcnPixelOutputKind {
    Float,
    Uint,
    Sint,
};

// One guest MRT export slot routed to a host fragment output location.
struct GcnPixelOutputBinding {
    u32 guest_slot     = 0;
    u32 host_location  = 0;
    GcnPixelOutputKind kind = GcnPixelOutputKind::Float;
};

// Image binding for a single image instruction (keyed by its pc).
// Mirrors the subset of SharpEmu's Gen5ImageBinding the emitter needs.
struct GcnSpirvImageBinding {
    u32 pc              = 0;
    u32 mip_level       = 0;    // ImageLoadMip only
    bool is_storage     = false;
    int component_kind  = 0;    // 0 = float, 1 = sint, 2 = uint
};

// Scalar/buffer memory binding: all listed instruction pcs read u32
// words from one StorageBuffer block (SharpEmu's Gen5GlobalMemoryBinding).
struct GcnSpirvBufferBinding {
    u32              scalar_address = 0; // diagnostic: SGPR holding the base
    std::vector<u32> instruction_pcs;
};

struct GcnTranslateOptions {
    GcnSpirvStage stage = GcnSpirvStage::Vertex;

    // Initial SGPR contents baked into the module as constants
    // (SharpEmu's `_initialScalarBufferIndex < 0` path).  Entries past
    // the vector end read as zero.
    std::vector<u32> initial_scalar_registers;

    // >= 0: load consumed SGPRs from this global-buffer binding at shader
    // start instead of baking constants (per-draw scalar state — the draw
    // executor packs evaluation.initial_scalar_registers into it; port of
    // SharpEmu's `_initialScalarBufferIndex >= 0` path).  The index refers
    // to a slot in buffer_bindings; use GcnTranslateAddInitialScalarBinding
    // to append it and GcnPackInitialScalarState to fill it per draw.
    int initial_scalar_buffer_index = -1;

    std::vector<GcnSpirvImageBinding>  image_bindings;
    std::vector<GcnSpirvBufferBinding> buffer_bindings;

    // Pixel stage: MRT outputs.  Vertex stage: number of param
    // locations the paired fragment shader reads (extras are
    // zero-filled so the interface always matches).
    std::vector<GcnPixelOutputBinding> pixel_outputs;
    int required_vertex_output_count = 0;

    // Pixel stage: SPI_PS_INPUT_ENA / SPI_PS_INPUT_ADDR register
    // values driving the initial VGPR compaction (EmitPixelInputState).
    u32 pixel_input_enable = 0;
    u32 pixel_input_address = 0;

    // Dispatcher-iteration safety valve (0 = unbounded).  Matches
    // SharpEmu's SHARPEMU_SHADER_MAX_STEPS default.
    u32 max_dispatcher_steps = 100000;
};

struct GcnSpirvShader {
    std::vector<u32> words;
    // Number of stage-interface attributes the program consumes
    // (vertex: exported params; pixel: interpolated inputs).
    u32 attribute_count = 0;
};

// Translates a decoded program to a SPIR-V 1.5 word stream.  Fails on
// reserved encodings, unsupported opcodes/controls, or invalid stage
// bindings; `error` names the instruction and cause.
bool GcnTranslateToSpirv(const GcnProgram&         program,
                         const GcnTranslateOptions& options,
                         GcnSpirvShader&           out,
                         std::string&              error);

// Builds a standalone binding set for a program with no runtime descriptor
// state: pixel outputs from export targets, one sampled-2D-float image per
// image instruction pc, global buffers grouped by scalar base register,
// vertex output coverage from param exports.  Used by the corpus tool and
// by the M3 draw path until per-draw descriptor evaluation lands.
GcnTranslateOptions GcnTranslateDefaultOptions(const GcnProgram& program,
                                               GcnSpirvStage     stage);

// 256-bit consumed-SGPR mask (port of SharpEmu's
// Gen5ShaderTranslator.ComputeConsumedScalarMask).  A bit is set for every
// SGPR the program can observe: scalar sources (pairs), SMEM/SMRD bases
// (4-dword descriptors), image resource/sampler descriptors (8/4), dynamic
// offsets, GLOBAL/MUBUF scalar addresses, plus the always-live wave-mask
// pairs s106:s107 (VCC), s124:s125, s126:s127 (EXEC).
using GcnConsumedScalarMask = std::array<u64, 4>;
GcnConsumedScalarMask GcnComputeConsumedScalarMask(const GcnProgram& program);
bool GcnIsScalarConsumed(const GcnConsumedScalarMask& mask, u32 reg);

// Appends the per-draw initial-scalar storage-buffer slot to
// options.buffer_bindings (no instruction pcs — the shader reads it only at
// start) and points options.initial_scalar_buffer_index at it.  Returns the
// new binding index.
int GcnTranslateAddInitialScalarBinding(GcnTranslateOptions& options);

// Packs the per-draw scalar-state buffer contents for a shader translated
// with initial_scalar_buffer_index >= 0: the evaluation's initial SGPR
// dwords zero-padded to 256 (SharpEmu's PackRuntimeScalarState minus the
// buffer-bias tail our binding model does not need yet).
std::vector<u32> GcnPackInitialScalarState(
    const std::vector<u32>& initial_scalar_registers);

} // namespace GPU::Shader
