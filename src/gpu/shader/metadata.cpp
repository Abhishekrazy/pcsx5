// AGC shader binary metadata reader — transliteration of SharpEmu's
// Gen5ShaderMetadataReader.cs + the AgcExports.cs header layout, adapted to
// the on-disk shader ELF container.  See metadata.h.

#include "metadata.h"

#include <cstring>

namespace GPU::Shader {

namespace {

// Header field offsets (AgcExports.cs Shader*Offset constants).
constexpr u64 kUserDataOffset         = 0x08;
constexpr u64 kCxRegistersOffset      = 0x18;
constexpr u64 kShRegistersOffset      = 0x20;
constexpr u64 kInputSemanticsOffset   = 0x30;
constexpr u64 kOutputSemanticsOffset  = 0x38;
constexpr u64 kNumInputSemanticsOffset  = 0x50;
constexpr u64 kNumOutputSemanticsOffset = 0x56;
constexpr u64 kShaderTypeOffset         = 0x5A;
constexpr u64 kNumShRegistersOffset     = 0x5C;

constexpr u32 kResourceClassCount  = 4;
constexpr u32 kMaxMetadataEntries  = 4096;

bool ReadU16(std::span<const u8> data, u64 offset, u16& value) {
    if (offset + sizeof(u16) > data.size()) {
        return false;
    }
    std::memcpy(&value, data.data() + offset, sizeof(u16));
    return true;
}

bool ReadU32(std::span<const u8> data, u64 offset, u32& value) {
    if (offset + sizeof(u32) > data.size()) {
        return false;
    }
    std::memcpy(&value, data.data() + offset, sizeof(u32));
    return true;
}

bool ReadU64(std::span<const u8> data, u64 offset, u64& value) {
    if (offset + sizeof(u64) > data.size()) {
        return false;
    }
    std::memcpy(&value, data.data() + offset, sizeof(u64));
    return true;
}

// Resolves a self-relative pointer field: target = field offset + value.
// Returns true when the field is present (non-zero); `target` is unchecked
// against the span — callers bounds-check on use.
bool ResolvePointer(std::span<const u8> data, u64 field_offset, u64& target) {
    u64 relative = 0;
    if (!ReadU64(data, field_offset, relative)) {
        return false;
    }
    if (relative == 0) {
        target = 0;
        return false;
    }
    target = field_offset + relative;
    return true;
}

bool ParseUserData(std::span<const u8> header, u64 user_data_offset,
                   AgcUserData& out, std::string& error) {
    u64 direct_resource_offsets = 0;
    const bool has_direct =
        ResolvePointer(header, user_data_offset, direct_resource_offsets);

    u64 resource_offsets[kResourceClassCount] = {};
    for (u64 resource_class = 0; resource_class < kResourceClassCount;
         ++resource_class) {
        ResolvePointer(header,
                       user_data_offset + 0x08 + resource_class * sizeof(u64),
                       resource_offsets[resource_class]);
    }

    if (!ReadU16(header, user_data_offset + 0x28,
                 out.extended_user_data_size_dwords) ||
        !ReadU16(header, user_data_offset + 0x2A,
                 out.shader_resource_table_size_dwords)) {
        error = "userdata-tail-unreadable";
        return false;
    }

    u16 direct_resource_count = 0;
    if (!ReadU16(header, user_data_offset + 0x2C, direct_resource_count) ||
        direct_resource_count > kMaxMetadataEntries) {
        error = "userdata-direct-count-invalid";
        return false;
    }

    u16 resource_counts[kResourceClassCount] = {};
    for (u64 resource_class = 0; resource_class < kResourceClassCount;
         ++resource_class) {
        if (!ReadU16(header,
                     user_data_offset + 0x2E + resource_class * sizeof(u16),
                     resource_counts[resource_class]) ||
            resource_counts[resource_class] > kMaxMetadataEntries) {
            error = "userdata-resource-count-invalid";
            return false;
        }
    }

    if (direct_resource_count != 0 && has_direct) {
        for (u32 type = 0; type < direct_resource_count; ++type) {
            u16 offset = 0;
            if (!ReadU16(header,
                         direct_resource_offsets + type * sizeof(u16),
                         offset)) {
                error = "userdata-direct-table-unreadable";
                return false;
            }
            if (offset != 0xFFFF) {
                out.direct_resources[type] = offset;
            }
        }
    }

    for (u32 resource_class = 0; resource_class < kResourceClassCount;
         ++resource_class) {
        const u32 count = resource_counts[resource_class];
        if (count == 0 || resource_offsets[resource_class] == 0) {
            continue;
        }
        for (u32 slot = 0; slot < count; ++slot) {
            u16 sharp = 0;
            if (!ReadU16(header,
                         resource_offsets[resource_class] + slot * sizeof(u16),
                         sharp)) {
                error = "userdata-resource-table-unreadable";
                return false;
            }
            const u32 offset = sharp & 0x7FFF;
            if (offset == 0x7FFF) {
                continue;
            }
            out.resources.push_back(AgcResourceMapping{
                static_cast<AgcResourceKind>(resource_class),
                slot,
                offset,
                (sharp & 0x8000) != 0,
            });
        }
    }

    return true;
}

// Parses an 8-byte {reg, value} table at a self-relative pointer field,
// stopping at `end_offset` (tables carry no count of their own except where
// the caller passes a bounded end from the fixed header).
bool ParseRegisterTable(std::span<const u8> header, u64 field_offset,
                        u64 end_offset,
                        std::vector<AgcRegisterEntry>& out) {
    u64 table = 0;
    if (!ResolvePointer(header, field_offset, table)) {
        return true; // absent table is not an error
    }
    while (table + 8 <= end_offset && table + 8 <= header.size()) {
        AgcRegisterEntry entry;
        if (!ReadU32(header, table, entry.reg) ||
            !ReadU32(header, table + 4, entry.value)) {
            break;
        }
        out.push_back(entry);
        table += 8;
    }
    return true;
}

} // namespace

const char* AgcShaderTypeName(u8 shader_type) {
    switch (shader_type) {
    case AgcShaderType::Compute:  return "compute";
    case AgcShaderType::Pixel:    return "pixel";
    case AgcShaderType::Export:   return "export";
    case AgcShaderType::Geometry: return "geometry";
    case AgcShaderType::Local:    return "local";
    default:                      return "unknown";
    }
}

const char* AgcResourceKindName(AgcResourceKind kind) {
    switch (kind) {
    case AgcResourceKind::ReadOnlyTexture:  return "ro_texture";
    case AgcResourceKind::ReadWriteTexture: return "rw_texture";
    case AgcResourceKind::Sampler:          return "sampler";
    case AgcResourceKind::ConstantBuffer:   return "const_buffer";
    }
    return "?";
}

bool AgcParseHeader(std::span<const u8> header, AgcShaderHeader& out,
                    std::string& error) {
    error.clear();
    out = AgcShaderHeader{};

    u32 magic = 0, version = 0;
    if (!ReadU32(header, 0, magic) || !ReadU32(header, 4, version)) {
        error = "header-too-small";
        return false;
    }
    if (magic != kAgcShaderFileMagic || version != kAgcShaderFileVersion) {
        error = "header-magic-version";
        return false;
    }

    if (!ReadU16(header, kNumInputSemanticsOffset, out.num_input_semantics) ||
        !ReadU16(header, kNumOutputSemanticsOffset, out.num_output_semantics)) {
        error = "header-fields-unreadable";
        return false;
    }
    // shader type is a single byte at 0x5A; num SH registers a byte at 0x5C.
    u16 shader_type16 = 0, num_sh16 = 0;
    ReadU16(header, kShaderTypeOffset, shader_type16);
    ReadU16(header, kNumShRegistersOffset, num_sh16);
    out.shader_type      = static_cast<u8>(shader_type16 & 0xFF);
    out.num_sh_registers = static_cast<u8>(num_sh16 & 0xFF);

    u64 semantics = 0;
    if (out.num_input_semantics != 0 &&
        ResolvePointer(header, kInputSemanticsOffset, semantics)) {
        for (u32 i = 0; i < out.num_input_semantics; ++i) {
            u32 semantic = 0;
            if (!ReadU32(header, semantics + i * sizeof(u32), semantic)) {
                error = "input-semantics-unreadable";
                return false;
            }
            out.input_semantics.push_back(semantic);
        }
    }
    if (out.num_output_semantics != 0 &&
        ResolvePointer(header, kOutputSemanticsOffset, semantics)) {
        for (u32 i = 0; i < out.num_output_semantics; ++i) {
            u32 semantic = 0;
            if (!ReadU32(header, semantics + i * sizeof(u32), semantic)) {
                error = "output-semantics-unreadable";
                return false;
            }
            out.output_semantics.push_back(semantic);
        }
    }

    u64 user_data = 0;
    const bool has_user_data = ResolvePointer(header, kUserDataOffset, user_data);

    // The SH table is counted (num_sh_registers); the CX table is not — bound
    // it by the userData block (which follows it in practice) or the blob end.
    u64 sh_table = 0;
    if (ResolvePointer(header, kShRegistersOffset, sh_table)) {
        ParseRegisterTable(header, kShRegistersOffset,
                           sh_table + out.num_sh_registers * 8,
                           out.sh_registers);
    }
    u64 cx_end = header.size();
    if (has_user_data) {
        u64 cx_table = 0;
        if (ResolvePointer(header, kCxRegistersOffset, cx_table) &&
            cx_table < user_data) {
            cx_end = user_data;
        }
    }
    ParseRegisterTable(header, kCxRegistersOffset, cx_end, out.cx_registers);

    if (has_user_data) {
        if (!ParseUserData(header, user_data, out.user_data, error)) {
            return false;
        }
        out.has_user_data = true;
    }

    return true;
}

bool AgcParseMetadataBlob(std::span<const u8> blob, AgcMetadataBlob& out) {
    out = AgcMetadataBlob{};
    // "sl00" little-endian.
    constexpr u32 kSl00Magic = 0x30306C73;
    u32 magic = 0;
    if (!ReadU32(blob, 0, magic) || magic != kSl00Magic) {
        return false;
    }
    ReadU32(blob, 4, out.size0);
    ReadU32(blob, 8, out.size1);

    // The compiler version string ("2.0.0.0 build ... revision ...") is the
    // first printable NUL-terminated run in the payload.
    for (size_t i = 12; i < blob.size(); ++i) {
        const u8 c = blob[i];
        if (c < 0x20 || c > 0x7E) {
            continue;
        }
        size_t end = i;
        while (end < blob.size() && blob[end] >= 0x20 && blob[end] <= 0x7E) {
            ++end;
        }
        if (end - i >= 8 && end < blob.size() && blob[end] == 0) {
            out.compiler_version.assign(
                reinterpret_cast<const char*>(blob.data() + i), end - i);
            break;
        }
        i = end;
    }

    out.valid = true;
    return true;
}

bool AgcLoadShaderFile(std::span<const u8> elf_file, AgcShaderFile& out,
                       std::string& error) {
    error.clear();
    out = AgcShaderFile{};

    if (elf_file.size() < 64 || elf_file[0] != 0x7F || elf_file[1] != 'E' ||
        elf_file[2] != 'L' || elf_file[3] != 'F' || elf_file[4] != 2) {
        error = "not-elf64";
        return false;
    }

    u64 e_shoff = 0;
    u16 e_shentsize = 0, e_shnum = 0, e_shstrndx = 0;
    ReadU64(elf_file, 0x28, e_shoff);
    ReadU16(elf_file, 0x3A, e_shentsize);
    ReadU16(elf_file, 0x3C, e_shnum);
    ReadU16(elf_file, 0x3E, e_shstrndx);
    if (e_shoff == 0 || e_shentsize < 64 || e_shnum == 0 ||
        e_shstrndx >= e_shnum ||
        e_shoff + static_cast<u64>(e_shnum) * e_shentsize > elf_file.size()) {
        error = "elf-section-headers-invalid";
        return false;
    }

    struct Section {
        u32 name;
        u64 offset;
        u64 size;
    };
    std::vector<Section> sections(e_shnum);
    for (u32 i = 0; i < e_shnum; ++i) {
        const u64 base = e_shoff + static_cast<u64>(i) * e_shentsize;
        ReadU32(elf_file, base, sections[i].name);
        ReadU64(elf_file, base + 0x18, sections[i].offset);
        ReadU64(elf_file, base + 0x20, sections[i].size);
    }

    const Section& strtab = sections[e_shstrndx];
    if (strtab.offset + strtab.size > elf_file.size()) {
        error = "elf-shstrtab-invalid";
        return false;
    }
    const auto section_name = [&](u32 name_offset) -> std::string {
        if (name_offset >= strtab.size) {
            return {};
        }
        const char* base = reinterpret_cast<const char*>(
            elf_file.data() + strtab.offset + name_offset);
        const size_t max_len = strtab.size - name_offset;
        const size_t len     = strnlen(base, max_len);
        return std::string(base, len);
    };

    bool have_code = false, have_header = false;
    for (const Section& section : sections) {
        if (section.offset + section.size > elf_file.size()) {
            error = "elf-section-out-of-bounds";
            return false;
        }
        const std::string name = section_name(section.name);
        const std::span<const u8> content(
            elf_file.data() + section.offset,
            static_cast<size_t>(section.size));
        if (name == ".shader_text") {
            out.code    = content;
            have_code   = true;
        } else if (name == ".shader_header") {
            out.header  = content;
            have_header = true;
        } else if (name == ".metadata") {
            out.metadata = content;
        } else if (name == ".cfg") {
            out.cfg = content;
        }
    }

    if (!have_code || !have_header) {
        error = "elf-missing-shader-sections";
        return false;
    }
    if (!AgcParseHeader(out.header, out.parsed_header, error)) {
        return false;
    }
    AgcParseMetadataBlob(out.metadata, out.parsed_metadata);
    return true;
}

} // namespace GPU::Shader
