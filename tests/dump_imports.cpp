// Dump the import list of a real PS5 game ELF (eboot.bin.esbak).
//
// Goal: produce a real, prioritized HLE work list for the game in
// I:\Personal\Windows\pcsx5\Games\PPSA02929-app0.  The test loads the
// already-decrypted inner ELF (e_type 0xFE10) through the standard
// Loader::Load path, then walks the module's import list and writes:
//   - one human-readable summary to stdout
//   - one machine-readable line per import to the dump file
//   - one per-library histogram (count + library name)
//
// The path to the game is hardcoded for this game; the test is skipped
// (with a clear message) if the file is not present on the build host.

#include "loader/elf.h"
#include "memory/memory.h"
#include "common/log.h"
#include "common/nid.h"
#include "common/types.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#ifdef PAGE_SIZE
#  undef PAGE_SIZE
#endif
#ifdef ALIGN_UP
#  undef ALIGN_UP
#endif
#ifdef ALIGN_DOWN
#  undef ALIGN_DOWN
#endif
#ifndef PAGE_SIZE
#  define PAGE_SIZE 0x4000
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace {

constexpr const char* kGameEboot =
    "I:/Personal/Windows/pcsx5/Games/PPSA02929-app0/eboot.bin.esbak";

constexpr const char* kDumpPath =
    "I:/Personal/Windows/pcsx5/Games/PPSA02929-app0/imports.txt";

constexpr const char* kSummaryPath =
    "I:/Personal/Windows/pcsx5/Games/PPSA02929-app0/imports_summary.txt";

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    LogConfig::SetLevel(LogCategory::Loader, LogLevel::Warn);
    LogConfig::SetLevel(LogCategory::Memory, LogLevel::Warn);

    const std::filesystem::path src(kGameEboot);
    if (!std::filesystem::exists(src)) {
        std::fprintf(stderr,
                     "[SKIP] %s not present on this host.  Skipping "
                     "dump_imports.\n",
                     kGameEboot);
        // Return 0 so ctest still passes — this is a "real-game
         // specific" test, not a portability check.
        return 0;
    }

    if (!Memory::Initialize()) {
        std::fprintf(stderr, "FATAL: Memory::Initialize failed\n");
        return 2;
    }

    Loader::LoadedModule m{};
    const bool ok = Loader::Load(src.string(), m);
    if (!ok) {
        std::fprintf(stderr, "FATAL: Loader::Load failed for %s\n",
                     src.string().c_str());
        Memory::Shutdown();
        return 1;
    }

    std::fprintf(stdout, "\n[dump_imports] %s\n", src.string().c_str());
    std::fprintf(stdout, "  size       = %llu bytes\n",
                 (unsigned long long)m.image_size);
    std::fprintf(stdout, "  base       = 0x%llx\n",
                 (unsigned long long)m.base_address);
    std::fprintf(stdout, "  entry      = 0x%llx\n",
                 (unsigned long long)m.entry_point);
    std::fprintf(stdout, "  e_type     = 0x%04X\n", m.e_type);
    std::fprintf(stdout, "  is_pie     = %s\n", m.is_pie ? "true" : "false");
    std::fprintf(stdout, "  symbols    = %zu\n", m.symbols.size());
    std::fprintf(stdout, "  relocs     = %zu\n", m.relocations.size());
    std::fprintf(stdout, "  strtab size= %zu\n", m.string_table.size());
    std::fprintf(stdout, "  needed lib = %zu\n", m.needed_libraries.size());
    for (size_t i = 0; i < m.needed_libraries.size(); ++i) {
        std::fprintf(stdout, "    [%zu] %s\n", i, m.needed_libraries[i].c_str());
    }

    // Build the metadata view.
    Loader::ModuleMetadata meta{};
    Loader::ParseModuleMetadata(m, meta);

    std::fprintf(stdout, "\n  ModuleMetadata view:\n");
    std::fprintf(stdout, "    e_type          = 0x%04X\n", meta.e_type);
    std::fprintf(stdout, "    is_pie          = %s\n", meta.is_pie ? "true" : "false");
    std::fprintf(stdout, "    is_shared_object= %s\n",
                 meta.is_shared_object ? "true" : "false");
    std::fprintf(stdout, "    has_tls         = %s\n", meta.has_tls ? "true" : "false");
    std::fprintf(stdout, "    tls.file_size   = %llu\n",
                 (unsigned long long)meta.tls.file_size);
    std::fprintf(stdout, "    tls.mem_size    = %llu\n",
                 (unsigned long long)meta.tls.mem_size);
    std::fprintf(stdout, "    dependencies    = %zu\n", meta.dependencies.size());
    std::fprintf(stdout, "    imports         = %zu\n", meta.imports.size());
    std::fprintf(stdout, "    exports         = %zu\n", meta.exports.size());
    std::fprintf(stdout, "    referenced_imports = %u\n", meta.referenced_import_count);

    // Sort imports by name for stable output.  ImportEntry does not
    // carry a library field; the library is determined by which
    // DT_NEEDED dependency provides the symbol, which the loader does
    // not currently disambiguate.  We attribute every import to the
    // first DT_NEEDED library as a coarse approximation so the
    // histogram has at least one bucket.
    std::vector<Loader::ImportEntry> imports = meta.imports;
    std::sort(imports.begin(), imports.end(),
              [](const Loader::ImportEntry& a, const Loader::ImportEntry& b) {
                  return a.name < b.name;
              });
    const std::string lib_guess = meta.dependencies.empty()
                                      ? std::string{"(unknown)"}
                                      : meta.dependencies.front();

    // Per-library histogram (single bucket — see note above).
    std::map<std::string, size_t> by_lib;
    by_lib[lib_guess] = imports.size();

    // Build a parallel array of NID analysis records.  Each record
    // carries the parsed NID, the type, the resolved name (if any),
    // and the relocation counts.  Building this once avoids repeating
    // the parse+lookup work for every section that needs the data.
    struct NidAnalysis {
        std::string          raw;           // the raw import name
        Common::Ps5Nid       nid;
        Common::Ps5NidType   type = Common::Ps5NidType::Unknown;
        std::string          name;          // resolved name, "" if unknown
        u32                  rela_refs = 0;
        u32                  plt_refs  = 0;
        u32                  total_refs() const { return rela_refs + plt_refs; }
    };
    std::vector<NidAnalysis> analysed;
    analysed.reserve(imports.size());
    size_t nid_hits    = 0;
    size_t nid_misses  = 0;
    size_t nid_parsed  = 0;
    size_t fn_count    = 0;
    size_t data_count  = 0;
    size_t obj_count   = 0;
    size_t block_count = 0;
    size_t unknown_count = 0;
    size_t dead_count    = 0;   // total_refs == 0
    size_t plt_only      = 0;   // plt_refs > 0 && rela_refs == 0
    size_t rela_only     = 0;   // rela_refs > 0 && plt_refs == 0
    size_t both_refs     = 0;   // both > 0
    for (const auto& imp : imports) {
        NidAnalysis a;
        a.raw = imp.name;
        a.rela_refs = imp.rela_refs;
        a.plt_refs  = imp.plt_refs;
        auto parsed = Common::ParseNidString(imp.name);
        if (!parsed) {
            ++nid_misses;
        } else {
            ++nid_parsed;
            a.nid  = parsed->nid;
            a.type = parsed->type;
            auto name = Common::LookupNidName(parsed->nid);
            if (name) {
                a.name = std::string(*name);
                ++nid_hits;
            }
            switch (a.type) {
                case Common::Ps5NidType::Function: ++fn_count;    break;
                case Common::Ps5NidType::Data:     ++data_count;  break;
                case Common::Ps5NidType::Object:   ++obj_count;   break;
                case Common::Ps5NidType::Block:    ++block_count; break;
                default:                           ++unknown_count; break;
            }
        }
        if (a.total_refs() == 0)         ++dead_count;
        if (a.plt_refs > 0 && a.rela_refs == 0) ++plt_only;
        if (a.rela_refs > 0 && a.plt_refs == 0) ++rela_only;
        if (a.plt_refs > 0 && a.rela_refs > 0)  ++both_refs;
        analysed.push_back(std::move(a));
    }

    // Write the detailed import list.
    {
        std::ofstream out(kDumpPath, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            std::fprintf(stderr, "FATAL: cannot open %s for write\n", kDumpPath);
            Memory::Shutdown();
            return 1;
        }
        out << "# Imports extracted from " << src.string() << "\n";
        out << "# Format: <library_guess>\\t<symbol>\\t<resolved_name?>\\t<rela_refs>\\t<plt_refs>\n";
        out << "# library_guess = first DT_NEEDED ('" << lib_guess << "')\n";
        for (const auto& a : analysed) {
            std::string resolved = a.name.empty() ? "(unknown)" : a.name;
            out << lib_guess << "\t" << a.raw << "\t" << resolved << "\t"
                << a.rela_refs << "\t" << a.plt_refs << "\n";
        }
    }
    std::fprintf(stdout, "\n  Wrote %zu imports to %s\n",
                 analysed.size(), kDumpPath);

    // Write the human-readable summary.
    {
        std::ofstream out(kSummaryPath, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            std::fprintf(stderr, "FATAL: cannot open %s for write\n", kSummaryPath);
            Memory::Shutdown();
            return 1;
        }
        out << "Import summary for " << src.filename().string() << "\n";
        out << "================================================\n";
        out << "Image base  : 0x" << std::hex << m.base_address << std::dec << "\n";
        out << "Entry point : 0x" << std::hex << m.entry_point << std::dec << "\n";
        out << "ELF type    : 0x" << std::hex << m.e_type << std::dec << "\n";
        out << "PIE         : " << (m.is_pie ? "yes" : "no") << "\n";
        out << "Image size  : " << m.image_size << " bytes\n";
        out << "Total imports       : " << analysed.size() << "\n";
        out << "Referenced imports  : " << meta.referenced_import_count << "\n";
        out << "Dependencies        : " << meta.dependencies.size() << "\n";
        for (const auto& d : meta.dependencies) out << "  - " << d << "\n";

        out << "\nNID resolution\n";
        out << "--------------\n";
        out << "Parsed as NID : " << nid_parsed << " / " << analysed.size() << "\n";
        out << "  functions   : " << fn_count    << "\n";
        out << "  data        : " << data_count  << "\n";
        out << "  object      : " << obj_count   << "\n";
        out << "  block       : " << block_count << "\n";
        out << "  unknown tag : " << unknown_count << "\n";
        out << "Name-table hits : " << nid_hits << " / " << nid_parsed << "\n";

        // ------------------------------------------------------------
        // 1. Per-type breakdown with reference totals
        // ------------------------------------------------------------
        struct TypeRow {
            const char* tag;
            size_t      count = 0;
            u64         refs_total = 0;
            u32         refs_max = 0;
            size_t      name_hits = 0;
        };
        TypeRow rows[5] = {
            {"function (#T#T)", 0, 0, 0, 0},
            {"data     (#A#B)", 0, 0, 0, 0},
            {"object   (#S#N)", 0, 0, 0, 0},
            {"block    (#B#C)", 0, 0, 0, 0},
            {"unknown tag",     0, 0, 0, 0},
        };
        auto row_of = [&](Common::Ps5NidType t) -> TypeRow* {
            switch (t) {
                case Common::Ps5NidType::Function: return &rows[0];
                case Common::Ps5NidType::Data:     return &rows[1];
                case Common::Ps5NidType::Object:   return &rows[2];
                case Common::Ps5NidType::Block:    return &rows[3];
                default:                           return &rows[4];
            }
        };
        for (const auto& a : analysed) {
            TypeRow* r = row_of(a.type);
            r->count      += 1;
            r->refs_total += a.total_refs();
            if (a.total_refs() > r->refs_max) r->refs_max = a.total_refs();
            if (!a.name.empty())              r->name_hits += 1;
        }
        out << "\nPer-type breakdown\n";
        out << "-------------------\n";
        out << "  type            count   refs   max   name_hits   name_rate\n";
        for (const auto& r : rows) {
            const double rate = r.count ? (100.0 * r.name_hits / r.count) : 0.0;
            out << "  " << r.tag
                << "  " << r.count
                << "  " << r.refs_total
                << "  " << r.refs_max
                << "  " << r.name_hits << "/" << r.count
                << "  " << rate << "%\n";
        }

        // ------------------------------------------------------------
        // 2. Reference-count distribution histogram
        // ------------------------------------------------------------
        // Buckets: 0 (dead), 1, 2-5, 6-20, 21-100, 101+
        struct Bucket { const char* label; size_t count = 0; };
        Bucket buckets[] = {
            {"0 (dead)        "},
            {"1               "},
            {"2..5            "},
            {"6..20           "},
            {"21..100         "},
            {"101+            "},
        };
        for (const auto& a : analysed) {
            const u32 r = a.total_refs();
            if      (r == 0)   buckets[0].count++;
            else if (r == 1)   buckets[1].count++;
            else if (r <= 5)   buckets[2].count++;
            else if (r <= 20)  buckets[3].count++;
            else if (r <= 100) buckets[4].count++;
            else               buckets[5].count++;
        }
        out << "\nReference-count distribution\n";
        out << "----------------------------\n";
        for (const auto& b : buckets) {
            out << "  " << b.label << " : " << b.count << "\n";
        }

        // ------------------------------------------------------------
        // 3. PLT vs RELA vs both
        // ------------------------------------------------------------
        out << "\nRelocation source\n";
        out << "-----------------\n";
        out << "  PLT only   (lazy-bound)        : " << plt_only  << "\n";
        out << "  RELA only  (static-linked)     : " << rela_only << "\n";
        out << "  both PLT and RELA              : " << both_refs << "\n";
        out << "  unreferenced (no relocs)       : " << dead_count << "\n";

        // ------------------------------------------------------------
        // 4. Top-30 most-referenced NIDs (overall)
        // ------------------------------------------------------------
        std::vector<const NidAnalysis*> by_refs;
        by_refs.reserve(analysed.size());
        for (const auto& a : analysed) by_refs.push_back(&a);
        std::sort(by_refs.begin(), by_refs.end(),
                  [](const NidAnalysis* x, const NidAnalysis* y) {
                      if (x->total_refs() != y->total_refs())
                          return x->total_refs() > y->total_refs();
                      return x->raw < y->raw;
                  });
        out << "\nTop-30 most-referenced NIDs\n";
        out << "----------------------------\n";
        out << "  refs  type   name         nid\n";
        size_t top_n = std::min<size_t>(30, by_refs.size());
        for (size_t i = 0; i < top_n; ++i) {
            const auto& a = *by_refs[i];
            out << "  " << a.total_refs()
                << "  " << std::string(Common::NidTypeToString(a.type)).substr(1, 1)
                << "    " << (a.name.empty() ? std::string{"-"} : a.name)
                << "  " << a.raw << "\n";
        }

        // ------------------------------------------------------------
        // 5. Per-type top-15
        // ------------------------------------------------------------
        auto print_per_type_top = [&](Common::Ps5NidType t,
                                      const char* header) {
            std::vector<const NidAnalysis*> v;
            for (const auto& a : analysed)
                if (a.type == t) v.push_back(&a);
            std::sort(v.begin(), v.end(),
                      [](const NidAnalysis* x, const NidAnalysis* y) {
                          if (x->total_refs() != y->total_refs())
                              return x->total_refs() > y->total_refs();
                          return x->raw < y->raw;
                      });
            if (v.empty()) return;
            out << "\nTop-15 " << header << " NIDs\n";
            std::string dashes(header);
            dashes += " NIDs";
            out << std::string(dashes.size() + 9, '-') << "\n";
            out << "  refs  name         nid\n";
            size_t n = std::min<size_t>(15, v.size());
            for (size_t i = 0; i < n; ++i) {
                const auto& a = *v[i];
                out << "  " << a.total_refs()
                    << "  " << (a.name.empty() ? std::string{"-"} : a.name)
                    << "  " << a.raw << "\n";
            }
        };
        print_per_type_top(Common::Ps5NidType::Function, "function");
        print_per_type_top(Common::Ps5NidType::Data,     "data");
        print_per_type_top(Common::Ps5NidType::Object,   "object");
        print_per_type_top(Common::Ps5NidType::Block,    "block");

        // ------------------------------------------------------------
        // 6. Dead imports (refs == 0) — symbols declared but never used
        // ------------------------------------------------------------
        if (dead_count > 0) {
            out << "\nDead imports (referenced 0x, declared but unused)\n";
            out << "--------------------------------------------------\n";
            size_t n = 0;
            for (const auto& a : analysed) {
                if (a.total_refs() != 0) continue;
                out << "  " << std::string(Common::NidTypeToString(a.type))
                    << "  " << a.raw << "\n";
                if (++n >= 50) {
                    out << "  ... (" << (dead_count - 50) << " more)\n";
                    break;
                }
            }
        }

        // ------------------------------------------------------------
        // 7. Per-DT_NEEDED library summary
        //
        // The dynamic linker resolves symbols by walking DT_NEEDED in
        // order.  We do not have the per-library export tables, so we
        // can only approximate which library a NID comes from.  We use
        // a simple heuristic: if a library exports a known NID from
        // our table, group all remaining unknown NIDs by NID-prefix
        // similarity.  In practice, this is unreliable; for now we
        // emit a summary that lists each DT_NEEDED with the count of
        // *known* NIDs that look like they belong to that library,
        // and leave proper attribution for a future loader pass.
        // ------------------------------------------------------------
        out << "\nPer-DT_NEEDED library (heuristic attribution)\n";
        out << "----------------------------------------------\n";
        out << "  order  library                          known   total\n";
        // Most-specific matches first: libSceAgcDriver contains
        // libSceAgc as a substring, so we check the longer name
        // first.  Each pair is (substring-in-library, substring-in-name).
        struct LibMatch { const char* lib; const char* sym; };
        static const LibMatch kLibMatches[] = {
            {"libSceAgcDriver",       "sceAgcDriver"},
            {"libSceAgc",             "sceAgc"},
            {"libSceGnmDriver",       "sceGnm"},
            {"libSceVideoOut",        "sceVideo"},
            {"libSceAudioOut",        "sceAudio"},
            {"libScePad",             "scePad"},
            {"libSceImeDialog",       "sceImeDialog"},
            {"libSceImeBackend",      "sceImeBackend"},
            {"libSceIme",             "sceIme"},
            {"libSceNpUniversalDataSystem", "sceNpUniversalDataSystem"},
            {"libSceNpTrophy2",       "sceNpTrophy"},
            {"libSceNpGameIntent",    "sceNpGame"},
            {"libSceSaveDataDialog",  "sceSaveDataDialog"},
            {"libSceSaveData",        "sceSaveData"},
            {"libSceCommonDialog",    "sceCommonDialog"},
            {"libSceSystemService",   "sceSystemService"},
            {"libSceUserService",     "sceUserService"},
            {"libSceSysmodule",       "sceSysmodule"},
            {"libSceUlt",             "sceUlt"},
            {"libkernel",             "sceKernel"},
            {"libc",                  ""},  // libc: we don't have libc NIDs named yet
        };
        for (size_t i = 0; i < meta.dependencies.size(); ++i) {
            const std::string& lib = meta.dependencies[i];
            out << "  " << i << "  " << lib;
            if (lib.size() < 32) out << std::string(32 - lib.size(), ' ');

            // Find the first matching LibMatch for this library.
            size_t known_for_lib = 0;
            for (const auto& lm : kLibMatches) {
                if (lib.find(lm.lib) != std::string::npos) {
                    // Count known NIDs whose name contains lm.sym.
                    if (lm.sym[0] == '\0') break;  // libc: no known names yet
                    for (const auto& a : analysed) {
                        if (!a.name.empty() &&
                            a.name.find(lm.sym) != std::string::npos) {
                            ++known_for_lib;
                        }
                    }
                    break;
                }
            }
            out << known_for_lib << "  " << "n/a\n";
        }
        out << "\n  Note: per-symbol library attribution is approximate.\n";
        out << "  The loader does not currently resolve DT_NEEDED\n";
        out << "  libraries, so every import is bucketed under the\n";
        out << "  first DT_NEEDED in the table above.  Future work:\n";
        out << "  load each .prx and use its symbol table for exact\n";
        out << "  attribution.\n";

        // ------------------------------------------------------------
        // 8. Full import list (alphabetical by symbol name)
        // ------------------------------------------------------------
        out << "\nFull import list (alphabetical by symbol name)\n";
        out << "-----------------------------------------------\n";
        for (const auto& a : analysed) {
            out << a.raw
                << "\t" << (a.name.empty() ? std::string{"(unknown)"} : a.name)
                << "\t(refs=" << a.rela_refs
                << ", plt=" << a.plt_refs << ")\n";
        }
    }
    std::fprintf(stdout, "  Wrote summary to %s\n", kSummaryPath);

    // Print the per-library histogram to stdout.
    std::fprintf(stdout, "\n  Per-library histogram (top entries first):\n");
    std::vector<std::pair<std::string, size_t>> hist(by_lib.begin(), by_lib.end());
    std::sort(hist.begin(), hist.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    for (const auto& [lib, count] : hist) {
        std::fprintf(stdout, "    %5zu  %s\n", count, lib.c_str());
    }

    // Print NID resolution summary to stdout.
    std::fprintf(stdout, "\n  NID resolution:\n");
    std::fprintf(stdout, "    parsed        : %zu / %zu\n", nid_parsed, analysed.size());
    std::fprintf(stdout, "      functions   : %zu\n", fn_count);
    std::fprintf(stdout, "      data        : %zu\n", data_count);
    std::fprintf(stdout, "      object      : %zu\n", obj_count);
    std::fprintf(stdout, "      block       : %zu\n", block_count);
    std::fprintf(stdout, "      unknown tag : %zu\n", unknown_count);
    std::fprintf(stdout, "    name hits     : %zu / %zu\n", nid_hits, nid_parsed);

    // Print the new analysis sections to stdout.
    std::fprintf(stdout, "\n  Reference-count distribution:\n");
    size_t b0=0, b1=0, b2_5=0, b6_20=0, b21_100=0, b101=0;
    for (const auto& a : analysed) {
        const u32 r = a.total_refs();
        if      (r == 0)   ++b0;
        else if (r == 1)   ++b1;
        else if (r <= 5)   ++b2_5;
        else if (r <= 20)  ++b6_20;
        else if (r <= 100) ++b21_100;
        else               ++b101;
    }
    std::fprintf(stdout, "    0 (dead)      : %zu\n", b0);
    std::fprintf(stdout, "    1             : %zu\n", b1);
    std::fprintf(stdout, "    2..5          : %zu\n", b2_5);
    std::fprintf(stdout, "    6..20         : %zu\n", b6_20);
    std::fprintf(stdout, "    21..100       : %zu\n", b21_100);
    std::fprintf(stdout, "    101+          : %zu\n", b101);

    std::fprintf(stdout, "\n  Relocation source:\n");
    std::fprintf(stdout, "    PLT only     : %zu\n", plt_only);
    std::fprintf(stdout, "    RELA only    : %zu\n", rela_only);
    std::fprintf(stdout, "    both         : %zu\n", both_refs);
    std::fprintf(stdout, "    unreferenced : %zu\n", dead_count);

    // HLE coverage requires linking against the full HLE / Kernel / GPU
    // stack, which this minimal dump target does not pull in.  The
    // hit/miss report is deferred: a future target can resolve each
    // import through HLE::Resolve and report a real hit/miss count.
    size_t miss = 0;
    for (const auto& a : analysed) {
        if (a.rela_refs > 0) ++miss;
    }
    std::fprintf(stdout, "\n  HLE coverage: %zu referenced imports (full "
                         "registry analysis pending GPU/Kernel link).\n",
                 miss);

    Memory::Shutdown();
    return 0;
}
