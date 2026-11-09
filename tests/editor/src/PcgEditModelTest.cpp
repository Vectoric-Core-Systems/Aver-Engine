// PcgEditModelTest -- the PCG graph editor's rules without a window (sandbox/src/PcgEditModel.hpp, header-only): the parameter schema covers every
// evaluator node, the palette searches, nodes and links are added and removed with the evaluator's pins, parameters drop out when they equal the
// default, a preview run counts points per node, and a node's output points come back for the viewport debug draw.
#include "PcgEditModel.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <set>
#include <string>

using namespace aver;
namespace pe = aver::editor::pcged;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static void testSchema() {
    AVER_INFO("=== schema ===");
    bool all = true;
    std::set<std::string> seen;
    for (const world::PcgNodeDef& d : world::pcgNodeDefs()) {
        if (!pe::specOf(d.type)) { all = false; AVER_ERROR("no schema for {}", d.type); }
        seen.insert(d.type);
    }
    check(all, "every evaluator node has a schema entry");
    bool known = true;
    for (const pe::NodeSpec& s : pe::specs()) known &= seen.count(s.type) != 0;
    check(known, "and every schema entry names a node the evaluator has");
    bool unique = true;
    for (const pe::NodeSpec& s : pe::specs()) {
        std::set<std::string> keys;
        for (const pe::ParamSpec& p : s.params) unique &= keys.insert(p.key).second;
    }
    check(unique, "no node declares a key twice");
    check(pe::declaredKeys("SelfPrune").size() == 4 && pe::declaredKeys("Nope").empty(), "declaredKeys follows the schema");
}

static void testPalette() {
    AVER_INFO("=== palette ===");
    const auto all = pe::palette();
    check(all.size() == world::pcgNodeDefs().size(), "an empty search lists every node");
    bool sorted = true;
    for (usize i = 1; i < all.size(); ++i) sorted &= all[i - 1].category <= all[i].category;
    check(sorted, "grouped by category");
    const auto prune = pe::palette("prune");
    check(!prune.empty() && prune[0].type == "SelfPrune", "search matches the type");
    check(pe::palette("SPAWNER").size() >= 2, "case-insensitive, and the category matches too");
    check(pe::palette("zzzz-not-a-node").empty(), "no match is an empty list");
    bool spawnerSink = false;
    for (const auto& e : pe::palette("WeightedMeshSpawner")) spawnerSink |= e.sink && e.inputs == 1;
    check(spawnerSink, "entries carry pin counts and the sink flag");
}

static void testEditing() {
    AVER_INFO("=== nodes, links, params ===");
    fmt::OcGraphData g;
    std::string grid, keep, trees;
    check(pe::addNode(g, "CreatePointsGrid", 0, 0, &grid) && g.domain == "pcg" && grid == "createpointsgrid", "the first node sets the PCG domain and gets an id");
    pe::addNode(g, "DensityFilter", 200, 0, &keep);
    pe::addNode(g, "WeightedMeshSpawner", 400, 0, &trees);
    std::string grid2;
    pe::addNode(g, "CreatePointsGrid", 0, 100, &grid2);
    check(grid2 == "createpointsgrid2", "ids stay unique");
    check(!pe::addNode(g, "NotANode", 0, 0), "an unknown type is refused");
    check(g.nodes[0].pins.size() == 1 && g.nodes[0].pins[0].isOutput && g.nodes[0].pins[0].type == "points", "a node carries the evaluator's pins");

    check(fmt::OcGraphData{}.domain.empty(), "a fresh graph has no domain");
    check(editor::tryAddLink(g, grid, "out", keep, "in").ok, "an output links to an input");
    check(!editor::tryAddLink(g, grid2, "out", keep, "in").ok, "a taken input refuses a second link");
    check(editor::tryAddLink(g, keep, "inside", trees, "in").ok, "filter output to spawner");
    check(!editor::tryAddLink(g, trees, "out", grid, "out").ok, "output to output is refused");
    check(g.links.size() == 2, "two links");

    check(pe::removeLink(g, 0) && g.links.size() == 1 && !pe::removeLink(g, 9), "remove a link");
    editor::tryAddLink(g, grid, "out", keep, "in");
    check(pe::removeNode(g, keep) && g.links.empty() && !pe::hasNode(g, keep) && !pe::removeNode(g, keep), "removing a node removes its links");

    fmt::OcGraphNode& n = g.nodes[0];
    pe::setParam(n, "spacing", "300");
    check(editor::getNodeAttribute(n, "spacing").value == "300", "a changed value is written");
    pe::setParam(n, "spacing", "200");
    check(!editor::getNodeAttribute(n, "spacing").found, "the schema default removes the token");
    pe::setParam(n, "z", "10");
    pe::setParam(n, "z", "");
    check(!editor::getNodeAttribute(n, "z").found, "an empty value removes it too");
    n.extraTokens.push_back("custom=1");
    pe::setParam(n, "spacing", "250");
    check(editor::getNodeAttribute(n, "custom").value == "1", "an unknown token survives an edit to another");
    const auto rows = editor::computeAttributeRows(n, pe::declaredKeys(n.type));
    check(rows.size() == 3 && rows[0].key == "spacing" && rows[0].present && rows[1].key == "z" && !rows[1].present && rows[2].key == "custom" && !rows[2].declared,
          "the inspector rows list declared keys, then leftovers");
    check(pe::pinColor("points") != pe::pinColor("float"), "points have their own pin colour");
}

static fmt::OcGraphData scatterGraph() {
    fmt::OcGraphData g;
    std::string grid, keep, trees;
    pe::addNode(g, "CreatePointsGrid", 0, 0, &grid);
    pe::addNode(g, "DensityFilter", 200, 0, &keep);
    pe::addNode(g, "WeightedMeshSpawner", 400, 0, &trees);
    pe::setParam(g.nodes[0], "spacing", "500");
    pe::setParam(g.nodes[2], "meshes", "Meshes/tree.ocmesh:1");
    editor::tryAddLink(g, grid, "out", keep, "in");
    editor::tryAddLink(g, keep, "inside", trees, "in");
    return g;
}

static void testPreview() {
    AVER_INFO("=== preview and debug points ===");
    const fmt::OcGraphData g = scatterGraph();
    check(pe::problems(g).empty(), "the scatter graph validates");
    pe::PreviewSetup s;
    s.seed = 3;
    s.bounds = world::PcgBox{Vec3{0, 0, -1000}, Vec3{5000, 5000, 1000}};
    const pe::PreviewResult r = pe::runPreview(g, s);
    check(r.ran && r.ok && r.instances > 0, "the preview runs and spawns instances");
    check(r.pointCount.count("createpointsgrid") && r.pointCount.at("createpointsgrid") >= 90 && r.pointCount.at("createpointsgrid") <= 121, "the grid's point count is reported for the inspector");
    check(r.nodesRun >= 3, "nodes run counted");
    const pe::PreviewResult again = pe::runPreview(g, s);
    check(again.instances == r.instances && again.pointCount == r.pointCount, "deterministic for the same seed");

    const auto pts = pe::debugPoints(g, "createpointsgrid", "out", s);
    check(pts.size() == r.pointCount.at("createpointsgrid"), "the debug tap returns the pin's points");
    check(pe::debugPoints(g, "createpointsgrid", "nope", s).size() == 0 && pe::debugPoints(g, "ghost", "out", s).size() == 0, "an unknown pin or node returns nothing");
    check(g.nodes.size() == 3, "the debug tap does not touch the graph");

    fmt::OcGraphData bad = g;
    bad.nodes.push_back(world::pcgMakeNode("loose", "DensityFilter"));
    bad.nodes.back().type = "Bogus";
    const pe::PreviewResult refused = pe::runPreview(bad, s);
    check(!refused.ran && !refused.diags.empty(), "a graph with a problem is reported, not run");
    fmt::OcGraphData empty;
    check(!pe::problems(empty).empty() || !pe::runPreview(empty, s).ok || true, "an empty graph does not crash");
}

int main() {
    AVER_INFO("==================================================");
    AVER_INFO("PCG graph editing model");
    testSchema();
    testPalette();
    testEditing();
    testPreview();
    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
