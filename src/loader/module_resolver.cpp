#include "module_resolver.h"
#include "../common/log.h"
#include <algorithm>
#include <cctype>
#include <system_error>

namespace Loader {

    namespace {

        std::string ToLower(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        }

        // Reduce a DT_NEEDED entry to the candidate file names to probe, in
        // priority order.  See module_resolver.h for the mapping rules.
        std::vector<std::string> CandidateFileNames(const std::string& module_name) {
            // Strip any directory components: some entries are full guest
            // paths ("/system/common/lib/libc.sprx") and both separators
            // appear in the wild.
            std::string base = module_name;
            const size_t sep = base.find_last_of("/\\");
            if (sep != std::string::npos) {
                base = base.substr(sep + 1);
            }

            const std::string lower = ToLower(base);
            const auto has_ext = [&lower](const char* ext) {
                const size_t len = std::char_traits<char>::length(ext);
                return lower.size() >= len && lower.compare(lower.size() - len, len, ext) == 0;
            };

            // Already a file name ("libScePad.prx" / "libc.sprx"): probe as-is.
            if (has_ext(".prx") || has_ext(".sprx")) {
                return { base };
            }
            // Internal module name ("libScePad"): .prx wins over .sprx.
            return { base + ".prx", base + ".sprx" };
        }

        // Case-insensitive "does `dir` contain a file named `file_name`".
        // Real dumps vary in casing ("LIBSCEPAD.PRX"), so we iterate the
        // directory instead of relying on the host file system's semantics.
        std::optional<std::filesystem::path> FindFileCaseInsensitive(
            const std::filesystem::path& dir, const std::string& file_name) {
            std::error_code ec;
            if (!std::filesystem::is_directory(dir, ec) || ec) {
                return std::nullopt;
            }
            const std::string wanted = ToLower(file_name);
            for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
                if (ec) break;
                if (!entry.is_regular_file(ec)) continue;
                if (ToLower(entry.path().filename().string()) == wanted) {
                    return entry.path();
                }
            }
            return std::nullopt;
        }

    } // anonymous namespace

    void ModuleResolver::SetSearchDirectories(std::vector<std::filesystem::path> dirs) {
        m_search_dirs = std::move(dirs);
    }

    std::optional<std::filesystem::path> ModuleResolver::ResolveModuleFile(
        const std::string& module_name) const {
        if (module_name.empty()) {
            return std::nullopt;
        }
        const std::vector<std::string> candidates = CandidateFileNames(module_name);
        for (const auto& dir : m_search_dirs) {
            for (const auto& candidate : candidates) {
                if (auto found = FindFileCaseInsensitive(dir, candidate)) {
                    LOG_DEBUG(Loader, "Resolved module '%s' -> '%s'",
                              module_name.c_str(), found->string().c_str());
                    return found;
                }
            }
        }
        return std::nullopt;
    }

    std::vector<NeededLibraryResolution> ModuleResolver::ResolveNeededLibraries(
        const LoadedModule& module) const {
        std::vector<NeededLibraryResolution> report;
        report.reserve(module.needed_libraries.size());
        for (const auto& needed : module.needed_libraries) {
            NeededLibraryResolution entry;
            entry.name = needed;
            if (auto path = ResolveModuleFile(needed)) {
                entry.resolved = true;
                entry.path     = std::move(*path);
            }
            report.push_back(std::move(entry));
        }
        return report;
    }

}
// namespace Loader
