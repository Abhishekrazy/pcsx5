// SPIR-V module builder (Phase 5 M2).
//
// Guided transliteration of SharpEmu's SpirvModuleBuilder.cs
// (sharpemu_clone/src/SharpEmu.ShaderCompiler.Vulkan/SpirvModuleBuilder.cs):
// sectioned word emission, id bound tracking, deduplicated types/constants,
// decorations, entry points and function scaffolding.  Emits a SPIR-V 1.5
// word stream identical in layout to the C# original.
#pragma once

#include "../../common/types.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace GPU::Shader {

enum class SpirvOp : u16 {
    Nop = 0,
    Name = 5,
    Extension = 10,
    ExtInstImport = 11,
    ExtInst = 12,
    MemoryModel = 14,
    EntryPoint = 15,
    ExecutionMode = 16,
    Capability = 17,
    TypeVoid = 19,
    TypeBool = 20,
    TypeInt = 21,
    TypeFloat = 22,
    TypeVector = 23,
    TypeImage = 25,
    TypeSampler = 26,
    TypeSampledImage = 27,
    TypeArray = 28,
    TypeRuntimeArray = 29,
    TypeStruct = 30,
    TypePointer = 32,
    TypeFunction = 33,
    ConstantTrue = 41,
    ConstantFalse = 42,
    Constant = 43,
    ConstantComposite = 44,
    ConstantNull = 46,
    Function = 54,
    FunctionParameter = 55,
    FunctionEnd = 56,
    FunctionCall = 57,
    Variable = 59,
    ImageTexelPointer = 60,
    Load = 61,
    Store = 62,
    AccessChain = 65,
    ArrayLength = 68,
    Decorate = 71,
    VectorExtractDynamic = 77,
    VectorInsertDynamic = 78,
    VectorShuffle = 79,
    CompositeConstruct = 80,
    CompositeExtract = 81,
    CompositeInsert = 82,
    CopyObject = 83,
    SampledImage = 86,
    ImageSampleImplicitLod = 87,
    ImageSampleExplicitLod = 88,
    ImageSampleDrefImplicitLod = 89,
    ImageSampleDrefExplicitLod = 90,
    ImageFetch = 95,
    ImageGather = 96,
    ImageDrefGather = 97,
    ImageRead = 98,
    ImageWrite = 99,
    Image = 100,
    ImageQuerySizeLod = 103,
    ImageQuerySize = 104,
    ImageQueryLod = 105,
    ImageQueryLevels = 106,
    ImageQuerySamples = 107,
    ConvertFToU = 109,
    ConvertFToS = 110,
    ConvertSToF = 111,
    ConvertUToF = 112,
    UConvert = 113,
    SConvert = 114,
    FConvert = 115,
    Bitcast = 124,
    SNegate = 126,
    FNegate = 127,
    IAdd = 128,
    FAdd = 129,
    ISub = 130,
    FSub = 131,
    IMul = 132,
    FMul = 133,
    UDiv = 134,
    SDiv = 135,
    FDiv = 136,
    UMod = 137,
    SRem = 138,
    SMod = 139,
    FRem = 140,
    FMod = 141,
    IAddCarry = 149,
    ISubBorrow = 150,
    UMulExtended = 151,
    SMulExtended = 152,
    Any = 154,
    All = 155,
    IsNan = 156,
    IsInf = 157,
    LogicalEqual = 164,
    LogicalNotEqual = 165,
    LogicalOr = 166,
    LogicalAnd = 167,
    LogicalNot = 168,
    Select = 169,
    IEqual = 170,
    INotEqual = 171,
    UGreaterThan = 172,
    SGreaterThan = 173,
    UGreaterThanEqual = 174,
    SGreaterThanEqual = 175,
    ULessThan = 176,
    SLessThan = 177,
    ULessThanEqual = 178,
    SLessThanEqual = 179,
    FOrdEqual = 180,
    FUnordEqual = 181,
    FOrdNotEqual = 182,
    FUnordNotEqual = 183,
    FOrdLessThan = 184,
    FUnordLessThan = 185,
    FOrdGreaterThan = 186,
    FUnordGreaterThan = 187,
    FOrdLessThanEqual = 188,
    FUnordLessThanEqual = 189,
    FOrdGreaterThanEqual = 190,
    FUnordGreaterThanEqual = 191,
    ShiftRightLogical = 194,
    ShiftRightArithmetic = 195,
    ShiftLeftLogical = 196,
    BitwiseOr = 197,
    BitwiseXor = 198,
    BitwiseAnd = 199,
    Not = 200,
    BitFieldInsert = 201,
    BitFieldSExtract = 202,
    BitFieldUExtract = 203,
    BitReverse = 204,
    BitCount = 205,
    ControlBarrier = 224,
    MemoryBarrier = 225,
    AtomicExchange = 229,
    AtomicCompareExchange = 230,
    AtomicIIncrement = 232,
    AtomicIDecrement = 233,
    AtomicIAdd = 234,
    AtomicISub = 235,
    AtomicSMin = 236,
    AtomicUMin = 237,
    AtomicSMax = 238,
    AtomicUMax = 239,
    AtomicAnd = 240,
    AtomicOr = 241,
    AtomicXor = 242,
    Phi = 245,
    LoopMerge = 246,
    SelectionMerge = 247,
    Label = 248,
    Branch = 249,
    BranchConditional = 250,
    Switch = 251,
    Kill = 252,
    Return = 253,
    ReturnValue = 254,
    Unreachable = 255,
    GroupNonUniformElect = 333,
    GroupNonUniformAll = 334,
    GroupNonUniformAny = 335,
    GroupNonUniformAllEqual = 336,
    GroupNonUniformBroadcast = 337,
    GroupNonUniformBroadcastFirst = 338,
    GroupNonUniformBallot = 339,
    GroupNonUniformShuffle = 345,
    GroupNonUniformShuffleXor = 346,
    GroupNonUniformShuffleUp = 347,
    GroupNonUniformShuffleDown = 348,
};

enum class SpirvCapability : u32 {
    Shader = 1,
    Float16 = 9,
    Float64 = 10,
    Int64 = 11,
    Int16 = 22,
    ImageGatherExtended = 25,
    StorageImageExtendedFormats = 49,
    ImageQuery = 50,
    StorageImageReadWithoutFormat = 55,
    StorageImageWriteWithoutFormat = 56,
    GroupNonUniform = 61,
    GroupNonUniformVote = 62,
    GroupNonUniformBallot = 64,
    GroupNonUniformShuffle = 65,
    RuntimeDescriptorArray = 5302,
};

enum class SpirvStorageClass : u32 {
    UniformConstant = 0,
    Input = 1,
    Uniform = 2,
    Output = 3,
    Workgroup = 4,
    Private = 6,
    Function = 7,
    PushConstant = 9,
    Image = 11,
    StorageBuffer = 12,
};

enum class SpirvExecutionModel : u32 {
    Vertex = 0,
    Fragment = 4,
    GLCompute = 5,
};

enum class SpirvExecutionMode : u32 {
    OriginUpperLeft = 7,
    DepthReplacing = 12,
    LocalSize = 17,
};

enum class SpirvDecoration : u32 {
    Block = 2,
    ArrayStride = 6,
    BuiltIn = 11,
    NoPerspective = 13,
    Flat = 14,
    Location = 30,
    Binding = 33,
    DescriptorSet = 34,
    Offset = 35,
    NoContraction = 42,
};

enum class SpirvBuiltIn : u32 {
    Position = 0,
    FragCoord = 15,
    FrontFacing = 17,
    WorkgroupId = 26,
    LocalInvocationId = 27,
    GlobalInvocationId = 28,
    LocalInvocationIndex = 29,
    SubgroupSize = 36,
    SubgroupLocalInvocationId = 41,
    VertexIndex = 42,
    InstanceIndex = 43,
};

enum class SpirvImageDim : u32 {
    Dim1D = 0,
    Dim2D = 1,
    Dim3D = 2,
    Cube = 3,
    Buffer = 5,
};

enum class SpirvImageFormat : u32 {
    Unknown = 0,
    Rgba32f = 1,
    Rgba16f = 2,
    R32f = 3,
    Rgba8 = 4,
    Rgba8Snorm = 5,
    Rg32f = 6,
    Rg16f = 7,
    R11fG11fB10f = 8,
    R16f = 9,
    Rgba16 = 10,
    Rgb10A2 = 11,
    Rg16 = 12,
    Rg8 = 13,
    R16 = 14,
    R8 = 15,
    Rgba16Snorm = 16,
    Rg16Snorm = 17,
    Rg8Snorm = 18,
    R16Snorm = 19,
    R8Snorm = 20,
    Rgba32i = 21,
    Rgba16i = 22,
    Rgba8i = 23,
    R32i = 24,
    Rg32i = 25,
    Rg16i = 26,
    Rg8i = 27,
    R16i = 28,
    R8i = 29,
    Rgba32ui = 30,
    Rgba16ui = 31,
    Rgba8ui = 32,
    R32ui = 33,
    Rgb10A2ui = 34,
    Rg32ui = 35,
    Rg16ui = 36,
    Rg8ui = 37,
    R16ui = 38,
    R8ui = 39,
};

// Direct port of SharpEmu's SpirvModuleBuilder.  Method names and emission
// order match the C# original so emitted word streams are comparable.
class SpirvModuleBuilder {
public:
    u32 AllocateId();

    void AddCapability(SpirvCapability capability);
    void AddExtension(const std::string& extension);
    u32  ImportExtInst(const std::string& name);
    void SetLogicalGlsl450MemoryModel();
    void AddEntryPoint(SpirvExecutionModel model, u32 function,
                       const std::string& name,
                       const std::vector<u32>& interfaces);
    void AddExecutionMode(u32 function, SpirvExecutionMode mode,
                          const std::vector<u32>& operands = {});
    void AddName(u32 target, const std::string& name);
    void AddDecoration(u32 target, SpirvDecoration decoration,
                       const std::vector<u32>& operands = {});
    void AddMemberDecoration(u32 target, u32 member, SpirvDecoration decoration,
                             const std::vector<u32>& operands = {});

    u32 TypeVoid();
    u32 TypeBool();
    u32 TypeInt(u32 width, bool signed_);
    u32 TypeFloat(u32 width);
    u32 TypeVector(u32 component_type, u32 count);
    u32 TypeImage(u32 sampled_type, SpirvImageDim dimension, bool depth,
                  bool arrayed, bool multisampled, u32 sampled,
                  SpirvImageFormat format);
    u32 TypeSampledImage(u32 image_type);
    u32 TypeArray(u32 element_type, u32 count);
    u32 TypeRuntimeArray(u32 element_type);
    u32 TypeStruct(const std::vector<u32>& member_types);
    u32 TypePointer(SpirvStorageClass storage_class, u32 type);
    u32 TypeFunction(u32 return_type, const std::vector<u32>& parameter_types);

    u32 ConstantBool(bool value);
    u32 Constant(u32 type, u32 value);
    u32 Constant64(u32 type, u64 value);
    u32 ConstantFloat(u32 type, float value);
    u32 ConstantComposite(u32 type, const std::vector<u32>& constituents);
    u32 ConstantNull(u32 type);

    u32 AddGlobalVariable(u32 pointer_type, SpirvStorageClass storage_class,
                          std::optional<u32> initializer = std::nullopt);
    u32 BeginFunction(u32 return_type, u32 function_type);
    u32 AddFunctionParameter(u32 type);
    u32 AddLabel(std::optional<u32> id = std::nullopt);
    u32 AddFunctionVariable(u32 pointer_type,
                            std::optional<u32> initializer = std::nullopt);
    u32  AddInstruction(SpirvOp opcode, u32 result_type,
                        const std::vector<u32>& operands = {});
    void AddStatement(SpirvOp opcode, const std::vector<u32>& operands = {});
    void EndFunction();

    // Assembles the final word stream (header + all sections in order).
    std::vector<u32> Build();

private:
    u32 EmitResult(std::vector<u32>& section, SpirvOp opcode, u32 result_type,
                   const std::vector<u32>& operands);

    static void Emit(std::vector<u32>& section, SpirvOp opcode,
                     const std::vector<u32>& operands);
    static void EmitRaw(std::vector<u32>& section, u16 opcode,
                        const std::vector<u32>& operands);
    static void EmitWithString(std::vector<u32>& section, SpirvOp opcode,
                               const std::vector<u32>& prefix,
                               const std::string& value,
                               int string_before_tail_count = -1);
    static std::vector<u32> EncodeString(const std::string& value);

    std::vector<u32> capabilities_;
    std::vector<u32> extensions_;
    std::vector<u32> imports_;
    std::vector<u32> memory_model_;
    std::vector<u32> entry_points_;
    std::vector<u32> execution_modes_;
    std::vector<u32> debug_;
    std::vector<u32> annotations_;
    std::vector<u32> types_constants_globals_;
    std::vector<u32> functions_;

    std::map<std::tuple<u32, bool>, u32> integer_types_;
    std::map<u32, u32>                   float_types_;
    std::map<std::tuple<u32, u32>, u32>  vector_types_;
    std::map<std::tuple<u32, SpirvImageDim, bool, bool, bool, u32,
                        SpirvImageFormat>,
             u32>                        image_types_;
    std::map<u32, u32>                   sampled_image_types_;
    std::map<std::tuple<SpirvStorageClass, u32>, u32> pointer_types_;
    std::map<std::tuple<u32, u32>, u32>  array_types_;
    std::map<u32, u32>                   runtime_array_types_;
    std::map<std::string, u32>           function_types_;
    std::map<std::tuple<u32, u64>, u32>  constants_;
    std::set<SpirvCapability>            declared_capabilities_;
    std::map<std::string, u32>           ext_inst_imports_;
    u32                                  next_id_ = 1;
    std::optional<u32>                   void_type_;
    std::optional<u32>                   bool_type_;
};

} // namespace GPU::Shader
