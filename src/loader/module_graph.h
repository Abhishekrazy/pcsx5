#pragma once
#include "../common/types.h"
#include <map>
#include <string>
#include <vector>

namespace Loader {

    // Dependency graph over PS5 module names (from DT_NEEDED entries).
    //
    // Each module names the modules that must be loaded before it.
    // ResolveLoadOrder() produces a deterministic topological order:
    // dependencies always appear before their dependents.  When several
    // modules are ready at the same step the lexicographically smallest
    // name is chosen, so the output is stable across runs and insertion
    // orders.
    //
    // Cycles: the order is still total.  Cycle members are broken out
    // deterministically (smallest name first) and reported via the
    // CycleReport out-param plus a LOG_WARN.  Missing dependencies
    // (referenced but never added) are excluded from the output order
    // but reported as warnings; they are expected to resolve to HLE
    // stubs at runtime.
    class ModuleGraph {
    public:
        // Report describing irregularities found during ResolveLoadOrder().
        // All names are sorted for deterministic consumption.
        struct CycleReport {
            // Groups of modules involved in dependency cycles (each group
            // is one residue left when no remaining node had all of its
            // dependencies satisfied).  Names within a group are sorted.
            std::vector<std::vector<std::string>> cycles;
            // Dependencies referenced by added modules but never added
            // themselves.  These are not loadable from disk; at runtime
            // they resolve to HLE implementations.
            std::vector<std::string> missing;
        };

        ModuleGraph() = default;

        // Registers a module and the names it needs loaded first.
        // Re-adding an existing name replaces its dependency list.
        // Duplicate names inside `dependencies` are ignored.
        void AddModule(const std::string& name,
                       const std::vector<std::string>& dependencies);

        // Computes the load order.  Always returns every added module
        // exactly once (dependencies before dependents whenever no cycle
        // prevents it).  `report` may be nullptr if the caller does not
        // care about cycles/missing dependencies.
        std::vector<std::string> ResolveLoadOrder(CycleReport* report = nullptr) const;

    private:
        struct Node {
            std::vector<std::string> dependencies;  // names needed first
        };

        // std::map keeps names sorted, so iteration order is
        // deterministic regardless of AddModule call order.
        std::map<std::string, Node> nodes_;

        // Tarjan SCC over the leftover subgraph (edges: module ->
        // dependency, both restricted to `remaining`).  Returns only
        // true cycles (SCCs of size > 1), each sorted by name, groups
        // sorted by their first member.
        static std::vector<std::vector<std::string>> FindCycles(
            const std::map<std::string, Node>& remaining);
    };

}  // namespace Loader
