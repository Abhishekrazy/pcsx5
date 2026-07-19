// RDNA2 -> SPIR-V translator: skeleton, stage IO, memory, image, export.
// Guided transliteration of SharpEmu's Gen5SpirvTranslator.cs (the non-ALU
// half).  See gcn_translate.h for the translation-model overview.
#include "gcn_translate_internal.h"

#include <algorithm>
#include <functional>

namespace GPU::Shader {

SpirvTranslateContext::SpirvTranslateContext(
    const GcnProgram&          program,
    const GcnTranslateOptions& options)
    : program_(program), options_(options), stage_(options.stage) {}

// ---------------------------------------------------------------------------
// Compile flow (AgcExports.cs TryCompile): declare, emit initial state, then
// a PC-dispatcher loop over basic blocks.
// ---------------------------------------------------------------------------
bool SpirvTranslateContext::TryCompile(GcnSpirvShader& out, std::string& error) {
    error.clear();

    DeclareModule();
    const std::vector<ShaderBlock> blocks = BuildBasicBlocks(program_);
    if (blocks.empty()) {
        error = "shader contains no executable blocks";
        return false;
    }

    const u32 function_type = module_.TypeFunction(void_type_, {});
    const u32 main = module_.BeginFunction(void_type_, function_type);
    module_.AddName(main, "main");
    module_.AddLabel();
    EmitInitialState();

    const u32 loop_header  = module_.AllocateId();
    const u32 switch_header = module_.AllocateId();
    const u32 switch_merge  = module_.AllocateId();
    const u32 loop_continue = module_.AllocateId();
    const u32 loop_merge    = module_.AllocateId();
    const u32 default_label = module_.AllocateId();
    std::vector<u32> case_labels(blocks.size());
    for (auto& label : case_labels) {
        label = module_.AllocateId();
    }

    module_.AddStatement(SpirvOp::Branch, {loop_header});
    module_.AddLabel(loop_header);
    module_.AddStatement(SpirvOp::LoopMerge, {loop_merge, loop_continue, 0});
    module_.AddStatement(SpirvOp::Branch, {switch_header});

    module_.AddLabel(switch_header);
    const u32 selector = Load(uint_type_, program_counter_);
    module_.AddStatement(SpirvOp::SelectionMerge, {switch_merge, 0});
    std::vector<u32> switch_operands;
    switch_operands.reserve(2 + blocks.size() * 2);
    switch_operands.push_back(selector);
    switch_operands.push_back(default_label);
    for (u32 index = 0; index < blocks.size(); ++index) {
        switch_operands.push_back(index);
        switch_operands.push_back(case_labels[index]);
    }
    module_.AddStatement(SpirvOp::Switch, switch_operands);

    for (u32 index = 0; index < blocks.size(); ++index) {
        module_.AddLabel(case_labels[index]);
        std::string block_error;
        if (!TryEmitBlock(blocks, index, block_error)) {
            error = "block=0x" + std::to_string(blocks[index].start_pc) + ": " + block_error;
            // Hex-format the pc for readability.
            char buf[160];
            std::snprintf(buf, sizeof(buf), "block=0x%X: %s",
                          blocks[index].start_pc, block_error.c_str());
            error = buf;
            return false;
        }
        module_.AddStatement(SpirvOp::Branch, {switch_merge});
    }

    module_.AddLabel(default_label);
    Store(program_active_, module_.ConstantBool(false));
    module_.AddStatement(SpirvOp::Branch, {switch_merge});

    module_.AddLabel(switch_merge);
    module_.AddStatement(SpirvOp::Branch, {loop_continue});
    module_.AddLabel(loop_continue);
    u32 active = Load(bool_type_, program_active_);
    if (options_.max_dispatcher_steps > 0) {
        const u32 steps = IAdd(Load(uint_type_, iteration_guard_), UInt(1));
        Store(iteration_guard_, steps);
        const u32 within_limit = module_.AddInstruction(
            SpirvOp::ULessThan, bool_type_,
            {steps, UInt(options_.max_dispatcher_steps)});
        active = module_.AddInstruction(
            SpirvOp::LogicalAnd, bool_type_, {active, within_limit});
    }
    module_.AddStatement(SpirvOp::BranchConditional, {active, loop_header, loop_merge});

    module_.AddLabel(loop_merge);
    if (stage_ == GcnSpirvStage::Pixel) {
        // A fragment lane still inactive at guest exit is a killed fragment:
        // terminate it instead of letting the zero-initialized outputs land.
        const u32 return_label = module_.AllocateId();
        const u32 kill_label   = module_.AllocateId();
        const u32 lane_active  = Load(bool_type_, exec_);
        module_.AddStatement(SpirvOp::SelectionMerge, {return_label, 0});
        module_.AddStatement(
            SpirvOp::BranchConditional, {lane_active, return_label, kill_label});
        module_.AddLabel(kill_label);
        module_.AddStatement(SpirvOp::Kill, {});
        module_.AddLabel(return_label);
    }

    module_.AddStatement(SpirvOp::Return, {});
    module_.EndFunction();

    const SpirvExecutionModel model = stage_ == GcnSpirvStage::Vertex
        ? SpirvExecutionModel::Vertex
        : SpirvExecutionModel::Fragment;
    module_.AddEntryPoint(model, main, "main", interfaces_);
    if (stage_ == GcnSpirvStage::Pixel) {
        module_.AddExecutionMode(main, SpirvExecutionMode::OriginUpperLeft);
    }

    out.words = module_.Build();
    out.attribute_count = stage_ == GcnSpirvStage::Vertex
        ? static_cast<u32>(vertex_outputs_.size())
        : static_cast<u32>(pixel_inputs_.size());
    return true;
}

// ---------------------------------------------------------------------------
// Module declaration (types, register file, resources, stage interface).
// ---------------------------------------------------------------------------
void SpirvTranslateContext::DeclareModule() {
    module_.AddCapability(SpirvCapability::Shader);
    module_.AddCapability(SpirvCapability::Int64);
    module_.AddCapability(SpirvCapability::ImageQuery);

    glsl_ = module_.ImportExtInst("GLSL.std.450");

    void_type_  = module_.TypeVoid();
    bool_type_  = module_.TypeBool();
    uint_type_  = module_.TypeInt(32, false);
    int_type_   = module_.TypeInt(32, true);
    long_type_  = module_.TypeInt(64, true);
    ulong_type_ = module_.TypeInt(64, false);
    float_type_ = module_.TypeFloat(32);
    vec2_type_  = module_.TypeVector(float_type_, 2);
    vec3_type_  = module_.TypeVector(float_type_, 3);
    vec4_type_  = module_.TypeVector(float_type_, 4);
    uvec2_type_ = module_.TypeVector(uint_type_, 2);
    uvec3_type_ = module_.TypeVector(uint_type_, 3);
    uvec4_type_ = module_.TypeVector(uint_type_, 4);
    private_uint_pointer_ =
        module_.TypePointer(SpirvStorageClass::Private, uint_type_);
    private_bool_pointer_ =
        module_.TypePointer(SpirvStorageClass::Private, bool_type_);

    const u32 scalar_array_type =
        module_.TypeArray(uint_type_, kScalarRegisterCount);
    const u32 vector_array_type =
        module_.TypeArray(uint_type_, kVectorRegisterCount);
    const u32 private_scalar_array_pointer =
        module_.TypePointer(SpirvStorageClass::Private, scalar_array_type);
    const u32 private_vector_array_pointer =
        module_.TypePointer(SpirvStorageClass::Private, vector_array_type);
    scalar_registers_ = module_.AddGlobalVariable(
        private_scalar_array_pointer, SpirvStorageClass::Private,
        module_.ConstantNull(scalar_array_type));
    vector_registers_ = module_.AddGlobalVariable(
        private_vector_array_pointer, SpirvStorageClass::Private,
        module_.ConstantNull(vector_array_type));
    scc_ = module_.AddGlobalVariable(
        private_bool_pointer_, SpirvStorageClass::Private,
        module_.ConstantBool(false));
    vcc_ = module_.AddGlobalVariable(
        private_bool_pointer_, SpirvStorageClass::Private,
        module_.ConstantBool(false));
    exec_ = module_.AddGlobalVariable(
        private_bool_pointer_, SpirvStorageClass::Private,
        module_.ConstantBool(true));
    reached_pixel_export_ = module_.AddGlobalVariable(
        private_bool_pointer_, SpirvStorageClass::Private,
        module_.ConstantBool(false));
    program_counter_ = module_.AddGlobalVariable(
        private_uint_pointer_, SpirvStorageClass::Private,
        module_.Constant(uint_type_, 0));
    program_active_ = module_.AddGlobalVariable(
        private_bool_pointer_, SpirvStorageClass::Private,
        module_.ConstantBool(true));
    if (options_.max_dispatcher_steps > 0) {
        iteration_guard_ = module_.AddGlobalVariable(
            private_uint_pointer_, SpirvStorageClass::Private,
            module_.Constant(uint_type_, 0));
        interfaces_.push_back(iteration_guard_);
        module_.AddName(iteration_guard_, "pcGuard");
    }

    interfaces_.push_back(scalar_registers_);
    interfaces_.push_back(vector_registers_);
    interfaces_.push_back(scc_);
    interfaces_.push_back(vcc_);
    interfaces_.push_back(exec_);
    interfaces_.push_back(reached_pixel_export_);
    interfaces_.push_back(program_counter_);
    interfaces_.push_back(program_active_);
    module_.AddName(scalar_registers_, "sgpr");
    module_.AddName(vector_registers_, "vgpr");

    DeclareBuffers();
    DeclareImages();
    DeclareStageInterface();
}

// ---------------------------------------------------------------------------
// Global buffers: one StorageBuffer variable typed as an array of
// runtime-array blocks; binding N is the array index (SharpEmu layout:
// descriptor set 0, binding 0).
// ---------------------------------------------------------------------------
void SpirvTranslateContext::DeclareBuffers() {
    for (u32 index = 0; index < options_.buffer_bindings.size(); ++index) {
        for (const u32 pc : options_.buffer_bindings[index].instruction_pcs) {
            buffer_binding_by_pc_.emplace(pc, static_cast<int>(index));
        }
    }
    if (options_.buffer_bindings.empty()) {
        return;
    }

    const u32 runtime_array = module_.TypeRuntimeArray(uint_type_);
    module_.AddDecoration(runtime_array, SpirvDecoration::ArrayStride, {4});
    const u32 block = module_.TypeStruct({runtime_array});
    module_.AddDecoration(block, SpirvDecoration::Block, {});
    module_.AddMemberDecoration(block, 0, SpirvDecoration::Offset, {0});
    const u32 descriptors = module_.TypeArray(
        block, static_cast<u32>(options_.buffer_bindings.size()));
    const u32 descriptors_pointer =
        module_.TypePointer(SpirvStorageClass::StorageBuffer, descriptors);
    storage_block_pointer_ =
        module_.TypePointer(SpirvStorageClass::StorageBuffer, block);
    storage_uint_pointer_ =
        module_.TypePointer(SpirvStorageClass::StorageBuffer, uint_type_);
    global_buffers_ = module_.AddGlobalVariable(
        descriptors_pointer, SpirvStorageClass::StorageBuffer);
    module_.AddName(global_buffers_, "guestBuffers");
    module_.AddDecoration(global_buffers_, SpirvDecoration::DescriptorSet, {0});
    module_.AddDecoration(global_buffers_, SpirvDecoration::Binding, {0});
    interfaces_.push_back(global_buffers_);
}

// ---------------------------------------------------------------------------
// Images: one UniformConstant variable per image binding (set 0, binding
// index+1).  All corpus images are 2D float sampled textures.
// ---------------------------------------------------------------------------
void SpirvTranslateContext::DeclareImages() {
    for (u32 index = 0; index < options_.image_bindings.size(); ++index) {
        const auto& binding = options_.image_bindings[index];
        image_binding_by_pc_.emplace(binding.pc, static_cast<int>(index));
        const auto kind = static_cast<ImageComponentKind>(binding.component_kind);
        const u32 component_type = kind == kImageSint ? int_type_
            : kind == kImageUint ? uint_type_
            : float_type_;
        if (binding.is_storage) {
            module_.AddCapability(SpirvCapability::StorageImageReadWithoutFormat);
            module_.AddCapability(SpirvCapability::StorageImageWriteWithoutFormat);
        }

        const u32 image_type = module_.TypeImage(
            component_type, SpirvImageDim::Dim2D,
            /*depth=*/false, /*arrayed=*/false, /*multisampled=*/false,
            /*sampled=*/binding.is_storage ? 2u : 1u,
            SpirvImageFormat::Unknown);
        const u32 object_type = binding.is_storage
            ? image_type
            : module_.TypeSampledImage(image_type);
        const u32 pointer = module_.TypePointer(
            SpirvStorageClass::UniformConstant, object_type);
        const u32 variable = module_.AddGlobalVariable(
            pointer, SpirvStorageClass::UniformConstant);
        module_.AddName(variable, binding.is_storage ? "image" : "tex");
        module_.AddDecoration(variable, SpirvDecoration::DescriptorSet, {0});
        module_.AddDecoration(variable, SpirvDecoration::Binding, {index + 1});
        image_resources_.push_back(ImageResource{
            variable, image_type, object_type, component_type,
            module_.TypeVector(component_type, 4), kind, binding.is_storage});
        interfaces_.push_back(variable);
    }
}

// ---------------------------------------------------------------------------
// Stage interface: builtins + inputs/outputs (DeclareStageInterface).
// Vertex: position + param outputs from Exp targets 32..63 (plus the count
// the paired fragment shader requires).  Pixel: interpolated inputs from
// VInterp attributes + FragCoord + the caller's MRT bindings.
// ---------------------------------------------------------------------------
void SpirvTranslateContext::DeclareStageInterface() {
    if (stage_ == GcnSpirvStage::Vertex) {
        const u32 input_pointer =
            module_.TypePointer(SpirvStorageClass::Input, uint_type_);
        vertex_index_input_ = module_.AddGlobalVariable(
            input_pointer, SpirvStorageClass::Input);
        module_.AddDecoration(
            vertex_index_input_, SpirvDecoration::BuiltIn,
            {static_cast<u32>(SpirvBuiltIn::VertexIndex)});
        interfaces_.push_back(vertex_index_input_);

        instance_index_input_ = module_.AddGlobalVariable(
            input_pointer, SpirvStorageClass::Input);
        module_.AddDecoration(
            instance_index_input_, SpirvDecoration::BuiltIn,
            {static_cast<u32>(SpirvBuiltIn::InstanceIndex)});
        interfaces_.push_back(instance_index_input_);

        const u32 output_pointer =
            module_.TypePointer(SpirvStorageClass::Output, vec4_type_);
        position_output_ = module_.AddGlobalVariable(
            output_pointer, SpirvStorageClass::Output);
        module_.AddDecoration(
            position_output_, SpirvDecoration::BuiltIn,
            {static_cast<u32>(SpirvBuiltIn::Position)});
        interfaces_.push_back(position_output_);

        // Param locations: every exported target plus 0..required-1.
        std::set<u32> parameters;
        for (const auto& instruction : program_.instructions) {
            if (const auto* export_ = std::get_if<GcnExportControl>(&instruction.control)) {
                if (export_->target >= 32 && export_->target < 64) {
                    parameters.insert(export_->target - 32);
                }
            }
        }
        for (int location = 0; location < options_.required_vertex_output_count;
             ++location) {
            parameters.insert(static_cast<u32>(location));
        }
        for (const u32 parameter : parameters) {
            const u32 variable = module_.AddGlobalVariable(
                output_pointer, SpirvStorageClass::Output);
            module_.AddDecoration(variable, SpirvDecoration::Location, {parameter});
            vertex_outputs_.emplace(parameter, variable);
            interfaces_.push_back(variable);
        }
        return;
    }

    // Pixel stage.
    const u32 input_vec4_pointer =
        module_.TypePointer(SpirvStorageClass::Input, vec4_type_);
    std::set<u32> attributes;
    for (const auto& instruction : program_.instructions) {
        if (const auto* interp = std::get_if<GcnInterpolationControl>(&instruction.control)) {
            attributes.insert(interp->attribute);
        }
    }
    for (const u32 attribute : attributes) {
        const u32 variable = module_.AddGlobalVariable(
            input_vec4_pointer, SpirvStorageClass::Input);
        module_.AddDecoration(variable, SpirvDecoration::Location, {attribute});
        pixel_inputs_.emplace(attribute, variable);
        interfaces_.push_back(variable);
    }

    frag_coord_input_ = module_.AddGlobalVariable(
        input_vec4_pointer, SpirvStorageClass::Input);
    module_.AddDecoration(
        frag_coord_input_, SpirvDecoration::BuiltIn,
        {static_cast<u32>(SpirvBuiltIn::FragCoord)});
    interfaces_.push_back(frag_coord_input_);

    for (const auto& binding : options_.pixel_outputs) {
        const u32 output_type = binding.kind == GcnPixelOutputKind::Uint
            ? uvec4_type_
            : binding.kind == GcnPixelOutputKind::Sint
                ? module_.TypeVector(int_type_, 4)
                : vec4_type_;
        const u32 output_pointer =
            module_.TypePointer(SpirvStorageClass::Output, output_type);
        const u32 variable = module_.AddGlobalVariable(
            output_pointer, SpirvStorageClass::Output);
        module_.AddName(variable, "mrt");
        module_.AddDecoration(
            variable, SpirvDecoration::Location, {binding.host_location});
        pixel_outputs_.emplace(
            binding.guest_slot, PixelOutput{variable, output_type, binding.kind});
        interfaces_.push_back(variable);
    }
}

// ---------------------------------------------------------------------------
// Initial register state (EmitInitialState): bake caller-supplied SGPRs,
// sync the wave-mask pairs, and preload stage-specific VGPRs.
// ---------------------------------------------------------------------------
void SpirvTranslateContext::EmitInitialState() {
    for (u32 index = 0;
         index < options_.initial_scalar_registers.size() && index < kScalarRegisterCount;
         ++index) {
        const u32 value = options_.initial_scalar_registers[index];
        if (value != 0) {
            StoreS(index, UInt(value));
        }
    }

    Store(scc_, module_.ConstantBool(false));
    Store(reached_pixel_export_, module_.ConstantBool(false));
    // Single emulated lane: the guest-visible scalar pairs mirror the
    // booleans (VCC=0, EXEC=lane 0 active).
    StoreS64(106, module_.Constant64(ulong_type_, 0));
    StoreS64(126, module_.Constant64(ulong_type_, 1));
    Store(program_counter_, UInt(0));
    Store(program_active_, module_.ConstantBool(true));

    if (stage_ == GcnSpirvStage::Vertex) {
        StoreV(5, Load(uint_type_, vertex_index_input_), /*guard_with_exec=*/false);
        StoreV(8, Load(uint_type_, instance_index_input_), /*guard_with_exec=*/false);
        // Give every declared param output a defined starting value; exports
        // overwrite it and the interface-matching extras stay zero.
        for (const auto& [location, output] : vertex_outputs_) {
            Store(output, module_.ConstantNull(vec4_type_));
        }
        return;
    }

    const u32 frag_coord = Load(vec4_type_, frag_coord_input_);
    EmitPixelInputState(frag_coord);
    for (const auto& [slot, output] : pixel_outputs_) {
        Store(output.variable, module_.ConstantNull(output.type));
    }
}

void SpirvTranslateContext::EmitPixelInputState(u32 frag_coord) {
    u32 vgpr = 0;
    // Pixel input VGPRs are compacted in SPI_PS_INPUT_ADDR order; the
    // interpolation inputs occupy slots even though VInterp lowers directly
    // from SPIR-V interpolants.
    AdvancePixelInput(0, 2, vgpr); // PERSP_SAMPLE
    AdvancePixelInput(1, 2, vgpr); // PERSP_CENTER
    AdvancePixelInput(2, 2, vgpr); // PERSP_CENTROID
    AdvancePixelInput(3, 3, vgpr); // PERSP_PULL_MODEL
    AdvancePixelInput(4, 2, vgpr); // LINEAR_SAMPLE
    AdvancePixelInput(5, 2, vgpr); // LINEAR_CENTER
    AdvancePixelInput(6, 2, vgpr); // LINEAR_CENTROID
    AdvancePixelInput(7, 1, vgpr); // LINE_STIPPLE

    EmitPixelPositionInput(8, 0, frag_coord, vgpr);  // POS_X_FLOAT
    EmitPixelPositionInput(9, 1, frag_coord, vgpr);  // POS_Y_FLOAT
    EmitPixelPositionInput(10, 2, frag_coord, vgpr); // POS_Z_FLOAT
    EmitPixelPositionInput(11, 3, frag_coord, vgpr); // POS_W_FLOAT

    AdvancePixelInput(12, 1, vgpr);
    AdvancePixelInput(13, 1, vgpr);
    AdvancePixelInput(14, 1, vgpr);
    AdvancePixelInput(15, 1, vgpr);
}

void SpirvTranslateContext::AdvancePixelInput(int bit, u32 dword_count, u32& vgpr) {
    if ((options_.pixel_input_address & (1u << bit)) != 0) {
        vgpr += dword_count;
    }
}

void SpirvTranslateContext::EmitPixelPositionInput(
    int bit, u32 component, u32 frag_coord, u32& vgpr) {
    const u32 mask = 1u << bit;
    if ((options_.pixel_input_address & mask) == 0) {
        return;
    }
    if ((options_.pixel_input_enable & mask) != 0) {
        const u32 value = module_.AddInstruction(
            SpirvOp::CompositeExtract, float_type_, {frag_coord, component});
        StoreV(vgpr, Bitcast(uint_type_, value), /*guard_with_exec=*/false);
    }
    ++vgpr;
}

} // namespace GPU::Shader

namespace GPU::Shader {

// ---------------------------------------------------------------------------
// Basic blocks + branch helpers (BuildBasicBlocks et al).
// ---------------------------------------------------------------------------
bool SpirvTranslateContext::IsBranch(const std::string& opcode) {
    return opcode == "SBranch" || opcode.rfind("SCbranch", 0) == 0;
}

bool SpirvTranslateContext::TryGetBranchTargetPc(
    const GcnInstruction& instruction, u32& target_pc) {
    target_pc = 0;
    if (instruction.encoding != GcnEncoding::Sopp || instruction.words.empty()) {
        return false;
    }
    const s16 offset = static_cast<s16>(instruction.words[0] & 0xFFFF);
    const s64 next_pc = static_cast<s64>(instruction.pc) +
        static_cast<s64>(instruction.words.size()) * 4;
    const s64 target = next_pc + static_cast<s64>(offset) * 4;
    if (target < 0 || target > 0xFFFFFFFFll) {
        return false;
    }
    target_pc = static_cast<u32>(target);
    return true;
}

std::vector<SpirvTranslateContext::ShaderBlock>
SpirvTranslateContext::BuildBasicBlocks(const GcnProgram& program) {
    std::vector<ShaderBlock> blocks;
    const auto& instructions = program.instructions;
    if (instructions.empty()) {
        return blocks;
    }

    std::set<u32> leaders{instructions[0].pc};
    for (u32 index = 0; index < instructions.size(); ++index) {
        const auto& instruction = instructions[index];
        u32 target_pc = 0;
        if (IsBranch(instruction.opcode) &&
            TryGetBranchTargetPc(instruction, target_pc)) {
            leaders.insert(target_pc);
        }
        if ((IsBranch(instruction.opcode) || instruction.opcode == "SEndpgm") &&
            index + 1 < instructions.size()) {
            leaders.insert(instructions[index + 1].pc);
        }
    }

    std::vector<u32> starts;
    starts.reserve(leaders.size());
    for (const u32 pc : leaders) {
        const auto it = std::find_if(
            instructions.begin(), instructions.end(),
            [pc](const GcnInstruction& i) { return i.pc == pc; });
        if (it != instructions.end()) {
            starts.push_back(pc);
        }
    }

    auto find_index = [&](u32 pc) -> u32 {
        for (u32 i = 0; i < instructions.size(); ++i) {
            if (instructions[i].pc == pc) {
                return i;
            }
        }
        return UINT32_MAX;
    };

    for (u32 index = 0; index < starts.size(); ++index) {
        const u32 start_index = find_index(starts[index]);
        const u32 end_index = index + 1 < starts.size()
            ? find_index(starts[index + 1])
            : static_cast<u32>(instructions.size());
        if (start_index != UINT32_MAX && end_index > start_index) {
            blocks.push_back(ShaderBlock{starts[index], start_index, end_index});
        }
    }
    return blocks;
}

bool SpirvTranslateContext::TryFindBlock(
    const std::vector<ShaderBlock>& blocks, u32 pc, u32& block_index) {
    for (u32 index = 0; index < blocks.size(); ++index) {
        if (blocks[index].start_pc == pc) {
            block_index = index;
            return true;
        }
    }
    return false;
}

bool SpirvTranslateContext::IsExitBranchTarget(
    const GcnProgram& program, u32 target_pc) {
    if (program.instructions.empty()) {
        return false;
    }
    const auto& last = program.instructions.back();
    const u32 last_end_pc =
        last.pc + static_cast<u32>(last.words.size()) * sizeof(u32);
    return target_pc >= last_end_pc;
}

bool SpirvTranslateContext::TryGetBranchCondition(
    const std::string& opcode, u32& condition) {
    condition = 0;
    if (opcode == "SCbranchScc0") {
        condition = LogicalNot(Load(bool_type_, scc_));
    } else if (opcode == "SCbranchScc1") {
        condition = Load(bool_type_, scc_);
    } else if (opcode == "SCbranchVccz") {
        // Single emulated lane: the lane-local bool is the whole wave vote.
        condition = LogicalNot(Load(bool_type_, vcc_));
    } else if (opcode == "SCbranchVccnz") {
        condition = Load(bool_type_, vcc_);
    } else if (opcode == "SCbranchExecz") {
        condition = LogicalNot(Load(bool_type_, exec_));
    } else if (opcode == "SCbranchExecnz") {
        condition = Load(bool_type_, exec_);
    }
    return condition != 0;
}

// ---------------------------------------------------------------------------
// Block / instruction emission.
// ---------------------------------------------------------------------------
bool SpirvTranslateContext::TryEmitBlock(
    const std::vector<ShaderBlock>& blocks, u32 block_index, std::string& error) {
    const ShaderBlock& block = blocks[block_index];
    for (u32 index = block.start_index; index < block.end_index; ++index) {
        const GcnInstruction& instruction = program_.instructions[index];
        if (IsBranch(instruction.opcode) || instruction.opcode == "SEndpgm") {
            continue;
        }
        if (!TryEmitInstruction(instruction, error)) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "pc=0x%X %s: ", instruction.pc,
                          instruction.opcode.c_str());
            error = buf + error;
            return false;
        }
    }

    const GcnInstruction& terminator = program_.instructions[block.end_index - 1];
    if (terminator.opcode == "SEndpgm") {
        Store(program_active_, module_.ConstantBool(false));
        return true;
    }

    const u32 fallthrough = block_index + 1 < blocks.size()
        ? block_index + 1
        : UINT32_MAX;
    if (terminator.opcode == "SBranch") {
        u32 target_pc = 0;
        if (!TryGetBranchTargetPc(terminator, target_pc)) {
            error = "invalid scalar branch target";
            return false;
        }
        if (IsExitBranchTarget(program_, target_pc)) {
            Store(program_active_, module_.ConstantBool(false));
            return true;
        }
        u32 target_block = 0;
        if (!TryFindBlock(blocks, target_pc, target_block)) {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "invalid scalar branch target pc=0x%X target=0x%X",
                          terminator.pc, target_pc);
            error = buf;
            return false;
        }
        Store(program_counter_, UInt(target_block));
        return true;
    }

    if (terminator.opcode.rfind("SCbranch", 0) == 0) {
        u32 target_pc = 0;
        const bool has_target = TryGetBranchTargetPc(terminator, target_pc);
        u32 target_block = 0;
        const bool has_target_block =
            has_target && TryFindBlock(blocks, target_pc, target_block);
        const bool target_exits =
            has_target && IsExitBranchTarget(program_, target_pc);
        u32 condition = 0;
        const bool has_condition = TryGetBranchCondition(terminator.opcode, condition);
        if (!has_target || (!has_target_block && !target_exits) || !has_condition) {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "invalid conditional scalar branch opcode=%s pc=0x%X",
                          terminator.opcode.c_str(), terminator.pc);
            error = buf;
            return false;
        }
        const u32 taken_block = target_exits ? UINT32_MAX : target_block;
        const u32 selected = module_.AddInstruction(
            SpirvOp::Select, uint_type_,
            {condition, UInt(taken_block), UInt(fallthrough)});
        Store(program_counter_, selected);
        return true;
    }

    if (fallthrough == UINT32_MAX) {
        Store(program_active_, module_.ConstantBool(false));
    } else {
        Store(program_counter_, UInt(fallthrough));
    }
    return true;
}

bool SpirvTranslateContext::TryEmitInstruction(
    const GcnInstruction& instruction, std::string& error) {
    error.clear();
    const std::string& opcode = instruction.opcode;
    if (opcode == "SNop" || opcode == "SWaitcnt" || opcode == "SInstPrefetch" ||
        opcode == "STtraceData" || opcode == "SSendmsg" ||
        opcode == "VInterpMovF32" || opcode == "VNop") {
        return true;
    }

    if (opcode == "SBarrier") {
        const u32 workgroup = UInt(2);
        module_.AddStatement(
            SpirvOp::ControlBarrier, {workgroup, workgroup, UInt(0x108)});
        return true;
    }

    if (const auto* scalar_memory =
            std::get_if<GcnScalarMemoryControl>(&instruction.control)) {
        return TryEmitScalarMemory(instruction, *scalar_memory, error);
    }
    if (const auto* interp =
            std::get_if<GcnInterpolationControl>(&instruction.control)) {
        return TryEmitInterpolation(instruction, *interp, error);
    }
    if (const auto* image = std::get_if<GcnImageControl>(&instruction.control)) {
        return TryEmitImage(instruction, *image, error);
    }
    if (std::get_if<GcnGlobalMemoryControl>(&instruction.control)) {
        error = "global-memory instructions are not supported yet";
        return false;
    }
    if (const auto* buffer_memory =
            std::get_if<GcnBufferMemoryControl>(&instruction.control)) {
        return TryEmitBufferMemory(instruction, *buffer_memory, error);
    }
    if (const auto* export_ = std::get_if<GcnExportControl>(&instruction.control)) {
        return TryEmitExport(instruction, *export_, error);
    }
    if (std::get_if<GcnDataShareControl>(&instruction.control)) {
        error = "data-share instructions are not supported yet";
        return false;
    }

    if (instruction.encoding == GcnEncoding::Sop1 ||
        instruction.encoding == GcnEncoding::Sop2 ||
        instruction.encoding == GcnEncoding::Sopc ||
        instruction.encoding == GcnEncoding::Sopk) {
        return TryEmitScalarAlu(instruction, error);
    }
    if (instruction.encoding == GcnEncoding::Sopp ||
        instruction.encoding == GcnEncoding::Smrd ||
        instruction.encoding == GcnEncoding::Smem) {
        return true;
    }
    return TryEmitVectorAlu(instruction, error);
}

// ---------------------------------------------------------------------------
// VInterp: lowered directly from the interpolated stage input.
// ---------------------------------------------------------------------------
bool SpirvTranslateContext::TryEmitInterpolation(
    const GcnInstruction& instruction,
    const GcnInterpolationControl& interp,
    std::string& error) {
    u32 destination = 0;
    const auto input_it = pixel_inputs_.find(interp.attribute);
    if (stage_ != GcnSpirvStage::Pixel || input_it == pixel_inputs_.end() ||
        !TryGetVectorDestination(instruction, destination)) {
        error = "invalid interpolated attribute";
        return false;
    }
    const u32 vector = Load(vec4_type_, input_it->second);
    const u32 component = module_.AddInstruction(
        SpirvOp::CompositeExtract, float_type_, {vector, interp.channel});
    StoreV(destination, Bitcast(uint_type_, component));
    return true;
}

// ---------------------------------------------------------------------------
// Scalar loads (SLoadDword*/SBufferLoadDword*): u32 words from the bound
// global-buffer block, bounds-checked.
// ---------------------------------------------------------------------------
bool SpirvTranslateContext::TryEmitScalarMemory(
    const GcnInstruction& instruction,
    const GcnScalarMemoryControl& control,
    std::string& error) {
    const auto binding_it = buffer_binding_by_pc_.find(instruction.pc);
    if (binding_it == buffer_binding_by_pc_.end()) {
        // No binding: SharpEmu zero-fills the destinations and moves on.
        for (const auto& destination : instruction.destinations) {
            if (destination.kind == GcnOperandKind::ScalarRegister) {
                StoreS(destination.value, UInt(0));
            }
        }
        return true;
    }
    const int binding = binding_it->second;

    const u32 dynamic_offset = control.dynamic_offset_register.has_value()
        ? LoadS(*control.dynamic_offset_register)
        : UInt(0);
    const u32 byte_address = IAdd(
        dynamic_offset, UInt(static_cast<u32>(control.immediate_offset_bytes)));
    const u32 dword_address = ShiftRightLogical(byte_address, UInt(2));
    for (u32 index = 0; index < instruction.destinations.size(); ++index) {
        const auto& destination = instruction.destinations[index];
        if (destination.kind != GcnOperandKind::ScalarRegister) {
            error = "invalid scalar-memory destination";
            return false;
        }
        const u32 address =
            index == 0 ? dword_address : IAdd(dword_address, UInt(index));
        StoreS(destination.value, LoadBufferWord(binding, address));
    }
    return true;
}

// ---------------------------------------------------------------------------
// Buffer loads (BufferLoadFormat*): M2.2 reads raw dwords from the bound
// block; guest vertex-format decoding lands with M3 descriptors.
// ---------------------------------------------------------------------------
bool SpirvTranslateContext::TryEmitBufferMemory(
    const GcnInstruction& instruction,
    const GcnBufferMemoryControl& control,
    std::string& error) {
    (void)error; // no failure modes yet (format decode arrives with M3)
    const auto binding_it = buffer_binding_by_pc_.find(instruction.pc);
    if (binding_it == buffer_binding_by_pc_.end()) {
        for (u32 index = 0; index < control.dword_count; ++index) {
            StoreV(control.vector_data + index, UInt(0));
        }
        return true;
    }
    const int binding = binding_it->second;

    u32 byte_address = control.index_enabled
        ? LoadV(control.vector_address)
        : UInt(0);
    byte_address = IAdd(byte_address, UInt(static_cast<u32>(control.offset_bytes)));
    const u32 base_dword = ShiftRightLogical(byte_address, UInt(2));
    for (u32 index = 0; index < control.dword_count; ++index) {
        const u32 address =
            index == 0 ? base_dword : IAdd(base_dword, UInt(index));
        StoreV(control.vector_data + index, LoadBufferWord(binding, address));
    }
    return true;
}

// ---------------------------------------------------------------------------
// Image instructions (TryEmitImage).  Corpus coverage: plain ImageSample
// (2D float, implicit LOD) plus the ImageGetResinfo/ImageLoad paths; the
// offset/compare/gradient Sample variants follow SharpEmu's operand walk.
// ---------------------------------------------------------------------------
u32 SpirvTranslateContext::LoadImageFloatAddress(
    const GcnImageControl& image, int component) {
    const u32 reg = component < static_cast<int>(image.address_registers.size())
        ? image.address_registers[component]
        : image.vector_address + static_cast<u32>(component);
    return Bitcast(float_type_, LoadV(reg));
}

u32 SpirvTranslateContext::BuildFloatCoordinates(
    const GcnImageControl& image, int start) {
    const u32 x = LoadImageFloatAddress(image, start);
    const u32 y = LoadImageFloatAddress(image, start + 1);
    return module_.AddInstruction(
        SpirvOp::CompositeConstruct, vec2_type_, {x, y});
}

bool SpirvTranslateContext::TryEmitImage(
    const GcnInstruction& instruction,
    const GcnImageControl& image,
    std::string& error) {
    const auto binding_it = image_binding_by_pc_.find(instruction.pc);
    if (binding_it == image_binding_by_pc_.end()) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "unresolved image binding t=s%u s=s%u",
                      image.scalar_resource, image.scalar_sampler);
        error = buf;
        return false;
    }
    const ImageResource& resource = image_resources_[binding_it->second];
    const u32 image_object = Load(resource.object_type, resource.variable);
    const std::string& opcode = instruction.opcode;

    uint32_t sampled = 0;
    bool write_all_components = false;
    if (opcode == "ImageGetResinfo") {
        const u32 query_image = resource.is_storage
            ? image_object
            : module_.AddInstruction(SpirvOp::Image, resource.image_type, {image_object});
        const u32 ivec2_type = module_.TypeVector(int_type_, 2);
        u32 size;
        if (resource.is_storage) {
            size = module_.AddInstruction(
                SpirvOp::ImageQuerySize, ivec2_type, {query_image});
        } else {
            size = module_.AddInstruction(
                SpirvOp::ImageQuerySizeLod, ivec2_type, {query_image, UInt(0)});
        }
        u32 output_index = 0;
        for (u32 component = 0; component < 4; ++component) {
            if ((image.dmask & (1u << component)) == 0) {
                continue;
            }
            u32 value;
            if (component < 2) {
                const u32 signed_value = module_.AddInstruction(
                    SpirvOp::CompositeExtract, int_type_, {size, component});
                value = Bitcast(uint_type_, signed_value);
            } else {
                value = UInt(1);
            }
            StoreV(image.vector_data + output_index++, value);
        }
        return true;
    }

    if (opcode == "ImageLoad" || opcode == "ImageLoadMip") {
        if (resource.is_storage) {
            error = "storage image load is not supported yet";
            return false;
        }
        // Non-mip corpus path: fetch at lod 0 through the sampled image.
        const u32 fetched_image = module_.AddInstruction(
            SpirvOp::Image, resource.image_type, {image_object});
        const auto binding = options_.image_bindings[binding_it->second];
        const u32 mip_level = UInt(binding.mip_level);
        const u32 x = Bitcast(
            int_type_,
            module_.AddInstruction(
                SpirvOp::ConvertFToS, int_type_,
                {LoadImageFloatAddress(image, 0)}));
        const u32 y = Bitcast(
            int_type_,
            module_.AddInstruction(
                SpirvOp::ConvertFToS, int_type_,
                {LoadImageFloatAddress(image, 1)}));
        const u32 coordinates = module_.AddInstruction(
            SpirvOp::CompositeConstruct, module_.TypeVector(int_type_, 2), {x, y});
        sampled = module_.AddInstruction(
            SpirvOp::ImageFetch, resource.vector_type,
            {fetched_image, coordinates, 2, mip_level});
    } else if (opcode.rfind("ImageSample", 0) == 0) {
        const bool has_compare = opcode.find("SampleC") != std::string::npos;
        const bool has_gradients = opcode.find("SampleD") != std::string::npos;
        const bool has_zero_lod = opcode.find("Lz") != std::string::npos;
        const bool has_lod =
            !has_zero_lod && opcode.find("SampleL") != std::string::npos;
        const bool has_bias = opcode.find("SampleB") != std::string::npos;
        if (opcode.back() == 'O' || has_compare) {
            // Offset / compare variants need machinery M2.2 does not use
            // (neither appears in the corpus).
            error = "unsupported image opcode " + opcode;
            return false;
        }

        int address_cursor = 0;
        const u32 lod_or_bias =
            has_bias ? LoadImageFloatAddress(image, address_cursor++) : 0;
        const u32 gradient_x =
            has_gradients ? BuildFloatCoordinates(image, address_cursor) : 0;
        const u32 gradient_y =
            has_gradients ? BuildFloatCoordinates(image, address_cursor + 2) : 0;
        if (has_gradients) {
            address_cursor += 4;
        }
        const u32 coordinates = BuildFloatCoordinates(image, address_cursor);
        const bool explicit_lod = has_gradients || has_zero_lod || has_lod;
        const u32 lod = has_zero_lod
            ? Float(0)
            : has_lod
                ? LoadImageFloatAddress(image, address_cursor + 2)
                : lod_or_bias;

        const u32 image_operands =
            has_gradients ? 4u : explicit_lod ? 2u : has_bias ? 1u : 0u;
        std::vector<u32> operands{image_object, coordinates};
        if (image_operands != 0) {
            operands.push_back(image_operands);
            if (has_gradients) {
                operands.push_back(gradient_x);
                operands.push_back(gradient_y);
            } else if (explicit_lod) {
                operands.push_back(lod);
            } else {
                operands.push_back(lod_or_bias);
            }
        }
        sampled = module_.AddInstruction(
            explicit_lod ? SpirvOp::ImageSampleExplicitLod
                         : SpirvOp::ImageSampleImplicitLod,
            resource.vector_type, operands);
    } else {
        error = "unsupported image opcode " + opcode;
        return false;
    }

    // Distribute the sampled vector across the dmask-enabled destination
    // VGPRs (D16 packing is not needed by the corpus).
    std::vector<u32> output_values;
    for (u32 component = 0; component < 4; ++component) {
        if (!write_all_components && (image.dmask & (1u << component)) == 0) {
            continue;
        }
        const u32 value = module_.AddInstruction(
            SpirvOp::CompositeExtract, resource.component_type,
            {sampled, component});
        output_values.push_back(resource.component_kind == kImageUint
            ? value
            : Bitcast(uint_type_, value));
    }
    for (u32 index = 0; index < output_values.size(); ++index) {
        StoreV(image.vector_data + index, output_values[index]);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Exports (TryEmitExport): vertex -> position/param outputs; pixel -> MRTs.
// Stores are EXEC-predicated like vector-register writes.
// ---------------------------------------------------------------------------
bool SpirvTranslateContext::TryEmitExport(
    const GcnInstruction& instruction,
    const GcnExportControl& export_,
    std::string& error) {
    if (instruction.sources.size() < 4) {
        error = "missing export sources";
        return false;
    }

    if (stage_ == GcnSpirvStage::Pixel) {
        const auto output_it = pixel_outputs_.find(export_.target);
        if (output_it == pixel_outputs_.end()) {
            return true;
        }
        const PixelOutput& output = output_it->second;
        Store(reached_pixel_export_, module_.ConstantBool(true));

        std::vector<u32> values(4);
        for (u32 component = 0; component < 4; ++component) {
            const bool enabled = (export_.enable_mask & (1u << component)) != 0;
            if (!enabled) {
                const u32 component_type =
                    output.kind == GcnPixelOutputKind::Uint ? uint_type_
                    : output.kind == GcnPixelOutputKind::Sint ? int_type_
                    : float_type_;
                values[component] = module_.AddInstruction(
                    SpirvOp::CompositeExtract, component_type,
                    {Load(output.type, output.variable), component});
                continue;
            }
            if (export_.compressed) {
                // Two f16 components per source register; unpack to f32
                // (SharpEmu's packed-half shadow file is a precision extra —
                // the plain UnpackHalf2x16 path is exact for f16 data).
                const u32 packed = LoadV(instruction.sources[component >> 1].value);
                const u32 unpacked = Ext(GlslExt::UnpackHalf2x16, vec2_type_, {packed});
                const u32 value = module_.AddInstruction(
                    SpirvOp::CompositeExtract, float_type_, {unpacked, component & 1});
                values[component] = output.kind == GcnPixelOutputKind::Uint
                    ? module_.AddInstruction(SpirvOp::ConvertFToU, uint_type_, {value})
                    : output.kind == GcnPixelOutputKind::Sint
                        ? module_.AddInstruction(SpirvOp::ConvertFToS, int_type_, {value})
                        : value;
                continue;
            }
            const u32 raw = LoadV(instruction.sources[component].value);
            values[component] = output.kind == GcnPixelOutputKind::Uint
                ? raw
                : output.kind == GcnPixelOutputKind::Sint
                    ? Bitcast(int_type_, raw)
                    : Bitcast(float_type_, raw);
        }
        u32 vector = module_.AddInstruction(
            SpirvOp::CompositeConstruct, output.type, values);
        vector = module_.AddInstruction(
            SpirvOp::Select, output.type,
            {Load(bool_type_, exec_), vector, Load(output.type, output.variable)});
        Store(output.variable, vector);
        return true;
    }

    // Vertex stage.
    u32 output_variable = 0;
    if (export_.target >= 12 && export_.target < 16) {
        if (export_.target != 12) {
            return true;
        }
        output_variable = position_output_;
    } else if (export_.target >= 32 && export_.target < 64) {
        const auto param_it = vertex_outputs_.find(export_.target - 32);
        if (param_it == vertex_outputs_.end()) {
            return true;
        }
        output_variable = param_it->second;
    } else {
        return true;
    }

    std::vector<u32> components(4);
    for (u32 component = 0; component < 4; ++component) {
        if ((export_.enable_mask & (1u << component)) != 0) {
            if (export_.compressed) {
                const u32 packed = LoadV(instruction.sources[component >> 1].value);
                const u32 unpacked = Ext(GlslExt::UnpackHalf2x16, vec2_type_, {packed});
                components[component] = module_.AddInstruction(
                    SpirvOp::CompositeExtract, float_type_, {unpacked, component & 1});
            } else {
                components[component] =
                    Bitcast(float_type_, LoadV(instruction.sources[component].value));
            }
        } else {
            components[component] = Float(component == 3 ? 1.f : 0.f);
        }
    }
    u32 output_value = module_.AddInstruction(
        SpirvOp::CompositeConstruct, vec4_type_, components);
    output_value = module_.AddInstruction(
        SpirvOp::Select, vec4_type_,
        {Load(bool_type_, exec_), output_value, Load(vec4_type_, output_variable)});
    Store(output_variable, output_value);
    return true;
}

// ---------------------------------------------------------------------------
// Buffer access (bounds-checked via OpArrayLength, SharpEmu LoadBufferWord).
// ---------------------------------------------------------------------------
u32 SpirvTranslateContext::LoadBufferWord(int binding, u32 dword_address) {
    const u32 in_range = IsBufferWordInRange(binding, dword_address);
    const u32 safe_address = module_.AddInstruction(
        SpirvOp::Select, uint_type_, {in_range, dword_address, UInt(0)});
    const u32 value = Load(uint_type_, BufferWordPointer(binding, safe_address));
    return module_.AddInstruction(
        SpirvOp::Select, uint_type_, {in_range, value, UInt(0)});
}

u32 SpirvTranslateContext::IsBufferWordInRange(int binding, u32 dword_address) {
    const u32 buffer = module_.AddInstruction(
        SpirvOp::AccessChain, storage_block_pointer_,
        {global_buffers_, UInt(static_cast<u32>(binding))});
    const u32 length =
        module_.AddInstruction(SpirvOp::ArrayLength, uint_type_, {buffer, 0});
    return module_.AddInstruction(
        SpirvOp::ULessThan, bool_type_, {dword_address, length});
}

u32 SpirvTranslateContext::BufferWordPointer(int binding, u32 dword_address) {
    return module_.AddInstruction(
        SpirvOp::AccessChain, storage_uint_pointer_,
        {global_buffers_, UInt(static_cast<u32>(binding)), UInt(0), dword_address});
}

// ---------------------------------------------------------------------------
// Register file.  SGPRs/VGPRs are raw-dword arrays; stores to the VCC/EXEC
// scalar pairs keep the per-lane booleans synchronized.
// ---------------------------------------------------------------------------
u32 SpirvTranslateContext::ScalarPointer(u32 reg) {
    return module_.AddInstruction(
        SpirvOp::AccessChain, private_uint_pointer_, {scalar_registers_, UInt(reg)});
}

u32 SpirvTranslateContext::VectorPointer(u32 reg) {
    return module_.AddInstruction(
        SpirvOp::AccessChain, private_uint_pointer_, {vector_registers_, UInt(reg)});
}

u32 SpirvTranslateContext::LoadS(u32 reg) {
    return Load(uint_type_, ScalarPointer(reg));
}

u32 SpirvTranslateContext::LoadV(u32 reg) {
    return Load(uint_type_, VectorPointer(reg));
}

void SpirvTranslateContext::StoreS(u32 reg, u32 value) {
    Store(ScalarPointer(reg), value);
    if (reg == 106 || reg == 107) {
        Store(vcc_, IsWaveMaskActive(LoadS64(106)));
    } else if (reg == 126 || reg == 127) {
        Store(exec_, IsWaveMaskActive(LoadS64(126)));
    }
}

void SpirvTranslateContext::StoreV(u32 reg, u32 value, bool guard_with_exec) {
    if (guard_with_exec) {
        const u32 active = Load(bool_type_, exec_);
        const u32 old_value = LoadV(reg);
        value = module_.AddInstruction(
            SpirvOp::Select, uint_type_, {active, value, old_value});
    }
    Store(VectorPointer(reg), value);
}

u32 SpirvTranslateContext::Load(u32 type, u32 pointer) {
    return module_.AddInstruction(SpirvOp::Load, type, {pointer});
}

void SpirvTranslateContext::Store(u32 pointer, u32 value) {
    module_.AddStatement(SpirvOp::Store, {pointer, value});
}

// ---------------------------------------------------------------------------
// Wave-mask helpers for the single emulated lane: the lane's bit is bit 0,
// so a wave mask is active iff it is non-zero.
// ---------------------------------------------------------------------------
u32 SpirvTranslateContext::IsWaveMaskActive(u32 mask64) {
    return IsNotZero64(mask64);
}

u32 SpirvTranslateContext::BooleanToWaveMask(u32 condition) {
    return module_.AddInstruction(
        SpirvOp::Select, ulong_type_,
        {condition, module_.Constant64(ulong_type_, 1),
         module_.Constant64(ulong_type_, 0)});
}

void SpirvTranslateContext::StoreWaveMask(u32 reg, u32 condition) {
    StoreS64(reg, BooleanToWaveMask(condition));
}

// ---------------------------------------------------------------------------
// Small emission helpers.
// ---------------------------------------------------------------------------
u32 SpirvTranslateContext::UInt(u32 value) {
    return module_.Constant(uint_type_, value);
}

u32 SpirvTranslateContext::Float(float value) {
    return module_.ConstantFloat(float_type_, value);
}

u32 SpirvTranslateContext::Bitcast(u32 type, u32 value) {
    return module_.AddInstruction(SpirvOp::Bitcast, type, {value});
}

u32 SpirvTranslateContext::IAdd(u32 left, u32 right) {
    return module_.AddInstruction(SpirvOp::IAdd, uint_type_, {left, right});
}

u32 SpirvTranslateContext::ShiftLeftLogical(u32 left, u32 right) {
    return module_.AddInstruction(
        SpirvOp::ShiftLeftLogical, uint_type_,
        {left, BitwiseAnd(right, UInt(31))});
}

u32 SpirvTranslateContext::ShiftRightLogical(u32 left, u32 right) {
    return module_.AddInstruction(
        SpirvOp::ShiftRightLogical, uint_type_,
        {left, BitwiseAnd(right, UInt(31))});
}

u32 SpirvTranslateContext::ShiftRightArithmetic(u32 left, u32 right) {
    return Bitcast(
        uint_type_,
        module_.AddInstruction(
            SpirvOp::ShiftRightArithmetic, int_type_,
            {Bitcast(int_type_, left), BitwiseAnd(right, UInt(31))}));
}

u32 SpirvTranslateContext::ShiftLeftLogical64(u32 left, u32 right) {
    return module_.AddInstruction(
        SpirvOp::ShiftLeftLogical, ulong_type_,
        {left,
         module_.AddInstruction(
             SpirvOp::BitwiseAnd, ulong_type_,
             {right, module_.Constant64(ulong_type_, 63)})});
}

u32 SpirvTranslateContext::ShiftRightLogical64(u32 left, u32 right) {
    return module_.AddInstruction(
        SpirvOp::ShiftRightLogical, ulong_type_,
        {left,
         module_.AddInstruction(
             SpirvOp::BitwiseAnd, ulong_type_,
             {right, module_.Constant64(ulong_type_, 63)})});
}

u32 SpirvTranslateContext::BitwiseAnd(u32 left, u32 right) {
    return module_.AddInstruction(SpirvOp::BitwiseAnd, uint_type_, {left, right});
}

u32 SpirvTranslateContext::BitwiseOr(u32 left, u32 right) {
    return module_.AddInstruction(SpirvOp::BitwiseOr, uint_type_, {left, right});
}

u32 SpirvTranslateContext::BitwiseXor(u32 left, u32 right) {
    return module_.AddInstruction(SpirvOp::BitwiseXor, uint_type_, {left, right});
}

u32 SpirvTranslateContext::LogicalNot(u32 value) {
    return module_.AddInstruction(SpirvOp::LogicalNot, bool_type_, {value});
}

u32 SpirvTranslateContext::Ext(
    u32 operation, u32 result_type, const std::vector<u32>& operands) {
    std::vector<u32> all;
    all.reserve(operands.size() + 1);
    all.push_back(glsl_);
    all.push_back(operation);
    all.insert(all.end(), operands.begin(), operands.end());
    return module_.AddInstruction(SpirvOp::ExtInst, result_type, all);
}

u32 SpirvTranslateContext::IsNotZero(u32 value) {
    return module_.AddInstruction(
        SpirvOp::INotEqual, bool_type_, {value, UInt(0)});
}

u32 SpirvTranslateContext::IsNotZero64(u32 value) {
    return module_.AddInstruction(
        SpirvOp::INotEqual, bool_type_,
        {value, module_.Constant64(ulong_type_, 0)});
}

void SpirvTranslateContext::EmitConditional(
    u32 condition, const std::function<void()>& emit) {
    const u32 active_label = module_.AllocateId();
    const u32 merge_label = module_.AllocateId();
    module_.AddStatement(SpirvOp::SelectionMerge, {merge_label, 0});
    module_.AddStatement(
        SpirvOp::BranchConditional, {condition, active_label, merge_label});
    module_.AddLabel(active_label);
    emit();
    module_.AddStatement(SpirvOp::Branch, {merge_label});
    module_.AddLabel(merge_label);
}

} // namespace GPU::Shader

namespace GPU::Shader {

// ---------------------------------------------------------------------------
// Public entry (gcn_translate.h).
// ---------------------------------------------------------------------------
bool GcnTranslateToSpirv(const GcnProgram&          program,
                         const GcnTranslateOptions& options,
                         GcnSpirvShader&           out,
                         std::string&              error) {
    SpirvTranslateContext context(program, options);
    return context.TryCompile(out, error);
}

} // namespace GPU::Shader

namespace GPU::Shader {

// ---------------------------------------------------------------------------
// Default options synthesis (gcn_translate.h): standalone bindings when no
// runtime descriptor state exists yet.
// ---------------------------------------------------------------------------
GcnTranslateOptions GcnTranslateDefaultOptions(const GcnProgram& program,
                                               GcnSpirvStage     stage) {
    GcnTranslateOptions options;
    options.stage = stage;

    if (stage == GcnSpirvStage::Pixel) {
        bool has_color = false;
        for (const GcnInstruction& ins : program.instructions) {
            if (const auto* export_ = std::get_if<GcnExportControl>(&ins.control)) {
                if (export_->target < 8) {
                    has_color = true;
                    options.pixel_outputs.push_back(GcnPixelOutputBinding{
                        export_->target, export_->target, GcnPixelOutputKind::Float});
                }
            }
        }
        if (!has_color) {
            options.pixel_outputs.push_back(
                GcnPixelOutputBinding{0, 0, GcnPixelOutputKind::Float});
        }
    } else {
        u32 max_param = 0;
        for (const GcnInstruction& ins : program.instructions) {
            if (const auto* export_ = std::get_if<GcnExportControl>(&ins.control)) {
                if (export_->target >= 32 && export_->target < 64) {
                    max_param = std::max(max_param, export_->target - 32 + 1);
                }
            }
        }
        options.required_vertex_output_count = static_cast<int>(max_param);
    }

    for (const GcnInstruction& ins : program.instructions) {
        if (std::get_if<GcnImageControl>(&ins.control)) {
            GcnSpirvImageBinding binding;
            binding.pc = ins.pc;
            options.image_bindings.push_back(binding);
        }
    }

    std::map<u32, std::vector<u32>> pcs_by_base;
    for (const GcnInstruction& ins : program.instructions) {
        if (std::get_if<GcnScalarMemoryControl>(&ins.control)) {
            u32 base = 0;
            if (!ins.sources.empty() &&
                ins.sources[0].kind == GcnOperandKind::ScalarRegister) {
                base = ins.sources[0].value;
            }
            pcs_by_base[base].push_back(ins.pc);
        } else if (const auto* buffer =
                       std::get_if<GcnBufferMemoryControl>(&ins.control)) {
            pcs_by_base[buffer->scalar_resource].push_back(ins.pc);
        }
    }
    for (const auto& [base, pcs] : pcs_by_base) {
        GcnSpirvBufferBinding binding;
        binding.scalar_address = base;
        binding.instruction_pcs = pcs;
        options.buffer_bindings.push_back(binding);
    }

    return options;
}

} // namespace GPU::Shader
