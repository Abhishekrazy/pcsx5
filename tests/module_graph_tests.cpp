// Module dependency graph / load-order resolution tests.
//
// Verifies:
//   - Empty graph, single module, linear chain, diamond topology.
//   - Cycles: order is still total, members are reported, no hang.
//   - Missing dependencies: excluded from the order but reported.
//   - Determinism: insertion order does not change the output.
//
// Self-contained: only needs src/loader/module_graph.cpp + log.cpp.

#include "../src/loader/module_graph.h"
#include "../src/loader/module_resolver.h"
#include "../src/common/log.h"
#include "../src/common/types.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks   = 0;

#define EXPECT(cond, msg) do {                                     \
    ++g_checks;                                                    \
    if (!(cond)) {                                                 \
        ++g_failures;                                              \
        std::fprintf(stderr, "FAIL [%s:%d] %s\n",                  \
                     __FILE__, __LINE__, (msg));                   \
    }                                                              \
} while (0)

#define EXPECT_EQ(a, b, msg) do {                                  \
    ++g_checks;                                                    \
    if (!((a) == (b))) {                                           \
        ++g_failures;                                              \
        std::fprintf(stderr, "FAIL [%s:%d] %s\n",                  \
                     __FILE__, __LINE__, (msg));                   \
    }                                                              \
} while (0)

void DumpOrder(const char* label, const std::vector<std::string>& order) {
    std::fprintf(stderr, "  %s:", label);
    for (const auto& s : order) std::fprintf(stderr, " %s", s.c_str());
    std::fprintf(stderr, "\n");
}

// True if `before` appears before `after` in `order`.
bool ComesBefore(const std::vector<std::string>& order,
                 const std::string& before, const std::string& after) {
    auto bi = std::find(order.begin(), order.end(), before);
    auto ai = std::find(order.begin(), order.end(), after);
    return bi != order.end() && ai != order.end() && bi < ai;
}

void TestEmpty() {
    Loader::ModuleGraph g;
    Loader::ModuleGraph::CycleReport report;
    auto order = g.ResolveLoadOrder(&report);
    EXPECT(order.empty(), "empty graph produces empty order");
    EXPECT(report.cycles.empty(), "empty graph reports no cycles");
    EXPECT(report.missing.empty(), "empty graph reports no missing deps");
}

void TestSingle() {
    Loader::ModuleGraph g;
    g.AddModule("libSceKernel", {});
    auto order = g.ResolveLoadOrder();
    EXPECT_EQ(order.size(), (size_t)1, "single module order size");
    EXPECT(order == std::vector<std::string>{"libSceKernel"},
           "single module order contents");
}

void TestLinearChain() {
    // A <- B <- C <- D (D needs C, C needs B, B needs A)
    Loader::ModuleGraph g;
    g.AddModule("D", {"C"});
    g.AddModule("B", {"A"});
    g.AddModule("A", {});
    g.AddModule("C", {"B"});
    auto order = g.ResolveLoadOrder();
    EXPECT(order == (std::vector<std::string>{"A", "B", "C", "D"}),
           "linear chain resolves A,B,C,D");
    if (order != (std::vector<std::string>{"A", "B", "C", "D"})) {
        DumpOrder("chain", order);
    }
}

void TestDiamond() {
    //     A
    //    / \
    //   B   C
    //    \ /
    //     D
    Loader::ModuleGraph g;
    g.AddModule("D", {"B", "C"});
    g.AddModule("C", {"A"});
    g.AddModule("B", {"A"});
    g.AddModule("A", {});
    Loader::ModuleGraph::CycleReport report;
    auto order = g.ResolveLoadOrder(&report);
    EXPECT_EQ(order.size(), (size_t)4, "diamond order size");
    EXPECT(ComesBefore(order, "A", "B"), "diamond A before B");
    EXPECT(ComesBefore(order, "A", "C"), "diamond A before C");
    EXPECT(ComesBefore(order, "B", "D"), "diamond B before D");
    EXPECT(ComesBefore(order, "C", "D"), "diamond C before D");
    EXPECT(report.cycles.empty(), "diamond reports no cycles");
    // Deterministic tie-break: B and C both ready after A, B wins.
    EXPECT(order == (std::vector<std::string>{"A", "B", "C", "D"}),
           "diamond exact deterministic order");
}

void TestCycle() {
    // A -> B -> C -> A, plus D hanging off A.
    Loader::ModuleGraph g;
    g.AddModule("A", {"C"});
    g.AddModule("B", {"A"});
    g.AddModule("C", {"B"});
    g.AddModule("D", {"A"});
    Loader::ModuleGraph::CycleReport report;
    auto order = g.ResolveLoadOrder(&report);

    // Order is still total: every module exactly once.
    EXPECT_EQ(order.size(), (size_t)4, "cycle order still total");
    {
        auto sorted = order;
        std::sort(sorted.begin(), sorted.end());
        EXPECT(sorted == (std::vector<std::string>{"A", "B", "C", "D"}),
               "cycle order contains every module once");
    }

    // Cycle members reported (sorted), no missing.
    EXPECT_EQ(report.cycles.size(), (size_t)1, "one cycle group reported");
    if (report.cycles.size() == 1) {
        EXPECT(report.cycles[0] == (std::vector<std::string>{"A", "B", "C"}),
               "cycle members reported sorted");
    }
    EXPECT(report.missing.empty(), "cycle test has no missing deps");
}

void TestMissingDependency() {
    // app needs libSceKernel (not added -> HLE at runtime) and libPresent.
    Loader::ModuleGraph g;
    g.AddModule("app", {"libSceKernel", "libPresent"});
    g.AddModule("libPresent", {});
    Loader::ModuleGraph::CycleReport report;
    auto order = g.ResolveLoadOrder(&report);

    EXPECT(order == (std::vector<std::string>{"libPresent", "app"}),
           "missing dep excluded from order");
    EXPECT(report.cycles.empty(), "missing dep is not a cycle");
    EXPECT(report.missing == (std::vector<std::string>{"libSceKernel"}),
           "missing dep reported");
    EXPECT(ComesBefore(order, "libPresent", "app"),
           "present dependency still ordered first");
}

void TestDeterminism() {
    // Same graph, different insertion orders -> identical output.
    auto build_a = [] {
        Loader::ModuleGraph g;
        g.AddModule("zeta", {"alpha"});
        g.AddModule("alpha", {});
        g.AddModule("mid", {"alpha"});
        g.AddModule("beta", {});
        g.AddModule("omega", {"mid", "zeta", "beta"});
        return g;
    };
    auto build_b = [] {
        Loader::ModuleGraph g;
        g.AddModule("omega", {"mid", "zeta", "beta"});
        g.AddModule("beta", {});
        g.AddModule("mid", {"alpha"});
        g.AddModule("alpha", {});
        g.AddModule("zeta", {"alpha"});
        return g;
    };

    auto ga = build_a();
    auto gb = build_b();
    auto oa = ga.ResolveLoadOrder();
    auto ob = gb.ResolveLoadOrder();
    EXPECT(oa == ob, "insertion order does not change output");
    if (oa != ob) {
        DumpOrder("order A", oa);
        DumpOrder("order B", ob);
    }

    // Exact expected order: alpha < beta ready first (alpha wins),
    // then beta, then mid < zeta (mid wins), then zeta, then omega.
    EXPECT(oa == (std::vector<std::string>{"alpha", "beta", "mid", "zeta", "omega"}),
           "deterministic lexicographic tie-breaking");
}

void TestReAddReplacesDependencies() {
    Loader::ModuleGraph g;
    g.AddModule("A", {"B"});
    g.AddModule("B", {});
    g.AddModule("A", {});  // replace: A no longer needs B
    Loader::ModuleGraph::CycleReport report;
    auto order = g.ResolveLoadOrder(&report);
    EXPECT_EQ(order.size(), (size_t)2, "re-add keeps single node");
    // Both ready; lexicographic order wins.
    EXPECT(order == (std::vector<std::string>{"A", "B"}),
           "re-added dependency list replaces the old one");
}

}  // namespace

// ------------------------------------------------------------------------
// Resolver -> graph interplay, mirroring Kernel::LoadNeededPrxModules:
// a root module's DT_NEEDED entries are resolved against a directory of
// PRX files, resolved modules contribute their own edges, and the graph
// then drives a dependency-first link order.
// ------------------------------------------------------------------------
namespace {

// Simulates the kernel discovery walk: register `graph_name` with its
// needed libraries, resolve each on disk, and recurse into the resolved
// modules with dedupe via `loaded`.
void DiscoverModules(Loader::ModuleResolver& resolver,
                     Loader::ModuleGraph& graph,
                     const std::string& graph_name,
                     const std::vector<std::string>& needed,
                     const std::vector<Loader::LoadedModule>& disk_modules,
                     std::set<std::string>& loaded) {
    graph.AddModule(graph_name, needed);
    Loader::LoadedModule fake;
    fake.name = graph_name;
    fake.needed_libraries = needed;
    for (const auto& res : resolver.ResolveNeededLibraries(fake)) {
        if (!res.resolved) continue;               // HLE fallback at runtime
        if (!loaded.insert(res.name).second) continue; // dedupe / cycle guard
        // Find the fake module standing in for the resolved PRX file.
        for (const auto& dm : disk_modules) {
            if (dm.name == res.path.filename().string()) {
                DiscoverModules(resolver, graph, res.name,
                                dm.needed_libraries, disk_modules, loaded);
                break;
            }
        }
    }
}

void TestResolverGraphInterplay() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path() / "pcsx5_module_graph_tests";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    EXPECT(!ec, "created temp dir for resolver test");

    auto touch = [&](const char* name) {
        FILE* f = nullptr;
        fopen_s(&f, (dir / name).string().c_str(), "wb");
        if (f) std::fclose(f);
    };
    touch("libSceB.prx");
    touch("LIBSCEC.SPRX");  // case-insensitive match for "libSceC"

    Loader::ModuleResolver resolver;
    resolver.SetSearchDirectories({dir});

    // Sanity: resolution works (prx direct, sprx case-insensitive, missing).
    EXPECT(resolver.ResolveModuleFile("libSceB").has_value(), "libSceB resolves to libSceB.prx");
    EXPECT(resolver.ResolveModuleFile("libSceC").has_value(), "libSceC resolves to LIBSCEC.SPRX");
    EXPECT(!resolver.ResolveModuleFile("libSceMissing").has_value(), "libSceMissing unresolved -> HLE");

    // Disk modules (stand-ins for the mapped PRXs).
    Loader::LoadedModule mod_b; mod_b.name = "libSceB.prx";    mod_b.needed_libraries = {"libSceC"};
    Loader::LoadedModule mod_c; mod_c.name = "LIBSCEC.SPRX";   mod_c.needed_libraries = {};
    const std::vector<Loader::LoadedModule> disk_modules = {mod_b, mod_c};

    Loader::ModuleGraph graph;
    std::set<std::string> loaded;
    DiscoverModules(resolver, graph, "eboot.bin",
                    {"libSceB", "libSceMissing"}, disk_modules, loaded);

    Loader::ModuleGraph::CycleReport report;
    const auto order = graph.ResolveLoadOrder(&report);
    EXPECT_EQ(order.size(), (size_t)3, "interplay: graph has eboot + 2 PRX modules");
    EXPECT(ComesBefore(order, "libSceC", "libSceB"), "interplay: libSceC links before libSceB");
    EXPECT(ComesBefore(order, "libSceB", "eboot.bin"), "interplay: libSceB links before eboot.bin");
    EXPECT(report.cycles.empty(), "interplay: no cycles");
    EXPECT(report.missing == (std::vector<std::string>{"libSceMissing"}),
           "interplay: unresolved dep reported as HLE-served missing");

    fs::remove_all(dir, ec);
}

void TestResolverGraphCycle() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path() / "pcsx5_module_graph_tests";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    auto touch = [&](const char* name) {
        FILE* f = nullptr;
        fopen_s(&f, (dir / name).string().c_str(), "wb");
        if (f) std::fclose(f);
    };
    touch("libSceB.prx");
    touch("libSceC.prx");

    Loader::ModuleResolver resolver;
    resolver.SetSearchDirectories({dir});

    // B <-> C dependency cycle; eboot depends on B.
    Loader::LoadedModule mod_b; mod_b.name = "libSceB.prx"; mod_b.needed_libraries = {"libSceC"};
    Loader::LoadedModule mod_c; mod_c.name = "libSceC.prx"; mod_c.needed_libraries = {"libSceB"};
    const std::vector<Loader::LoadedModule> disk_modules = {mod_b, mod_c};

    Loader::ModuleGraph graph;
    std::set<std::string> loaded;
    DiscoverModules(resolver, graph, "eboot.bin", {"libSceB"}, disk_modules, loaded);

    // The dedupe set must have stopped the recursion: exactly B and C loaded.
    EXPECT_EQ(loaded.size(), (size_t)2, "cycle: discovery terminates via dedupe set");

    Loader::ModuleGraph::CycleReport report;
    const auto order = graph.ResolveLoadOrder(&report);
    EXPECT_EQ(order.size(), (size_t)3, "cycle: order still total");
    // The {libSceB, libSceC} cycle must be reported (possibly once per
    // stall, so search all groups rather than requiring exactly one).
    bool found_cycle = false;
    for (const auto& group : report.cycles) {
        if (group == (std::vector<std::string>{"libSceB", "libSceC"})) found_cycle = true;
    }
    EXPECT(found_cycle, "cycle: {libSceB, libSceC} reported as a cycle group");
    // Deterministic stall-breaking emits the smallest leftover name first,
    // so a dependent may precede its cyclic dependencies; only totality
    // and cycle reporting are guaranteed here.
    {
        auto sorted = order;
        std::sort(sorted.begin(), sorted.end());
        EXPECT(sorted == (std::vector<std::string>{"eboot.bin", "libSceB", "libSceC"}),
               "cycle: order contains every module once");
    }

    fs::remove_all(dir, ec);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    LogConfig::SetLevel(LogCategory::Loader, LogLevel::Warn);

    std::fprintf(stdout, "=== module_graph_tests ===\n");

    TestEmpty();
    TestSingle();
    TestLinearChain();
    TestDiamond();
    TestCycle();
    TestMissingDependency();
    TestDeterminism();
    TestReAddReplacesDependencies();
    TestResolverGraphInterplay();
    TestResolverGraphCycle();

    std::fprintf(stdout, "  %d/%d checks passed\n",
                 g_checks - g_failures, g_checks);
    if (g_failures != 0) {
        std::fprintf(stderr, "  %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::fprintf(stdout, "OK\n");
    return 0;
}
