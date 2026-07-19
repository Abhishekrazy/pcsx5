// SPIR-V module builder — transliteration of SharpEmu's SpirvModuleBuilder.cs.
// See spirv_builder.h.

#include "spirv_builder.h"

#include <bit>
#include <cstring>

namespace GPU::Shader {

namespace {
constexpr u32 kMagic     = 0x07230203;
constexpr u32 kVersion15 = 0x00010500;
constexpr u32 kGenerator = 0x53504500; // "SPE"
} // namespace

u32 SpirvModuleBuilder::AllocateId() {
    return next_id_++;
}

void SpirvModuleBuilder::AddCapability(SpirvCapability capability) {
    if (declared_capabilities_.insert(capability).second) {
        Emit(capabilities_, SpirvOp::Capability,
             {static_cast<u32>(capability)});
    }
}

void SpirvModuleBuilder::AddExtension(const std::string& extension) {
    EmitWithString(extensions_, SpirvOp::Extension, {}, extension);
}

u32 SpirvModuleBuilder::ImportExtInst(const std::string& name) {
    if (const auto it = ext_inst_imports_.find(name);
        it != ext_inst_imports_.end()) {
        return it->second;
    }
    const u32 id = AllocateId();
    EmitWithString(imports_, SpirvOp::ExtInstImport, {id}, name);
    ext_inst_imports_.emplace(name, id);
    return id;
}

void SpirvModuleBuilder::SetLogicalGlsl450MemoryModel() {
    Emit(memory_model_, SpirvOp::MemoryModel, {0, 1});
}

void SpirvModuleBuilder::AddEntryPoint(
    SpirvExecutionModel model, u32 function, const std::string& name,
    const std::vector<u32>& interfaces) {
    std::vector<u32> prefix;
    prefix.reserve(2 + interfaces.size());
    prefix.push_back(static_cast<u32>(model));
    prefix.push_back(function);
    prefix.insert(prefix.end(), interfaces.begin(), interfaces.end());
    EmitWithString(entry_points_, SpirvOp::EntryPoint, prefix, name,
                   /*string_before_tail_count=*/2);
}

void SpirvModuleBuilder::AddExecutionMode(u32 function, SpirvExecutionMode mode,
                                          const std::vector<u32>& operands) {
    std::vector<u32> values;
    values.reserve(2 + operands.size());
    values.push_back(function);
    values.push_back(static_cast<u32>(mode));
    values.insert(values.end(), operands.begin(), operands.end());
    Emit(execution_modes_, SpirvOp::ExecutionMode, values);
}

void SpirvModuleBuilder::AddName(u32 target, const std::string& name) {
    EmitWithString(debug_, SpirvOp::Name, {target}, name);
}

void SpirvModuleBuilder::AddDecoration(u32 target, SpirvDecoration decoration,
                                       const std::vector<u32>& operands) {
    std::vector<u32> values;
    values.reserve(2 + operands.size());
    values.push_back(target);
    values.push_back(static_cast<u32>(decoration));
    values.insert(values.end(), operands.begin(), operands.end());
    Emit(annotations_, SpirvOp::Decorate, values);
}

void SpirvModuleBuilder::AddMemberDecoration(
    u32 target, u32 member, SpirvDecoration decoration,
    const std::vector<u32>& operands) {
    std::vector<u32> values;
    values.reserve(3 + operands.size());
    values.push_back(target);
    values.push_back(member);
    values.push_back(static_cast<u32>(decoration));
    values.insert(values.end(), operands.begin(), operands.end());
    EmitRaw(annotations_, /*opcode=*/72, values);
}

u32 SpirvModuleBuilder::TypeVoid() {
    if (void_type_.has_value()) {
        return *void_type_;
    }
    const u32 id = AllocateId();
    Emit(types_constants_globals_, SpirvOp::TypeVoid, {id});
    void_type_ = id;
    return id;
}

u32 SpirvModuleBuilder::TypeBool() {
    if (bool_type_.has_value()) {
        return *bool_type_;
    }
    const u32 id = AllocateId();
    Emit(types_constants_globals_, SpirvOp::TypeBool, {id});
    bool_type_ = id;
    return id;
}

u32 SpirvModuleBuilder::TypeInt(u32 width, bool signed_) {
    const auto key = std::make_tuple(width, signed_);
    if (const auto it = integer_types_.find(key); it != integer_types_.end()) {
        return it->second;
    }
    const u32 id = AllocateId();
    Emit(types_constants_globals_, SpirvOp::TypeInt,
         {id, width, signed_ ? 1u : 0u});
    integer_types_.emplace(key, id);
    return id;
}

u32 SpirvModuleBuilder::TypeFloat(u32 width) {
    if (const auto it = float_types_.find(width); it != float_types_.end()) {
        return it->second;
    }
    const u32 id = AllocateId();
    Emit(types_constants_globals_, SpirvOp::TypeFloat, {id, width});
    float_types_.emplace(width, id);
    return id;
}

u32 SpirvModuleBuilder::TypeVector(u32 component_type, u32 count) {
    const auto key = std::make_tuple(component_type, count);
    if (const auto it = vector_types_.find(key); it != vector_types_.end()) {
        return it->second;
    }
    const u32 id = AllocateId();
    Emit(types_constants_globals_, SpirvOp::TypeVector,
         {id, component_type, count});
    vector_types_.emplace(key, id);
    return id;
}

u32 SpirvModuleBuilder::TypeImage(
    u32 sampled_type, SpirvImageDim dimension, bool depth, bool arrayed,
    bool multisampled, u32 sampled, SpirvImageFormat format) {
    const auto key = std::make_tuple(sampled_type, dimension, depth, arrayed,
                                     multisampled, sampled, format);
    if (const auto it = image_types_.find(key); it != image_types_.end()) {
        return it->second;
    }
    const u32 id = AllocateId();
    Emit(types_constants_globals_, SpirvOp::TypeImage,
         {id,
          sampled_type,
          static_cast<u32>(dimension),
          depth ? 1u : 0u,
          arrayed ? 1u : 0u,
          multisampled ? 1u : 0u,
          sampled,
          static_cast<u32>(format)});
    image_types_.emplace(key, id);
    return id;
}

u32 SpirvModuleBuilder::TypeSampledImage(u32 image_type) {
    if (const auto it = sampled_image_types_.find(image_type);
        it != sampled_image_types_.end()) {
        return it->second;
    }
    const u32 id = AllocateId();
    Emit(types_constants_globals_, SpirvOp::TypeSampledImage, {id, image_type});
    sampled_image_types_.emplace(image_type, id);
    return id;
}

u32 SpirvModuleBuilder::TypeArray(u32 element_type, u32 count) {
    const auto key = std::make_tuple(element_type, count);
    if (const auto it = array_types_.find(key); it != array_types_.end()) {
        return it->second;
    }
    const u32 length = Constant(TypeInt(32, false), count);
    const u32 id     = AllocateId();
    Emit(types_constants_globals_, SpirvOp::TypeArray, {id, element_type, length});
    array_types_.emplace(key, id);
    return id;
}

u32 SpirvModuleBuilder::TypeRuntimeArray(u32 element_type) {
    if (const auto it = runtime_array_types_.find(element_type);
        it != runtime_array_types_.end()) {
        return it->second;
    }
    const u32 id = AllocateId();
    Emit(types_constants_globals_, SpirvOp::TypeRuntimeArray, {id, element_type});
    runtime_array_types_.emplace(element_type, id);
    return id;
}

u32 SpirvModuleBuilder::TypeStruct(const std::vector<u32>& member_types) {
    const u32 id = AllocateId();
    std::vector<u32> operands;
    operands.reserve(member_types.size() + 1);
    operands.push_back(id);
    operands.insert(operands.end(), member_types.begin(), member_types.end());
    Emit(types_constants_globals_, SpirvOp::TypeStruct, operands);
    return id;
}

u32 SpirvModuleBuilder::TypePointer(SpirvStorageClass storage_class, u32 type) {
    const auto key = std::make_tuple(storage_class, type);
    if (const auto it = pointer_types_.find(key); it != pointer_types_.end()) {
        return it->second;
    }
    const u32 id = AllocateId();
    Emit(types_constants_globals_, SpirvOp::TypePointer,
         {id, static_cast<u32>(storage_class), type});
    pointer_types_.emplace(key, id);
    return id;
}

u32 SpirvModuleBuilder::TypeFunction(u32 return_type,
                                     const std::vector<u32>& parameter_types) {
    std::string key = std::to_string(return_type) + ":";
    for (u32 parameter : parameter_types) {
        key += std::to_string(parameter);
        key += ',';
    }
    if (const auto it = function_types_.find(key); it != function_types_.end()) {
        return it->second;
    }
    const u32 id = AllocateId();
    std::vector<u32> operands;
    operands.reserve(parameter_types.size() + 2);
    operands.push_back(id);
    operands.push_back(return_type);
    operands.insert(operands.end(), parameter_types.begin(),
                    parameter_types.end());
    Emit(types_constants_globals_, SpirvOp::TypeFunction, operands);
    function_types_.emplace(key, id);
    return id;
}

u32 SpirvModuleBuilder::ConstantBool(bool value) {
    const u32 type = TypeBool();
    const auto key = std::make_tuple(type, value ? 1ull : 0ull);
    if (const auto it = constants_.find(key); it != constants_.end()) {
        return it->second;
    }
    const u32 id = AllocateId();
    Emit(types_constants_globals_,
         value ? SpirvOp::ConstantTrue : SpirvOp::ConstantFalse, {type, id});
    constants_.emplace(key, id);
    return id;
}

u32 SpirvModuleBuilder::Constant(u32 type, u32 value) {
    const auto key = std::make_tuple(type, static_cast<u64>(value));
    if (const auto it = constants_.find(key); it != constants_.end()) {
        return it->second;
    }
    const u32 id = AllocateId();
    Emit(types_constants_globals_, SpirvOp::Constant, {type, id, value});
    constants_.emplace(key, id);
    return id;
}

u32 SpirvModuleBuilder::Constant64(u32 type, u64 value) {
    const auto key = std::make_tuple(type, value);
    if (const auto it = constants_.find(key); it != constants_.end()) {
        return it->second;
    }
    const u32 id = AllocateId();
    Emit(types_constants_globals_, SpirvOp::Constant,
         {type, id, static_cast<u32>(value), static_cast<u32>(value >> 32)});
    constants_.emplace(key, id);
    return id;
}

u32 SpirvModuleBuilder::ConstantFloat(u32 type, float value) {
    return Constant(type, std::bit_cast<u32>(value));
}

u32 SpirvModuleBuilder::ConstantComposite(u32 type,
                                          const std::vector<u32>& constituents) {
    const u32 id = AllocateId();
    std::vector<u32> operands;
    operands.reserve(constituents.size() + 2);
    operands.push_back(type);
    operands.push_back(id);
    operands.insert(operands.end(), constituents.begin(), constituents.end());
    Emit(types_constants_globals_, SpirvOp::ConstantComposite, operands);
    return id;
}

u32 SpirvModuleBuilder::ConstantNull(u32 type) {
    const auto key = std::make_tuple(type, ~0ull);
    if (const auto it = constants_.find(key); it != constants_.end()) {
        return it->second;
    }
    const u32 id = AllocateId();
    Emit(types_constants_globals_, SpirvOp::ConstantNull, {type, id});
    constants_.emplace(key, id);
    return id;
}

u32 SpirvModuleBuilder::AddGlobalVariable(
    u32 pointer_type, SpirvStorageClass storage_class,
    std::optional<u32> initializer) {
    const u32 id = AllocateId();
    if (initializer.has_value()) {
        Emit(types_constants_globals_, SpirvOp::Variable,
             {pointer_type, id, static_cast<u32>(storage_class), *initializer});
    } else {
        Emit(types_constants_globals_, SpirvOp::Variable,
             {pointer_type, id, static_cast<u32>(storage_class)});
    }
    return id;
}

u32 SpirvModuleBuilder::BeginFunction(u32 return_type, u32 function_type) {
    const u32 id = AllocateId();
    Emit(functions_, SpirvOp::Function, {return_type, id, 0, function_type});
    return id;
}

u32 SpirvModuleBuilder::AddFunctionParameter(u32 type) {
    return EmitResult(functions_, SpirvOp::FunctionParameter, type, {});
}

u32 SpirvModuleBuilder::AddLabel(std::optional<u32> id) {
    const u32 result = id.has_value() ? *id : AllocateId();
    Emit(functions_, SpirvOp::Label, {result});
    return result;
}

u32 SpirvModuleBuilder::AddFunctionVariable(u32 pointer_type,
                                            std::optional<u32> initializer) {
    const u32 id = AllocateId();
    if (initializer.has_value()) {
        Emit(functions_, SpirvOp::Variable,
             {pointer_type, id, static_cast<u32>(SpirvStorageClass::Function),
              *initializer});
    } else {
        Emit(functions_, SpirvOp::Variable,
             {pointer_type, id, static_cast<u32>(SpirvStorageClass::Function)});
    }
    return id;
}

u32 SpirvModuleBuilder::AddInstruction(SpirvOp opcode, u32 result_type,
                                       const std::vector<u32>& operands) {
    return EmitResult(functions_, opcode, result_type, operands);
}

void SpirvModuleBuilder::AddStatement(SpirvOp opcode,
                                      const std::vector<u32>& operands) {
    Emit(functions_, opcode, operands);
}

void SpirvModuleBuilder::EndFunction() {
    Emit(functions_, SpirvOp::FunctionEnd, {});
}

std::vector<u32> SpirvModuleBuilder::Build() {
    if (memory_model_.empty()) {
        SetLogicalGlsl450MemoryModel();
    }

    std::vector<u32> words;
    words.reserve(5 + capabilities_.size() + extensions_.size() +
                  imports_.size() + memory_model_.size() +
                  entry_points_.size() + execution_modes_.size() +
                  debug_.size() + annotations_.size() +
                  types_constants_globals_.size() + functions_.size());
    words.push_back(kMagic);
    words.push_back(kVersion15);
    words.push_back(kGenerator);
    words.push_back(next_id_);
    words.push_back(0);
    const auto append = [&words](const std::vector<u32>& section) {
        words.insert(words.end(), section.begin(), section.end());
    };
    append(capabilities_);
    append(extensions_);
    append(imports_);
    append(memory_model_);
    append(entry_points_);
    append(execution_modes_);
    append(debug_);
    append(annotations_);
    append(types_constants_globals_);
    append(functions_);
    return words;
}

u32 SpirvModuleBuilder::EmitResult(std::vector<u32>& section, SpirvOp opcode,
                                   u32 result_type,
                                   const std::vector<u32>& operands) {
    const u32 result = AllocateId();
    std::vector<u32> values;
    values.reserve(operands.size() + 2);
    values.push_back(result_type);
    values.push_back(result);
    values.insert(values.end(), operands.begin(), operands.end());
    Emit(section, opcode, values);
    return result;
}

void SpirvModuleBuilder::Emit(std::vector<u32>& section, SpirvOp opcode,
                              const std::vector<u32>& operands) {
    EmitRaw(section, static_cast<u16>(opcode), operands);
}

void SpirvModuleBuilder::EmitRaw(std::vector<u32>& section, u16 opcode,
                                 const std::vector<u32>& operands) {
    section.push_back(
        (static_cast<u32>(operands.size() + 1) << 16) | opcode);
    section.insert(section.end(), operands.begin(), operands.end());
}

void SpirvModuleBuilder::EmitWithString(
    std::vector<u32>& section, SpirvOp opcode, const std::vector<u32>& prefix,
    const std::string& value, int string_before_tail_count) {
    const std::vector<u32> encoded = EncodeString(value);
    std::vector<u32> operands;
    operands.reserve(prefix.size() + encoded.size());
    if (string_before_tail_count < 0) {
        operands.insert(operands.end(), prefix.begin(), prefix.end());
        operands.insert(operands.end(), encoded.begin(), encoded.end());
    } else {
        const auto tail = static_cast<size_t>(string_before_tail_count);
        operands.insert(operands.end(), prefix.begin(), prefix.begin() + tail);
        operands.insert(operands.end(), encoded.begin(), encoded.end());
        operands.insert(operands.end(), prefix.begin() + tail, prefix.end());
    }
    Emit(section, opcode, operands);
}

std::vector<u32> SpirvModuleBuilder::EncodeString(const std::string& value) {
    // SPIR-V strings are NUL-terminated and padded to a dword boundary;
    // the input is expected to be ASCII (as in the C# original's use).
    std::vector<u32> words((value.size() + 1 + 3) / 4, 0);
    std::memcpy(words.data(), value.data(), value.size());
    return words;
}

} // namespace GPU::Shader
