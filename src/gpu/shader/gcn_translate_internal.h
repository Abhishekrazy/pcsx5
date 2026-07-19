// Internal translation context shared between gcn_translate.cpp (skeleton,
// stage IO, memory, image, export) and gcn_translate_alu.cpp (scalar/vector
// ALU).  Mirrors the field/method layout of SharpEmu's
// Gen5SpirvTranslator.CompilationContext (a C# partial class split across
// the same two files).
#pragma once

#include "gcn_translate.h"
#include "spirv_builder.h"

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace GPU::Shader {

// GLSL.std.450 extended instruction numbers used by the emitter.
namespace GlslExt {
inline constexpr u32 RoundEven      = 2;
inline constexpr u32 Trunc          = 3;
inline constexpr u32 FAbs           = 4;
inline constexpr u32 SAbs           = 5;
inline constexpr u32 Floor          = 8;
inline constexpr u32 Ceil           = 9;
inline constexpr u32 Fract          = 10;
inline constexpr u32 Sin            = 13;
inline constexpr u32 Cos            = 14;
inline constexpr u32 Exp2           = 29;
inline constexpr u32 Log2           = 30;
inline constexpr u32 Sqrt           = 31;
inline constexpr u32 InverseSqrt    = 32;
inline constexpr u32 FMin           = 37;
inline constexpr u32 UMin           = 38;
inline constexpr u32 SMin           = 39;
inline constexpr u32 FMax           = 40;
inline constexpr u32 UMax           = 41;
inline constexpr u32 SMax           = 42;
inline constexpr u32 FClamp         = 43;
inline constexpr u32 Fma            = 50;
inline constexpr u32 Ldexp          = 53;
inline constexpr u32 PackHalf2x16   = 58;
inline constexpr u32 UnpackHalf2x16 = 62;
inline constexpr u32 FindILsb       = 73;
} // namespace GlslExt

class SpirvTranslateContext {
public:
    SpirvTranslateContext(const GcnProgram&          program,
                          const GcnTranslateOptions& options);

    bool TryCompile(GcnSpirvShader& out, std::string& error);

private:
    // ---- constants -------------------------------------------------
    static constexpr u32 kScalarRegisterCount = 256;
    static constexpr u32 kVectorRegisterCount = 256;

    enum ImageComponentKind { kImageFloat = 0, kImageSint = 1, kImageUint = 2 };

    struct ImageResource {
        u32 variable       = 0;
        u32 image_type     = 0;
        u32 object_type    = 0;
        u32 component_type = 0;
        u32 vector_type    = 0;
        ImageComponentKind component_kind = kImageFloat;
        bool is_storage    = false;
    };

    struct PixelOutput {
        u32 variable = 0;
        u32 type     = 0;
        GcnPixelOutputKind kind = GcnPixelOutputKind::Float;
    };

    struct ShaderBlock {
        u32 start_pc    = 0;
        u32 start_index = 0;
        u32 end_index   = 0;
    };

    // ---- skeleton (gcn_translate.cpp) ------------------------------
    void DeclareModule();
    void DeclareBuffers();
    void DeclareImages();
    void DeclareStageInterface();
    void EmitInitialState();
    void EmitPixelInputState(u32 frag_coord);
    void AdvancePixelInput(int bit, u32 dword_count, u32& vgpr);
    void EmitPixelPositionInput(int bit, u32 component, u32 frag_coord, u32& vgpr);

    static std::vector<ShaderBlock> BuildBasicBlocks(const GcnProgram& program);
    static bool IsBranch(const std::string& opcode);
    static bool TryGetBranchTargetPc(const GcnInstruction& instruction, u32& target_pc);
    static bool TryFindBlock(const std::vector<ShaderBlock>& blocks, u32 pc, u32& block_index);
    static bool IsExitBranchTarget(const GcnProgram& program, u32 target_pc);
    bool TryGetBranchCondition(const std::string& opcode, u32& condition);
    bool TryEmitBlock(const std::vector<ShaderBlock>& blocks, u32 block_index, std::string& error);
    bool TryEmitInstruction(const GcnInstruction& instruction, std::string& error);

    bool TryEmitInterpolation(const GcnInstruction& instruction, const GcnInterpolationControl& interp, std::string& error);
    bool TryEmitScalarMemory(const GcnInstruction& instruction, const GcnScalarMemoryControl& control, std::string& error);
    bool TryEmitBufferMemory(const GcnInstruction& instruction, const GcnBufferMemoryControl& control, std::string& error);
    bool TryEmitImage(const GcnInstruction& instruction, const GcnImageControl& image, std::string& error);
    bool TryEmitExport(const GcnInstruction& instruction, const GcnExportControl& export_, std::string& error);

    // ---- buffer / image helpers (gcn_translate.cpp) -----------------
    u32  LoadBufferWord(int binding, u32 dword_address);
    u32  IsBufferWordInRange(int binding, u32 dword_address);
    u32  BufferWordPointer(int binding, u32 dword_address);
    u32  LoadImageFloatAddress(const GcnImageControl& image, int component);
    u32  BuildFloatCoordinates(const GcnImageControl& image, int start);

    // ---- register file helpers (gcn_translate.cpp) ------------------
    u32  ScalarPointer(u32 reg);
    u32  VectorPointer(u32 reg);
    u32  LoadS(u32 reg);
    u32  LoadV(u32 reg);
    void StoreS(u32 reg, u32 value);
    void StoreV(u32 reg, u32 value, bool guard_with_exec = true);
    u32  Load(u32 type, u32 pointer);
    void Store(u32 pointer, u32 value);

    // ---- wave-mask helpers (single-lane graphics model) -------------
    u32  IsWaveMaskActive(u32 mask64);   // == IsNotZero64 (1 emulated lane)
    void StoreWaveMask(u32 reg, u32 condition);
    u32  BooleanToWaveMask(u32 condition);

    // ---- small emission helpers (gcn_translate.cpp) -----------------
    u32  UInt(u32 value);
    u32  Float(float value);
    u32  Bitcast(u32 type, u32 value);
    u32  IAdd(u32 left, u32 right);
    u32  ShiftLeftLogical(u32 left, u32 right);
    u32  ShiftRightLogical(u32 left, u32 right);
    u32  ShiftRightArithmetic(u32 left, u32 right);
    u32  ShiftLeftLogical64(u32 left, u32 right);
    u32  ShiftRightLogical64(u32 left, u32 right);
    u32  BitwiseAnd(u32 left, u32 right);
    u32  BitwiseOr(u32 left, u32 right);
    u32  BitwiseXor(u32 left, u32 right);
    u32  LogicalNot(u32 value);
    u32  Ext(u32 operation, u32 result_type, const std::vector<u32>& operands);
    u32  IsNotZero(u32 value);
    u32  IsNotZero64(u32 value);
    void EmitConditional(u32 condition, const std::function<void()>& emit);

    // ---- ALU (gcn_translate_alu.cpp) --------------------------------
    bool TryEmitScalarAlu(const GcnInstruction& instruction, std::string& error);
    bool TryEmitScalarCompare(const GcnInstruction& instruction, std::string& error);
    bool TryEmitScalar64(const GcnInstruction& instruction, u32 destination, std::string& error);
    bool TryEmitVectorAlu(const GcnInstruction& instruction, std::string& error);
    bool TryEmitVectorCompare(const GcnInstruction& instruction, std::string& error);

    bool TryGetVectorDestination(const GcnInstruction& instruction, u32& destination);
    u32  GetRawSource(const GcnInstruction& instruction, int source_index,
                      bool apply_sdwa_integer_modifiers = true);
    u32  ApplySdwaDestination(const GcnSdwaControl& control, u32 value, u32 previous);
    u32  GetRawSource64(const GcnInstruction& instruction, int source_index);
    u32  GetFloatSource(const GcnInstruction& instruction, int source_index);
    u32  LoadS64(u32 reg);
    void StoreS64(u32 reg, u32 value);
    u32  EmitFloatBinary(const GcnInstruction& instruction, SpirvOp operation, bool reverse = false);
    u32  EmitFloatExtBinary(const GcnInstruction& instruction, u32 operation);
    u32  EmitFloatTernaryExt(const GcnInstruction& instruction, u32 operation);
    u32  EmitIntegerBinary(const GcnInstruction& instruction, SpirvOp operation, bool reverse = false);
    u32  EmitFloatResult(const GcnInstruction& instruction, u32 value);
    u32  EmitFloatClamp(u32 value);
    u32  TruncateFloat32ForPack(u32 value);
    u32  SignBit(u32 value);
    u32  SignedAddOverflow(u32 left, u32 right, u32 result);
    u32  SignedSubOverflow(u32 left, u32 right, u32 result);
    void StoreCarryOut(const GcnInstruction& instruction, u32 carry);
    u32  EmitAddWithCarry(const GcnInstruction& instruction);
    u32  EmitSubtractWithBorrow(const GcnInstruction& instruction, bool reverse);

    // ---- inputs ------------------------------------------------------
    const GcnProgram&          program_;
    const GcnTranslateOptions& options_;
    GcnSpirvStage              stage_;

    SpirvModuleBuilder module_;
    u32 glsl_ = 0;

    // ---- stage interface state --------------------------------------
    std::vector<u32>                    interfaces_;
    std::map<u32, u32>                  pixel_inputs_;      // attribute -> variable
    std::map<u32, PixelOutput>          pixel_outputs_;     // guest slot -> output
    std::map<u32, u32>                  vertex_outputs_;    // location -> variable
    std::vector<ImageResource>          image_resources_;
    std::map<u32, int>                  image_binding_by_pc_;
    std::map<u32, int>                  buffer_binding_by_pc_;
    u32 position_output_      = 0;
    u32 vertex_index_input_   = 0;
    u32 instance_index_input_ = 0;
    u32 frag_coord_input_     = 0;

    // ---- type ids ----------------------------------------------------
    u32 void_type_   = 0;
    u32 bool_type_   = 0;
    u32 uint_type_   = 0;
    u32 int_type_    = 0;
    u32 long_type_   = 0;
    u32 ulong_type_  = 0;
    u32 float_type_  = 0;
    u32 vec2_type_   = 0;
    u32 vec3_type_   = 0;
    u32 vec4_type_   = 0;
    u32 uvec2_type_  = 0;
    u32 uvec3_type_  = 0;
    u32 uvec4_type_  = 0;
    u32 private_uint_pointer_ = 0;
    u32 private_bool_pointer_ = 0;

    // ---- register file / dispatcher state ----------------------------
    u32 scalar_registers_      = 0;
    u32 vector_registers_      = 0;
    u32 scc_                   = 0;
    u32 vcc_                   = 0;
    u32 exec_                  = 0;
    u32 reached_pixel_export_  = 0;
    u32 program_counter_       = 0;
    u32 program_active_        = 0;
    u32 iteration_guard_       = 0;

    // ---- resources ---------------------------------------------------
    u32 global_buffers_        = 0;
    u32 storage_block_pointer_ = 0;
    u32 storage_uint_pointer_  = 0;
};

} // namespace GPU::Shader
