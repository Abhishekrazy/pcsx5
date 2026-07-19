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
#include "gpu/shader/metadata.h"

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

} // namespace

int main(int argc, char** argv) {
    if (argc >= 4 && std::string(argv[1]) == "--corpus") {
        return RunCorpus(argv[2], argv[3]);
    }
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: shader_dump <shader-file> [...]\n"
                     "       shader_dump --corpus <dir> <report.txt>\n");
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
