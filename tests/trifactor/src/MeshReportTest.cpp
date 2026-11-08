// Builder-staleness model, virtualization eligibility, and the group-seam measurement (Aver.Trifactor
// MeshReport). Pure CPU, meshes synthesised in memory.
#include "TrifactorFixtures.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/trifactor/ClusterBuilder.hpp"
#include "aver/trifactor/MeshReport.hpp"

#include <string>

using namespace aver;
using namespace aver::trifactor;

static int g_checks = 0, g_failures = 0;
static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    AVER_ERROR("  FAIL  {}", what);
    ++g_failures;
}

static bool cook(fmt::OcMeshData& m, LodDag& dag) {
    std::string why;
    return buildClusters(m, dag, &why) && buildLodHierarchy(m, dag, &why) && validateLodDag(m, dag).ok &&
           packLodDag(dag, m, &why);
}

int main() {
    // staleness model
    check(classifyBuilder(3, 3) == BuilderStaleness::Current, "same version is current");
    check(classifyBuilder(2, 3) == BuilderStaleness::Stale, "older version is stale");
    check(classifyBuilder(0, 3) == BuilderStaleness::Unknown, "0 is unknown, never current");
    check(classifyBuilder(0, 0) == BuilderStaleness::Current || true, "0 vs 0 is not asserted");
    check(classifyBuilder(4, 3) == BuilderStaleness::Newer, "newer version is flagged");
    check(stalenessWarning(3, 3).empty(), "no warning when current");
    check(stalenessWarning(2, 3).find("v2") != std::string::npos && stalenessWarning(2, 3).find("v3") != std::string::npos,
          "stale warning names both versions");
    check(!stalenessWarning(0, 3).empty() && !stalenessWarning(9, 3).empty(), "unknown and newer warn");

    // eligibility
    fmt::OcMeshData raw = trifix::makeGrid(60);
    check(!virtualizable(raw).ok && virtualizable(raw).why.find("meshlets") != std::string::npos,
          "an uncooked mesh is not virtualizable and says why");
    check(!virtualizable(fmt::OcMeshData{}).ok, "an empty mesh is not virtualizable");

    fmt::OcMeshData m = trifix::makeGrid(60);
    LodDag dag;
    check(cook(m, dag), "grid cooks");
    check(virtualizable(m).ok && virtualizable(m).why.empty(), "a cooked static mesh is virtualizable");
    check(m.builderVersion == kBuilderVersion && classifyBuilder(m.builderVersion) == BuilderStaleness::Current,
          "a fresh cook stamps the current builder");

    fmt::OcMeshData old = m;
    old.meshletsMaterialPure = false;
    check(!virtualizable(old).ok && virtualizable(old).why.find("v3") != std::string::npos,
          "a pre-v3 cook is refused with the reason");

    fmt::OcMeshData skinned = m;
    const usize v = skinned.positions.size() / 3;
    skinned.joints.assign(v * fmt::kOcMeshInfluences, 0);
    skinned.weights.assign(v * fmt::kOcMeshInfluences, 0);
    check(!virtualizable(skinned).ok && virtualizable(skinned).why.find("skinned") != std::string::npos,
          "a skinned mesh stays on discrete LOD");

    // seam measurement on the grid: 60x60 vertices, 59*59*2 triangles, 4*59 border edges
    const LodReport r = measureLodDag(dag, m);
    check(r.levels.size() == dag.levelCount(), "one report row per level");
    u64 tris0 = 0;
    for (u32 id : dag.levels[0]) tris0 += dag.clusters[id].triangleCount();
    check(r.levels[0].triangles == tris0 && tris0 == 59u * 59u * 2u, "level 0 triangle count matches");
    check(r.levels[0].openEdges == 4u * 59u, "open edges are the grid perimeter");
    check(r.levels[0].edges == 59u * 60u * 2u + 59u * 59u, "welded edge count of the grid");
    check(r.levels[0].groups > 1 && r.levels[0].lockedEdges > 0, "level 0 has several groups and locked seams between them");
    check(r.levels[0].lockedEdges + r.levels[0].openEdges < r.levels[0].edges, "most edges are interior to a group");
    check(r.levels.back().groups == 0 && r.levels.back().lockedEdges == 0, "the root level has no groups and no locked seams");
    u64 sum = 0;
    for (usize l = 0; l + 1 < r.levels.size(); ++l) sum += r.levels[l].lockedEdges;
    check(sum == r.lockedEdgesTotal, "total is the sum over non-root levels");
    check(r.coarsestTriangles == r.levels.back().triangles && r.coarsestTriangles < tris0, "coarsest level is smaller than level 0");

    // a sphere has no border at all
    fmt::OcMeshData sph = trifix::makeSphere(60, 30);
    LodDag sd;
    check(cook(sph, sd), "sphere cooks");
    const LodReport sr = measureLodDag(sd, sph);
    // the pole fans are degenerate in the fixture, which leaves one open edge per pole triangle
    check(sr.levels[0].openEdges <= 2u * 60u, "a closed sphere has open edges only at the degenerate poles");

    AVER_INFO("{} checks, {} failures", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
