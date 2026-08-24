// pbr::MaterialGraphRegistry -- the process-wide bookkeeping around compileMaterialGraph(), checked
// in isolation from the HLSL it emits.
//
// WHY THIS IS A SEPARATE EXECUTABLE FROM MaterialGraphTest. That file proves compileMaterialGraph()
// emits HLSL a real compiler accepts; this one proves the registry sitting on top of it hands out
// the right IDS and the right REVISIONS, and it never needs dxcompiler.dll to do that -- the
// registry never looks at whether its own hlsl() compiles, only at whether it changed. Mixing the
// two would make a missing dxcompiler.dll on some machine SKIP bookkeeping assertions that have
// nothing to do with it.
//
// EVERY TEST BUILDS ITS OWN MaterialGraphRegistry RATHER THAN USING materialGraphs(). The header is
// explicit that ids are stable "for the process", which is exactly the property a shared, ordered
// test run must not accidentally lean on: if two tests shared the singleton, the second test's
// "first id is 1" assertion would actually be testing "how many graphs the previous test added",
// and a passing suite would stop proving anything the moment someone reordered the calls below. A
// local instance gives each test the fresh-process view the class promises on id 1.
#include "aver/pbr/MaterialGraphRegistry.hpp"

#include "aver/formats/OcGraph.hpp"
#include "aver/core/Log.hpp"

#include <cstdio>
#include <string>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("   PASS  {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

// --------------------------------------------------------------------------------- graph builders
//
// Copied from MaterialGraphTest.cpp rather than shared with it: this tree keeps test files
// self-contained, so a change to one test's fixtures cannot silently change what another test is
// proving.

static fmt::OcGraphNode node(const char* id, const char* type) {
    fmt::OcGraphNode n;
    n.id = id;
    n.type = type;
    return n;
}

static void addPin(fmt::OcGraphNode& n, const char* name, const char* type, bool out,
                   const char* def = "") {
    fmt::OcGraphPin p;
    p.name = name;
    p.type = type;
    p.isOutput = out;
    p.defaultValue = def;
    n.pins.push_back(p);
}

static void link(fmt::OcGraphData& g, const char* sn, const char* sp, const char* dn, const char* dp) {
    fmt::OcGraphLink l;
    l.sourceNode = sn;
    l.sourcePin = sp;
    l.destNode = dn;
    l.destPin = dp;
    g.links.push_back(l);
}

// A MaterialOutput node with the eight surface inputs and NO defaults on any of them, matching
// MaterialGraphTest.cpp's own fixture: this is what makes "this pin carries a literal" mean "a
// person typed one" rather than a default the palette invented.
static fmt::OcGraphNode materialOutput(const char* id) {
    fmt::OcGraphNode n = node(id, "MaterialOutput");
    addPin(n, "BaseColor",   "float3", false);
    addPin(n, "Metallic",    "float",  false);
    addPin(n, "Roughness",   "float",  false);
    addPin(n, "Normal",      "float3", false);
    addPin(n, "Emissive",    "float3", false);
    addPin(n, "Occlusion",   "float",  false);
    addPin(n, "Opacity",     "float",  false);
    addPin(n, "AlphaCutoff", "float",  false);
    return n;
}

// A two-node graph that compiles: a constant colour into BaseColor. `rgb` is a parameter, not a
// constant, so the SAME shape of graph can be produced with two different bodies -- which is exactly
// what the "changed graph, same key" test below needs in order to tell a body change from a
// no-op re-add.
static fmt::OcGraphData flatColorGraph(const char* name, const char* rgb) {
    fmt::OcGraphData g;
    g.name = name;
    g.domain = "material";
    fmt::OcGraphNode c = node("colour", "ConstFloat3");
    addPin(c, "value", "float3", true, rgb);
    g.nodes.push_back(c);
    g.nodes.push_back(materialOutput("out"));
    link(g, "colour", "value", "out", "BaseColor");
    return g;
}

// The same shape, but with no DOMAIN record -- i.e. an ordinary gameplay graph, which is the
// realistic way a broken key ends up in add(): a project references the wrong .ocgraph, or one
// written before the DOMAIN record existed.
static fmt::OcGraphData gameplayGraph(const char* name) {
    fmt::OcGraphData g = flatColorGraph(name, "1,1,1");
    g.domain.clear();
    return g;
}

// The same shape again, but with a node type this build has no material emitter for -- the other
// realistic way a graph fails to compile (see MaterialGraphTest.cpp's testUnknownNodeFails, which
// proves compileMaterialGraph() itself refuses this; here it is the registry's reaction that is
// under test).
static fmt::OcGraphData unknownNodeGraph(const char* name) {
    fmt::OcGraphData g = flatColorGraph(name, "1,1,1");
    g.nodes[0].type = "CharacterMove";
    return g;
}

// --------------------------------------------------------------------------------- the tests

static void testFreshRegistryIsEmpty() {
    AVER_INFO("=== a fresh registry ===");
    pbr::MaterialGraphRegistry reg;
    check(reg.count() == 0, "count() is 0 before anything is added");
    // EMPTY IS THE SIGNAL A RENDERER USES TO NOT DEFINE AVER_MATERIAL_GRAPH AT ALL. If hlsl() were
    // ever non-empty on a registry nobody added a graph to, every renderer that gates on
    // "hlsl().empty()" would start compiling a second averEvalMaterial into projects that have never
    // heard of material graphs, for no reason any of them could see in their own content.
    check(reg.hlsl().empty(), "hlsl() is EMPTY before anything is added -- a project with no material "
                              "graphs, which is every project that exists today, must be completely "
                              "unaffected by this feature existing");
}

static void testFirstAndSecondIdsAreSequentialAndNeverZero() {
    AVER_INFO("=== ids for the first two graphs ===");
    pbr::MaterialGraphRegistry reg;
    const u32 id1 = reg.add("mat/a.ocgraph", "A", flatColorGraph("A", "1,0,0"));
    // NEVER 0 FOR A GRAPH THAT COMPILES. 0 is what a material's constant block holds when it has no
    // graph at all; if a compiling graph could also get 0, a material author who wires one up would
    // shade identically to a material with none, and nothing would say why.
    check(id1 == 1, "the first graph that compiles gets id 1, not 0");
    check(reg.count() == 1, "count() reflects the one entry");

    const u32 id2 = reg.add("mat/b.ocgraph", "B", flatColorGraph("B", "0,1,0"));
    check(id2 == 2, "the second graph gets id 2");
    check(reg.count() == 2, "count() reflects both entries");
}

static void testSameKeyTwiceReturnsSameId() {
    AVER_INFO("=== registering the same key twice ===");
    pbr::MaterialGraphRegistry reg;
    const u32 id1 = reg.add("mat/a.ocgraph", "A", flatColorGraph("A", "1,0,0"));
    const u32 id2 = reg.add("mat/a.ocgraph", "A", flatColorGraph("A", "1,0,0"));
    // THE ID MAY ALREADY BE INSIDE AN UPLOADED CONSTANT BLOCK. A reload -- a hot-reload of the same
    // .ocgraph, or a second material naming the same graph path -- must return the id it returned
    // before; renumbering it would leave every material already drawn with the old id pointing at
    // whatever the new id's case arm happens to be.
    check(id2 == id1, "re-adding the same key returns the SAME id");
    check(reg.count() == 1, "and does not create a second entry: count() is still 1");
    check(reg.idOf("mat/a.ocgraph") == id1, "idOf() agrees with what add() returned");
}

static void testIdenticalReAddDoesNotBumpRevision() {
    AVER_INFO("=== re-adding an UNCHANGED graph ===");
    pbr::MaterialGraphRegistry reg;
    reg.add("mat/a.ocgraph", "A", flatColorGraph("A", "1,0,0"));
    const u64 revBefore = reg.revision();
    const std::string hlslBefore = reg.hlsl();

    reg.add("mat/a.ocgraph", "A", flatColorGraph("A", "1,0,0"));   // same name, same body
    // A RENDERER REBUILDS ITS PIPELINES ON A REVISION CHANGE. If re-adding a graph that compiled to
    // byte-identical HLSL still bumped the revision, every editor action that merely re-saves a
    // material graph without changing it -- which is most of them -- would force every pipeline that
    // reads this registry to rebuild for nothing.
    check(reg.revision() == revBefore, "revision() does NOT move when the re-added graph is identical");
    check(reg.hlsl() == hlslBefore, "and hlsl() is unchanged too");
}

static void testChangedGraphKeepsIdButBumpsRevision() {
    AVER_INFO("=== re-adding a CHANGED graph under the same key ===");
    pbr::MaterialGraphRegistry reg;
    const u32 id1 = reg.add("mat/a.ocgraph", "A", flatColorGraph("A", "1,0,0"));
    const u64 revBefore = reg.revision();
    const std::string hlslBefore = reg.hlsl();

    const u32 id2 = reg.add("mat/a.ocgraph", "A", flatColorGraph("A", "0,0,1"));   // different colour
    check(id2 == id1, "the id stays the same: the key did not change, only what it compiles to");
    check(reg.hlsl() != hlslBefore, "hlsl() reflects the new body");
    check(reg.revision() > revBefore, "and revision() is bumped, so a renderer watching it rebuilds "
                                      "its pipelines against the new text");
}

static void testNonCompilingGraphReturnsZeroAndChangesNothing() {
    AVER_INFO("=== a graph that does not compile ===");
    {
        pbr::MaterialGraphRegistry reg;
        reg.add("mat/a.ocgraph", "A", flatColorGraph("A", "1,0,0"));   // one real entry, for contrast
        const usize countBefore = reg.count();
        const u64 revBefore = reg.revision();
        const std::string hlslBefore = reg.hlsl();

        const u32 id = reg.add("mat/bad.ocgraph", "Bad", gameplayGraph("Bad"));
        // 0 MEANS "NO GRAPH" IN THE CONSTANT BLOCK. Handing out a real id for a graph that failed to
        // compile would mean some material's constant block points at a case arm that was never
        // emitted, which is a switch falling through to nothing rather than to the documented
        // `default:`.
        check(id == 0, "a gameplay-domain graph (no DOMAIN material) returns id 0 from add()");
        check(reg.count() == countBefore, "and does not become an entry: count() is unchanged");
        check(reg.revision() == revBefore, "nor is the dispatch function rebuilt: revision() is unchanged");
        check(reg.hlsl() == hlslBefore, "hlsl() itself did not change either");
    }
    {
        pbr::MaterialGraphRegistry reg;
        reg.add("mat/a.ocgraph", "A", flatColorGraph("A", "1,0,0"));
        const usize countBefore = reg.count();
        const u64 revBefore = reg.revision();

        const u32 id = reg.add("mat/bad2.ocgraph", "Bad2", unknownNodeGraph("Bad2"));
        check(id == 0, "a graph with an unknown node type ALSO returns id 0 -- the same defect the "
                       "gameplay-domain case exercises, reached the other way compileMaterialGraph() "
                       "can fail");
        check(reg.count() == countBefore, "count() is unchanged");
        check(reg.revision() == revBefore, "revision() is unchanged");
    }
}

static void testHlslHasACaseArmPerGraphAndAlwaysADefault() {
    AVER_INFO("=== the shape of hlsl() ===");
    pbr::MaterialGraphRegistry reg;
    reg.add("mat/a.ocgraph", "A", flatColorGraph("A", "1,0,0"));
    reg.add("mat/b.ocgraph", "B", flatColorGraph("B", "0,1,0"));
    const std::string& s = reg.hlsl();
    check(s.find("case 1:") != std::string::npos, "graph 1 gets its own case arm");
    check(s.find("case 2:") != std::string::npos, "graph 2 gets its own case arm");
    // ALWAYS A default:, EVEN WITH GRAPHS REGISTERED. This is the arm every material authored before
    // this feature takes, and it is also what a graph that failed to compile (id 0) falls into; if
    // registering real graphs ever made the default arm disappear, every stock material sharing the
    // same pipeline would stop shading correctly the moment one project added its first material
    // graph.
    check(s.find("default:") != std::string::npos, "and the switch still has its default arm");
}

static void testClearEmptiesButNeverRecyclesIds() {
    AVER_INFO("=== clear() ===");
    pbr::MaterialGraphRegistry reg;
    const u32 id1 = reg.add("mat/a.ocgraph", "A", flatColorGraph("A", "1,0,0"));
    const u32 id2 = reg.add("mat/b.ocgraph", "B", flatColorGraph("B", "0,1,0"));
    const u64 revBefore = reg.revision();

    reg.clear();
    check(reg.count() == 0, "clear() empties the registry: count() is 0");
    check(reg.hlsl().empty(), "and hlsl() goes back to empty, same as a fresh registry");
    check(reg.revision() > revBefore, "clear() bumps revision(), so a renderer watching it rebuilds "
                                      "down to the stock material rather than keeping stale pipelines");

    const u32 id3 = reg.add("mat/c.ocgraph", "C", flatColorGraph("C", "0,0,1"));
    // IDS ARE NEVER RECYCLED WITHIN A PROCESS. A cleared registry cannot know whether some material's
    // constant block, uploaded before the clear, still holds id 1 or id 2; handing either of them to
    // a DIFFERENT graph would make that stale material suddenly shade as whatever was just registered.
    check(id3 > id1 && id3 > id2, "a graph added after clear() gets an id greater than every id issued "
                                  "before the clear, not id 1 again");
}

int main() {
    // UNBUFFERED, matching MaterialGraphTest.cpp's own reasoning: redirected to a file, the CRT
    // full-buffers stdout, and a run that stops mid-way reports its progress well behind where it
    // actually got to.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AVER_INFO("==================================================");
    AVER_INFO("material graph registry");

    testFreshRegistryIsEmpty();
    testFirstAndSecondIdsAreSequentialAndNeverZero();
    testSameKeyTwiceReturnsSameId();
    testIdenticalReAddDoesNotBumpRevision();
    testChangedGraphKeepsIdButBumpsRevision();
    testNonCompilingGraphReturnsZeroAndChangesNothing();
    testHlslHasACaseArmPerGraphAndAlwaysADefault();
    testClearEmptiesButNeverRecyclesIds();

    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== all material graph registry tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
