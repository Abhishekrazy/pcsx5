// pcsx5_boot_parser.cpp - Standalone game boot & memory parser
// Implements PS5 game metadata parsing (param.sfo) and ELF/SELF memory layout analysis

#include <iostream>
#include <string>
#include <filesystem>
#include <fstream>
#include <vector>
#include <map>
#include <cstring>
#include <cstdint>
#include <iomanip>
#include <algorithm>

#include "src/common/types.h"
#include "src/common/log.h"
#include "src/loader/elf.h"

namespace fs = std::filesystem;

namespace BootParser {

// -----------------------------------------------------------------------------
// param.sfo binary format parser
// -----------------------------------------------------------------------------
// PSF format (little-endian):
//   Header (0x14 bytes):
//     u32 magic;           // 0x46535000 ("PSF\0")
//     u32 version;         // 0x00000101
//     u32 key_table_offset;
//     u32 data_table_offset;
//     u32 entry_count;
//   Key table: null-terminated strings
//   Data table: entry_count * 0x10 bytes each:
//     u16 param_type;      // 0x0204 = string, 0x0404 = integer
//     u16 data_len;        // actual data length
//     u16 max_data_len;    // max data length (for strings)
//     u32 data_offset;     // offset from start of data section
//   Data section: raw values (strings null-terminated, integers u32 LE)

#pragma pack(push, 1)
struct SfoHeader {
    u32 magic;
    u32 version;
    u32 key_table_offset;
    u32 data_table_offset;
    u32 entry_count;
};
static_assert(sizeof(SfoHeader) == 0x14, "SfoHeader must be 0x14 bytes");

struct SfoDataEntry {
    u16 param_type;
    u16 data_len;
    u16 max_data_len;
    u32 data_offset;
    u32 reserved;
    u16 reserved2; // padding to make struct 0x10 bytes
};
static_assert(sizeof(SfoDataEntry) == 0x10, "SfoDataEntry must be 0x10 bytes");
#pragma pack(pop)

enum class SfoParamType : u16 {
    String = 0x0204,
    Integer = 0x0404,
};

struct SfoEntry {
    std::string key;
    SfoParamType type;
    u32 data_len;
    u32 max_data_len;
    u32 data_offset;
    std::string string_value;
    u32 int_value = 0;
};

struct SfoData {
    std::map<std::string, SfoEntry> entries;
    
    // Helper getters for common fields
    std::string GetString(const std::string& key, const std::string& def = "") const {
        auto it = entries.find(key);
        if (it != entries.end() && it->second.type == SfoParamType::String) {
            return it->second.string_value;
        }
        return def;
    }
    
    u32 GetInteger(const std::string& key, u32 def = 0) const {
        auto it = entries.find(key);
        if (it != entries.end() && it->second.type == SfoParamType::Integer) {
            return it->second.int_value;
        }
        return def;
    }
    
    bool HasKey(const std::string& key) const {
        return entries.find(key) != entries.end();
    }
};

bool ParseParamSfo(const std::string& filepath, SfoData& out_data) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file) {
        LOG_ERROR(Loader, "Failed to open param.sfo: {}", filepath.c_str());
        return false;
    }
    
    // Read header
    SfoHeader header;
    file.read(reinterpret_cast<char*>(&header), sizeof(SfoHeader));
    if (!file) {
        LOG_ERROR(Loader, "Failed to read SFO header: {}", filepath.c_str());
        return false;
    }
    
    // Validate magic
    if (header.magic != 0x46535000) {  // "PSF\0" in little-endian
        LOG_ERROR(Loader, "Invalid SFO magic: 0x{:08X} (expected 0x46535000)", header.magic);
        return false;
    }
    
    LOG_DEBUG(Loader, "SFO header: version=0x{:08X}, key_off=0x{:X}, data_off=0x{:X}, entries={}",
              header.version, header.key_table_offset, header.data_table_offset, header.entry_count);
    
    // Read key table
    file.seekg(header.key_table_offset, std::ios::beg);
    std::vector<char> key_table(header.data_table_offset - header.key_table_offset);
    file.read(key_table.data(), key_table.size());
    if (!file) {
        LOG_ERROR(Loader, "Failed to read SFO key table");
        return false;
    }
    
    // Parse keys (null-terminated strings)
    std::vector<std::string> keys;
    std::string current_key;
    for (char c : key_table) {
        if (c == '\0') {
            if (!current_key.empty()) {
                keys.push_back(current_key);
                current_key.clear();
            }
        } else {
            current_key += c;
        }
    }
    
    if (keys.size() != header.entry_count) {
        LOG_WARN(Loader, "Key count mismatch: parsed {} keys, header says {}", keys.size(), header.entry_count);
    }
    
    // Read data table
    file.seekg(header.data_table_offset, std::ios::beg);
    std::vector<SfoDataEntry> data_entries(header.entry_count);
    file.read(reinterpret_cast<char*>(data_entries.data()), 
              header.entry_count * sizeof(SfoDataEntry));
    if (!file) {
        LOG_ERROR(Loader, "Failed to read SFO data table");
        return false;
    }
    
    // Read data section
    u64 data_section_start = file.tellg();
    file.seekg(0, std::ios::end);
    u64 file_size = file.tellg();
    u64 data_section_size = file_size - data_section_start;
    file.seekg(data_section_start, std::ios::beg);
    
    std::vector<u8> data_section(data_section_size);
    file.read(reinterpret_cast<char*>(data_section.data()), data_section_size);
    if (!file) {
        LOG_ERROR(Loader, "Failed to read SFO data section");
        return false;
    }
    
    // Parse entries
    for (size_t i = 0; i < header.entry_count && i < keys.size() && i < data_entries.size(); ++i) {
        const auto& key = keys[i];
        const auto& entry = data_entries[i];
        
        SfoEntry parsed;
        parsed.key = key;
        parsed.type = static_cast<SfoParamType>(entry.param_type);
        parsed.data_len = entry.data_len;
        parsed.max_data_len = entry.max_data_len;
        parsed.data_offset = entry.data_offset;
        
        // Extract value from data section
        if (entry.data_offset + entry.data_len <= data_section_size) {
            const u8* data_ptr = data_section.data() + entry.data_offset;
            
            if (parsed.type == SfoParamType::String) {
                // String: null-terminated, but use data_len as safety bound
                parsed.string_value.assign(
                    reinterpret_cast<const char*>(data_ptr),
                    reinterpret_cast<const char*>(data_ptr) + entry.data_len
                );
                // Remove trailing null if present
                if (!parsed.string_value.empty() && parsed.string_value.back() == '\0') {
                    parsed.string_value.pop_back();
                }
            } else if (parsed.type == SfoParamType::Integer) {
                // Integer: 4-byte little-endian
                if (entry.data_len >= 4) {
                    parsed.int_value = *reinterpret_cast<const u32*>(data_ptr);
                }
            }
        }
        
        LOG_DEBUG(Loader, "SFO entry: {} = {} (type=0x{:04X}, len={})", 
                  key.c_str(), 
                  parsed.type == SfoParamType::String ? parsed.string_value.c_str() : std::to_string(parsed.int_value).c_str(),
                  static_cast<u16>(parsed.type), entry.data_len);
        
        out_data.entries[key] = std::move(parsed);
    }
    
    LOG_INFO(Loader, "Parsed param.sfo: {} entries", out_data.entries.size());
    return true;
}

// -----------------------------------------------------------------------------
// Game metadata structure
// -----------------------------------------------------------------------------
struct GameMetadata {
    std::string title_id;        // TITLE_ID (e.g., "PPSA01234")
    std::string title;           // TITLE (game name)
    std::string version;         // VERSION (e.g., "01.00")
    std::string category;        // CATEGORY (e.g., "GD" for game disc)
    std::string parental_level;  // PARENTAL_LEVEL
    std::string region;          // REGION
    std::string resolution;      // RESOLUTION
    std::string sound_format;    // SOUND_FORMAT
    std::string app_ver;         // APP_VER
    std::string sdk_version;     // SDK_VERSION
    std::string content_id;      // CONTENT_ID
    std::string target_app_ver;  // TARGET_APP_VER
    
    // ELF/SELF info
    std::string eboot_path;      // Path to eboot.bin or main executable
    Loader::LoadedModule loaded_module;
    Loader::ModuleMetadata module_metadata;
    bool module_loaded = false;
};

void ExtractMetadataFromSfo(const SfoData& sfo, GameMetadata& out_meta) {
    out_meta.title_id = sfo.GetString("TITLE_ID");
    out_meta.title = sfo.GetString("TITLE");
    out_meta.version = sfo.GetString("VERSION");
    out_meta.category = sfo.GetString("CATEGORY");
    out_meta.parental_level = sfo.GetString("PARENTAL_LEVEL");
    out_meta.region = sfo.GetString("REGION");
    out_meta.resolution = sfo.GetString("RESOLUTION");
    out_meta.sound_format = sfo.GetString("SOUND_FORMAT");
    out_meta.app_ver = sfo.GetString("APP_VER");
    out_meta.sdk_version = sfo.GetString("SDK_VERSION");
    out_meta.content_id = sfo.GetString("CONTENT_ID");
    out_meta.target_app_ver = sfo.GetString("TARGET_APP_VER");
    
    LOG_INFO(Loader, "Game Metadata:");
    LOG_INFO(Loader, "  TITLE_ID:       {}", out_meta.title_id.c_str());
    LOG_INFO(Loader, "  TITLE:          {}", out_meta.title.c_str());
    LOG_INFO(Loader, "  VERSION:        {}", out_meta.version.c_str());
    LOG_INFO(Loader, "  CATEGORY:       {}", out_meta.category.c_str());
    LOG_INFO(Loader, "  PARENTAL_LEVEL: {}", out_meta.parental_level.c_str());
    LOG_INFO(Loader, "  REGION:         {}", out_meta.region.c_str());
    LOG_INFO(Loader, "  RESOLUTION:     {}", out_meta.resolution.c_str());
    LOG_INFO(Loader, "  SOUND_FORMAT:   {}", out_meta.sound_format.c_str());
    LOG_INFO(Loader, "  APP_VER:        {}", out_meta.app_ver.c_str());
    LOG_INFO(Loader, "  SDK_VERSION:    {}", out_meta.sdk_version.c_str());
    LOG_INFO(Loader, "  CONTENT_ID:     {}", out_meta.content_id.c_str());
    LOG_INFO(Loader, "  TARGET_APP_VER: {}", out_meta.target_app_ver.c_str());
}

// -----------------------------------------------------------------------------
// Find main executable (eboot.bin, *.self, *.elf)
// -----------------------------------------------------------------------------
std::string FindMainExecutable(const std::string& game_path) {
    fs::path root(game_path);
    
    // Common PS5 executable locations and names
    std::vector<std::string> search_paths = {
        "eboot.bin",
        "EBOOT.BIN",
        "eboot.elf",
        "EBOOT.ELF",
        "ps5/eboot.bin",
        "PS5/EBOOT.BIN",
        "sce_sys/eboot.bin",
        "sce_sys/EBOOT.BIN",
    };
    
    // First check common paths
    for (const auto& rel_path : search_paths) {
        fs::path full = root / rel_path;
        if (fs::exists(full)) {
            LOG_INFO(Loader, "Found executable at: {}", full.string().c_str());
            return full.string();
        }
    }
    
    // Recursive search for .bin, .self, .elf files
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) continue;
        
        std::string ext = entry.path().extension().string();
        std::string filename = entry.path().filename().string();
        
        // Convert to lowercase for comparison
        std::string ext_lower = ext;
        std::transform(ext_lower.begin(), ext_lower.end(), ext_lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::string filename_lower = filename;
        std::transform(filename_lower.begin(), filename_lower.end(), filename_lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        
        if (ext_lower == ".bin" || ext_lower == ".self" || ext_lower == ".elf") {
            // Prefer eboot.bin or files in sce_sys
            if (filename_lower == "eboot.bin" || 
                entry.path().string().find("sce_sys") != std::string::npos) {
                LOG_INFO(Loader, "Found executable (preferred): {}", entry.path().string().c_str());
                return entry.path().string();
            }
        }
    }
    
    // Fallback: any .bin/.self/.elf
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".bin" || ext == ".self" || ext == ".elf") {
            LOG_INFO(Loader, "Found executable (fallback): {}", entry.path().string().c_str());
            return entry.path().string();
        }
    }
    
    return "";
}

// -----------------------------------------------------------------------------
// Load and parse ELF/SELF module
// -----------------------------------------------------------------------------
bool LoadGameModule(const std::string& executable_path, GameMetadata& out_meta) {
    LOG_INFO(Loader, "Loading module: {}", executable_path.c_str());
    
    // Check if it's a SELF file
    if (Loader::IsSelfFile(executable_path)) {
        LOG_INFO(Loader, "Detected SELF container, using LoadSelf");
        if (Loader::LoadSelf(executable_path, out_meta.loaded_module)) {
            out_meta.module_loaded = true;
            out_meta.eboot_path = executable_path;
            
            // Parse module metadata
            Loader::ParseModuleMetadata(out_meta.loaded_module, out_meta.module_metadata);
            
            LOG_INFO(Loader, "SELF module loaded successfully");
            LOG_INFO(Loader, "  e_type: 0x{:04X} ({})", 
                     out_meta.loaded_module.e_type,
                     out_meta.loaded_module.is_pie ? "PIE (ET_DYN)" : "EXEC");
            LOG_INFO(Loader, "  Entry point: 0x{:016X}", out_meta.loaded_module.init_address);
            LOG_INFO(Loader, "  Segments: {}", out_meta.loaded_module.segments.size());
            LOG_INFO(Loader, "  Imports: {}", out_meta.module_metadata.imports.size());
            LOG_INFO(Loader, "  Exports: {}", out_meta.module_metadata.exports.size());
            LOG_INFO(Loader, "  TLS: {}", out_meta.module_metadata.has_tls ? "yes" : "no");
            
            return true;
        } else {
            LOG_ERROR(Loader, "Failed to load SELF module");
            return false;
        }
    } else {
        // Try as regular ELF
        LOG_INFO(Loader, "Attempting to load as ELF");
        if (Loader::Load(executable_path, out_meta.loaded_module)) {
            out_meta.module_loaded = true;
            out_meta.eboot_path = executable_path;
            
            // Parse module metadata
            Loader::ParseModuleMetadata(out_meta.loaded_module, out_meta.module_metadata);
            
            LOG_INFO(Loader, "ELF module loaded successfully");
            LOG_INFO(Loader, "  e_type: 0x{:04X} ({})", 
                     out_meta.loaded_module.e_type,
                     out_meta.loaded_module.is_pie ? "PIE (ET_DYN)" : "EXEC");
            LOG_INFO(Loader, "  Entry point: 0x{:016X}", out_meta.loaded_module.init_address);
            LOG_INFO(Loader, "  Segments: {}", out_meta.loaded_module.segments.size());
            LOG_INFO(Loader, "  Imports: {}", out_meta.module_metadata.imports.size());
            LOG_INFO(Loader, "  Exports: {}", out_meta.module_metadata.exports.size());
            LOG_INFO(Loader, "  TLS: {}", out_meta.module_metadata.has_tls ? "yes" : "no");
            
            return true;
        } else {
            LOG_ERROR(Loader, "Failed to load ELF module");
            return false;
        }
    }
}

// -----------------------------------------------------------------------------
// Print memory layout
// -----------------------------------------------------------------------------
void PrintMemoryLayout(const GameMetadata& meta) {
    if (!meta.module_loaded) {
        LOG_WARN(Loader, "No module loaded, cannot print memory layout");
        return;
    }
    
    const auto& module = meta.loaded_module;
    
    std::cout << "\n=== Memory Layout ===\n";
    std::cout << "Module: " << meta.eboot_path << "\n";
    std::cout << "Type: " << (module.is_pie ? "PIE (ET_DYN)" : "EXEC (ET_EXEC)") << "\n";
    std::cout << "Entry Point: 0x" << std::hex << module.init_address << std::dec << "\n";
    std::cout << "Base Address: 0x" << std::hex << (module.is_pie ? 0x800000000ULL : 0) << std::dec << "\n";
    std::cout << "\nSegments:\n";
    std::cout << "  Idx  Type           File Off    File Size   Mem Size    VAddr       Flags\n";
    std::cout << "  ---- -------------- ----------- ----------- ----------- ----------- ------\n";
    
    for (size_t i = 0; i < module.segments.size(); ++i) {
        const auto& seg = module.segments[i];
        std::string type_str;
        switch (seg.type) {
            case Loader::PT_LOAD: type_str = "PT_LOAD"; break;
            case Loader::PT_DYNAMIC: type_str = "PT_DYNAMIC"; break;
            case Loader::PT_TLS: type_str = "PT_TLS"; break;
            case Loader::PT_SCE_PROC_PARAM: type_str = "PT_SCE_PROC_PARAM"; break;
            case Loader::PT_SCE_MODULE_PARAM: type_str = "PT_SCE_MODULE_PARAM"; break;
            default: type_str = "0x" + std::to_string(seg.type); break;
        }
        
        std::cout << "  " << std::setw(4) << i << " "
                  << std::setw(14) << type_str << " "
                  << "0x" << std::setw(9) << std::hex << seg.file_offset << std::dec << " "
                  << "0x" << std::setw(9) << std::hex << seg.file_size << std::dec << " "
                  << "0x" << std::setw(9) << std::hex << seg.mem_size << std::dec << " "
                  << "0x" << std::setw(9) << std::hex << seg.vaddr << std::dec << " "
                  << "0x" << std::hex << seg.flags << std::dec << "\n";
    }
    
    // Print TLS info if present
    if (module.has_tls) {
        std::cout << "\nTLS Template:\n";
        std::cout << "  File Size: 0x" << std::hex << module.tls_file_size << std::dec << "\n";
        std::cout << "  Mem Size:  0x" << std::hex << module.tls_mem_size << std::dec << "\n";
        std::cout << "  Align:     0x" << std::hex << module.tls_align << std::dec << "\n";
        std::cout << "  Offset:    0x" << std::hex << module.tls_template_offset << std::dec << "\n";
    }
    
    // Print imports
    if (!meta.module_metadata.imports.empty()) {
        std::cout << "\nImports (" << meta.module_metadata.imports.size() << "):\n";
        for (const auto& imp : meta.module_metadata.imports) {
            std::cout << "  " << imp.name;
            if (imp.is_weak) std::cout << " [WEAK]";
            if (imp.is_tls) std::cout << " [TLS]";
            std::cout << " (refs: " << imp.rela_refs << " RELA, " << imp.plt_refs << " PLT)\n";
        }
    }
    
    // Print exports
    if (!meta.module_metadata.exports.empty()) {
        std::cout << "\nExports (" << meta.module_metadata.exports.size() << "):\n";
        for (const auto& exp : meta.module_metadata.exports) {
            std::cout << "  " << exp.name << " @ 0x" << std::hex << exp.address << std::dec 
                      << " (size: 0x" << std::hex << exp.size << std::dec << ")\n";
        }
    }
    
    // Print dependencies
    if (!meta.module_metadata.dependencies.empty()) {
        std::cout << "\nDependencies (load order):\n";
        for (const auto& dep : meta.module_metadata.dependencies) {
            std::cout << "  " << dep << "\n";
        }
    }
}

} // namespace BootParser

int main(int argc, char* argv[]) {
    // Initialize logging
    LogConfig::SetLevel(LogCategory::Loader, LogLevel::Debug);
    LogConfig::SetLevel(LogCategory::General, LogLevel::Info);
    
    std::cout << "PCSX5 Boot Parser v0.2\n";
    std::cout << "Usage: pcsx5_boot_parser [game_path]\n\n";
    
    if (argc < 2) {
        std::cerr << "Error: No game path provided\n";
        return 1;
    }
    
    std::string gamePath = argv[1];
    std::cout << "Parsing game at: " << gamePath << "\n\n";
    
    // Check if path exists
    if (!fs::exists(gamePath)) {
        std::cerr << "Error: Game path does not exist: " << gamePath << "\n";
        return 1;
    }
    
    BootParser::GameMetadata meta;
    
    // 1. Parse param.sfo
    fs::path sfo_path = fs::path(gamePath) / "sce_sys" / "param.sfo";
    if (!fs::exists(sfo_path)) {
        // Try alternative locations
        std::vector<fs::path> alt_paths = {
            fs::path(gamePath) / "param.sfo",
            fs::path(gamePath) / "PARAM.SFO",
            fs::path(gamePath) / "sce_sys" / "PARAM.SFO",
        };
        
        for (const auto& alt : alt_paths) {
            if (fs::exists(alt)) {
                sfo_path = alt;
                break;
            }
        }
    }
    
    if (fs::exists(sfo_path)) {
        std::cout << "Found param.sfo: " << sfo_path << "\n";
        BootParser::SfoData sfo_data;
        if (BootParser::ParseParamSfo(sfo_path.string(), sfo_data)) {
            BootParser::ExtractMetadataFromSfo(sfo_data, meta);
        } else {
            std::cerr << "Warning: Failed to parse param.sfo\n";
        }
    } else {
        std::cerr << "Warning: param.sfo not found\n";
    }
    
    // 2. Find and load main executable
    std::string executable_path = BootParser::FindMainExecutable(gamePath);
    if (!executable_path.empty()) {
        BootParser::LoadGameModule(executable_path, meta);
    } else {
        std::cerr << "Warning: No executable found (eboot.bin, *.self, *.elf)\n";
    }
    
    // 3. Print memory layout
    BootParser::PrintMemoryLayout(meta);
    
    // 4. Summary
    std::cout << "\n=== Summary ===\n";
    std::cout << "Title ID:       " << (meta.title_id.empty() ? "(unknown)" : meta.title_id) << "\n";
    std::cout << "Title:          " << (meta.title.empty() ? "(unknown)" : meta.title) << "\n";
    std::cout << "Version:        " << (meta.version.empty() ? "(unknown)" : meta.version) << "\n";
    std::cout << "Category:       " << (meta.category.empty() ? "(unknown)" : meta.category) << "\n";
    std::cout << "Executable:     " << (meta.eboot_path.empty() ? "(not found)" : meta.eboot_path) << "\n";
    std::cout << "Module Loaded:  " << (meta.module_loaded ? "Yes" : "No") << "\n";
    
    return 0;
}