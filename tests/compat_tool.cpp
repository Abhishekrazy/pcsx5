// CLI tool for managing the per-title compatibility database.
//
// Subcommands:
//   list [--status STATUS]                 List every title
//   show <TITLE_ID>                        Print one entry
//   add    <TITLE_ID> --name "..."         Create a new entry
//               [--region R] [--version V] [--status STATUS]
//               [--notes "..."] [--workaround "..."] [...]
//   update  <TITLE_ID> [...]              Update curated fields of an entry
//   remove  <TITLE_ID>                     Delete an entry
//   search <QUERY>                         Free-text search
//   report [--out PATH]                    Write a markdown report
//   seed-elf <ELF_PATH>                    Create a stub entry from a real ELF
//
// Run with --root <DIR> to choose the compat directory (default: ./compat).
// Run with --help to see the usage line.

#include "compat/compat.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

void PrintUsage() {
    std::fprintf(stdout,
        "Usage: pcsx5_compat [--root DIR] <command> [args]\n"
        "\n"
        "Commands:\n"
        "  list [--status STATUS]                 List every entry (table)\n"
        "  show <TITLE_ID>                        Print one entry as JSON\n"
        "  add  <TITLE_ID> --name NAME            Create an entry\n"
        "       [--region R] [--version V]\n"
        "       [--status STATUS]\n"
        "       [--notes TEXT] [--workaround T]*\n"
        "  update <TITLE_ID> [--name N]           Update curated fields\n"
        "          [--region R] [--version V]\n"
        "          [--status STATUS] [--notes T]\n"
        "          [--workaround T]* [--add-workaround T] [--clear-workarounds]\n"
        "  remove <TITLE_ID>                      Delete an entry\n"
        "  search <QUERY>                         Free-text search\n"
        "  report [--out PATH]                    Markdown report (default: compat_report.md)\n"
        "  seed-elf <ELF_PATH> [--name NAME]      Create a stub from a real ELF header\n"
        "\n"
        "Status values: untested | intro | menu | playable | complete\n");
}

bool ArgFlag(const std::vector<std::string>& a, const std::string& name) {
    for (const auto& x : a) if (x == name) return true;
    return false;
}
std::string ArgValue(const std::vector<std::string>& a, const std::string& name,
                     const std::string& fallback = {}) {
    for (std::size_t i = 0; i + 1 < a.size(); ++i) {
        if (a[i] == name) return a[i + 1];
    }
    return fallback;
}
std::vector<std::string> ArgValues(const std::vector<std::string>& a,
                                   const std::string& name) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i + 1 < a.size(); ++i) {
        if (a[i] == name) out.push_back(a[i + 1]);
    }
    return out;
}

void PrintEntry(const Compat::Entry& e) {
    std::printf("title_id  : %s\n", e.title_id.c_str());
    std::printf("name      : %s\n", e.name.c_str());
    std::printf("region    : %s\n", e.region.c_str());
    std::printf("version   : %s\n", e.version.c_str());
    std::printf("status    : %s\n", Compat::StatusName(e.status));
    std::printf("notes     : %s\n", e.notes.c_str());
    std::printf("curated_at: %s\n", e.curated_at.c_str());
    std::printf("workarounds (%zu):\n", e.workarounds.size());
    for (const auto& w : e.workarounds) std::printf("  - %s\n", w.c_str());
    std::printf("auto:\n");
    std::printf("  last_tested                 : %s\n", e.auto_fields.last_tested.c_str());
    std::printf("  last_run_status             : %s\n", e.auto_fields.last_run_status.c_str());
    std::printf("  last_run_git_revision       : %s\n", e.auto_fields.last_run_git_revision.c_str());
    std::printf("  last_run_duration_ms        : %.2f\n", e.auto_fields.last_run_duration_ms);
    std::printf("  last_run_resolved_imports   : %llu\n",
                (unsigned long long)e.auto_fields.last_run_resolved_imports);
    std::printf("  last_run_unresolved_imports : %llu\n",
                (unsigned long long)e.auto_fields.last_run_unresolved_imports);
}

// Minimal raw JSON dump (no pretty printer in the library API surface).
void PrintEntryJson(const Compat::Entry& e) {
    auto esc = [](const std::string& s) {
        std::string out;
        out.reserve(s.size() + 2);
        out += '"';
        for (char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:   out += c;
            }
        }
        out += '"';
        return out;
    };
    std::printf("{\n");
    std::printf("  \"schema_version\": %d,\n", Compat::kCurrentSchemaVersion);
    std::printf("  \"title_id\": %s,\n", esc(e.title_id).c_str());
    std::printf("  \"name\": %s,\n",     esc(e.name).c_str());
    std::printf("  \"region\": %s,\n",   esc(e.region).c_str());
    std::printf("  \"version\": %s,\n",  esc(e.version).c_str());
    std::printf("  \"status\": %s,\n",   esc(Compat::StatusName(e.status)).c_str());
    std::printf("  \"notes\": %s,\n",    esc(e.notes).c_str());
    std::printf("  \"curated_at\": %s,\n", esc(e.curated_at).c_str());
    std::printf("  \"workarounds\": [");
    for (std::size_t i = 0; i < e.workarounds.size(); ++i) {
        if (i) std::printf(", ");
        std::printf("%s", esc(e.workarounds[i]).c_str());
    }
    std::printf("],\n");
    std::printf("  \"auto\": {\n");
    std::printf("    \"last_tested\": %s,\n", esc(e.auto_fields.last_tested).c_str());
    std::printf("    \"last_run_status\": %s,\n", esc(e.auto_fields.last_run_status).c_str());
    std::printf("    \"last_run_git_revision\": %s,\n", esc(e.auto_fields.last_run_git_revision).c_str());
    std::printf("    \"last_run_duration_ms\": %.2f,\n", e.auto_fields.last_run_duration_ms);
    std::printf("    \"last_run_resolved_imports\": %llu,\n",
                (unsigned long long)e.auto_fields.last_run_resolved_imports);
    std::printf("    \"last_run_unresolved_imports\": %llu\n",
                (unsigned long long)e.auto_fields.last_run_unresolved_imports);
    std::printf("  }\n");
    std::printf("}\n");
}

int CmdList(const std::vector<std::string>& args) {
    const std::string status = ArgValue(args, "--status");
    auto titles = Compat::ListTitles();
    if (titles.empty()) {
        std::printf("(no entries — use `pcsx5_compat add <TITLE_ID>`)\n");
        return 0;
    }
    std::printf("%-12s %-40s %-8s %-10s %-10s %s\n",
                "TITLE", "NAME", "REGION", "VERSION", "STATUS", "LAST TESTED");
    std::printf("%-12s %-40s %-8s %-10s %-10s %s\n",
                "-----", "----", "------", "-------", "------", "-----------");
    for (const auto& t : titles) {
        Compat::Entry e;
        if (!Compat::Load(t, e, nullptr)) continue;
        if (!status.empty() && Compat::StatusName(e.status) != status) continue;
        std::printf("%-12s %-40.40s %-8.8s %-10.10s %-10.10s %s\n",
                    e.title_id.c_str(),
                    e.name.c_str(),
                    e.region.c_str(),
                    e.version.c_str(),
                    Compat::StatusName(e.status),
                    e.auto_fields.last_tested.c_str());
    }
    return 0;
}

int CmdShow(const std::vector<std::string>& args) {
    if (args.empty()) { std::fprintf(stderr, "show: TITLE_ID required\n"); return 2; }
    Compat::Entry e;
    if (!Compat::Load(args[0], e, nullptr)) {
        std::fprintf(stderr, "show: '%s' not found\n", args[0].c_str());
        return 1;
    }
    if (ArgFlag(args, "--json")) PrintEntryJson(e);
    else                          PrintEntry(e);
    return 0;
}

bool ApplyEditableFields(Compat::Entry& e, const std::vector<std::string>& args) {
    if (auto v = ArgValue(args, "--name");    !v.empty()) e.name    = v;
    if (auto v = ArgValue(args, "--region");  !v.empty()) e.region  = v;
    if (auto v = ArgValue(args, "--version"); !v.empty()) e.version = v;
    if (auto v = ArgValue(args, "--notes");   !v.empty()) e.notes   = v;
    if (auto v = ArgValue(args, "--status");  !v.empty()) {
        Compat::Status s;
        if (!Compat::StatusFromName(v, s)) {
            std::fprintf(stderr, "update: unknown status '%s'\n", v.c_str());
            return false;
        }
        e.status = s;
    }
    auto wa = ArgValues(args, "--workaround");
    if (!wa.empty()) e.workarounds = wa;
    auto add_wa = ArgValues(args, "--add-workaround");
    for (const auto& a : add_wa) e.workarounds.push_back(a);
    if (ArgFlag(args, "--clear-workarounds")) e.workarounds.clear();
    return true;
}

int CmdAdd(const std::vector<std::string>& args) {
    if (args.empty()) { std::fprintf(stderr, "add: TITLE_ID required\n"); return 2; }
    Compat::Entry e;
    e.title_id = args[0];
    if (auto v = ArgValue(args, "--name"); v.empty()) {
        std::fprintf(stderr, "add: --name is required\n");
        return 2;
    } else {
        e.name = v;
    }
    if (!ApplyEditableFields(e, args)) return 2;
    if (Compat::Find(e.title_id) != nullptr) {
        std::fprintf(stderr, "add: '%s' already exists (use `update`)\n",
                     e.title_id.c_str());
        return 1;
    }
    std::string err;
    if (!Compat::Save(std::move(e), &err)) {
        std::fprintf(stderr, "add: %s\n", err.c_str());
        return 1;
    }
    std::printf("added %s\n", args[0].c_str());
    return 0;
}

int CmdUpdate(const std::vector<std::string>& args) {
    if (args.empty()) { std::fprintf(stderr, "update: TITLE_ID required\n"); return 2; }
    Compat::Entry e;
    if (!Compat::Load(args[0], e, nullptr)) {
        std::fprintf(stderr, "update: '%s' not found\n", args[0].c_str());
        return 1;
    }
    if (!ApplyEditableFields(e, args)) return 2;
    std::string err;
    if (!Compat::Save(std::move(e), &err)) {
        std::fprintf(stderr, "update: %s\n", err.c_str());
        return 1;
    }
    std::printf("updated %s\n", args[0].c_str());
    return 0;
}

int CmdRemove(const std::vector<std::string>& args) {
    if (args.empty()) { std::fprintf(stderr, "remove: TITLE_ID required\n"); return 2; }
    std::string err;
    if (!Compat::Remove(args[0], &err)) {
        std::fprintf(stderr, "remove: %s\n", err.c_str());
        return 1;
    }
    std::printf("removed %s\n", args[0].c_str());
    return 0;
}

int CmdSearch(const std::vector<std::string>& args) {
    const std::string q = args.empty() ? std::string() : args[0];
    auto hits = Compat::Search(q);
    if (hits.empty()) {
        std::printf("(no matches for '%s')\n", q.c_str());
        return 0;
    }
    for (const auto& e : hits) {
        std::printf("%-12s %-10s %s\n",
                    e.title_id.c_str(),
                    Compat::StatusName(e.status),
                    e.name.c_str());
    }
    return 0;
}

int CmdReport(const std::vector<std::string>& args) {
    const std::string out = ArgValue(args, "--out", "compat_report.md");
    std::string err;
    if (!Compat::WriteMarkdownReport(out, &err)) {
        std::fprintf(stderr, "report: %s\n", err.c_str());
        return 1;
    }
    std::printf("wrote %s\n", out.c_str());
    return 0;
}

int CmdSeedElf(const std::vector<std::string>& args) {
    if (args.empty()) { std::fprintf(stderr, "seed-elf: ELF_PATH required\n"); return 2; }
    const std::string elf_path = args[0];
    // Read first 16 bytes (e_ident) and the e_type field.  Heuristic to pull
    // a title_id out of the path if it lives under Games/PPSA02929-app0/.
#ifdef _WIN32
    std::FILE* f = nullptr;
    if (fopen_s(&f, elf_path.c_str(), "rb") != 0 || f == nullptr) {
        std::fprintf(stderr, "seed-elf: cannot open '%s'\n", elf_path.c_str());
        return 1;
    }
#else
    std::FILE* f = std::fopen(elf_path.c_str(), "rb");
    if (!f) {
        std::fprintf(stderr, "seed-elf: cannot open '%s'\n", elf_path.c_str());
        return 1;
    }
#endif
    unsigned char hdr[20] = {};
    std::fread(hdr, 1, sizeof(hdr), f);
    std::fclose(f);

    // e_ident[EI_NIDENT]; e_type lives at offset 16 (2 bytes).
    const std::string title_id = ArgValue(args, "--title-id");
    std::string detected_id = title_id;
    if (detected_id.empty()) {
        // Try to extract the title id from a path like
        //   Games/PPSA02929-app0/eboot.bin
        //   Games/PPSA02929-patch0/eboot.bin
        // Conventionally the *parent* of the ELF is the patch/app directory,
        // and the leaf before "-appN"/"-patchN" is the canonical title id.
        const std::string norm = std::filesystem::path(elf_path).generic_string();
        const auto slash = norm.find_last_of('/');
        const std::string parent = (slash == std::string::npos) ? norm : norm.substr(0, slash);
        const auto p_slash = parent.find_last_of('/');
        const std::string leaf = (p_slash == std::string::npos) ? parent : parent.substr(p_slash + 1);
        detected_id = leaf;
        // Strip trailing "-app<N>" or "-patch<N>".
        for (const char* suffix : {"-app", "-patch"}) {
            const auto pos = detected_id.find(suffix);
            if (pos != std::string::npos) {
                detected_id = detected_id.substr(0, pos);
                break;
            }
        }
    }
    if (detected_id.empty()) detected_id = "UNTITLED";

    Compat::Entry e;
    e.title_id = detected_id;
    // Name: prefer an explicit --name, then the human-readable title
    // from the game's own `sce_sys/param.json` (same logic the UI
    // uses), then the "(unnamed)" placeholder.  This prevents seeded
    // entries from silently overwriting the real game name with the
    // title id when the UI later loads them.
    std::string name = ArgValue(args, "--name");
    if (name.empty() || name == "(unnamed)") {
        // Look for sce_sys/param.json in the parent of the ELF.
        const std::filesystem::path elf_p(elf_path);
        std::filesystem::path game_dir = elf_p.parent_path();
        // If the ELF lives in a -appN/-patchN subdir, walk up one.
        const std::string gd = game_dir.filename().generic_string();
        if (gd.find("-app") != std::string::npos ||
            gd.find("-patch") != std::string::npos) {
            game_dir = game_dir.parent_path();
        }
        const std::filesystem::path param = game_dir / "sce_sys" / "param.json";
        std::error_code ec;
        if (std::filesystem::exists(param, ec)) {
            std::FILE* pf = nullptr;
#ifdef _WIN32
            if (fopen_s(&pf, param.generic_string().c_str(), "rb") != 0) {
                pf = nullptr;
            }
#else
            pf = std::fopen(param.generic_string().c_str(), "rb");
#endif
            if (pf) {
                std::string text;
                char chunk[4096];
                size_t n = 0;
                while ((n = std::fread(chunk, 1, sizeof(chunk), pf)) > 0) {
                    text.append(chunk, n);
                }
                std::fclose(pf);
                // Heuristic: find "defaultLanguage" then "<lang>" block
                // then "titleName".  Mirrors ui_scan.cpp:ReadTitleFromParamJson
                // so seeded entries stay in sync with the UI.
                auto find_quoted_after = [&](const std::string& key,
                                             const std::string& src,
                                             std::size_t from = 0)
                    -> std::string {
                    const std::string needle = "\"" + key + "\"";
                    const auto kp = src.find(needle, from);
                    if (kp == std::string::npos) return {};
                    const auto cl = src.find(':', kp + needle.size());
                    if (cl == std::string::npos) return {};
                    const auto q1 = src.find('"', cl + 1);
                    if (q1 == std::string::npos) return {};
                    const auto q2 = src.find('"', q1 + 1);
                    if (q2 == std::string::npos) return {};
                    return src.substr(q1 + 1, q2 - q1 - 1);
                };
                const std::string lang = find_quoted_after("defaultLanguage", text);
                std::string found;
                if (!lang.empty()) {
                    const std::string lang_key = "\"" + lang + "\"";
                    const auto lpos = text.find(lang_key);
                    if (lpos != std::string::npos) {
                        const auto brace = text.find('{', lpos + lang_key.size());
                        if (brace != std::string::npos) {
                            int depth = 0;
                            std::size_t end = brace;
                            for (; end < text.size(); ++end) {
                                if (text[end] == '{') ++depth;
                                else if (text[end] == '}') {
                                    --depth; if (depth == 0) break;
                                }
                            }
                            if (end < text.size()) {
                                found = find_quoted_after("titleName",
                                    text.substr(brace, end - brace));
                            }
                        }
                    }
                }
                if (found.empty()) {
                    found = find_quoted_after("titleName", text);
                }
                if (!found.empty()) name = found;
            }
        }
    }
    e.name     = !name.empty() ? name : "(unnamed)";
    e.notes    = "Seeded from " + elf_path;
    e.status   = Compat::Status::Untested;

    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%04X",
                  (unsigned)(hdr[16] | (hdr[17] << 8)));
    e.notes += " (e_type=" + std::string(buf) + ")";

    std::string err;
    if (Compat::Find(e.title_id) != nullptr) {
        std::fprintf(stderr, "seed-elf: '%s' already exists\n", e.title_id.c_str());
        return 1;
    }
    if (!Compat::Save(std::move(e), &err)) {
        std::fprintf(stderr, "seed-elf: %s\n", err.c_str());
        return 1;
    }
    std::printf("seeded %s -> %s\n", e.title_id.c_str(),
                Compat::Directory().c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> argv_;
    argv_.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) argv_.emplace_back(argv[i]);

    // Pull --root out of the front, if present.
    std::string root = "./compat";
    for (std::size_t i = 1; i + 1 < argv_.size(); ++i) {
        if (argv_[i] == "--root") { root = argv_[i + 1]; argv_.erase(argv_.begin() + i, argv_.begin() + i + 2); break; }
    }
    Compat::Initialize(root);

    if (argv_.size() < 2 || argv_[1] == "-h" || argv_[1] == "--help") {
        PrintUsage();
        return argv_.size() < 2 ? 1 : 0;
    }

    const std::string cmd = argv_[1];
    std::vector<std::string> args(argv_.begin() + 2, argv_.end());

    if (cmd == "list")         return CmdList(args);
    if (cmd == "show")         return CmdShow(args);
    if (cmd == "add")          return CmdAdd(args);
    if (cmd == "update")       return CmdUpdate(args);
    if (cmd == "remove" ||
        cmd == "rm")           return CmdRemove(args);
    if (cmd == "search")       return CmdSearch(args);
    if (cmd == "report")       return CmdReport(args);
    if (cmd == "seed-elf")     return CmdSeedElf(args);

    std::fprintf(stderr, "unknown command: %s\n", cmd.c_str());
    PrintUsage();
    return 2;
}
