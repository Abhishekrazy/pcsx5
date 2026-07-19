// RDNA2 shader scalar-state evaluator (Phase 5 M3, slice 1).
//
// Guided transliteration of SharpEmu's Gen5ShaderScalarEvaluator.cs
// (sharpemu_clone/src/SharpEmu.ShaderCompiler).  Symbolically executes a
// shader's scalar instruction stream over a path stack (EXEC mask + SCC),
// following forward S_BRANCH targets and re-visiting their fall-through
// regions once as supplemental resource discovery, to resolve the resource
// descriptors the pipeline stage needs per draw:
//
//   * image bindings — the 8-dword resource + 4-dword sampler descriptors
//     copied verbatim from the evaluated SGPR pairs (texture truth);
//   * buffer bindings — decoded Gfx10 buffer descriptors (base/size) for
//     every scalar/buffer load, keyed by (scalar register, base address).
//
// Descriptor truth comes from evaluated SGPRs (user-data registers seeded
// from the draw-time register shadow), never from metadata tables.
#pragma once

#include "../../common/types.h"
#include "gcn_decode.h"

#include <array>
#include <vector>

namespace GPU::Shader {

// Decoded Gfx10 buffer descriptor (TryDecodeBufferDescriptor).
struct GcnBufferDescriptor {
    u64 base_address  = 0;
    u32 stride        = 0;
    u32 num_records   = 0;
    u64 size_bytes    = 0;
    u32 number_format = 0;
    u32 data_format   = 0;
};

// One image instruction's evaluated descriptors.
struct GcnEvalImageBinding {
    u32                 pc = 0;
    std::string         opcode;
    std::array<u32, 8>  resource_descriptor{};
    std::array<u32, 4>  sampler_descriptor{};
    bool                has_sampler = false;
};

// One scalar/buffer load group: every instruction pc that reads through the
// same (scalar register, base address) pair.
struct GcnEvalBufferBinding {
    u32              scalar_address = 0;
    u64              base_address   = 0;
    u64              size_bytes     = 0;
    bool             writable       = false;
    std::vector<u32> instruction_pcs;
};

struct GcnEvaluation {
    std::vector<u32> initial_scalar_registers; // 256 dwords (seeded)
    std::vector<u32> scalar_registers;         // 256 dwords (final path)
    std::vector<GcnEvalImageBinding>  image_bindings;
    std::vector<GcnEvalBufferBinding> buffer_bindings;
};

// Decodes the 4-dword Gfx10 buffer descriptor at scalarRegisters[base..base+3].
// strict_type rejects non-zero descriptor types (image-style); otherwise a
// typed descriptor degrades to null (returns true with zeros).
bool GcnTryDecodeBufferDescriptor(const std::vector<u32>& scalar_registers,
                                  u32 base, bool strict_type,
                                  GcnBufferDescriptor& out);

// RDNA2 ISA table 47: unified FORMAT field -> (data format, number format).
bool GcnTryDecodeUnifiedFormat(u32 unified, u32& data_format, u32& number_format);

// Symbolically executes the program's scalar stream.  `user_data` holds the
// draw-time user SGPR dwords seeded at `user_data_scalar_register_base`
// (PS=0x0C, VS=0x4C, GS=0x8C, ES=0xCC).  `program_address` is only used for
// S_GETPC_B64 results.  Guest memory reads go through the emulator's Memory
// service.  Fails with `error` naming the instruction and cause on invalid
// state; unsupported scalar opcodes evaluate as best-effort no-ops (the
// descriptor discovery paths are always fully supported).
bool GcnEvaluateScalarState(const GcnProgram&    program,
                            u64                  program_address,
                            const std::vector<u32>& user_data,
                            u32                  user_data_scalar_register_base,
                            GcnEvaluation&       out,
                            std::string&         error);

} // namespace GPU::Shader
