// shader_dump — offline RDNA2 shader disassembly / corpus analysis tool
// (Phase 5 M2, slice 1).  Not a ctest target.
//
// Usage:
//   shader_dump <shader-file> [...]      decode + print listing & histogram
//   shader_dump --corpus <dir> <report>  decode every file in <dir> and write
//                                        a corpus report (per-file counts,
//                                        opcode histogram, translation
//                                        worklist ranked by frequency).

#include "gpu/shader/gcn_decode.h"
#include "gpu/shader/gcn_translate.h"
#include "gpu/shader/metadata.h"

#include <vulkan/vulkan.h>

#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace GPU::Shader;

struct FileResult {
    std::string name;
    std::string stage;
    bool        decoded = false;
    std::string error;
    size_t      instruction_count = 0;
    size_t      unknown_count     = 0;
    u8          shader_type       = 0xFF;
    GcnProgram  program;
};

bool LoadFileBytes(const std::filesystem::path& path, std::vector<u8>& out) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        return false;
    }
    const std::streamsize size = stream.tellg();
    if (size <= 0) {
        return false;
    }
    out.resize(static_cast<size_t>(size));
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(out.data()), size);
    return stream.good();
}

FileResult ProcessFile(const std::filesystem::path& path) {
    FileResult result;
    result.name = path.filename().string();

    std::vector<u8> bytes;
    if (!LoadFileBytes(path, bytes)) {
        result.error = "file-unreadable";
        return result;
    }

    AgcShaderFile shader;
    std::string   error;
    if (!AgcLoadShaderFile(bytes, shader, error)) {
        result.error = "container:" + error;
        return result;
    }
    result.stage = AgcShaderTypeName(shader.parsed_header.shader_type);
    result.shader_type = shader.parsed_header.shader_type;

    if (shader.code.size() % sizeof(u32) != 0) {
        result.error = "code-not-dword-aligned";
        return result;
    }
    if (!GcnDecodeProgram(
            reinterpret_cast<const u32*>(shader.code.data()),
            shader.code.size() / sizeof(u32), result.program, error)) {
        result.error = "decode:" + error;
        return result;
    }

    result.decoded           = true;
    result.instruction_count = result.program.instructions.size();
    for (const GcnInstruction& instruction : result.program.instructions) {
        if (!instruction.opcode_known) {
            ++result.unknown_count;
        }
    }
    return result;
}

std::string FormatOperandList(const std::vector<GcnOperand>& operands) {
    std::string text;
    for (size_t i = 0; i < operands.size(); ++i) {
        if (i != 0) {
            text += ", ";
        }
        text += operands[i].ToString();
    }
    return text;
}

void PrintListing(const FileResult& result) {
    std::printf("== %s (%s) — %zu instructions, %zu unknown ==\n",
                result.name.c_str(), result.stage.c_str(),
                result.instruction_count, result.unknown_count);
    for (const GcnInstruction& ins : result.program.instructions) {
        std::string line;
        char        pc[16];
        std::snprintf(pc, sizeof(pc), "%04X: ", ins.pc);
        line = pc;
        line += ins.opcode;
        if (!ins.destinations.empty()) {
            line += " " + FormatOperandList(ins.destinations);
            if (!ins.sources.empty()) {
                line += ",";
            }
        }
        if (!ins.sources.empty()) {
            line += " " + FormatOperandList(ins.sources);
        }
        line += "   ; " + std::string(GcnEncodingName(ins.encoding));
        for (u32 word : ins.words) {
            char hex[16];
            std::snprintf(hex, sizeof(hex), " %08X", word);
            line += hex;
        }
        std::printf("%s\n", line.c_str());
    }
}

struct OpcodeStat {
    u64        count = 0;
    u64        files = 0;
    std::string category;
};

int RunCorpus(const std::filesystem::path& directory,
              const std::filesystem::path& report_path) {
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.is_regular_file()) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        std::fprintf(stderr, "shader_dump: no files in %s\n",
                     directory.string().c_str());
        return 1;
    }

    std::vector<FileResult> results;
    results.reserve(files.size());
    std::map<std::string, OpcodeStat> histogram;
    std::map<std::string, u64>        category_histogram;
    std::map<std::string, u64>        unknown_opcodes;
    size_t decoded_count   = 0;
    size_t total_unknown   = 0;
    u64    total_instructions = 0;

    for (const auto& path : files) {
        FileResult result = ProcessFile(path);
        if (result.decoded) {
            ++decoded_count;
            total_instructions += result.instruction_count;
            total_unknown += result.unknown_count;
            std::set<std::string> seen_in_file;
            for (const GcnInstruction& ins : result.program.instructions) {
                OpcodeStat& stat = histogram[ins.opcode];
                ++stat.count;
                stat.category = GcnClassifyInstruction(ins.opcode);
                seen_in_file.insert(ins.opcode);
                ++category_histogram[stat.category];
                if (!ins.opcode_known) {
                    ++unknown_opcodes[ins.opcode];
                }
            }
            for (const std::string& opcode : seen_in_file) {
                ++histogram[opcode].files;
            }
        } else {
            std::fprintf(stderr, "shader_dump: %s: %s\n",
                         result.name.c_str(), result.error.c_str());
        }
        results.push_back(std::move(result));
    }

    // Sort histogram by frequency (ties alphabetical) — this doubles as the
    // translation worklist for slice 2.
    std::vector<std::pair<std::string, OpcodeStat>> ranked(
        histogram.begin(), histogram.end());
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        if (a.second.count != b.second.count) {
            return a.second.count > b.second.count;
        }
        return a.first < b.first;
    });

    std::ofstream report(report_path, std::ios::binary | std::ios::trunc);
    if (!report) {
        std::fprintf(stderr, "shader_dump: cannot write %s\n",
                     report_path.string().c_str());
        return 1;
    }

    report << "pcsx5 shader corpus report (Phase 5 M2 slice 1)\n";
    report << "corpus: " << directory.string() << "\n";
    report << "files: " << results.size() << "  decoded: " << decoded_count
           << "  failed: " << (results.size() - decoded_count) << "\n";
    report << "total instructions: " << total_instructions
           << "  distinct opcodes: " << histogram.size()
           << "  unknown-table opcodes: " << unknown_opcodes.size()
           << " (" << total_unknown << " instances)\n\n";

    report << "== per-file results ==\n";
    for (const FileResult& result : results) {
        if (result.decoded) {
            char line[256];
            std::snprintf(line, sizeof(line),
                          "%-40s %-8s ins=%-5zu unknown=%zu\n",
                          result.name.c_str(), result.stage.c_str(),
                          result.instruction_count, result.unknown_count);
            report << line;
        } else {
            report << result.name << "  DECODE-FAILURE: " << result.error
                   << "\n";
        }
    }

    report << "\n== category histogram ==\n";
    for (const auto& [category, count] : category_histogram) {
        report << category << ": " << count << "\n";
    }

    report << "\n== opcode histogram (ranked by frequency; translation "
              "worklist for slice 2) ==\n";
    int rank = 0;
    for (const auto& [opcode, stat] : ranked) {
        char line[256];
        std::snprintf(line, sizeof(line), "%4d. %-28s count=%-6llu files=%-4zu"
                      " [%s]\n",
                      ++rank, opcode.c_str(),
                      static_cast<unsigned long long>(stat.count),
                      static_cast<size_t>(stat.files), stat.category.c_str());
        report << line;
    }

    report << "\n== opcodes missing from decode tables ==\n";
    if (unknown_opcodes.empty()) {
        report << "(none — every decoded opcode has a table entry)\n";
    } else {
        for (const auto& [opcode, count] : unknown_opcodes) {
            report << opcode << ": " << count << "\n";
        }
    }

    std::printf("shader_dump: %zu/%zu files decoded, %llu instructions, "
                "%zu distinct opcodes, %zu unknown-table opcodes\n",
                decoded_count, results.size(),
                static_cast<unsigned long long>(total_instructions),
                histogram.size(), unknown_opcodes.size());
    return results.size() == decoded_count ? 0 : 2;
}

// Builds the synthetic binding set a standalone corpus translation needs:
// images keyed by instruction pc, global buffers grouped by their scalar
// base register.  Runtime bindings arrive with M3; these keep every memory
// instruction mapped so the corpus exercises the full emission surface.
GcnTranslateOptions BuildCorpusOptions(const FileResult& result) {
    GcnTranslateOptions options;
    options.stage = result.shader_type == AgcShaderType::Pixel
        ? GcnSpirvStage::Pixel
        : GcnSpirvStage::Vertex;

    std::set<u32> export_targets;
    for (const GcnInstruction& ins : result.program.instructions) {
        if (const auto* export_ = std::get_if<GcnExportControl>(&ins.control)) {
            export_targets.insert(export_->target);
        }
    }

    if (options.stage == GcnSpirvStage::Pixel) {
        bool has_color = false;
        for (const u32 target : export_targets) {
            if (target < 8) {
                has_color = true;
                options.pixel_outputs.push_back(
                    GcnPixelOutputBinding{target, target, GcnPixelOutputKind::Float});
            }
        }
        if (!has_color) {
            options.pixel_outputs.push_back(
                GcnPixelOutputBinding{0, 0, GcnPixelOutputKind::Float});
        }
        // Standard GameMaker PS input model: position at v0/v1, attributes
        // after.  These only steer the VGPR preload; interp lowers directly.
        options.pixel_input_enable  = 0x300;
        options.pixel_input_address = 0x300;
    } else {
        u32 max_param = 0;
        for (const u32 target : export_targets) {
            if (target >= 32 && target < 64) {
                max_param = std::max(max_param, target - 32 + 1);
            }
        }
        options.required_vertex_output_count = static_cast<int>(max_param);
    }

    // One sampled-2D-float image binding per image instruction pc.
    for (const GcnInstruction& ins : result.program.instructions) {
        if (std::get_if<GcnImageControl>(&ins.control)) {
            GcnSpirvImageBinding binding;
            binding.pc = ins.pc;
            options.image_bindings.push_back(binding);
        }
    }

    // Global buffers grouped by scalar base register: scalar loads use
    // sources[0]; buffer loads use the control's scalar_resource.
    std::map<u32, std::vector<u32>> pcs_by_base;
    for (const GcnInstruction& ins : result.program.instructions) {
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

int RunTranslateCorpus(const std::filesystem::path& directory,
                       const std::filesystem::path& out_directory) {
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.is_regular_file()) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        std::fprintf(stderr, "shader_dump: no files in %s\n",
                     directory.string().c_str());
        return 1;
    }
    std::filesystem::create_directories(out_directory);

    size_t translated = 0;
    size_t failed     = 0;
    for (const auto& path : files) {
        FileResult result = ProcessFile(path);
        if (!result.decoded) {
            std::fprintf(stderr, "shader_dump: %s: %s\n",
                         result.name.c_str(), result.error.c_str());
            ++failed;
            continue;
        }

        const GcnTranslateOptions options = BuildCorpusOptions(result);
        GcnSpirvShader shader;
        std::string    error;
        if (!GcnTranslateToSpirv(result.program, options, shader, error)) {
            std::fprintf(stderr, "shader_dump: %s: translate: %s\n",
                         result.name.c_str(), error.c_str());
            ++failed;
            continue;
        }
        if (shader.words.size() < 5 ||
            shader.words[0] != 0x07230203u) {
            std::fprintf(stderr, "shader_dump: %s: bad SPIR-V magic\n",
                         result.name.c_str());
            ++failed;
            continue;
        }

        const std::filesystem::path out_path =
            out_directory / (result.name + ".spv");
        std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            std::fprintf(stderr, "shader_dump: cannot write %s\n",
                         out_path.string().c_str());
            ++failed;
            continue;
        }
        out.write(reinterpret_cast<const char*>(shader.words.data()),
                  static_cast<std::streamsize>(shader.words.size() * sizeof(u32)));
        ++translated;
    }

    std::printf("shader_dump: translated %zu/%zu shaders -> %s\n",
                translated, files.size(), out_directory.string().c_str());
    return failed == 0 ? 0 : 2;
}

// ---------------------------------------------------------------------------
// --validate-spv: driver-level validation of translated modules.  Loads
// vulkan-1.dll dynamically (no SDK runtime assumption), creates a headless
// device, and runs vkCreateShaderModule over every .spv in a directory —
// the ICD's own SPIR-V validator is the strictest checker we have.
// ---------------------------------------------------------------------------
int RunValidateSpv(const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.is_regular_file() && entry.path().extension() == ".spv") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        std::fprintf(stderr, "shader_dump: no .spv files in %s\n",
                     directory.string().c_str());
        return 1;
    }

    HMODULE dll = LoadLibraryA("vulkan-1.dll");
    if (!dll) {
        std::fprintf(stderr, "shader_dump: vulkan-1.dll not available\n");
        return 1;
    }
    const auto get_instance_proc =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            GetProcAddress(dll, "vkGetInstanceProcAddr"));
    if (!get_instance_proc) {
        std::fprintf(stderr, "shader_dump: vkGetInstanceProcAddr missing\n");
        return 1;
    }
    const auto create_instance =
        reinterpret_cast<PFN_vkCreateInstance>(
            get_instance_proc(nullptr, "vkCreateInstance"));
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "pcsx5-shader-validate";
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo instance_ci{};
    instance_ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_ci.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (create_instance(&instance_ci, nullptr, &instance) != VK_SUCCESS) {
        std::fprintf(stderr, "shader_dump: vkCreateInstance failed\n");
        return 1;
    }

    const auto enumerate_devices =
        reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
            get_instance_proc(instance, "vkEnumeratePhysicalDevices"));
    const auto create_device =
        reinterpret_cast<PFN_vkCreateDevice>(
            get_instance_proc(instance, "vkCreateDevice"));
    const auto get_device_proc =
        reinterpret_cast<PFN_vkGetDeviceProcAddr>(
            get_instance_proc(instance, "vkGetDeviceProcAddr"));
    u32 device_count = 0;
    std::vector<VkPhysicalDevice> devices;
    if (enumerate_devices(instance, &device_count, nullptr) == VK_SUCCESS &&
        device_count > 0) {
        devices.resize(device_count);
        enumerate_devices(instance, &device_count, devices.data());
    }
    if (devices.empty()) {
        std::fprintf(stderr, "shader_dump: no Vulkan devices\n");
        return 1;
    }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_ci{};
    queue_ci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_ci.queueFamilyIndex = 0;
    queue_ci.queueCount = 1;
    queue_ci.pQueuePriorities = &priority;
    VkDeviceCreateInfo device_ci{};
    device_ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_ci.queueCreateInfoCount = 1;
    device_ci.pQueueCreateInfos = &queue_ci;
    VkDevice device = VK_NULL_HANDLE;
    if (create_device(devices[0], &device_ci, nullptr, &device) != VK_SUCCESS) {
        std::fprintf(stderr, "shader_dump: vkCreateDevice failed\n");
        return 1;
    }
    const auto create_shader_module =
        reinterpret_cast<PFN_vkCreateShaderModule>(
            get_device_proc(device, "vkCreateShaderModule"));
    const auto destroy_shader_module =
        reinterpret_cast<PFN_vkDestroyShaderModule>(
            get_device_proc(device, "vkDestroyShaderModule"));
    const auto destroy_device =
        reinterpret_cast<PFN_vkDestroyDevice>(
            get_device_proc(device, "vkDestroyDevice"));
    const auto destroy_instance =
        reinterpret_cast<PFN_vkDestroyInstance>(
            get_instance_proc(instance, "vkDestroyInstance"));

    size_t valid = 0;
    size_t invalid = 0;
    for (const auto& path : files) {
        std::vector<u8> bytes;
        if (!LoadFileBytes(path, bytes) || bytes.size() % sizeof(u32) != 0) {
            std::fprintf(stderr, "shader_dump: %s: unreadable/unaligned\n",
                         path.filename().string().c_str());
            ++invalid;
            continue;
        }
        VkShaderModuleCreateInfo module_ci{};
        module_ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        module_ci.codeSize = bytes.size();
        module_ci.pCode = reinterpret_cast<const u32*>(bytes.data());
        VkShaderModule shader_module = VK_NULL_HANDLE;
        const VkResult result =
            create_shader_module(device, &module_ci, nullptr, &shader_module);
        if (result != VK_SUCCESS) {
            std::fprintf(stderr, "shader_dump: %s: vkCreateShaderModule -> %d\n",
                         path.filename().string().c_str(),
                         static_cast<int>(result));
            ++invalid;
            continue;
        }
        destroy_shader_module(device, shader_module, nullptr);
        ++valid;
    }

    destroy_device(device, nullptr);
    destroy_instance(instance, nullptr);
    std::printf("shader_dump: validated %zu/%zu SPIR-V modules (%s)\n",
                valid, files.size(),
                invalid == 0 ? "all accepted by the ICD" : "FAILURES above");
    return invalid == 0 ? 0 : 2;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 4 && std::string(argv[1]) == "--corpus") {
        return RunCorpus(argv[2], argv[3]);
    }
    if (argc >= 4 && std::string(argv[1]) == "--translate-corpus") {
        return RunTranslateCorpus(argv[2], argv[3]);
    }
    if (argc >= 3 && std::string(argv[1]) == "--validate-spv") {
        return RunValidateSpv(argv[2]);
    }
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: shader_dump <shader-file> [...]\n"
                     "       shader_dump --corpus <dir> <report.txt>\n"
                     "       shader_dump --translate-corpus <dir> <outdir>\n"
                     "       shader_dump --validate-spv <dir>\n");
        return 1;
    }

    int failures = 0;
    for (int i = 1; i < argc; ++i) {
        FileResult result = ProcessFile(argv[i]);
        if (!result.decoded) {
            std::fprintf(stderr, "shader_dump: %s: %s\n", argv[i],
                         result.error.c_str());
            ++failures;
            continue;
        }
        PrintListing(result);
        std::map<std::string, u64> histogram;
        for (const GcnInstruction& ins : result.program.instructions) {
            ++histogram[ins.opcode];
        }
        std::printf("-- opcode histogram --\n");
        for (const auto& [opcode, count] : histogram) {
            std::printf("%-28s %llu\n", opcode.c_str(),
                        static_cast<unsigned long long>(count));
        }
    }
    return failures == 0 ? 0 : 2;
}
