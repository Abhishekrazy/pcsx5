// RDNA2 -> SPIR-V translator: skeleton, stage IO, memory, image, export.
// Guided transliteration of SharpEmu's Gen5SpirvTranslator.cs (the non-ALU
// half).  See gcn_translate.h for the translation-model overview.
#include "gcn_translate_internal.h"
#include "gcn_eval.h"
#include "../shader_cache.h"

#include <algorithm>
#include <functional>
#include <unordered_map>
#include <unordered_set>

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

    if (options_.initial_scalar_buffer_index >=
        static_cast<int>(options_.buffer_bindings.size())) {
        error = "initial_scalar_buffer_index out of range (no such buffer binding)";
        return false;
    }

    // H6-1.2: Reject unsupported compute patterns early.
    if (stage_ == GcnSpirvStage::Compute) {
        for (const GcnInstruction& ins : program_.instructions) {
            const std::string& opcode = ins.opcode;
            // Data-share (LDS/scratch) is not yet implemented.
            if (opcode.compare(0, 3, "DS_") == 0) {
                error = "H6: data-share instruction '" + opcode +
                        "' not yet implemented for compute";
                return false;
            }
            // Global/image atomics are not yet implemented.
            if (opcode == "BufferAtomic" || opcode == "GlobalAtomic" ||
                opcode == "ImageAtomic" || opcode.find("Atomic") != std::string::npos) {
                error = "H6: atomic instruction '" + opcode +
                        "' not yet implemented for compute";
                return false;
            }
        }
    }

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
    } else if (stage_ == GcnSpirvStage::Compute) {
        // H6: compute shaders have no fragment kill.  No special exit needed.
    }

    module_.AddStatement(SpirvOp::Return, {});
    module_.EndFunction();

    const SpirvExecutionModel model = stage_ == GcnSpirvStage::Vertex
        ? SpirvExecutionModel::Vertex
        : stage_ == GcnSpirvStage::Compute
        ? SpirvExecutionModel::GLCompute
        : SpirvExecutionModel::Fragment;
    module_.AddEntryPoint(model, main, "main", interfaces_);
    if (stage_ == GcnSpirvStage::Pixel) {
        module_.AddExecutionMode(main, SpirvExecutionMode::OriginUpperLeft);
    } else if (stage_ == GcnSpirvStage::Compute) {
        // Workgroup size declared via LocalSize execution mode from options.
        const u32 wx = options_.workgroup_size_x ? options_.workgroup_size_x : 1;
        const u32 wy = options_.workgroup_size_y ? options_.workgroup_size_y : 1;
        const u32 wz = options_.workgroup_size_z ? options_.workgroup_size_z : 1;
        module_.AddExecutionMode(main, SpirvExecutionMode::LocalSize, {wx, wy, wz});
    }

    out.words = module_.Build();
    out.attribute_count = stage_ == GcnSpirvStage::Vertex
        ? static_cast<u32>(vertex_outputs_.size())
        : stage_ == GcnSpirvStage::Compute
        ? 0u
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
    if (stage_ == GcnSpirvStage::Compute) {
        // Compute-specific capabilities: workgroup builtins, shared memory.
        // StorageImageRead/WriteWithoutFormat may be needed if compute uses
        // UAV images — added by DeclareImages when bindings require them.
    }

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

        // Arrayed sample/gather bindings declare a 2D-array image so the
        // layer index resolves to a real slice (SharpEmu #471); the bound
        // view must agree, which is the backend's half of the same rule.
        const bool arrayed = binding.is_arrayed && !binding.is_storage;
        const u32 image_type = module_.TypeImage(
            component_type, SpirvImageDim::Dim2D,
            /*depth=*/false, /*arrayed=*/arrayed, /*multisampled=*/false,
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
            module_.TypeVector(component_type, 4), kind, binding.is_storage,
            arrayed});
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

    // Compute stage: workgroup builtin input variables (H6).
    if (stage_ == GcnSpirvStage::Compute) {
        const u32 input_uvec3_pointer =
            module_.TypePointer(SpirvStorageClass::Input, uvec3_type_);
        compute_workgroup_id_ = module_.AddGlobalVariable(
            input_uvec3_pointer, SpirvStorageClass::Input);
        module_.AddDecoration(compute_workgroup_id_, SpirvDecoration::BuiltIn,
            {static_cast<u32>(SpirvBuiltIn::WorkgroupId)});
        module_.AddName(compute_workgroup_id_, "gl_WorkGroupID");
        interfaces_.push_back(compute_workgroup_id_);

        compute_local_invocation_id_ = module_.AddGlobalVariable(
            input_uvec3_pointer, SpirvStorageClass::Input);
        module_.AddDecoration(compute_local_invocation_id_, SpirvDecoration::BuiltIn,
            {static_cast<u32>(SpirvBuiltIn::LocalInvocationId)});
        module_.AddName(compute_local_invocation_id_, "gl_LocalInvocationID");
        interfaces_.push_back(compute_local_invocation_id_);

        compute_global_invocation_id_ = module_.AddGlobalVariable(
            input_uvec3_pointer, SpirvStorageClass::Input);
        module_.AddDecoration(compute_global_invocation_id_, SpirvDecoration::BuiltIn,
            {static_cast<u32>(SpirvBuiltIn::GlobalInvocationId)});
        module_.AddName(compute_global_invocation_id_, "gl_GlobalInvocationID");
        interfaces_.push_back(compute_global_invocation_id_);
    }
}

// ---------------------------------------------------------------------------
// Initial register state (EmitInitialState): bake caller-supplied SGPRs,
// sync the wave-mask pairs, and preload stage-specific VGPRs.
// ---------------------------------------------------------------------------
void SpirvTranslateContext::EmitInitialState() {
    if (options_.initial_scalar_buffer_index >= 0) {
        // Initial scalar registers arrive in a per-draw storage buffer
        // instead of being baked as constants, so animated user data
        // (colors, texture descriptors) reuses one translation and pipeline
        // (SharpEmu's `_initialScalarBufferIndex >= 0` path).  Only
        // registers the program can observe need loading.
        const GcnConsumedScalarMask consumed =
            GcnComputeConsumedScalarMask(program_);
        for (u32 index = 0; index < kScalarRegisterCount; ++index) {
            if (GcnIsScalarConsumed(consumed, index)) {
                StoreS(index, LoadBufferWord(
                                  options_.initial_scalar_buffer_index,
                                  UInt(index)));
            }
        }
    } else {
        for (u32 index = 0;
             index < options_.initial_scalar_registers.size() && index < kScalarRegisterCount;
             ++index) {
            const u32 value = options_.initial_scalar_registers[index];
            if (value != 0) {
                StoreS(index, UInt(value));
            }
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

    if (stage_ == GcnSpirvStage::Compute) {
        // H6: Load workgroup builtins into SGPR/VGPR slots as the guest
        // compute ABI expects.  WorkgroupId → SGPR[0..2],
        // LocalInvocationId → VGPR[0..2],
        // GlobalInvocationId is available as a builtin but not stored
        // (recomputed by guest code if needed — not part of GCN ABI).
        const u32 wg_id   = Load(uvec3_type_, compute_workgroup_id_);
        const u32 li_id   = Load(uvec3_type_, compute_local_invocation_id_);
        StoreS(0, module_.AddInstruction(SpirvOp::CompositeExtract, uint_type_,
                                         {wg_id, 0u}));
        StoreS(1, module_.AddInstruction(SpirvOp::CompositeExtract, uint_type_,
                                         {wg_id, 1u}));
        StoreS(2, module_.AddInstruction(SpirvOp::CompositeExtract, uint_type_,
                                         {wg_id, 2u}));
        StoreV(0, module_.AddInstruction(SpirvOp::CompositeExtract, uint_type_,
                                         {li_id, 0u}), /*guard_with_exec=*/false);
        StoreV(1, module_.AddInstruction(SpirvOp::CompositeExtract, uint_type_,
                                         {li_id, 1u}), /*guard_with_exec=*/false);
        StoreV(2, module_.AddInstruction(SpirvOp::CompositeExtract, uint_type_,
                                         {li_id, 2u}), /*guard_with_exec=*/false);
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
// Guest buffer-format decode (SharpEmu EmitBufferFormatLoad): the descriptor's
// unified-format field is read at shader-execution time, so decoding stays
// fully dynamic — a module-level lookup table maps it to (data, number).
// ---------------------------------------------------------------------------
void SpirvTranslateContext::DeclareBufferFormatTable() {
    if (buffer_format_table_ != 0) {
        return;
    }
    constexpr u32 kFormatCount = 128;
    const u32 table_type = module_.TypeArray(uint_type_, kFormatCount);
    std::vector<u32> entries(kFormatCount);
    for (u32 format = 0; format < kFormatCount; ++format) {
        u32 data_format = 0;
        u32 number_format = 0;
        GcnTryDecodeUnifiedFormat(format, data_format, number_format);
        entries[format] = module_.Constant(uint_type_, data_format | (number_format << 8));
    }
    const u32 table_pointer =
        module_.TypePointer(SpirvStorageClass::Private, table_type);
    buffer_format_table_ = module_.AddGlobalVariable(
        table_pointer, SpirvStorageClass::Private,
        module_.ConstantComposite(table_type, entries));
    module_.AddName(buffer_format_table_, "gfx10BufferFormats");
    interfaces_.push_back(buffer_format_table_);
}

u32 SpirvTranslateContext::DecodeGfx10BufferFormat(u32 unified_format,
                                                   u32& number_format_out) {
    DeclareBufferFormatTable();
    const u32 entry_pointer = module_.AddInstruction(
        SpirvOp::AccessChain, private_uint_pointer_,
        {buffer_format_table_, unified_format});
    const u32 entry = Load(uint_type_, entry_pointer);
    number_format_out = ShiftRightLogical(entry, UInt(8));
    number_format_out = BitwiseAnd(number_format_out, UInt(0xFF));
    return BitwiseAnd(entry, UInt(0xFF));
}

u32 SpirvTranslateContext::SelectUInt(u32 selector, u32 expected,
                                      u32 when_true, u32 when_false) {
    return module_.AddInstruction(
        SpirvOp::Select, uint_type_,
        {module_.AddInstruction(SpirvOp::IEqual, bool_type_, {selector, UInt(expected)}),
         when_true, when_false});
}

u32 SpirvTranslateContext::Gfx10FormatOne(u32 number_format) {
    const u32 is_uint =
        module_.AddInstruction(SpirvOp::IEqual, bool_type_, {number_format, UInt(4)});
    const u32 is_sint =
        module_.AddInstruction(SpirvOp::IEqual, bool_type_, {number_format, UInt(5)});
    return module_.AddInstruction(
        SpirvOp::Select, uint_type_,
        {module_.AddInstruction(SpirvOp::LogicalOr, bool_type_, {is_uint, is_sint}),
         UInt(1), UInt(0x3F800000)});
}

u32 SpirvTranslateContext::LoadUnalignedBufferWord(int binding, u32 byte_address) {
    u32 result = UInt(0);
    for (u32 index = 0; index < 4; ++index) {
        const u32 address = index == 0 ? byte_address : IAdd(byte_address, UInt(index));
        const u32 dword_address = ShiftRightLogical(address, UInt(2));
        const u32 bit_offset = ShiftLeftLogical(BitwiseAnd(address, UInt(3)), UInt(3));
        const u32 value = BitwiseAnd(
            ShiftRightLogical(LoadBufferWord(binding, dword_address), bit_offset),
            UInt(0xFF));
        result = BitwiseOr(result, ShiftLeftLogical(value, UInt(index * 8)));
    }
    return result;
}

u32 SpirvTranslateContext::DecodeUnsignedMiniFloat(u32 raw, u32 bit_count) {
    const u32 mantissa_bits = module_.AddInstruction(
        SpirvOp::ISub, uint_type_, {bit_count, UInt(5)});
    const u32 mantissa_mask = module_.AddInstruction(
        SpirvOp::ISub, uint_type_, {ShiftLeftLogical(UInt(1), mantissa_bits), UInt(1)});
    const u32 mantissa = BitwiseAnd(raw, mantissa_mask);
    const u32 exponent = BitwiseAnd(ShiftRightLogical(raw, mantissa_bits), UInt(0x1F));
    const u32 mantissa_shift =
        module_.AddInstruction(SpirvOp::ISub, uint_type_, {UInt(23), mantissa_bits});
    const u32 normal_bits = BitwiseOr(
        ShiftLeftLogical(IAdd(exponent, UInt(112)), UInt(23)),
        ShiftLeftLogical(mantissa, mantissa_shift));
    const float inv = 1.0f / 524288.0f; // 2^-19 (10-bit UFLOAT; 11-bit uses 2^-20)
    const u32 subnormal = Bitcast(
        uint_type_,
        module_.AddInstruction(
            SpirvOp::FMul, float_type_,
            {module_.AddInstruction(SpirvOp::ConvertUToF, float_type_, {mantissa}),
             module_.AddInstruction(
                 SpirvOp::Select, float_type_,
                 {module_.AddInstruction(
                      SpirvOp::IEqual, bool_type_, {mantissa_bits, UInt(6)}),
                  Float(1.0f / 1048576.0f), Float(inv)})}));
    const u32 special =
        BitwiseOr(UInt(0x7F800000u), ShiftLeftLogical(mantissa, mantissa_shift));
    const u32 result = module_.AddInstruction(
        SpirvOp::Select, uint_type_,
        {module_.AddInstruction(SpirvOp::IEqual, bool_type_, {exponent, UInt(0)}),
         subnormal, normal_bits});
    return module_.AddInstruction(
        SpirvOp::Select, uint_type_,
        {module_.AddInstruction(SpirvOp::IEqual, bool_type_, {exponent, UInt(31)}),
         special, result});
}

u32 SpirvTranslateContext::ConvertGfx10BufferComponent(
    u32 raw, u32 bit_count, u32 number_format, u32 data_format) {
    const u32 width_is_32 =
        module_.AddInstruction(SpirvOp::IEqual, bool_type_, {bit_count, UInt(32)});
    u32 low_mask = module_.AddInstruction(
        SpirvOp::ISub, uint_type_, {ShiftLeftLogical(UInt(1), bit_count), UInt(1)});
    low_mask = module_.AddInstruction(
        SpirvOp::Select, uint_type_, {width_is_32, UInt(0xFFFFFFFFu), low_mask});

    const u32 signed_raw = module_.AddInstruction(
        SpirvOp::BitFieldSExtract, int_type_,
        {Bitcast(int_type_, raw), UInt(0), bit_count});
    const u32 signed_bits = Bitcast(uint_type_, signed_raw);
    const u32 unsigned_float =
        module_.AddInstruction(SpirvOp::ConvertUToF, float_type_, {raw});
    const u32 signed_float =
        module_.AddInstruction(SpirvOp::ConvertSToF, float_type_, {signed_raw});

    const u32 unorm = Bitcast(
        uint_type_,
        module_.AddInstruction(
            SpirvOp::FDiv, float_type_,
            {unsigned_float,
             module_.AddInstruction(SpirvOp::ConvertUToF, float_type_, {low_mask})}));
    const u32 signed_maximum = ShiftRightLogical(low_mask, UInt(1));
    u32 snorm_float = module_.AddInstruction(
        SpirvOp::FDiv, float_type_,
        {signed_float,
         module_.AddInstruction(SpirvOp::ConvertUToF, float_type_, {signed_maximum})});
    snorm_float = module_.AddInstruction(
        SpirvOp::Select, float_type_,
        {module_.AddInstruction(
             SpirvOp::FOrdLessThan, bool_type_, {snorm_float, Float(-1.f)}),
         Float(-1.f), snorm_float});
    const u32 snorm = Bitcast(uint_type_, snorm_float);
    const u32 uscaled = Bitcast(uint_type_, unsigned_float);
    const u32 sscaled = Bitcast(uint_type_, signed_float);

    const u32 unpacked_half = Ext(
        GlslExt::UnpackHalf2x16, vec2_type_, {BitwiseAnd(raw, UInt(0xFFFF))});
    const u32 half = Bitcast(
        uint_type_,
        module_.AddInstruction(SpirvOp::CompositeExtract, float_type_, {unpacked_half, 0}));
    u32 floating = module_.AddInstruction(
        SpirvOp::Select, uint_type_,
        {module_.AddInstruction(SpirvOp::IEqual, bool_type_, {bit_count, UInt(16)}),
         half, raw});

    // 10_11_11 / 11_11_10 use unsigned mini-floats under NUM_FORMAT FLOAT.
    const u32 is_packed_float = module_.AddInstruction(
        SpirvOp::LogicalOr, bool_type_,
        {module_.AddInstruction(SpirvOp::IEqual, bool_type_, {data_format, UInt(6)}),
         module_.AddInstruction(SpirvOp::IEqual, bool_type_, {data_format, UInt(7)})});
    floating = module_.AddInstruction(
        SpirvOp::Select, uint_type_,
        {is_packed_float, DecodeUnsignedMiniFloat(raw, bit_count), floating});

    u32 result = raw;
    result = SelectUInt(number_format, 0, unorm, result);
    result = SelectUInt(number_format, 1, snorm, result);
    result = SelectUInt(number_format, 2, uscaled, result);
    result = SelectUInt(number_format, 3, sscaled, result);
    result = SelectUInt(number_format, 4, raw, result);
    result = SelectUInt(number_format, 5, signed_bits, result);
    result = SelectUInt(number_format, 7, floating, result);
    return result;
}

u32 SpirvTranslateContext::LoadGfx10BufferFormatComponent(
    int binding, u32 element_address, u32 data_format, u32 number_format,
    u32 component) {
    u32 byte_offset = UInt(0);
    u32 bit_offset = UInt(0);
    u32 bit_count = UInt(0);

    auto set_layout = [&](u32 format, u32 bytes, u32 bits, u32 count) {
        const u32 matches =
            module_.AddInstruction(SpirvOp::IEqual, bool_type_, {data_format, UInt(format)});
        byte_offset = module_.AddInstruction(
            SpirvOp::Select, uint_type_, {matches, UInt(bytes), byte_offset});
        bit_offset = module_.AddInstruction(
            SpirvOp::Select, uint_type_, {matches, UInt(bits), bit_offset});
        bit_count = module_.AddInstruction(
            SpirvOp::Select, uint_type_, {matches, UInt(count), bit_count});
    };

    // Legacy DATA_FORMAT layouts (SharpEmu table; packed formats keep their
    // bit offset in the first dword).
    switch (component) {
        case 0:
            set_layout(1, 0, 0, 8);   // 8
            set_layout(2, 0, 0, 16);  // 16
            set_layout(3, 0, 0, 8);   // 8_8
            set_layout(4, 0, 0, 32);  // 32
            set_layout(5, 0, 0, 16);  // 16_16
            set_layout(6, 0, 0, 10);  // 10_11_11
            set_layout(7, 0, 0, 11);  // 11_11_10
            set_layout(8, 0, 0, 10);  // 10_10_10_2
            set_layout(9, 0, 0, 2);   // 2_10_10_10
            set_layout(10, 0, 0, 8);  // 8_8_8_8
            set_layout(11, 0, 0, 32); // 32_32
            set_layout(12, 0, 0, 16); // 16_16_16_16
            set_layout(13, 0, 0, 32); // 32_32_32
            set_layout(14, 0, 0, 32); // 32_32_32_32
            break;
        case 1:
            set_layout(3, 1, 0, 8);
            set_layout(5, 2, 0, 16);
            set_layout(6, 0, 10, 11);
            set_layout(7, 0, 11, 11);
            set_layout(8, 0, 10, 10);
            set_layout(9, 0, 2, 10);
            set_layout(10, 1, 0, 8);
            set_layout(11, 4, 0, 32);
            set_layout(12, 2, 0, 16);
            set_layout(13, 4, 0, 32);
            set_layout(14, 4, 0, 32);
            break;
        case 2:
            set_layout(6, 0, 21, 11);
            set_layout(7, 0, 22, 10);
            set_layout(8, 0, 20, 10);
            set_layout(9, 0, 12, 10);
            set_layout(10, 2, 0, 8);
            set_layout(12, 4, 0, 16);
            set_layout(13, 8, 0, 32);
            set_layout(14, 8, 0, 32);
            break;
        default:
            set_layout(8, 0, 30, 2);
            set_layout(9, 0, 22, 10);
            set_layout(10, 3, 0, 8);
            set_layout(12, 6, 0, 16);
            set_layout(14, 12, 0, 32);
            break;
    }

    const u32 packed = LoadUnalignedBufferWord(binding, IAdd(element_address, byte_offset));
    const u32 raw = module_.AddInstruction(
        SpirvOp::BitFieldUExtract, uint_type_, {packed, bit_offset, bit_count});
    const u32 converted =
        ConvertGfx10BufferComponent(raw, bit_count, number_format, data_format);
    const u32 valid =
        module_.AddInstruction(SpirvOp::INotEqual, bool_type_, {bit_count, UInt(0)});
    return module_.AddInstruction(
        SpirvOp::Select, uint_type_,
        {valid, converted,
         component == 3 ? Gfx10FormatOne(number_format) : UInt(0)});
}

void SpirvTranslateContext::EmitBufferFormatLoad(
    int binding, u32 byte_address, u32 scalar_resource, u32 vector_data,
    u32 component_count) {
    const u32 descriptor_word3 = LoadS(scalar_resource + 3);
    const u32 unified_format =
        BitwiseAnd(ShiftRightLogical(descriptor_word3, UInt(12)), UInt(0x7F));
    u32 number_format = 0;
    const u32 data_format = DecodeGfx10BufferFormat(unified_format, number_format);

    u32 canonical[4];
    for (u32 component = 0; component < 4; ++component) {
        canonical[component] = LoadGfx10BufferFormatComponent(
            binding, byte_address, data_format, number_format, component);
    }

    const u32 one = Gfx10FormatOne(number_format);
    for (u32 destination = 0; destination < component_count; ++destination) {
        const u32 selector = BitwiseAnd(
            ShiftRightLogical(descriptor_word3, UInt(destination * 3)), UInt(7));
        u32 value = UInt(0);
        value = SelectUInt(selector, 1, one, value);
        value = SelectUInt(selector, 4, canonical[0], value);
        value = SelectUInt(selector, 5, canonical[1], value);
        value = SelectUInt(selector, 6, canonical[2], value);
        value = SelectUInt(selector, 7, canonical[3], value);
        StoreV(vector_data + destination, value);
    }
}

// ---------------------------------------------------------------------------
// Buffer loads.  Raw dword loads read u32 words; BufferLoadFormat* decodes
// components dynamically from the descriptor (SharpEmu TryEmitBufferMemory).
// ---------------------------------------------------------------------------
bool SpirvTranslateContext::TryEmitBufferMemory(
    const GcnInstruction& instruction,
    const GcnBufferMemoryControl& control,
    std::string& error) {
    const auto binding_it = buffer_binding_by_pc_.find(instruction.pc);
    if (binding_it == buffer_binding_by_pc_.end()) {
        for (u32 index = 0; index < control.dword_count; ++index) {
            StoreV(control.vector_data + index, UInt(0));
        }
        return true;
    }
    const int binding = binding_it->second;

    const u32 scalar_offset = instruction.sources.size() > 2
        ? GetRawSource(instruction, 2)
        : UInt(0);
    u32 stride = ShiftRightLogical(LoadS(control.scalar_resource + 1), UInt(16));
    stride = BitwiseAnd(stride, UInt(0x3FFF));
    const u32 vector_index = control.index_enabled
        ? LoadV(control.vector_address)
        : UInt(0);
    const u32 vector_offset = control.offset_enabled
        ? LoadV(control.vector_address + (control.index_enabled ? 1u : 0u))
        : UInt(0);
    u32 byte_address = IAdd(UInt(static_cast<u32>(control.offset_bytes)), scalar_offset);
    byte_address = IAdd(byte_address, vector_offset);
    byte_address = IAdd(
        byte_address,
        module_.AddInstruction(SpirvOp::IMul, uint_type_, {vector_index, stride}));

    if (instruction.opcode.rfind("BufferStore", 0) == 0 ||
        instruction.opcode.rfind("TBufferStore", 0) == 0 ||
        instruction.opcode.rfind("BufferAtomic", 0) == 0 ||
        instruction.opcode.rfind("TBufferAtomic", 0) == 0) {
        error = "buffer store/atomic opcodes are not supported yet";
        return false;
    }

    if (instruction.opcode.rfind("BufferLoadFormat", 0) == 0 ||
        instruction.opcode.rfind("TBufferLoadFormat", 0) == 0) {
        EmitBufferFormatLoad(binding, byte_address, control.scalar_resource,
                             control.vector_data, control.dword_count);
        return true;
    }

    // Raw dword loads.
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

// Arrayed sample/gather coordinates: (u, v, slice) — the third address
// component picks the array layer (SharpEmu BuildFloatArrayCoordinates).
u32 SpirvTranslateContext::BuildFloatArrayCoordinates(
    const GcnImageControl& image, int start) {
    const u32 x = LoadImageFloatAddress(image, start);
    const u32 y = LoadImageFloatAddress(image, start + 1);
    const u32 slice = LoadImageFloatAddress(image, start + 2);
    return module_.AddInstruction(
        SpirvOp::CompositeConstruct, vec3_type_, {x, y, slice});
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
        } else if (resource.arrayed) {
            // Arrayed images answer the query with an ivec3 (w, h, layers);
            // the guest expects the 2D size here.
            const u32 ivec3_type = module_.TypeVector(int_type_, 3);
            const u32 size3 = module_.AddInstruction(
                SpirvOp::ImageQuerySizeLod, ivec3_type, {query_image, UInt(0)});
            size = module_.AddInstruction(
                SpirvOp::VectorShuffle, ivec2_type, {size3, size3, 0u, 1u});
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
        const u32 coordinates = resource.arrayed
            ? BuildFloatArrayCoordinates(image, address_cursor)
            : BuildFloatCoordinates(image, address_cursor);
        const bool explicit_lod = has_gradients || has_zero_lod || has_lod;
        const u32 lod = has_zero_lod
            ? Float(0)
            : has_lod
                ? LoadImageFloatAddress(image,
                                        address_cursor + (resource.arrayed ? 3 : 2))
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

    // Compute stage: no exports (returns true above for the non-pixel case
    // when target doesn't match vertex conditions — safe no-op).
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

// ---------------------------------------------------------------------------
// H7 disk-cache hook (gcn_translate.h).  Lives next to the translator so the
// cache key input (flattened program words) stays in sync with the decoder's
// instruction model.
// ---------------------------------------------------------------------------
namespace GPU::Shader {
namespace {

// Counts the stage-interface attributes of a cached module: variables of
// the interface storage class (Output for vertex, Input for pixel) that
// carry a Location decoration — the same set attribute_count reports from a
// live translation (vertex_outputs_ / pixel_inputs_).
u32 CachedAttributeCount(const std::vector<u32>& words, GcnSpirvStage stage) {
    constexpr u32 kOpDecorate  = 71;
    constexpr u32 kOpVariable  = 59;
    constexpr u32 kDecLocation = 30;
    // SPIR-V StorageClass: Input = 1, Output = 3.
    if (stage == GcnSpirvStage::Compute) return 0; // no interface attributes
    const u32 wanted_class = stage == GcnSpirvStage::Vertex ? 3u /*Output*/
                                                            : 1u /*Input*/;

    std::unordered_map<u32, u32> variable_classes; // result id -> storage class
    std::unordered_set<u32>      located;          // ids with Location
    size_t cursor = 5; // header
    while (cursor < words.size()) {
        const u32 word  = words[cursor];
        const u32 op    = word & 0xFFFF;
        const u32 count = word >> 16;
        if (count == 0 || cursor + count > words.size()) {
            return 0; // malformed stream: refuse to derive metadata
        }
        if (op == kOpVariable && count >= 4) {
            variable_classes.emplace(words[cursor + 2], words[cursor + 3]);
        } else if (op == kOpDecorate && count >= 4 &&
                   words[cursor + 2] == kDecLocation) {
            located.insert(words[cursor + 1]);
        }
        cursor += count;
    }
    u32 attributes = 0;
    for (const u32 id : located) {
        const auto it = variable_classes.find(id);
        if (it != variable_classes.end() && it->second == wanted_class) {
            ++attributes;
        }
    }
    return attributes;
}

} // namespace

std::vector<u32> GcnProgramFlattenWords(const GcnProgram& program) {
    std::vector<u32> words;
    for (const GcnInstruction& ins : program.instructions) {
        words.insert(words.end(), ins.words.begin(), ins.words.end());
    }
    return words;
}

bool GcnTranslateWithCache(const GcnProgram&          program,
                           const GcnTranslateOptions& options,
                           GPU::GcnShaderDiskCache*   cache,
                           GcnSpirvShader&            out,
                           std::string&               error) {
    if (cache == nullptr) {
        return GcnTranslateToSpirv(program, options, out, error);
    }
    const std::vector<u32> words = GcnProgramFlattenWords(program);
    const GPU::GcnShaderCacheKey key =
        GPU::GcnShaderCacheComputeKey(words.data(), words.size(), options);
    if (cache->TryLoad(key, out.words)) {
        out.attribute_count = CachedAttributeCount(out.words, options.stage);
        return true;
    }
    if (!GcnTranslateToSpirv(program, options, out, error)) {
        return false;
    }
    // A failed store is not fatal: the translation is still valid, the next
    // run just pays the translation cost again.
    cache->Store(key, out.words);
    return true;
}

} // namespace GPU::Shader

namespace GPU::Shader {

// ---------------------------------------------------------------------------
// Consumed-SGPR mask + per-draw scalar-state helpers (gcn_translate.h).
// Port of SharpEmu's Gen5ShaderTranslator.ComputeConsumedScalarMask /
// AddConsumedScalar / IsScalarConsumed and the AgcExports packing helpers.
// ---------------------------------------------------------------------------
namespace {
void AddConsumedScalar(GcnConsumedScalarMask& mask, u32 reg, u32 count) {
    for (u32 index = 0; index < count; ++index) {
        const u32 target = reg + index;
        if (target < 256) {
            mask[target >> 6] |= 1ull << (target & 63);
        }
    }
}
} // namespace

GcnConsumedScalarMask GcnComputeConsumedScalarMask(const GcnProgram& program) {
    GcnConsumedScalarMask mask{};
    AddConsumedScalar(mask, 106, 2);
    AddConsumedScalar(mask, 124, 2);
    AddConsumedScalar(mask, 126, 2);
    for (const GcnInstruction& instruction : program.instructions) {
        for (const GcnOperand& source : instruction.sources) {
            if (source.kind == GcnOperandKind::ScalarRegister) {
                AddConsumedScalar(mask, source.value, 2);
            }
        }

        // Scalar memory bases can be a 4-dword buffer descriptor.
        if ((instruction.encoding == GcnEncoding::Smem ||
             instruction.encoding == GcnEncoding::Smrd) &&
            !instruction.sources.empty() &&
            instruction.sources[0].kind == GcnOperandKind::ScalarRegister) {
            AddConsumedScalar(mask, instruction.sources[0].value, 4);
        }

        if (const auto* image = std::get_if<GcnImageControl>(&instruction.control)) {
            AddConsumedScalar(mask, image->scalar_resource, 8);
            AddConsumedScalar(mask, image->scalar_sampler, 4);
        } else if (const auto* scalar =
                       std::get_if<GcnScalarMemoryControl>(&instruction.control)) {
            if (scalar->dynamic_offset_register.has_value()) {
                AddConsumedScalar(mask, *scalar->dynamic_offset_register, 2);
            }
        } else if (const auto* global =
                       std::get_if<GcnGlobalMemoryControl>(&instruction.control)) {
            AddConsumedScalar(mask, global->scalar_address, 2);
        } else if (const auto* buffer =
                       std::get_if<GcnBufferMemoryControl>(&instruction.control)) {
            AddConsumedScalar(mask, buffer->scalar_resource, 4);
        }
    }
    return mask;
}

bool GcnIsScalarConsumed(const GcnConsumedScalarMask& mask, u32 reg) {
    return reg < 256 && (mask[reg >> 6] & (1ull << (reg & 63))) != 0;
}

bool GcnIsArrayedImageBinding(const GcnInstruction& instruction) {
    const auto* control = std::get_if<GcnImageControl>(&instruction.control);
    return control != nullptr && control->is_array &&
        (instruction.opcode.rfind("ImageSample", 0) == 0 ||
         instruction.opcode.rfind("ImageGather4", 0) == 0);
}

int GcnTranslateAddInitialScalarBinding(GcnTranslateOptions& options) {
    // No instruction pcs: the shader reads this slot only in the initial
    // state prologue via initial_scalar_buffer_index.
    options.buffer_bindings.push_back(GcnSpirvBufferBinding{});
    options.initial_scalar_buffer_index =
        static_cast<int>(options.buffer_bindings.size()) - 1;
    return options.initial_scalar_buffer_index;
}

std::vector<u32> GcnPackInitialScalarState(
    const std::vector<u32>& initial_scalar_registers) {
    std::vector<u32> packed(256, 0);
    const size_t count =
        std::min(initial_scalar_registers.size(), packed.size());
    std::copy_n(initial_scalar_registers.begin(),
                static_cast<std::ptrdiff_t>(count), packed.begin());
    return packed;
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
    } else if (stage == GcnSpirvStage::Vertex) {
        u32 max_param = 0;
        for (const GcnInstruction& ins : program.instructions) {
            if (const auto* export_ = std::get_if<GcnExportControl>(&ins.control)) {
                if (export_->target >= 32 && export_->target < 64) {
                    max_param = std::max(max_param, export_->target - 32 + 1);
                }
            }
        }
        options.required_vertex_output_count = static_cast<int>(max_param);
    } else if (stage == GcnSpirvStage::Compute) {
        // H6: compute has no vertex exports or pixel MRTs.  Workgroup size
        // must be set by the caller from COMPUTE_PGM_RSRC2 before translation.
        // Default to (64, 1, 1) if not set (common GCN wavefront size).
        if (options.workgroup_size_x == 0) options.workgroup_size_x = 64;
        if (options.workgroup_size_y == 0) options.workgroup_size_y = 1;
        if (options.workgroup_size_z == 0) options.workgroup_size_z = 1;
    }

    for (const GcnInstruction& ins : program.instructions) {
        if (std::get_if<GcnImageControl>(&ins.control)) {
            GcnSpirvImageBinding binding;
            binding.pc = ins.pc;
            binding.is_arrayed = GcnIsArrayedImageBinding(ins);
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
