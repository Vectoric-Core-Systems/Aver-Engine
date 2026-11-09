// PcgGraphEditorTest -- the PCG Graph tab as an asset, headless (sandbox/src/PcgGraphEditor.cpp compiled without ImGui): the factory claims only
// `DOMAIN pcg` files, an untouched open and save is byte-identical, edits go through undo and redo, parameters write tokens, the preview counts points,
// Generate and Clean up reach the host, and Debug Points publishes a cross per point of the selected node.
#include "PcgGraphEditor.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

using namespace aver;
namespace ed = aver::editor;
namespace fs = std::filesystem;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static void writeFile(const fs::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary);
    out << text;
}

static std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static const char* kPcg =
    "OCGRAPH 1\nDOMAIN pcg\nNAME Test\n# a comment the editor must keep\n"
    "NODE grid CreatePointsGrid 0 0 spacing=500\nPIN grid out out points\n"
    "NODE keep DensityFilter 200 0 min=0.5\nPIN keep in in points\nPIN keep inside out points\nPIN keep outside out points\n"
    "NODE trees WeightedMeshSpawner 400 0 meshes=Meshes/tree.ocmesh:1\nPIN trees in in points\nPIN trees out out points\n"
    "LINK grid.out keep.in\nLINK keep.inside trees.in\n";

int main() {
    AVER_INFO("==================================================");
    AVER_INFO("PCG graph editor");
    const fs::path dir = fs::temp_directory_path() / "aver-pcg-editor-test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    const std::string pcgPath = (dir / "Scatter.ocgraph").string();
    const std::string gameplayPath = (dir / "Game.ocgraph").string();
    writeFile(pcgPath, kPcg);
    writeFile(gameplayPath, "OCGRAPH 1\nNAME Game\n");

    check(ed::makePcgGraphEditor(gameplayPath) == nullptr, "a gameplay graph is left to the graph editor");
    check(ed::makePcgGraphEditor((dir / "missing.ocgraph").string()) == nullptr && ed::makePcgGraphEditor((dir / "x.txt").string()) == nullptr, "a missing file or another extension is declined");
    auto base = ed::makePcgGraphEditor(pcgPath);
    check(base != nullptr, "a DOMAIN pcg graph is claimed");
    auto* tab = static_cast<ed::PcgGraphEditor*>(base.get());
    check(tab->loaded() && !tab->dirty() && tab->title() == "Scatter.ocgraph" && tab->graph().nodes.size() == 3, "it opens clean with its nodes");

    std::string why;
    check(tab->save(&why) && readFile(pcgPath) == kPcg, "an untouched open and save is byte-identical (comment kept)");

    std::string made;
    check(tab->addNode("SelfPrune", 600, 0, &made) && made == "selfprune" && tab->dirty() && tab->selectedNode() == "selfprune", "add a node: it is selected and the tab is dirty");
    check(!tab->addNode("Bogus", 0, 0), "an unknown type is refused");
    check(tab->linkPins("keep", "outside", made, "in", &why), "link a filter output to the new node");
    check(!tab->linkPins("grid", "out", made, "in", &why) && !why.empty(), "a taken input is refused with a reason");
    check(tab->graph().links.size() == 3, "three links");
    tab->undo();
    check(tab->graph().links.size() == 2, "undo removes the link");
    tab->undo();
    check(tab->graph().nodes.size() == 3 && tab->selectedNode().empty(), "undo removes the node and drops the selection");
    tab->redo(); tab->redo();
    check(tab->graph().nodes.size() == 4 && tab->graph().links.size() == 3, "redo restores both");

    check(tab->setParam("selfprune", "radius", "300") && ed::getNodeAttribute(tab->graph().nodes[3], "radius").value == "300", "a parameter writes a token");
    check(!tab->setParam("selfprune", "radius", "300"), "setting the same value is a no-op (no undo entry)");
    check(tab->setParam("selfprune", "radius", "0") && !ed::getNodeAttribute(tab->graph().nodes[3], "radius").found, "the schema default removes the token");
    check(!tab->setParam("ghost", "radius", "1"), "an unknown node is refused");

    tab->select("selfprune");
    check(tab->removeSelected() && tab->graph().nodes.size() == 3 && tab->graph().links.size() == 2, "remove the selection and its links");
    check(!tab->removeSelected(), "nothing selected, nothing removed");
    check(tab->unlink(0) && tab->graph().links.size() == 1 && !tab->unlink(5), "unlink by index");
    tab->undo();

    // preview
    tab->setAreaHalfCm(2500.0f);
    const auto& r = tab->run();
    check(r.ran && r.ok && r.instances > 0, "the preview runs");
    check(r.pointCount.count("grid") && r.pointCount.at("grid") >= 80, "the grid's points are counted (" + std::to_string(r.pointCount.count("grid") ? r.pointCount.at("grid") : 0) + ")");

    // debug points
    tab->select("grid");
    tab->setDebugPoints(true);
    check(tab->debugPointsOn() && ed::pcgDebugSegments().size() == r.pointCount.at("grid") * 6, "debug points publish three segments per point");
    check(tab->debugPointsOfSelection().size() == r.pointCount.at("grid"), "debugPointsOfSelection gives the points");
    tab->setDebugPoints(false);
    check(ed::pcgDebugSegments().empty(), "turning it off clears the overlay");

    // generate / clean up through a fake host
    std::string gotPath;
    usize gotRows = 0;
    bool cleaned = false;
    ed::PcgEditorHost host;
    host.publish = [&](const fmt::OcInstanceData& t, const std::vector<std::string>& mats, const std::string& graphPath, std::string*) {
        gotPath = graphPath;
        gotRows = t.transforms.size() / 12;
        return std::string("Foliage/Scatter_pcg.ocinst") + (mats.size() == t.groups.size() ? "" : "?");
    };
    host.clean = [&](const std::string&, std::string*) { cleaned = true; return true; };
    ed::setPcgEditorHost(host);
    std::string rel;
    check(tab->generate(&rel, &why) && rel == "Foliage/Scatter_pcg.ocinst" && gotPath == pcgPath && gotRows == r.instances, "Generate hands the table to the host (" + why + ")");
    check(tab->cleanUp(&why) && cleaned, "Clean up reaches the host");
    ed::setPcgEditorHost({});
    check(!tab->generate(&rel, &why) && !tab->cleanUp(&why), "without a host both are refused");

    // save writes positions only for what the user placed
    check(tab->save(&why) && !tab->dirty(), "save after edits");
    const std::string saved = readFile(pcgPath);
    fmt::OcGraphData back;
    check(fmt::parseOcgraph(saved, back, nullptr) && back.nodes.size() == 3 && saved.find("# a comment the editor must keep") != std::string::npos, "the saved file parses and keeps the comment");

    base.reset();
    fs::remove_all(dir, ec);
    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
