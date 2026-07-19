// AGC shader binary metadata reader (Phase 5 M2).
//
// Guided transliteration of SharpEmu's Gen5ShaderMetadataReader.cs plus the
// sceAgcCreateShader header layout from SharpEmu.Libs/Agc/AgcExports.cs,
// adapted to the on-disk shader ELF container (sections .shader_text /
// .shader_header / .metadata / .cfg, as produced by the PSSL compiler and
// shipped in game dumps).
//
// Pointer fields inside the shader header are *self-relative*: the stored
// value plus the field's own offset yields the target offset within the
// header blob (AgcExports.cs RelocatePointerField: field += *field).
#pragma once

#include "../../common/types.h"

#include <map>
#include <span>
#include <string>
#include <vector>

namespace GPU::Shader {

// AGC shader header file magic ("1234") and supported version.
inline constexpr u32 kAgcShaderFileMagic   = 0x34333231;
inline constexpr u32 kAgcShaderFileVersion = 0x18;

// Shader type byte at header+0x5A (AgcExports.cs PatchShaderProgramRegisters).
namespace AgcShaderType {
inline constexpr u8 Compute  = 0;
inline constexpr u8 Pixel    = 1;
inline constexpr u8 Export   = 2; // ES (vertex/geometry export stage)
inline constexpr u8 Geometry = 4;
inline constexpr u8 Local    = 7;
} // namespace AgcShaderType

const char* AgcShaderTypeName(u8 shader_type);

// Mirrors SharpEmu's Gen5ShaderResourceKind.
enum class AgcResourceKind : u32 {
    ReadOnlyTexture  = 0,
    ReadWriteTexture = 1,
    Sampler          = 2,
    ConstantBuffer   = 3,
};

const char* AgcResourceKindName(AgcResourceKind kind);

// Mirrors SharpEmu's Gen5ShaderResourceMapping.
struct AgcResourceMapping {
    AgcResourceKind kind = AgcResourceKind::ReadOnlyTexture;
    u32  slot          = 0;
    u32  offset_dwords = 0;
    bool size_flag     = false;
};

// One 8-byte {register id, value} pair from the SH/CX register tables.
struct AgcRegisterEntry {
    u32 reg   = 0;
    u32 value = 0;
};

// Mirrors SharpEmu's Gen5ShaderMetadata (the userData block).
struct AgcUserData {
    u16 extended_user_data_size_dwords  = 0;
    u16 shader_resource_table_size_dwords = 0;
    std::map<u32, u32>              direct_resources; // type -> offset dwords
    std::vector<AgcResourceMapping> resources;
};

// Parsed shader binary header (.shader_header section content).
struct AgcShaderHeader {
    u8  shader_type          = 0xFF;
    u8  num_sh_registers     = 0;
    u16 num_input_semantics  = 0;
    u16 num_output_semantics = 0;

    std::vector<u32> input_semantics;
    std::vector<u32> output_semantics;
    std::vector<AgcRegisterEntry> sh_registers;
    std::vector<AgcRegisterEntry> cx_registers;

    bool        has_user_data = false;
    AgcUserData user_data;
};

// Minimal parse of the PSSL ".metadata" blob ("sl00" magic).  The blob
// format is proprietary; we surface its sizes and the embedded compiler
// version string, which is all slice 1 needs for reporting.
struct AgcMetadataBlob {
    bool        valid  = false;
    u32         size0  = 0; // u32 at +4 (payload size)
    u32         size1  = 0; // u32 at +8 (== section size in practice)
    std::string compiler_version;
};

// Sections of an on-disk shader ELF, plus their parsed forms.
struct AgcShaderFile {
    std::span<const u8> code;     // .shader_text   (RDNA instruction dwords)
    std::span<const u8> header;   // .shader_header (AGC shader binary header)
    std::span<const u8> metadata; // .metadata      (PSSL "sl00" blob)
    std::span<const u8> cfg;      // .cfg           (control-flow graph data)

    AgcShaderHeader parsed_header;
    AgcMetadataBlob parsed_metadata;
};

// Parses a shader binary header blob (self-relative pointer fields).
bool AgcParseHeader(std::span<const u8> header, AgcShaderHeader& out,
                    std::string& error);

// Parses the PSSL ".metadata" blob (magic check + sizes + version string).
bool AgcParseMetadataBlob(std::span<const u8> blob, AgcMetadataBlob& out);

// Walks an on-disk shader ELF64, extracts the four shader sections and
// parses header + metadata.  Fails if .shader_text or .shader_header is
// absent or the header does not parse; .metadata/.cfg are optional.
bool AgcLoadShaderFile(std::span<const u8> elf_file, AgcShaderFile& out,
                       std::string& error);

} // namespace GPU::Shader
