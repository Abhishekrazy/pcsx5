#pragma once
//
// PRX module file resolver.
//
// A PS5 module declares its dependencies via DT_NEEDED entries in the
// dynamic table (surfaced here as `LoadedModule::needed_libraries`).  Those
// entries are *names*, not host paths, and come in two flavours:
//
//   - internal module names, e.g. "libScePad", "libc", "libSceSysmodule"
//   - on-disk file names,    e.g. "libScePad.prx", "libc.sprx", and
//     occasionally full guest paths such as
//     "/system/common/lib/libSceSysmodule.sprx"
//
// `ModuleResolver` maps such a name to a real PRX/SPRX file supplied by the
// user by probing an ordered list of search directories:
//
//   1. the game's own `sce_module/` directory (game-bundled modules win)
//   2. the user-configured firmware modules directory (system modules)
//
// Anything that cannot be resolved to a file is left to the HLE
// implementations, which is the status quo.
//
// Name-to-file mapping rules (applied in ResolveModuleFile):
//
//   - Directory components are stripped; only the final file-name portion
//     of a DT_NEEDED entry is used for the probe.
//   - If the name already ends in ".prx" or ".sprx" (case-insensitive), it
//     is probed as-is.
//   - Otherwise "<name>.prx" is probed first, then "<name>.sprx".
//   - File-name comparison is case-insensitive: real dumps use a mix of
//     "libScePad.prx", "LIBSCEPAD.PRX", etc., and the resolver must find
//     them regardless of how the user named the files.
//
#include "../common/types.h"
#include "elf.h"
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Loader {

    // Per-dependency resolution result, produced by ResolveNeededLibraries.
    struct NeededLibraryResolution {
        std::string             name;            // DT_NEEDED entry, as-is
        bool                    resolved = false; // false => will be served by HLE
        std::filesystem::path   path;            // valid only when resolved
    };

    class ModuleResolver {
    public:
        // Replace the ordered search-directory list.  Earlier directories
        // take precedence.  Non-existent directories are skipped at probe
        // time (not an error).
        void SetSearchDirectories(std::vector<std::filesystem::path> dirs);

        const std::vector<std::filesystem::path>& SearchDirectories() const {
            return m_search_dirs;
        }

        // Map a DT_NEEDED / internal module name to a file on disk.
        // Returns std::nullopt when no directory contains a matching file.
        std::optional<std::filesystem::path> ResolveModuleFile(
            const std::string& module_name) const;

        // Resolve every entry of `module.needed_libraries`, preserving the
        // declaration order.  Useful for logging / diagnostics.
        std::vector<NeededLibraryResolution> ResolveNeededLibraries(
            const LoadedModule& module) const;

    private:
        std::vector<std::filesystem::path> m_search_dirs;
    };

}
// namespace Loader
