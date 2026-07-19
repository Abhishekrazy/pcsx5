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

} // namespace GPU::Shader
