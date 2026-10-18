// GraphFindTest -- Find and Find References (sandbox/src/GraphFind.hpp): text search over ids, types, display
// names, attribute values, comments, variables and functions with its ranking and scope; stepping through
// the hits; every reference kind; and the whole-project scan over a real temporary content tree. No ImGui,
// no .NET runtime.
#include "GraphFind.hpp"

#include "aver/core/Log.hpp"

#include <filesystem>
#include <fstream>
#include <string>

using namespace aver;
using namespace aver::editor;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("   PASS  {}", what); }
    else { AVER_ERROR("   FAIL  {}", what); ++g_failures; }
}

static fmt::OcGraphData parse(const std::string& text) {
    fmt::OcGraphData g;
    std::string err;
    if (!fmt::parseOcgraph(text, g, &err)) { AVER_ERROR("   FAIL  fixture did not parse: {}", err); ++g_failures; }
    return g;
}

static void writeFile(const std::filesystem::path& p, const std::string& text) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

// One graph with a little of everything findable.
static const char* kGraph = R"(OCGRAPH 1
DOMAIN gameplay
NAME FindMe
VAR score float 0
VAR health int 100
FUNC Fire
FUNCIN Fire power float
COMMENT note1 0 0 300 100 60 70 90 Remember to tune the score curve
NODE tick OnTick 0 0
NODE sv SetVar 100 0 var=score
NODE gv GetVar 200 0 var=score
NODE gh GetVar 200 100 var=health
NODE call CallFunc 300 0 call=Fire
NODE fe FuncEntry 0 400 func=Fire
NODE evt CustomEvent 400 0 name=Boom
NODE fire FireEvent 500 0 event=Boom
NODE onev AN_OnEvent 600 0
NODE disp AN_DispatchEvent 700 0 event=Boom
NODE sp Spawn 800 0 class=Bullet
NODE sp2 Spawn 900 0 class=Rocket
ENTRY tick OnTick
ENTRY onev Boom
)";

static void testTextSearch() {
    AVER_INFO("=== findInGraph: what it matches and how it ranks ===");
    const fmt::OcGraphData g = parse(kGraph);
    const FindDisplayNameFn names = [](const std::string& type) -> std::string {
        if (type == "GetVar") return "Get Variable";
        if (type == "SetVar") return "Set Variable";
        if (type == "OnTick") return "On Tick";
        return {};
    };

    check(findInGraph(g, "", FindScope::All, names).empty(), "an empty query matches nothing");
    check(findInGraph(g, "zzzzz", FindScope::All, names).empty(), "and so does one nothing contains");

    auto hits = findInGraph(g, "TICK", FindScope::All, names);
    check(!hits.empty() && hits[0].target == FindTarget::Node && hits[0].id == "tick" && hits[0].rank == 0,
          "an exact node id (any case) ranks first");

    hits = findInGraph(g, "get variable", FindScope::All, names);
    check(hits.size() == 2 && hits[0].id == "gv" && hits[1].id == "gh" && hits[0].field == "name",
          "a palette display name matches (both GetVar nodes, graph order)");

    hits = findInGraph(g, "bullet", FindScope::All, names);
    check(hits.size() == 1 && hits[0].id == "sp" && hits[0].field == "attr:class", "an attribute VALUE matches, and says which");

    hits = findInGraph(g, "tune the", FindScope::All, names);
    check(hits.size() == 1 && hits[0].target == FindTarget::Comment && hits[0].id == "note1", "comment text matches");

    hits = findInGraph(g, "score", FindScope::All, names);
    int vars = 0, nodes = 0, comments = 0;
    for (const auto& h : hits) {
        vars += h.target == FindTarget::Variable;
        nodes += h.target == FindTarget::Node;
        comments += h.target == FindTarget::Comment;
    }
    check(vars == 1 && nodes == 2 && comments == 1, "'score' finds the variable, its two users and the comment mentioning it");

    hits = findInGraph(g, "score", FindScope::Variables, names);
    check(hits.size() == 1 && hits[0].target == FindTarget::Variable && hits[0].id == "score", "scope Variables limits it");
    hits = findInGraph(g, "fire", FindScope::Functions, names);
    check(hits.size() == 1 && hits[0].target == FindTarget::Function && hits[0].id == "Fire", "scope Functions finds a function by name");
    hits = findInGraph(g, "score", FindScope::Comments, names);
    check(hits.size() == 1 && hits[0].target == FindTarget::Comment, "scope Comments");
    hits = findInGraph(g, "fire", FindScope::Nodes, names);
    bool anyNonNode = false;
    for (const auto& h : hits) anyNonNode |= h.target != FindTarget::Node;
    check(!hits.empty() && !anyNonNode, "scope Nodes returns nodes only");

    // Ranking: exact id, then starts-with, then contains, whatever order the file lists them in.
    const fmt::OcGraphData r = parse(
        "OCGRAPH 1\nNODE bigboom Foo 0 0\nNODE boom Foo 0 0\nNODE boomer Foo 0 0\nNODE other Bar 0 0\n");
    hits = findInGraph(r, "boom");
    check(hits.size() == 3 && hits[0].id == "boom" && hits[1].id == "boomer" && hits[2].id == "bigboom",
          "exact id, then prefix, then substring");
    check(hits.size() == 3 && hits[0].rank == 0 && hits[1].rank == 1 && hits[2].rank == 2, "with ranks 0, 1, 2");
}

static void testCursor() {
    AVER_INFO("=== stepping through hits wraps at both ends ===");
    const fmt::OcGraphData g = parse(kGraph);
    FindCursor c;
    check(c.current() == nullptr && c.next() == nullptr && c.label() == "0/0", "an empty cursor has nothing and says 0/0");
    c.reset(findInGraph(g, "var", FindScope::Nodes, [](const std::string& t) { return t == "GetVar" || t == "SetVar" ? t : std::string(); }));
    check(c.hits.size() == 3, "SetVar, GetVar and GetVar match 'var', got " + std::to_string(c.hits.size()));
    check(c.index == 0 && c.label() == "1/3", "the first hit is selected: 1/3");
    c.next(); c.next();
    check(c.label() == "3/3", "next, next: 3/3");
    check(c.next() != nullptr && c.label() == "1/3", "next past the end wraps to 1/3");
    check(c.previous() != nullptr && c.label() == "3/3", "previous before the start wraps to 3/3");
    c.previous();
    check(c.label() == "2/3", "and steps back normally");
}

static void testReferences() {
    AVER_INFO("=== Find References: each kind ===");
    const fmt::OcGraphData g = parse(kGraph);
    const auto ids = [](const std::vector<RefHit>& v) {
        std::string s;
        for (const auto& h : v) s += h.nodeId + ":" + h.what + " ";
        return s;
    };

    auto v = findReferences(g, RefKind::Variable, "score");
    check(ids(v) == "sv:writes gv:reads ", "Variable: SetVar writes, GetVar reads, got " + ids(v));
    check(findReferences(g, RefKind::Variable, "nothing").empty(), "an unused name has no references");
    check(findReferences(g, RefKind::Variable, "").empty(), "an empty name has none");

    v = findReferences(g, RefKind::Function, "Fire");
    check(ids(v) == "call:calls fe:defines ", "Function: the call and the FuncEntry that defines it, got " + ids(v));

    v = findReferences(g, RefKind::Event, "Boom");
    check(ids(v) == "evt:defines fire:fires disp:dispatches onev:handles ",
          "Event: CustomEvent defines, FireEvent fires, DispatchEvent dispatches, the ENTRY-bound node handles; got " + ids(v));
    v = findReferences(g, RefKind::Event, "OnTick");
    check(ids(v) == "tick:handles ", "Event: a built-in event is found through its ENTRY, got " + ids(v));

    v = findReferences(g, RefKind::Class, "Bullet");
    check(ids(v) == "sp:spawns ", "Class: Spawn spawns it, got " + ids(v));
    check(findReferences(g, RefKind::Class, "Missing").empty(), "an unspawned class has none");

    // A node is listed once even when two rules would name it.
    const fmt::OcGraphData twice = parse("OCGRAPH 1\nNODE e CustomEvent 0 0 name=Go event=Go\nENTRY e Go\n");
    check(findReferences(twice, RefKind::Event, "Go").size() == 1, "one node, one row");
}

static void testProjectScan() {
    AVER_INFO("=== Find References across a project's Content tree ===");
    std::error_code ec;
    const std::filesystem::path root = std::filesystem::temp_directory_path(ec) / "AverGraphFindTest";
    std::filesystem::remove_all(root, ec);

    const std::string usesScore =
        "OCGRAPH 1\nDOMAIN gameplay\nNAME %s\nVAR score float 0\nNODE g GetVar 0 0 var=score\nNODE s SetVar 0 0 var=score\n";
    const auto make = [&](const char* name) {
        std::string t = usesScore;
        t.replace(t.find("%s"), 2, name);
        return t;
    };
    writeFile(root / "Content" / "b.ocgraph", make("Beta"));
    writeFile(root / "Content" / "a.ocgraph", make("Alpha"));
    writeFile(root / "Content" / "sub" / "c.ocgraph", make("Gamma"));
    writeFile(root / "Content" / "m.ocgraph",
              "OCGRAPH 1\nDOMAIN material\nNAME Mat\nVAR score float 0\nNODE g GetVar 0 0 var=score\n");
    writeFile(root / "Content" / "broken.ocgraph", "this is not a graph at all\n");
    writeFile(root / "Content" / "notes.txt", "NODE g GetVar 0 0 var=score\n");
    writeFile(root / "Content" / "other.ocgraph", "OCGRAPH 1\nNAME Other\nNODE x Add 0 0\n");

    const auto hits = findInProject((root / "Content").string(), RefKind::Variable, "score");
    check(hits.size() == 6, "a.ocgraph, b.ocgraph and sub/c.ocgraph give two hits each, got " + std::to_string(hits.size()));
    if (hits.size() == 6) {
        const auto leaf = [&](usize i) { return std::filesystem::path(hits[i].file).filename().string(); };
        check(leaf(0) == "a.ocgraph" && leaf(2) == "b.ocgraph" && leaf(4) == "c.ocgraph",
              "files come in path order (a, b, then sub/c)");
        check(hits[0].nodeId == "g" && hits[0].what == "reads" && hits[1].nodeId == "s" && hits[1].what == "writes",
              "each carries its node and what it does");
    }
    bool sawMaterial = false, sawBroken = false, sawText = false;
    for (const auto& h : hits) {
        const std::string leaf = std::filesystem::path(h.file).filename().string();
        sawMaterial |= leaf == "m.ocgraph";
        sawBroken |= leaf == "broken.ocgraph";
        sawText |= leaf == "notes.txt";
    }
    check(!sawMaterial, "a material graph's variable is not a gameplay reference");
    check(!sawBroken && !sawText, "a file that does not parse, and a non-graph file, are skipped quietly");
    check(findInProject((root / "nowhere").string(), RefKind::Variable, "score").empty(), "a missing directory has no hits");

    const std::string gamma = findGraphFileByName((root / "Content").string(), "Gamma");
    check(!gamma.empty() && std::filesystem::equivalent(gamma, root / "Content" / "sub" / "c.ocgraph", ec),
          "a graph is found by its NAME record");
    check(findGraphFileByName((root / "Content").string(), "Nope").empty(), "an unknown name finds nothing");

    std::filesystem::remove_all(root, ec);
}

int main() {
    AVER_INFO("======== GraphFindTest ========");
    testTextSearch();
    testCursor();
    testReferences();
    testProjectScan();
    if (g_failures == 0) AVER_INFO("======== GraphFindTest: all passed ========");
    else AVER_ERROR("======== GraphFindTest: {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
