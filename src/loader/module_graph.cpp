// Module dependency graph and deterministic load-order resolution.
//
// See module_graph.h for the contract.  Implementation notes:
//   - Kahn's algorithm over a name-sorted map gives determinism for
//     free: at every step we scan the remaining nodes in sorted order
//     and pick the first one whose pending dependency count is zero.
//   - Missing dependencies (referenced but never added) are collected
//     up front and simply not counted, so they neither block nor appear
//     in the output order.
//   - If a pass stalls with nodes left over, the leftover subgraph
//     contains at least one cycle.  True cycle members (Tarjan SCCs of
//     size > 1) are reported and logged; the stall is then broken
//     deterministically by force-emitting the smallest leftover name
//     and resuming, so dependents of a cycle still get ordered and the
//     algorithm never hangs.

#include "module_graph.h"
#include "../common/log.h"

#include <algorithm>
#include <set>

namespace Loader {

void ModuleGraph::AddModule(const std::string& name,
                            const std::vector<std::string>& dependencies) {
    Node node;
    node.dependencies.reserve(dependencies.size());
    for (const auto& dep : dependencies) {
        if (dep == name) {
            // Self-dependency is meaningless for load order; drop it so
            // it cannot create a fake one-node cycle.
            LOG_WARN(Loader, "ModuleGraph: module '%s' lists itself as a dependency; ignoring",
                     name.c_str());
            continue;
        }
        if (std::find(node.dependencies.begin(), node.dependencies.end(), dep)
                == node.dependencies.end()) {
            node.dependencies.push_back(dep);
        }
    }
    nodes_[name] = std::move(node);
}

std::vector<std::string> ModuleGraph::ResolveLoadOrder(CycleReport* report) const {
    std::vector<std::string> order;
    order.reserve(nodes_.size());

    if (report) {
        report->cycles.clear();
        report->missing.clear();
    }

    // Collect missing dependencies (referenced, never added).
    std::set<std::string> missing;
    for (const auto& [name, node] : nodes_) {
        for (const auto& dep : node.dependencies) {
            if (nodes_.find(dep) == nodes_.end()) {
                missing.insert(dep);
            }
        }
    }
    if (!missing.empty()) {
        std::string joined;
        for (const auto& m : missing) {
            if (!joined.empty()) joined += ", ";
            joined += m;
        }
        LOG_WARN(Loader,
                 "ModuleGraph: %zu missing dependencies (will resolve to HLE at runtime): %s",
                 missing.size(), joined.c_str());
        if (report) {
            report->missing.assign(missing.begin(), missing.end());
        }
    }

    // Kahn's algorithm.  pending[name] counts how many of the module's
    // *present* dependencies have not been emitted yet.
    std::map<std::string, size_t> pending;
    for (const auto& [name, node] : nodes_) {
        size_t count = 0;
        for (const auto& dep : node.dependencies) {
            if (nodes_.find(dep) != nodes_.end()) {
                ++count;
            }
        }
        pending[name] = count;
    }

    std::map<std::string, Node> remaining = nodes_;
    while (!remaining.empty()) {
        // Pick the lexicographically smallest ready node.
        auto ready = remaining.end();
        for (auto it = remaining.begin(); it != remaining.end(); ++it) {
            if (pending[it->first] == 0) {
                ready = it;
                break;
            }
        }

        if (ready == remaining.end()) {
            // Cycle stall: no remaining node has all dependencies
            // satisfied.  Report the true cycle members (SCCs of size
            // > 1 within the leftover subgraph) so nodes that merely
            // depend on a cycle are not misreported as cycle members.
            auto sccs = FindCycles(remaining);
            for (auto& scc : sccs) {
                std::string joined;
                for (const auto& m : scc) {
                    if (!joined.empty()) joined += ", ";
                    joined += m;
                }
                LOG_WARN(Loader,
                         "ModuleGraph: dependency cycle detected (%zu modules): %s; breaking in name order",
                         scc.size(), joined.c_str());
                if (report) {
                    report->cycles.push_back(scc);
                }
            }

            // Break deterministically: force-emit the smallest leftover
            // name, unblock its dependents, and resume the drain.  The
            // stall/cycle detection above will re-run if another cycle
            // still blocks progress.
            ready = remaining.begin();
            pending[ready->first] = 0;
        }

        const std::string chosen = ready->first;
        order.push_back(chosen);
        remaining.erase(ready);

        // Unblock dependents of `chosen`.
        for (const auto& [name, node] : remaining) {
            if (pending[name] == 0) continue;
            for (const auto& dep : node.dependencies) {
                if (dep == chosen) {
                    --pending[name];
                    break;
                }
            }
        }
    }

    return order;
}

// Tarjan's algorithm (recursive; PS5 module counts are small enough
// that recursion depth is not a concern).
std::vector<std::vector<std::string>> ModuleGraph::FindCycles(
    const std::map<std::string, Node>& remaining) {
    struct Frame {
        std::map<std::string, int> index;
        std::map<std::string, int> lowlink;
        std::map<std::string, bool> on_stack;
        std::vector<std::string> stack;
        int next_index = 0;
        std::vector<std::vector<std::string>> cycles;
    } f;

    // Iterated in sorted (map) order for determinism.
    struct StrongConnect {
        static void Run(Frame& f, const std::map<std::string, Node>& g,
                        const std::string& v) {
            f.index[v] = f.lowlink[v] = f.next_index++;
            f.stack.push_back(v);
            f.on_stack[v] = true;

            for (const auto& dep : g.at(v).dependencies) {
                if (g.find(dep) == g.end()) continue;  // missing dep
                if (f.index.find(dep) == f.index.end()) {
                    Run(f, g, dep);
                    f.lowlink[v] = std::min(f.lowlink[v], f.lowlink[dep]);
                } else if (f.on_stack[dep]) {
                    f.lowlink[v] = std::min(f.lowlink[v], f.index[dep]);
                }
            }

            if (f.lowlink[v] == f.index[v]) {
                std::vector<std::string> scc;
                std::string w;
                do {
                    w = f.stack.back();
                    f.stack.pop_back();
                    f.on_stack[w] = false;
                    scc.push_back(w);
                } while (w != v);
                if (scc.size() > 1) {
                    // Self-loops are dropped in AddModule, so any SCC
                    // larger than one node is a true cycle.
                    std::sort(scc.begin(), scc.end());
                    f.cycles.push_back(std::move(scc));
                }
            }
        }
    };

    for (const auto& [name, node] : remaining) {
        (void)node;
        if (f.index.find(name) == f.index.end()) {
            StrongConnect::Run(f, remaining, name);
        }
    }

    std::sort(f.cycles.begin(), f.cycles.end(),
              [](const auto& a, const auto& b) { return a.front() < b.front(); });
    return f.cycles;
}

}  // namespace Loader
