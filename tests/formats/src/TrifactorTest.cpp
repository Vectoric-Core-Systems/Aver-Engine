// Aver.Trifactor: the cluster builder, the LOD DAG, and persisting LOD-0 clusters through .ocmesh's
// MLET chunk. CPU only, no device, no GPU -- like MeshTest and LandscapeTest, everything a
// clustering/simplification bug would show up in is checkable with nothing but this binary.
//
// A FOURTH executable rather than more cases in MeshTest, for MeshTest's own reason repeated here:
// the failure has to name the layer that broke. A wrong triangle count out of meshopt_buildMeshlets,
// a broken group-boundary lock in meshopt_simplify, and a byte-offset bug in the MLET writer/reader
// are three different bugs in three different files (ClusterBuilder.cpp, and the MLET code paths in
// OcMesh.cpp), and folding them into MeshTest's ".ocmesh round trip" section would make a Trifactor
// regression look like an OcMesh regression, or vice versa, to whoever reads the failing line first.
//
// It links Aver.Trifactor, which THIS FILE'S OWN CMake registration makes conditional
// (`if(TARGET Aver.Trifactor)` in tests/formats/CMakeLists.txt, mirroring MaterialTest/
// ActorScriptTest needing Aver.Formats.Material): Aver.Trifactor is OFF by default in this tree
// because its one allowed dependency, meshoptimizer, is not vendored at third_party/meshoptimizer
// (see modules/trifactor/CMakeLists.txt). So this executable does not exist at all in the tree's
// default configuration -- that is expected, not a bug in this file, and is exactly why step 6 of
// the task that wrote this file asks for a SEPARATE AVER_MODULE_TRIFACTOR=OFF build to confirm nothing
// else regresses when this target is simply absent.
//
// MESHES ARE SYNTHESISED IN MEMORY, never shipped as fixtures -- the same reasoning LandscapeTest's
// and MeshTest's CMakeLists.txt comments give: a fixture file would be a second source of truth for
// the geometry a bug report has to describe, and this repo can generate anything it needs to test
// against instead.
#include "aver/trifactor/ClusterBuilder.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/formats/Avr1.hpp"
#include "aver/core/Log.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

using namespace aver;

static int g_checks = 0, g_failures = 0;

// Logs one assertion and counts the checks and the failures.
static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    AVER_ERROR("  FAIL  {}", what);
    ++g_failures;
}

// ---- fixtures --------------------------------------------------------------------------------

// A flat n x n grid, two triangles per quad, in the XY plane. Large enough (n=24 -> 576 vertices,
// 1058 triangles) to need many LOD-0 meshlets and several simplification levels; flat and regular
// enough that meshopt_simplify has an enormous number of valid, harmless collapses available, so the
// hierarchy actually reduces rather than bottoming out on the first group.
static fmt::OcMeshData makeGridMesh(u32 n, f32 spacing = 100.0f) {
    fmt::OcMeshData m;
    m.positions.reserve(usize(n) * n * 3);
    m.normals.reserve(usize(n) * n * 3);
    m.uvs.reserve(usize(n) * n * 2);
    for (u32 y = 0; y < n; ++y) {
        for (u32 x = 0; x < n; ++x) {
            m.positions.insert(m.positions.end(),
                                {static_cast<f32>(x) * spacing, static_cast<f32>(y) * spacing, 0.0f});
            m.normals.insert(m.normals.end(), {0.0f, 0.0f, 1.0f});
            m.uvs.insert(m.uvs.end(), {static_cast<f32>(x) / f32(n - 1), static_cast<f32>(y) / f32(n - 1)});
        }
    }
    for (u32 y = 0; y + 1 < n; ++y) {
        for (u32 x = 0; x + 1 < n; ++x) {
            const u32 i00 = y * n + x, i10 = y * n + x + 1;
            const u32 i01 = (y + 1) * n + x, i11 = (y + 1) * n + x + 1;
            for (u32 idx : {i00, i10, i11, i00, i11, i01}) m.indices.push_back(idx);
        }
    }
    m.submeshes.push_back(fmt::OcMeshSubmesh{"grid", 0, 0, static_cast<u32>(m.indices.size()), 0, m.vertexCount()});
    m.materialSlots = {"M_Grid"};
    return m;
}

// The same grid, plus skin streams: four influences per vertex, bound to two bones by which half of
// the grid a vertex sits in. Deliberately NOT uniform -- if every vertex had identical influences,
// a remap that dropped or reordered them would still produce a plausible-looking result, and the
// test would pass while the rig was destroyed.
static fmt::OcMeshData makeSkinnedGridMesh(u32 n, f32 spacing = 100.0f) {
    fmt::OcMeshData m = makeGridMesh(n, spacing);
    const u32 v = m.vertexCount();
    m.joints.resize(usize(v) * fmt::kOcMeshInfluences, 0);
    m.weights.resize(usize(v) * fmt::kOcMeshInfluences, 0.0f);
    for (u32 i = 0; i < v; ++i) {
        const u16 bone = static_cast<u16>((i % 2 == 0) ? 0 : 1);
        m.joints [usize(i) * fmt::kOcMeshInfluences + 0] = bone;
        m.weights[usize(i) * fmt::kOcMeshInfluences + 0] = 1.0f;   // rest stay 0, as a rig may
    }
    return m;
}

// A single triangle: the smallest possible input, well under one meshlet's 64-vertex/124-triangle
// capacity. Exercises "cluster this" degenerating gracefully to "one cluster, no hierarchy above it".
static fmt::OcMeshData makeSingleTriangle() {
    fmt::OcMeshData m;
    m.positions = {0, 0, 0,  100, 0, 0,  0, 100, 0};
    m.normals   = {0, 0, 1,  0, 0, 1,    0, 0, 1};
    m.uvs       = {0, 0,     1, 0,       0, 1};
    m.indices   = {0, 1, 2};
    m.submeshes.push_back(fmt::OcMeshSubmesh{"tri", 0, 0, 3, 0, 3});
    m.materialSlots = {"M"};
    return m;
}

// A single quad (two triangles): a second "too small to cluster" shape distinct from the triangle --
// it shares an edge across two triangles, which the single-triangle fixture cannot exercise at all.
static fmt::OcMeshData makeSingleQuad() {
    fmt::OcMeshData m;
    m.positions = {0, 0, 0,  100, 0, 0,  100, 100, 0,  0, 100, 0};
    m.normals   = {0, 0, 1,  0, 0, 1,    0, 0, 1,       0, 0, 1};
    m.uvs       = {0, 0,     1, 0,       1, 1,          0, 1};
    m.indices   = {0, 1, 2,  0, 2, 3};
    m.submeshes.push_back(fmt::OcMeshSubmesh{"quad", 0, 0, 6, 0, 4});
    m.materialSlots = {"M"};
    return m;
}

// Two triangles that share an EDGE geometrically (both endpoints occupy the same position) but share
// NO INDEX -- vertices 3 and 4 sit at the exact same positions as vertices 1 and 2, but are distinct,
// duplicated vertex-buffer entries, the shape every UV island boundary in a real asset produces
// (island A and island B need their own UVs along the seam they otherwise share, so the vertex buffer
// carries two copies of each seam position). Under triangle-EDGE union-find alone this looks like two
// disconnected shells -- {0,1,2} and {3,4,5} never appear together in a triangle -- and only the
// meshopt_generatePositionRemap union in computeShellIds (ClusterBuilder.cpp) closes the gap by
// noticing vertex 3 and vertex 1 (and 4 and 2) occupy the same position. This fixture exists to prove
// that union is actually wired in, not merely argued for in a comment.
static fmt::OcMeshData makeUvSeamFixture() {
    fmt::OcMeshData m;
    m.positions = {0, 0, 0,    100, 0, 0,    0, 100, 0,      // triangle A: 0, 1, 2
                   100, 0, 0,  0, 100, 0,    100, 100, 0};   // triangle B: 3, 4, 5 -- 3~1, 4~2 by position
    m.normals   = {0, 0, 1,    0, 0, 1,      0, 0, 1,
                   0, 0, 1,    0, 0, 1,      0, 0, 1};
    m.uvs       = {0, 0,       1, 0,         0, 1,
                   1, 0,       0, 1,         1, 1};
    m.indices   = {0, 1, 2,    3, 4, 5};
    m.submeshes.push_back(fmt::OcMeshSubmesh{"seam", 0, 0, 6, 0, 6});
    m.materialSlots = {"M"};
    return m;
}

// task step 5-8 fixture: one large connected blob (a grid, `gridN` x `gridN`, far too big to be a
// small shell) plus `fragmentCount` small DISJOINT fragments -- individual two-triangle quads, each
// its own connected component, well under the 64-vertex/124-triangle cutoff. Fragments are placed far
// from the grid AND from each other (a few thousand units apart, against a grid spanning a few
// thousand units at spacing=100) so meshopt's own spatial-proximity fallbacks (groupClusters' own
// comment on meshopt_partitionClusters; buildDirectCluster's on meshopt_buildMeshlets) have no
// geometric reason to ever confuse a fragment for part of the grid, or two fragments for each other --
// this fixture is testing the SHELL-lineage routing, not relying on it to also paper over ambiguous
// geometry.
static fmt::OcMeshData makeMixedShellFixture(u32 gridN, u32 fragmentCount) {
    fmt::OcMeshData m = makeGridMesh(gridN, /*spacing=*/100.0f);
    for (u32 f = 0; f < fragmentCount; ++f) {
        const f32 ox = 100000.0f + static_cast<f32>(f) * 1000.0f;
        const f32 oy = 100000.0f;
        const u32 base = m.vertexCount();
        m.positions.insert(m.positions.end(),
                            {ox, oy, 0.0f,  ox + 50, oy, 0.0f,  ox + 50, oy + 50, 0.0f,  ox, oy + 50, 0.0f});
        m.normals.insert(m.normals.end(), {0, 0, 1,  0, 0, 1,  0, 0, 1,  0, 0, 1});
        m.uvs.insert(m.uvs.end(), {0, 0,  1, 0,  1, 1,  0, 1});
        m.indices.insert(m.indices.end(), {base, base + 1, base + 2,  base, base + 2, base + 3});
    }
    return m;
}

// toMeshlets/toIndices, the private per-level DAG-to-OcMeshMeshlet/index-buffer conversion this file
// used to duplicate from tests/formats/src/ConvertTool.cpp (having no header of its own to share one
// from), are GONE from here: both this file and ConvertTool now call aver::trifactor::packLodDag
// (ClusterBuilder.hpp) instead, which is precisely the point of lifting that conversion into the
// module -- see packLodDag's own doc comment for the full reasoning. Every "persisting ... through
// .ocmesh" section below builds `src.meshlets`/`src.coarserLods` via a single packLodDag call rather
// than hand-assembling them, so this file now exercises the SAME code ConvertTool and RelodTool's
// write path exercise, not a fourth copy that could quietly stop agreeing with the other three.

int main() {
    AVER_INFO("=== buildClusters: LOD-0 coverage and per-cluster limits ===");
    {
        const fmt::OcMeshData grid = makeGridMesh(24);
        trifactor::LodDag dag;
        std::string why;
        check(trifactor::buildClusters(grid, dag, &why), "buildClusters on a 24x24 grid: " + why);
        check(dag.levelCount() == 1, "buildClusters populates only level 0");
        check(!dag.levels.empty() && dag.levels[0].size() > 1,
              "a 1058-triangle mesh needs more than one LOD-0 meshlet (got " +
                  std::to_string(dag.levels.empty() ? 0 : dag.levels[0].size()) + ")");

        const trifactor::ValidationReport report0 = trifactor::validateLodDag(grid, dag);
        for (const auto& issue : report0.issues) AVER_ERROR("  validateLodDag: {} -- {}", issue.where, issue.detail);
        check(report0.ok, "validateLodDag passes on LOD-0-only output (coverage + limits + bounds)");
    }

    AVER_INFO("=== computeShellIds (via buildClusters): a UV seam is ONE shell, not two ===");
    {
        // THE PROPERTY THAT MAKES THE SHELL-AWARE LOCKBORDER FIX SAFE TO BUILD ON. Trifactor's shell
        // classification (task steps 2-4, ClusterBuilder.cpp) has to union vertices by POSITION, not
        // just by shared index, or a UV seam -- two triangles that share an edge geometrically but not
        // through a single shared vertex index, which every real asset with a UV island boundary has
        // -- would misclassify as two disconnected shells, and the routing (task steps 5-8, tested
        // below) could then drop meshopt_SimplifyLockBorder along a seam that is genuinely part of one
        // continuous surface.
        const fmt::OcMeshData seam = makeUvSeamFixture();
        trifactor::LodDag dag;
        std::string why;
        check(trifactor::buildClusters(seam, dag, &why), "buildClusters on the UV-seam fixture: " + why);
        check(dag.smallShells.size() == 1,
              "two triangles sharing a position through DUPLICATE (not shared) indices classify as ONE "
              "shell (got " + std::to_string(dag.smallShells.size()) + " shell(s)) -- this is what the "
              "meshopt_generatePositionRemap union in computeShellIds buys; triangle-edge union-find "
              "alone would see two");
        check(!dag.smallShells.empty() && dag.isSmallShell(0),
              "the fixture (6 vertices, 2 triangles) is well under the 64-vert/124-tri cutoff, so its "
              "one shell classifies as small");
    }

    AVER_INFO("=== task step 5: a zero-small-shell mesh's routing is exclusively the large-shell path ===");
    {
        // THE PROPERTY THAT PROTECTS dead_tree_trunk, dead_tree_trunk_02, rock_07, rock_09, and
        // rock_moss_set_02 -- the five meshes that got WORSE when LockBorder was dropped outright (see
        // this file's own header, and ClusterBuilder.cpp's buildLodHierarchy comment for the actual
        // numbers), all of which report ZERO small shells on the real corpus. buildClusters' routing
        // (ClusterBuilder.cpp) is ORDER-PRESERVING: it walks mesh.indices once and removes ONLY
        // small-shell triangles from the stream handed to meshopt_buildMeshlets, so a mesh with no
        // small shells at all has NOTHING removed -- that buffer is mesh.indices, verbatim, and the
        // meshopt_buildMeshlets call downstream is therefore the exact same call (same function, same
        // input) this file always made, not merely one that happens to agree with it. A 24x24 grid is
        // one large connected shell, far over the 64-vertex/124-triangle small-shell cutoff.
        const fmt::OcMeshData grid = makeGridMesh(24);
        trifactor::LodDag dag;
        std::string why;
        check(trifactor::buildClusters(grid, dag, &why), "buildClusters on a single-large-shell mesh: " + why);
        check(dag.smallShells.size() == 1 && !dag.isSmallShell(0), "the grid is exactly one shell, and it is large");

        bool everyClusterLargeLineage = true;
        for (u32 cid : dag.levels[0]) if (dag.clusters[cid].smallShellLineage) everyClusterLargeLineage = false;
        check(everyClusterLargeLineage,
              "not one LOD-0 cluster took the small-shell direct-build path -- every cluster came from "
              "the unmodified meshopt_buildMeshlets call on the unmodified large-shell stream");

        // The LOD-0 triangle set (as a sorted multiset of vertex triples -- the same "coverage" form
        // validateLodDag's own lod0-coverage check and the very first test in this file use, since
        // meshopt_buildMeshlets itself reorders triangles into meshlets and is not expected to
        // preserve mesh.indices' literal order) must equal mesh.indices' own triangle set exactly: the
        // routing must not have gained, lost, or duplicated a single triangle in the process of
        // deciding that none of them were small-shell.
        std::vector<std::array<u32, 3>> fromClusters, fromSource;
        for (u32 cid : dag.levels[0]) {
            const trifactor::Cluster& c = dag.clusters[cid];
            for (usize t = 0; t + 2 < c.triangles.size(); t += 3)
                fromClusters.push_back({c.vertices[c.triangles[t]], c.vertices[c.triangles[t + 1]],
                                         c.vertices[c.triangles[t + 2]]});
        }
        for (usize t = 0; t + 2 < grid.indices.size(); t += 3)
            fromSource.push_back({grid.indices[t], grid.indices[t + 1], grid.indices[t + 2]});
        std::sort(fromClusters.begin(), fromClusters.end());
        std::sort(fromSource.begin(), fromSource.end());
        check(fromClusters == fromSource,
              "LOD-0's triangles are exactly mesh.indices' triangles, as a set -- the large-shell "
              "routing path changed no geometry");

        // Determinism: buildClusters is a pure function of `mesh` -- re-running it produces an
        // IDENTICAL dag (cluster count, and every cluster's vertices/triangles/shellId/lineage), which
        // rules out the routing having introduced any hash-container-iteration-order dependency (task
        // step 5's small-shell bucketing is keyed by shellId, an ascending loop, specifically to avoid
        // this class of bug -- see buildClusters' own comment).
        trifactor::LodDag dag2;
        check(trifactor::buildClusters(grid, dag2, &why), "buildClusters a second time on the same mesh: " + why);
        bool identical = dag.clusters.size() == dag2.clusters.size() && dag.levels == dag2.levels;
        for (usize i = 0; identical && i < dag.clusters.size(); ++i) {
            const trifactor::Cluster &a = dag.clusters[i], &b = dag2.clusters[i];
            identical = a.vertices == b.vertices && a.triangles == b.triangles && a.shellId == b.shellId &&
                        a.smallShellLineage == b.smallShellLineage;
        }
        check(identical, "buildClusters on the same mesh twice produces an identical LOD-0 DAG");
    }

    AVER_INFO("=== task step 8: shell lineage survives multiple grouping/simplify passes (mixed fixture) ===");
    {
        const fmt::OcMeshData mesh = makeMixedShellFixture(/*gridN=*/28, /*fragmentCount=*/12);
        trifactor::LodDag dag;
        std::string why;
        check(trifactor::buildClusters(mesh, dag, &why), "buildClusters on the mixed-shell fixture: " + why);

        u32 largeShellCount = 0, smallShellCount = 0;
        for (u32 s = 0; s < dag.smallShells.size(); ++s) (dag.isSmallShell(s) ? smallShellCount : largeShellCount)++;
        check(largeShellCount == 1,
              "the fixture has exactly one large shell (the grid) -- got " + std::to_string(largeShellCount));
        check(smallShellCount == 12,
              "and exactly twelve small shells (the fragments) -- got " + std::to_string(smallShellCount));

        // At LOD 0, every cluster's lineage must agree with its own (representative) shell's
        // classification -- see Cluster::shellId's comment for why that agreement is guaranteed at
        // level 0 specifically (a large cluster can only ever contain large-shell triangles).
        u32 lod0SmallClusters = 0, lod0LargeClusters = 0;
        for (u32 cid : dag.levels[0]) {
            const trifactor::Cluster& c = dag.clusters[cid];
            check(c.smallShellLineage == dag.isSmallShell(c.shellId),
                  "LOD-0 cluster " + std::to_string(cid) + "'s lineage flag agrees with its shell's own "
                  "classification");
            (c.smallShellLineage ? lod0SmallClusters : lod0LargeClusters)++;
        }
        check(lod0SmallClusters == 12, "one direct LOD-0 cluster per small shell (got " +
                                            std::to_string(lod0SmallClusters) + ")");
        check(lod0LargeClusters >= 1, "the grid produced at least one large-shell LOD-0 cluster");

        check(trifactor::buildLodHierarchy(mesh, dag, &why), "buildLodHierarchy on the mixed-shell fixture: " + why);
        check(dag.levelCount() >= 4,
              "the fixture reaches at least level 3 (got " + std::to_string(dag.levelCount()) +
                  " levels) -- otherwise the regression test below cannot exercise what it claims to");

        const trifactor::ValidationReport mixedReport = trifactor::validateLodDag(mesh, dag);
        for (const auto& issue : mixedReport.issues) AVER_ERROR("  validateLodDag: {} -- {}", issue.where, issue.detail);
        check(mixedReport.ok, "validateLodDag passes on the mixed-shell hierarchy -- the routing did not "
                               "break coverage, cluster limits, error-monotonicity, or acyclicity");

        // THE STEP-8 REGRESSION TEST ITSELF. Follow one grid-descended (large-shell) LOD-0 cluster's
        // parents[0] chain upward and confirm smallShellLineage stays false -- i.e. it still routes to
        // the LockBorder path -- at EVERY level along the way, down to level 3 or deeper. Without
        // PendingGroup carrying lineage forward in buildLodHierarchy's Pass 2 (see its own comment),
        // every level >= 1 cluster would keep Cluster::smallShellLineage's default (false) regardless
        // of what actually produced it -- which would make this exact assertion pass for the WRONG
        // reason (a coincidental default, not a propagated fact) on a fixture with no small-shell
        // lineage to diverge against. The block after this one supplies that divergence.
        u32 largeLeaf = static_cast<u32>(dag.clusters.size());
        for (u32 cid : dag.levels[0]) {
            if (!dag.clusters[cid].smallShellLineage) { largeLeaf = cid; break; }
        }
        check(largeLeaf < dag.clusters.size(), "found a large-shell LOD-0 cluster to trace");

        u32 cur = largeLeaf;
        u32 deepestLevelSeen = dag.clusters[cur].level;
        bool largeLineageHeldThroughout = !dag.clusters[cur].smallShellLineage;
        for (u32 guard = 0; guard <= dag.levelCount() && !dag.clusters[cur].parents.empty(); ++guard) {
            cur = dag.clusters[cur].parents[0];
            deepestLevelSeen = std::max(deepestLevelSeen, dag.clusters[cur].level);
            if (dag.clusters[cur].smallShellLineage) largeLineageHeldThroughout = false;
        }
        check(deepestLevelSeen >= 3,
              "the traced large-shell chain actually reaches level 3 or deeper (reached " +
                  std::to_string(deepestLevelSeen) + ") -- the assertion below is meaningless otherwise");
        check(largeLineageHeldThroughout,
              "the large-shell lineage held smallShellLineage == false at every level from 0 up "
              "through " + std::to_string(deepestLevelSeen) + " -- large-shell descendants still route "
              "to the LockBorder path this deep");

        // AND THE DIVERGING CASE: at least one cluster ABOVE level 0 has smallShellLineage == true,
        // proving the field is an actually-propagated fact (true for SOME clusters at depth, not
        // merely "false everywhere, including at its own default") rather than a value the check above
        // could pass by accident.
        bool sawPropagatedSmallLineage = false;
        for (u32 level = 1; level < dag.levelCount() && !sawPropagatedSmallLineage; ++level)
            for (u32 cid : dag.levels[level])
                if (dag.clusters[cid].smallShellLineage) { sawPropagatedSmallLineage = true; break; }
        check(sawPropagatedSmallLineage,
              "at least one cluster above level 0 has smallShellLineage == true -- lineage is a "
              "propagated fact, not a name for 'still at its default'");
    }

    AVER_INFO("=== simplifyMesh keeps a skinned mesh saveable ===");
    {
        // THE REGRESSION. simplifyMesh remapped positions/normals/uvs down to the new vertex count
        // and left joints/weights sized for the old one, so hasSkin() -- which wants both at exactly
        // v*4 for the NEW v -- went false and writeOcMesh refused the mesh entirely. It made
        // `ConvertTool ... --lod <ratio>` unusable on every rigged asset, and reported it as
        // "mesh has no vertices, no indices, or mismatched attribute counts", which names everything
        // except the cause.
        fmt::OcMeshData skinned = makeSkinnedGridMesh(16);
        const u32 beforeVerts = skinned.vertexCount();
        check(skinned.hasSkin(), "the fixture starts out skinned");
        check(skinned.valid(), "...and valid before simplifying");

        std::string why;
        check(trifactor::simplifyMesh(skinned, 0.5f, &why), "simplifyMesh on a skinned mesh: " + why);

        const u32 afterVerts = skinned.vertexCount();
        check(afterVerts < beforeVerts, "the vertex count actually fell (" +
              std::to_string(beforeVerts) + " -> " + std::to_string(afterVerts) + ")");
        check(skinned.joints.size() == usize(afterVerts) * fmt::kOcMeshInfluences,
              "joints were resized with the vertex buffer, not left at the old count");
        check(skinned.weights.size() == usize(afterVerts) * fmt::kOcMeshInfluences,
              "weights were resized with the vertex buffer");
        check(skinned.hasSkin(), "the mesh is STILL skinned after simplifying");
        check(skinned.valid(), "...and still valid, which is what writeOcMesh refuses without");

        // Influences must survive as data, not merely as a correctly-sized buffer. Every surviving
        // vertex came from exactly one source vertex, so every weight should still be one of the
        // values the fixture assigned -- never a blend, and never zeroed.
        bool sawBone0 = false, sawBone1 = false, weightsIntact = true;
        for (u32 i = 0; i < afterVerts; ++i) {
            const u16 b = skinned.joints[usize(i) * fmt::kOcMeshInfluences + 0];
            const f32 w = skinned.weights[usize(i) * fmt::kOcMeshInfluences + 0];
            if (b == 0) sawBone0 = true;
            if (b == 1) sawBone1 = true;
            if (w != 1.0f) weightsIntact = false;
        }
        check(weightsIntact, "every surviving vertex kept its full weight (no blending, no zeroing)");
        check(sawBone0 && sawBone1, "both bones still have vertices bound to them");

        // The end of the road that actually failed: the writer.
        const std::string out = (std::filesystem::temp_directory_path() / "trifactor_skin.ocmesh").string();
        std::string saveWhy;
        check(fmt::saveOcMesh(out, skinned, &saveWhy), "writeOcMesh accepts the simplified skinned mesh: " + saveWhy);
        fmt::OcMeshData back;
        check(fmt::loadOcMesh(out, back, &saveWhy), "it reloads: " + saveWhy);
        check(back.hasSkin(), "the reloaded mesh still carries its skin");
        check(back.joints == skinned.joints && back.weights == skinned.weights,
              "joints and weights survive the round trip byte for byte");
        std::error_code rmec;
        std::filesystem::remove(out, rmec);
    }

    AVER_INFO("=== buildLodHierarchy: multiple levels, monotonic error, acyclic ===");
    {
        const fmt::OcMeshData grid = makeGridMesh(24);
        trifactor::LodDag dag;
        std::string why;
        check(trifactor::buildClusters(grid, dag, &why), "buildClusters: " + why);
        check(trifactor::buildLodHierarchy(grid, dag, &why), "buildLodHierarchy: " + why);

        check(dag.levelCount() >= 2,
              "a 1058-triangle flat grid produces >= 2 LOD levels (got " + std::to_string(dag.levelCount()) + ")");

        u32 maxTriCount = 0, maxVertCount = 0;
        for (const trifactor::Cluster& c : dag.clusters) {
            maxVertCount = std::max(maxVertCount, static_cast<u32>(c.vertices.size()));
            maxTriCount  = std::max(maxTriCount, c.triangleCount());
        }
        check(maxVertCount <= trifactor::kMaxClusterVertices,
              "no cluster across any level exceeds 64 vertices (worst " + std::to_string(maxVertCount) + ")");
        check(maxTriCount <= trifactor::kMaxClusterTriangles,
              "no cluster across any level exceeds 124 triangles (worst " + std::to_string(maxTriCount) + ")");

        bool monotonic = true;
        for (const trifactor::Cluster& c : dag.clusters)
            for (u32 parentId : c.parents)
                if (dag.clusters[parentId].error + 1e-6f < c.error) monotonic = false;
        check(monotonic, "error is monotonic (non-decreasing) child -> parent across every DAG edge");

        const trifactor::ValidationReport report = trifactor::validateLodDag(grid, dag);
        for (const auto& issue : report.issues) AVER_ERROR("  validateLodDag: {} -- {}", issue.where, issue.detail);
        check(report.ok, "validateLodDag passes on the full hierarchy");
    }

    AVER_INFO("=== Stage 4: fallbackAncestorId is each child's group's own nearest-centre output ===");
    {
        const fmt::OcMeshData grid = makeGridMesh(24);
        trifactor::LodDag dag;
        std::string why;
        check(trifactor::buildClusters(grid, dag, &why), "buildClusters: " + why);
        check(trifactor::buildLodHierarchy(grid, dag, &why), "buildLodHierarchy: " + why);
        check(dag.levelCount() >= 3, "the fixture needs several levels for this test to be meaningful "
                                      "(got " + std::to_string(dag.levelCount()) + ")");
        check(!dag.groupNodes.empty(), "the hierarchy actually produced group nodes to test");

        // ---- every non-root cluster has a VALID fallbackAncestorId, at exactly level+1 ------------
        const u32 topLevel = dag.levelCount() - 1;
        bool everyNonRootValid = true, everyRootInvalid = true, everyOneLevelCoarser = true;
        for (const trifactor::Cluster& c : dag.clusters) {
            if (c.level == topLevel) {
                if (c.fallbackAncestorId != fmt::kInvalidClusterId) everyRootInvalid = false;
                continue;
            }
            if (c.fallbackAncestorId == fmt::kInvalidClusterId) { everyNonRootValid = false; continue; }
            if (c.fallbackAncestorId >= dag.clusters.size()) { everyNonRootValid = false; continue; }
            if (dag.clusters[c.fallbackAncestorId].level != c.level + 1) everyOneLevelCoarser = false;
        }
        check(everyNonRootValid, "every non-root cluster has a valid (in-range) fallbackAncestorId");
        check(everyRootInvalid, "every root cluster's fallbackAncestorId stays at its default "
                                 "(kInvalidClusterId) -- a root has no coarser level to fall back to");
        check(everyOneLevelCoarser, "every fallbackAncestorId names a cluster at EXACTLY one level coarser, "
                                     "never further, never the same level");

        // ---- THE NEAREST-CENTRE CLAIM ITSELF, recomputed INDEPENDENTLY here (a plain O(members *
        // ownCount) scan, not a call into anything ClusterBuilder.cpp defines) for every group, and
        // compared against what buildLodHierarchy actually chose. This is what proves "nearest centre"
        // is the real rule, not merely documented as one. ----
        bool everyChoiceIsTrulyNearest = true;
        u32 childrenChecked = 0;
        for (const trifactor::ClusterGroupNode& g : dag.groupNodes) {
            for (u32 k = 0; k < g.childClusterCount; ++k) {
                const u32 childId = dag.groupChildren[usize(g.childClusterStart) + k];
                const Vec3& childCenter = dag.clusters[childId].bounds.sphereCenter;

                u32 bestId = fmt::kInvalidClusterId;
                f32 bestDistSq = std::numeric_limits<f32>::max();
                for (u32 o = 0; o < g.ownClusterCount; ++o) {
                    const u32 ownId = g.ownClusterStart + o;
                    const Vec3 diff = dag.clusters[ownId].bounds.sphereCenter - childCenter;
                    const f32 distSq = diff.x * diff.x + diff.y * diff.y + diff.z * diff.z;
                    if (distSq < bestDistSq) { bestDistSq = distSq; bestId = ownId; }
                }
                ++childrenChecked;
                if (dag.clusters[childId].fallbackAncestorId != bestId) everyChoiceIsTrulyNearest = false;
            }
        }
        check(childrenChecked > 0, "at least one group's children were actually checked");
        check(everyChoiceIsTrulyNearest,
              "every child's fallbackAncestorId is EXACTLY the nearest-bounding-sphere-centre member of "
              "its own group's output clusters, independently recomputed over " +
                  std::to_string(childrenChecked) + " children -- not merely A member of the group");

        std::string hierWhy;
        check(trifactor::validateClusterHierarchy(dag, &hierWhy),
              "validateClusterHierarchy passes on the full hierarchy: " + hierWhy);
    }

    AVER_INFO("=== Stage 4: a group node's sphere genuinely CONTAINS every child cluster's own sphere ===");
    {
        // A DIFFERENT fixture from the plain grid above: the mixed-shell fixture (large grid + many
        // small disjoint fragments) groups clusters that are spatially FAR APART from each other in
        // the same DAG (though never the same GROUP -- see groupClusters' own two-bucket comment), so
        // its group spheres are a much sterner test of genuine containment than a smooth, uniformly
        // spaced flat grid's would be, where every child sphere sits close to its neighbours anyway.
        const fmt::OcMeshData mesh = makeMixedShellFixture(/*gridN=*/28, /*fragmentCount=*/12);
        trifactor::LodDag dag;
        std::string why;
        check(trifactor::buildClusters(mesh, dag, &why), "buildClusters on the mixed-shell fixture: " + why);
        check(trifactor::buildLodHierarchy(mesh, dag, &why), "buildLodHierarchy: " + why);
        check(!dag.groupNodes.empty(), "the hierarchy actually produced group nodes to test");

        // NUMERIC containment, not eyeballed: centreDist + childRadius must not exceed groupRadius by
        // more than a tiny slop absorbing the handful of sequential f32 sphere-merges that produced
        // groupRadius (mergeSphere, ClusterBuilder.cpp) -- see validateClusterHierarchy's own epsilon
        // comment for why this is scaled to the sphere's own radius rather than a bare constant.
        bool everyGroupContainsEveryChild = true;
        f32 worstOverrun = 0.0f;   // how far the WORST violation (if any) exceeded groupRadius, for the report
        u32 pairsChecked = 0;
        for (const trifactor::ClusterGroupNode& g : dag.groupNodes) {
            const f32 eps = 1e-3f + g.sphereRadius * 1e-4f;
            for (u32 k = 0; k < g.childClusterCount; ++k) {
                const u32 childId = dag.groupChildren[usize(g.childClusterStart) + k];
                const trifactor::Cluster& child = dag.clusters[childId];
                const Vec3 diff = g.sphereCenter - child.bounds.sphereCenter;
                const f32 centreDist = std::sqrt(diff.x * diff.x + diff.y * diff.y + diff.z * diff.z);
                const f32 overrun = (centreDist + child.bounds.sphereRadius) - (g.sphereRadius + eps);
                ++pairsChecked;
                if (overrun > 0.0f) {
                    everyGroupContainsEveryChild = false;
                    worstOverrun = std::max(worstOverrun, overrun);
                }
            }
        }
        check(pairsChecked > 0, "at least one group/child pair was actually checked (" +
                                     std::to_string(pairsChecked) + " pairs)");
        check(everyGroupContainsEveryChild,
              "every group node's sphere numerically contains every child cluster's own sphere in full, "
              "over " + std::to_string(pairsChecked) + " group/child pairs (worst overrun " +
                  std::to_string(worstOverrun) + " if any failed)");

        std::string hierWhy;
        check(trifactor::validateClusterHierarchy(dag, &hierWhy),
              "validateClusterHierarchy independently confirms the same containment property: " + hierWhy);
    }

    AVER_INFO("=== Stage 4: ownerGroupId round-trips between a cluster and the group that produced it ===");
    {
        const fmt::OcMeshData grid = makeGridMesh(24);
        trifactor::LodDag dag;
        std::string why;
        check(trifactor::buildClusters(grid, dag, &why), "buildClusters: " + why);
        check(trifactor::buildLodHierarchy(grid, dag, &why), "buildLodHierarchy: " + why);
        check(!dag.groupNodes.empty(), "the hierarchy actually produced group nodes to test");

        // ---- LOD 0 is never produced by a group, BY CONSTRUCTION -- every one of its clusters must
        // carry the default, not merely happen to. ----
        bool everyLod0Invalid = true;
        for (const u32 cid : dag.levels[0])
            if (dag.clusters[cid].ownerGroupId != fmt::kInvalidClusterId) everyLod0Invalid = false;
        check(everyLod0Invalid, "every LOD-0 cluster's ownerGroupId stays at kInvalidClusterId -- "
                                 "buildClusters has no notion of a group at all");

        // ---- every cluster with level >= 1 has a VALID ownerGroupId, at its own level, and -- the
        // actual round trip -- that group's OWN ownClusterRange contains this exact cluster back. ----
        bool everyDeepClusterValid = true, everyGroupAtOwnLevel = true, everyRoundTripHolds = true;
        u32 deepClustersChecked = 0;
        for (const trifactor::Cluster& c : dag.clusters) {
            if (c.level == 0) continue;
            ++deepClustersChecked;
            if (c.ownerGroupId == fmt::kInvalidClusterId || c.ownerGroupId >= dag.groupNodes.size()) {
                everyDeepClusterValid = false;
                continue;
            }
            const trifactor::ClusterGroupNode& g = dag.groupNodes[c.ownerGroupId];
            if (g.level != c.level) everyGroupAtOwnLevel = false;
            const bool inRange = c.id >= g.ownClusterStart && c.id < g.ownClusterStart + g.ownClusterCount;
            if (!inRange) everyRoundTripHolds = false;
        }
        check(deepClustersChecked > 0, "at least one level->=1 cluster was actually checked (" +
                                            std::to_string(deepClustersChecked) + ")");
        check(everyDeepClusterValid, "every cluster above LOD 0 has a valid ownerGroupId");
        check(everyGroupAtOwnLevel, "every ownerGroupId names a group whose OWN level matches the cluster's");
        check(everyRoundTripHolds,
              "every cluster's ownerGroupId names the group whose ownClusterRange ACTUALLY CONTAINS that "
              "same cluster back -- the round trip a traversal's recursion depends on");

        std::string hierWhy;
        check(trifactor::validateClusterHierarchy(dag, &hierWhy),
              "validateClusterHierarchy independently confirms the same round trip: " + hierWhy);
    }

    AVER_INFO("=== persisting LOD-0 clusters through .ocmesh (MLET) ===");
    {
        const fmt::OcMeshData grid = makeGridMesh(24);
        trifactor::LodDag dag;
        std::string why;
        check(trifactor::buildClusters(grid, dag, &why), "buildClusters: " + why);

        // LOD-0-only: every cluster is level 0 == topLevel, so every one is a root by
        // computeClusterErrorBounds' definition, and every one should get parentError == FLT_MAX.
        const f32 scale = trifactor::worldExtentScale(grid);
        const std::vector<trifactor::ClusterErrorBounds> errorBounds = trifactor::computeClusterErrorBounds(dag, scale);
        std::string boundsWhy;
        check(trifactor::validateClusterErrorBounds(dag, errorBounds, &boundsWhy),
              "validateClusterErrorBounds passes on a single-level (all-root) DAG: " + boundsWhy);

        fmt::OcMeshData src = grid;
        std::string packWhy;
        check(trifactor::packLodDag(dag, src, &packWhy), "packLodDag packs the LOD-0-only DAG: " + packWhy);
        check(!src.meshlets.empty(), "the fixture actually has meshlets to persist");
        check(src.builderVersion == trifactor::kBuilderVersion,
              "packLodDag stamps builderVersion with the current builder (got " +
                  std::to_string(src.builderVersion) + ")");

        std::vector<u8> bytes;
        check(fmt::writeOcMesh(src, bytes, &why), "writes with meshlets: " + why);

        fmt::OcMeshData back;
        check(fmt::parseOcMesh(bytes.data(), bytes.size(), back, &why), "reads back: " + why);
        check((back.flags & fmt::kOcMeshMeshlets) != 0, "the HasMeshlets flag is set by the writer");
        check(back.builderVersion == src.builderVersion, "builderVersion survives the round trip bit-exact");
        check(back.meshlets.size() == src.meshlets.size(),
              "meshlet count survives (" + std::to_string(back.meshlets.size()) + " vs " +
                  std::to_string(src.meshlets.size()) + ")");

        bool everyMeshletExact = true;
        for (usize i = 0; i < src.meshlets.size() && everyMeshletExact; ++i) {
            const fmt::OcMeshMeshlet& a = src.meshlets[i];
            const fmt::OcMeshMeshlet& b = back.meshlets[i];
            everyMeshletExact = everyMeshletExact && a.vertices == b.vertices && a.triangles == b.triangles;
        }
        check(everyMeshletExact, "every meshlet's vertex/triangle lists survive exactly (u32/u8, no quantization)");

        // ownError/parentError: raw f32 in, raw f32 out (no quantization anywhere in the MLET path),
        // so this is a bit-exact check, not a tolerance one -- exactly like vertices/triangles above.
        bool everyErrorExact = true;
        for (usize i = 0; i < src.meshlets.size() && everyErrorExact; ++i)
            everyErrorExact = everyErrorExact && src.meshlets[i].ownError == back.meshlets[i].ownError &&
                               src.meshlets[i].parentError == back.meshlets[i].parentError;
        check(everyErrorExact, "every meshlet's ownError/parentError survive the round trip bit-exact");

        bool everyRootParentInfinite = true;
        for (const fmt::OcMeshMeshlet& ml : back.meshlets)
            if (ml.parentError != std::numeric_limits<f32>::max()) everyRootParentInfinite = false;
        check(everyRootParentInfinite,
              "every LOD-0-only meshlet (all root, single level) has parentError == FLT_MAX on disk");

        f32 worstBoundsErr = 0.0f;
        for (usize i = 0; i < src.meshlets.size(); ++i) {
            worstBoundsErr = std::fmax(worstBoundsErr, std::fabs(src.meshlets[i].sphereRadius - back.meshlets[i].sphereRadius));
            worstBoundsErr = std::fmax(worstBoundsErr, std::fabs(src.meshlets[i].sphereCenter.x - back.meshlets[i].sphereCenter.x));
        }
        check(worstBoundsErr < 1e-3f, "bounding sphere survives (f32 in, f32 out; worst diff " +
                                           std::to_string(worstBoundsErr) + ")");

        // ---- determinism: save -> load -> save is byte-identical ----
        std::vector<u8> bytes2;
        check(fmt::writeOcMesh(back, bytes2, &why), "re-writes the reloaded mesh: " + why);
        check(bytes == bytes2, "save -> load -> save produces byte-identical output (" +
                                    std::to_string(bytes.size()) + " vs " + std::to_string(bytes2.size()) + " bytes)");
    }

    AVER_INFO("=== persisting the FULL LOD hierarchy through .ocmesh (multi-level MLET) ===");
    {
        const fmt::OcMeshData grid = makeGridMesh(24);
        trifactor::LodDag dag;
        std::string why;
        check(trifactor::buildClusters(grid, dag, &why), "buildClusters: " + why);
        check(trifactor::buildLodHierarchy(grid, dag, &why), "buildLodHierarchy: " + why);
        check(dag.levelCount() >= 2,
              "the fixture actually produces more than one LOD level (got " + std::to_string(dag.levelCount()) + ")");

        // ---- geometric error -> ScreenErrorThreshold, and monotonicity re-checked AFTER conversion,
        // not inferred from the raw error's own (already-checked) invariant ----
        const f32 scale = trifactor::worldExtentScale(grid);
        check(scale > 0.0f, "worldExtentScale is positive for a non-degenerate mesh (" + std::to_string(scale) + ")");

        std::string monoWhy;
        check(trifactor::validateScreenErrorMonotonic(dag, scale, &monoWhy),
              "ScreenErrorThreshold stays monotonic (non-decreasing child -> parent) after conversion: " + monoWhy);

        // ---- ownError/parentError for every cluster, and the invariant the local cut test's whole
        // correctness argument rests on, validated BEFORE anything is packed (item 3 of the task) ----
        const std::vector<trifactor::ClusterErrorBounds> errorBounds = trifactor::computeClusterErrorBounds(dag, scale);
        check(errorBounds.size() == dag.clusters.size(), "computeClusterErrorBounds returns one entry per cluster");
        std::string boundsWhy;
        check(trifactor::validateClusterErrorBounds(dag, errorBounds, &boundsWhy),
              "validateClusterErrorBounds passes on the full multi-level hierarchy: " + boundsWhy);

        // ---- build the on-disk shape: LOD 0 into meshlets (as ever), every coarser level into
        // coarserLods with its own index buffer and its converted screen error -- via packLodDag
        // (ClusterBuilder.hpp), the same call ConvertTool and RelodTool's write path make, rather
        // than this file re-assembling the same structs by hand. errorBounds/scale above were
        // already computed and validated independently for this test's own assertions; packLodDag
        // recomputes them internally too, which doubles as a check that the two computations agree.
        fmt::OcMeshData src = grid;
        std::string packWhy;
        check(trifactor::packLodDag(dag, src, &packWhy), "packLodDag packs the multi-level DAG: " + packWhy);
        check(!src.coarserLods.empty(), "the fixture actually has coarser LODs to persist");
        check(src.builderVersion == trifactor::kBuilderVersion,
              "packLodDag stamps builderVersion on a multi-level hierarchy too (got " +
                  std::to_string(src.builderVersion) + ")");

        std::vector<u8> bytes;
        check(fmt::writeOcMesh(src, bytes, &why), "writes the full hierarchy: " + why);

        fmt::OcMeshData back;
        check(fmt::parseOcMesh(bytes.data(), bytes.size(), back, &why), "reads it back: " + why);
        check(back.lodCount() == src.lodCount(),
              "LOD count survives (" + std::to_string(back.lodCount()) + " vs " + std::to_string(src.lodCount()) + ")");
        check(back.coarserLods.size() == src.coarserLods.size(), "coarser LOD count survives");
        check(back.builderVersion == src.builderVersion,
              "builderVersion survives the round trip on a multi-level hierarchy too");

        bool everyLevelExact = back.coarserLods.size() == src.coarserLods.size();
        bool everyLevelSelfConsistent = true;   // meshlets reconstruct that level's OWN index buffer
        bool screenErrorNonDecreasing = true;
        bool everyLevelErrorExact = true;        // ownError/parentError survive bit-exact, every level
        f32 prevError = 0.0f;   // LOD 0's error is 0 by definition
        for (usize i = 0; i < src.coarserLods.size() && everyLevelExact; ++i) {
            const fmt::OcMeshLod& a = src.coarserLods[i];
            const fmt::OcMeshLod& b = back.coarserLods[i];
            everyLevelExact = everyLevelExact && a.indices == b.indices && a.meshlets.size() == b.meshlets.size();
            for (usize mi = 0; mi < a.meshlets.size() && everyLevelExact; ++mi)
                everyLevelExact = everyLevelExact && a.meshlets[mi].vertices == b.meshlets[mi].vertices &&
                                   a.meshlets[mi].triangles == b.meshlets[mi].triangles;
            for (usize mi = 0; mi < a.meshlets.size() && mi < b.meshlets.size(); ++mi)
                everyLevelErrorExact = everyLevelErrorExact &&
                                        a.meshlets[mi].ownError == b.meshlets[mi].ownError &&
                                        a.meshlets[mi].parentError == b.meshlets[mi].parentError;

            // "per-level meshlet ranges are correct and non-overlapping": reconstructing every
            // meshlet's global triangles at this level must reproduce that level's OWN index buffer
            // exactly, as a SET (no gaps, no duplicates) -- the same coverage check
            // validateLodDag uses for LOD 0, applied here to a coarser level's meshlets after a
            // round trip through the file.
            std::vector<std::array<u32, 3>> fromMeshlets, fromIndices;
            for (const fmt::OcMeshMeshlet& ml : b.meshlets)
                for (usize t = 0; t + 2 < ml.triangles.size(); t += 3)
                    fromMeshlets.push_back({ml.vertices[ml.triangles[t]], ml.vertices[ml.triangles[t + 1]],
                                             ml.vertices[ml.triangles[t + 2]]});
            for (usize t = 0; t + 2 < b.indices.size(); t += 3)
                fromIndices.push_back({b.indices[t], b.indices[t + 1], b.indices[t + 2]});
            std::sort(fromMeshlets.begin(), fromMeshlets.end());
            std::sort(fromIndices.begin(), fromIndices.end());
            if (fromMeshlets != fromIndices) everyLevelSelfConsistent = false;

            if (b.screenErrorThreshold + 1e-3f < prevError) screenErrorNonDecreasing = false;
            prevError = std::max(prevError, b.screenErrorThreshold);
        }
        check(everyLevelExact, "every coarser level's indices/meshlets survive the round trip exactly");
        check(everyLevelSelfConsistent,
              "every coarser level's meshlets reconstruct that level's own triangle list exactly "
              "(no gaps, no duplicates -- i.e. per-level meshlet ranges are correct and non-overlapping)");
        check(screenErrorNonDecreasing,
              "ScreenErrorThreshold is non-decreasing from LOD 0 up through every coarser level, on disk");
        check(everyLevelErrorExact,
              "every coarser level's meshlets' ownError/parentError survive the round trip bit-exact");

        // ---- ownError <= parentError, and only the root level is infinite, RE-CHECKED on the
        // round-tripped bytes (not just on the in-memory values before writing) ----
        bool decodedOwnLeParent = true, decodedRootInfinite = true, decodedNonRootFinite = true;
        for (const fmt::OcMeshMeshlet& ml : back.meshlets) {   // LOD 0: never the root, dag.levelCount() >= 2 here
            if (ml.ownError > ml.parentError + 1e-3f) decodedOwnLeParent = false;
            if (ml.parentError == std::numeric_limits<f32>::max()) decodedNonRootFinite = false;
        }
        for (usize i = 0; i < back.coarserLods.size(); ++i) {
            const bool isRootLevel = (i + 1 == back.coarserLods.size());   // coarserLods.back() is the DAG root
            for (const fmt::OcMeshMeshlet& ml : back.coarserLods[i].meshlets) {
                if (ml.ownError > ml.parentError + 1e-3f) decodedOwnLeParent = false;
                const bool isInf = ml.parentError == std::numeric_limits<f32>::max();
                if (isRootLevel && !isInf) decodedRootInfinite = false;
                if (!isRootLevel && isInf) decodedNonRootFinite = false;
            }
        }
        check(decodedOwnLeParent, "on the round-tripped bytes, ownError <= parentError for every meshlet at every level");
        check(decodedRootInfinite, "on the round-tripped bytes, every meshlet at the coarsest (root) level has parentError == FLT_MAX");
        check(decodedNonRootFinite, "on the round-tripped bytes, every meshlet below the root level has a finite parentError");

        // ---- Stage 4: fallbackAncestorId/ownerGroupId survive the round trip bit-exact, at every
        // level including LOD 0 -- the same "src vs back, field for field" style the ownError/
        // parentError check above already uses, extended to the two new MLET chunk-version-3 fields. ----
        bool everyTopologyFieldExact = src.meshlets.size() == back.meshlets.size();
        for (usize mi = 0; mi < src.meshlets.size() && mi < back.meshlets.size(); ++mi)
            everyTopologyFieldExact = everyTopologyFieldExact &&
                src.meshlets[mi].fallbackAncestorId == back.meshlets[mi].fallbackAncestorId &&
                src.meshlets[mi].ownerGroupId == back.meshlets[mi].ownerGroupId;
        for (usize i = 0; i < src.coarserLods.size() && i < back.coarserLods.size(); ++i) {
            const fmt::OcMeshLod& a = src.coarserLods[i];
            const fmt::OcMeshLod& b = back.coarserLods[i];
            for (usize mi = 0; mi < a.meshlets.size() && mi < b.meshlets.size(); ++mi)
                everyTopologyFieldExact = everyTopologyFieldExact &&
                    a.meshlets[mi].fallbackAncestorId == b.meshlets[mi].fallbackAncestorId &&
                    a.meshlets[mi].ownerGroupId == b.meshlets[mi].ownerGroupId;
        }
        check(everyTopologyFieldExact,
              "every meshlet's fallbackAncestorId/ownerGroupId survive the round trip bit-exact, LOD 0 "
              "through the coarsest level");

        // ---- ClusterGroupNode[]/groupChildren survive the round trip bit-exact too, per level ------
        bool everyGroupLevelExact = src.coarserLods.size() == back.coarserLods.size();
        u32 groupNodesComparedTotal = 0;
        for (usize i = 0; i < src.coarserLods.size() && everyGroupLevelExact; ++i) {
            const fmt::OcMeshLod& a = src.coarserLods[i];
            const fmt::OcMeshLod& b = back.coarserLods[i];
            if (a.groupNodes.size() != b.groupNodes.size()) { everyGroupLevelExact = false; continue; }
            groupNodesComparedTotal += static_cast<u32>(a.groupNodes.size());
            for (usize gi = 0; gi < a.groupNodes.size(); ++gi) {
                const fmt::OcMeshClusterGroup& ga = a.groupNodes[gi];
                const fmt::OcMeshClusterGroup& gb = b.groupNodes[gi];
                everyGroupLevelExact = everyGroupLevelExact &&
                    ga.sphereCenter.x == gb.sphereCenter.x && ga.sphereCenter.y == gb.sphereCenter.y &&
                    ga.sphereCenter.z == gb.sphereCenter.z && ga.sphereRadius == gb.sphereRadius &&
                    ga.ownClusterStart == gb.ownClusterStart && ga.ownClusterCount == gb.ownClusterCount &&
                    ga.childClusterStart == gb.childClusterStart && ga.childClusterCount == gb.childClusterCount;
            }
            if (a.groupChildren != b.groupChildren) everyGroupLevelExact = false;
        }
        check(groupNodesComparedTotal > 0, "at least one level's group nodes were actually compared (" +
                                                std::to_string(groupNodesComparedTotal) + " total)");
        check(everyGroupLevelExact,
              "every level's ClusterGroupNode[] and groupChildren survive the round trip bit-exact");

        // ---- SEMANTIC trace-back: decode the persisted, LEVEL-LOCAL fallbackAncestorId of EVERY
        // meshlet back into the DAG's own global cluster id space (using dag.levels, the same
        // contiguous-range fact packLodDag's own translation relies on) and confirm it names the SAME
        // cluster buildLodHierarchy actually chose -- not just that pack/write/read agree with EACH
        // OTHER (the bit-exact checks above), but that the whole pipeline traces back to the in-memory
        // decision the earlier "nearest-centre" Stage-4 section already proved was correct. ----
        bool everyTraceMatches = true;
        u32 traced = 0;
        for (u32 level = 0; level < dag.levelCount(); ++level) {
            const std::vector<u32>& ids = dag.levels[level];
            const std::vector<fmt::OcMeshMeshlet>& diskLevel =
                (level == 0) ? back.meshlets : back.coarserLods[usize(level) - 1].meshlets;
            if (diskLevel.size() != ids.size()) { everyTraceMatches = false; continue; }
            for (usize mi = 0; mi < ids.size(); ++mi) {
                const trifactor::Cluster& c = dag.clusters[ids[mi]];
                const u32 diskFallback = diskLevel[mi].fallbackAncestorId;
                if (c.fallbackAncestorId == fmt::kInvalidClusterId) {
                    if (diskFallback != fmt::kInvalidClusterId) everyTraceMatches = false;
                    continue;
                }
                if (level + 1 >= dag.levelCount()) { everyTraceMatches = false; continue; }
                const std::vector<u32>& nextIds = dag.levels[level + 1];
                if (diskFallback >= nextIds.size()) { everyTraceMatches = false; continue; }
                ++traced;
                if (nextIds[diskFallback] != c.fallbackAncestorId) everyTraceMatches = false;
            }
        }
        check(traced > 0, "at least one cluster's fallbackAncestorId was actually traced back to the DAG (" +
                               std::to_string(traced) + " traced)");
        check(everyTraceMatches,
              "every meshlet's on-disk, level-local fallbackAncestorId decodes back to EXACTLY the global "
              "cluster id buildLodHierarchy chose, across every level -- the pack/write/read translation "
              "is correct, not merely internally self-consistent");

        // ---- determinism: save -> load -> save is byte-identical, now for a multi-LOD mesh ----
        std::vector<u8> bytes2;
        check(fmt::writeOcMesh(back, bytes2, &why), "re-writes the reloaded multi-LOD mesh: " + why);
        check(bytes == bytes2, "save -> load -> save produces byte-identical output for a multi-LOD mesh (" +
                                    std::to_string(bytes.size()) + " vs " + std::to_string(bytes2.size()) + " bytes)");
    }

    AVER_INFO("=== backward compatibility: MLET chunk versions 1 and 2 (pre-Stage-4) still load ===");
    {
        // Every .ocmesh with meshlets ever written before Stage 2's per-cluster error fields landed
        // has a MLET chunk at version 1 (32 B MeshletBounds: Sphere+ConeApex+ConeAxis/Cutoff, no
        // OwnError/ParentError); every one written before THIS stage has version 2 (40 B: +OwnError/
        // ParentError, no FallbackAncestorId/OwnerGroupId -- see kMlChunkVersionLegacy/Errors,
        // OcMesh.cpp). The CURRENT writer always emits version 3 now (kMlChunkVersionTopology), so to
        // prove BOTH older shapes still load, each is reconstructed here the same way: write a real
        // mesh with today's writer (version 3), then splice the MLET chunk back down to what the
        // older version actually looked like and re-serialize with the container's own writeAvr1 --
        // this exercises the real AVR1 header/CRC/offset machinery rather than hand-rolling it, and
        // only touches bytes this test computed itself.
        //
        // LAYOUT FOR ONE MESHLET, ONE LOD, TODAY'S WRITER (version 3), WHICH IS WHAT THE BYTE SURGERY
        // BELOW IS COMPUTED AGAINST: GroupTable[0..24) (one LOD's worth, all-zero: this fixture's
        // single LOD-0-only DAG never grows a group -- see LodView's own comment in OcMesh.cpp for why
        // a null groupNodes still costs 24 B of all-zero table here), MeshletDesc[24..36),
        // MeshletBounds[36..84) (48 B: 32 B Sphere/Cone + 4 B OwnError [68,72) + 4 B ParentError
        // [72,76) + 4 B FallbackAncestorId [76,80) + 4 B OwnerGroupId [80,84)), MeshletVertices
        // [84..96) (3 global indices), MeshletTriangles [96..100) (3 B, padded to 4).
        //
        // THE GROUPTABLE'S 24 LEADING BYTES ARE NEVER STRIPPED, on either byte-surgery path below, and
        // that is deliberate, not an oversight: a version 1/2 reader never looks for a GroupTable
        // at all (gated on `mlet->version >= kMlChunkVersionTopology`, OcMesh.cpp), and every offset it
        // computes is relative to `meshletOffset` (from MHDR's LodDesc, a DIFFERENT chunk this test
        // never touches) rather than assumed to be chunk-byte-0 -- so those 24 B simply sit there as
        // inert, never-read padding, EXACTLY the shape a real pre-Stage-4 file's own MeshletOffset=0
        // start would have looked like to a version 1/2 reader if this test had bothered to strip them.
        // Trimming ONLY MeshletBounds' newer trailing fields (below) is therefore sufficient, and is
        // the same "erase exactly the extra tail, let the reader's own stride arithmetic land the rest
        // in the right place" trick this test always used, just at the shifted offsets Stage 4's
        // GroupTable prefix introduces.
        const fmt::OcMeshData tri = makeSingleTriangle();
        trifactor::LodDag dag;
        std::string why;
        check(trifactor::buildClusters(tri, dag, &why), "buildClusters on one triangle: " + why);
        check(dag.levels[0].size() == 1, "exactly one LOD-0 cluster -- keeps the byte surgery below simple "
                                          "(one MeshletDesc, one MeshletBounds entry)");

        fmt::OcMeshData src = tri;
        std::string packWhy;
        check(trifactor::packLodDag(dag, src, &packWhy), "packLodDag packs the single-triangle DAG: " + packWhy);
        check(src.meshlets.size() == 1, "exactly one meshlet in the MLET chunk");

        std::vector<u8> bytesV3;
        check(fmt::writeOcMesh(src, bytesV3, &why), "writes with today's writer (chunk version 3): " + why);

        const auto findMlet = [&](fmt::Avr1File& container) -> fmt::AvrChunk* {
            for (fmt::AvrChunk& c : container.chunks) if (c.id == fmt::avrFourCC("MLET")) return &c;
            return nullptr;
        };

        // ---- version 2: trim FallbackAncestorId/OwnerGroupId ([76,84), 8 B) off the bounds tail ----
        {
            fmt::Avr1File container;
            check(fmt::parseAvr1(bytesV3.data(), bytesV3.size(), container, &why), "parses the v3 container: " + why);
            fmt::AvrChunk* mlet = findMlet(container);
            check(mlet != nullptr, "the v3 file has an MLET chunk");
            check(mlet && mlet->version == 3, "the writer stamped MLET chunk version 3");
            check(mlet && mlet->data.size() >= 84, "MLET holds at least the GroupTable + one MeshletDesc + one v3 MeshletBounds");
            if (mlet && mlet->data.size() >= 84) {
                mlet->data.erase(mlet->data.begin() + 76, mlet->data.begin() + 84);
                mlet->version = 2;
            }

            std::vector<u8> bytesV2;
            check(fmt::writeAvr1(container, bytesV2, &why), "re-serializes as a version-2-style MLET container: " + why);

            fmt::OcMeshData backV2;
            check(fmt::parseOcMesh(bytesV2.data(), bytesV2.size(), backV2, &why),
                  "the reconstructed old (chunk version 2) file still loads: " + why);
            check(backV2.meshlets.size() == 1, "the meshlet survives on the v2 file");
            if (backV2.meshlets.size() == 1) {
                check(backV2.meshlets[0].vertices == src.meshlets[0].vertices &&
                      backV2.meshlets[0].triangles == src.meshlets[0].triangles,
                      "geometry survives on the v2 file, unaffected by the Stage-4 fields' absence");
                check(backV2.meshlets[0].ownError == src.meshlets[0].ownError &&
                      backV2.meshlets[0].parentError == src.meshlets[0].parentError,
                      "ownError/parentError (a version-2 field) survive on the v2 file");
                check(backV2.meshlets[0].fallbackAncestorId == fmt::kInvalidClusterId,
                      "a version-2 file's meshlet defaults fallbackAncestorId to kInvalidClusterId "
                      "(the field was never written)");
                check(backV2.meshlets[0].ownerGroupId == fmt::kInvalidClusterId,
                      "...and ownerGroupId too");
            }
        }

        // ---- version 1: ALSO trim OwnError/ParentError ([68,84), 16 B) off the bounds tail ----
        {
            fmt::Avr1File container;
            check(fmt::parseAvr1(bytesV3.data(), bytesV3.size(), container, &why), "re-parses the v3 container: " + why);
            fmt::AvrChunk* mlet = findMlet(container);
            check(mlet != nullptr, "the v3 file has an MLET chunk (second parse)");
            check(mlet && mlet->data.size() >= 84, "MLET holds at least the GroupTable + one MeshletDesc + one v3 MeshletBounds (second parse)");
            if (mlet && mlet->data.size() >= 84) {
                mlet->data.erase(mlet->data.begin() + 68, mlet->data.begin() + 84);
                mlet->version = 1;
            }

            std::vector<u8> bytesV1;
            check(fmt::writeAvr1(container, bytesV1, &why), "re-serializes as a version-1-style MLET container: " + why);

            fmt::OcMeshData backV1;
            check(fmt::parseOcMesh(bytesV1.data(), bytesV1.size(), backV1, &why),
                  "the reconstructed old (chunk version 1) file still loads: " + why);
            check(backV1.meshlets.size() == 1, "the meshlet survives on the v1 file");
            if (backV1.meshlets.size() == 1) {
                check(backV1.meshlets[0].vertices == src.meshlets[0].vertices &&
                      backV1.meshlets[0].triangles == src.meshlets[0].triangles,
                      "geometry (vertices/triangles) survives on the old file, unaffected by the newer fields' absence");
                check(backV1.meshlets[0].ownError == 0.0f,
                      "a version-1 file's meshlet defaults ownError to 0.0f (the field was never written)");
                check(backV1.meshlets[0].parentError == std::numeric_limits<f32>::max(),
                      "...and parentError to FLT_MAX -- the same safe 'always drawable, nothing finer needed' "
                      "default a root cluster gets, not whatever this test's v3 cook actually computed");
                check(backV1.meshlets[0].fallbackAncestorId == fmt::kInvalidClusterId &&
                      backV1.meshlets[0].ownerGroupId == fmt::kInvalidClusterId,
                      "a version-1 file's meshlet also defaults both Stage-4 fields to kInvalidClusterId");
            }
        }
    }

    AVER_INFO("=== the local cut test: for several pixel budgets, selected clusters cover the DAG exactly once ===");
    {
        // THE PROPERTY THAT PROVES per-cluster LOD is right, not merely plumbed: for ANY pixel
        // budget, { c : ownError(c) < budget <= parentError(c) } must select exactly one cluster
        // along every leaf's ancestry -- no gap (some region left undrawn) and no overlap (a cluster
        // AND its own ancestor both drawn, double-shading the same area).
        const fmt::OcMeshData grid = makeGridMesh(24);
        trifactor::LodDag dag;
        std::string why;
        check(trifactor::buildClusters(grid, dag, &why), "buildClusters: " + why);
        check(trifactor::buildLodHierarchy(grid, dag, &why), "buildLodHierarchy: " + why);
        check(dag.levelCount() >= 3, "the fixture needs several levels for this test to be meaningful "
                                      "(got " + std::to_string(dag.levelCount()) + ")");

        const f32 scale = trifactor::worldExtentScale(grid);
        const std::vector<trifactor::ClusterErrorBounds> bounds = trifactor::computeClusterErrorBounds(dag, scale);
        check(bounds.size() == dag.clusters.size(), "computeClusterErrorBounds returns one entry per cluster");

        std::string boundsWhy;
        check(trifactor::validateClusterErrorBounds(dag, bounds, &boundsWhy),
              "validateClusterErrorBounds passes on the real hierarchy: " + boundsWhy);

        // Restated directly here too (not only folded into validateClusterErrorBounds above), because
        // the task calls both properties out by name.
        const u32 topLevel = dag.levelCount() - 1;
        bool rootsInfinite = true, nonRootsFinite = true, ownLeParent = true;
        for (const trifactor::Cluster& c : dag.clusters) {
            const bool isRoot = (c.level == topLevel);
            const bool isInf = bounds[c.id].parentError == std::numeric_limits<f32>::max();
            if (isRoot && !isInf) rootsInfinite = false;
            if (!isRoot && isInf) nonRootsFinite = false;
            if (bounds[c.id].ownError > bounds[c.id].parentError + 1e-6f) ownLeParent = false;
        }
        check(rootsInfinite, "every root cluster's parentError is +FLT_MAX");
        check(nonRootsFinite, "every non-root cluster's parentError is finite");
        check(ownLeParent, "ownError <= parentError for every cluster");

        // Budgets: just above zero (should resolve to the finest available detail), every distinct
        // ownError value actually produced (the exact thresholds where the cut moves from one level
        // to the next -- the values most likely to expose a < vs <= off-by-one), and well past the
        // coarsest cluster's own error (should resolve to the root).
        std::vector<f32> budgets = {1e-6f};
        f32 maxOwn = 0.0f;
        for (const auto& b : bounds) {
            if (b.ownError > 0.0f) budgets.push_back(b.ownError);
            maxOwn = std::max(maxOwn, b.ownError);
        }
        budgets.push_back(maxOwn * 2.0f + 1.0f);

        const auto selected = [&](u32 cid, f32 budget) {
            return bounds[cid].ownError < budget && bounds[cid].parentError >= budget;
        };

        bool everyEdgeExclusive = true;
        bool everyLeafCoveredOnce = true;
        for (f32 budget : budgets) {
            // No overlap: no DAG edge (child, parent) has both endpoints selected. Sufficient to
            // rule out overlap between ANY ancestor pair, not just adjacent ones: parentError(child)
            // == ownError(parent) by construction, so if child and parent were both selected,
            // budget <= parentError(child) == ownError(parent) < budget from the parent's own
            // selection -- a direct contradiction. Checking every edge therefore checks every chain.
            for (const trifactor::Cluster& c : dag.clusters)
                for (u32 parentId : c.parents)
                    if (selected(c.id, budget) && selected(parentId, budget)) everyEdgeExclusive = false;

            // No gap: every LOD-0 cluster's ancestor chain (following parents[0] -- every parent from
            // the same group shares the identical propagatedError, so any one is a valid
            // representative, see computeClusterErrorBounds's doc comment) contains EXACTLY one
            // selected cluster, for this budget.
            for (u32 leafId : dag.levels[0]) {
                u32 cur = leafId;
                u32 hits = 0;
                for (u32 guard = 0; guard <= dag.levelCount(); ++guard) {
                    if (selected(cur, budget)) ++hits;
                    if (dag.clusters[cur].parents.empty()) break;
                    cur = dag.clusters[cur].parents[0];
                }
                if (hits != 1) everyLeafCoveredOnce = false;
            }
        }
        check(everyEdgeExclusive, "no DAG edge is ever selected at both ends, across " +
                                       std::to_string(budgets.size()) + " tested pixel budgets");
        check(everyLeafCoveredOnce, "every LOD-0 cluster's ancestor chain has EXACTLY ONE selected cluster "
                                     "for every tested budget -- the surface is covered exactly once, no "
                                     "gaps and no overlap");
    }

    AVER_INFO("=== compatibility: a mesh without meshlets round-trips exactly as before ===");
    {
        fmt::OcMeshData plain = makeGridMesh(4);   // no clustering call at all -- meshlets stays empty
        check(plain.meshlets.empty(), "the fixture carries no meshlets");

        std::string why;
        std::vector<u8> bytes;
        check(fmt::writeOcMesh(plain, bytes, &why), "writes without meshlets: " + why);

        fmt::OcMeshData back;
        check(fmt::parseOcMesh(bytes.data(), bytes.size(), back, &why), "reads back: " + why);
        check(back.meshlets.empty(), "no meshlets appear from nowhere");
        check((back.flags & fmt::kOcMeshMeshlets) == 0, "the HasMeshlets flag is not set");
        check(back.vertexCount() == plain.vertexCount(), "vertex count survives");
        check(back.indices == plain.indices, "indices survive exactly");
        check(back.valid(), "the decoded mesh is still self-consistent");

        // Same bytes on a second write -- this is the actual backward-compat contract: nothing about
        // this feature existing changes what a meshlet-free mesh writes.
        std::vector<u8> bytes2;
        check(fmt::writeOcMesh(back, bytes2, &why), "re-writes: " + why);
        check(bytes == bytes2, "a meshlet-free mesh's bytes are unaffected by MLET support existing");
    }

    AVER_INFO("=== degenerate input: a single triangle ===");
    {
        const fmt::OcMeshData tri = makeSingleTriangle();
        trifactor::LodDag dag;
        std::string why;
        check(trifactor::buildClusters(tri, dag, &why), "buildClusters on one triangle: " + why);
        check(dag.levelCount() == 1 && dag.levels[0].size() == 1, "exactly one LOD-0 cluster");
        check(dag.clusters[0].vertices.size() == 3 && dag.clusters[0].triangleCount() == 1,
              "the single cluster holds the whole triangle, nothing dropped or duplicated");

        check(trifactor::buildLodHierarchy(tri, dag, &why), "buildLodHierarchy does not crash on one cluster: " + why);
        check(dag.levelCount() == 1, "no bogus level is created above a single cluster (still just LOD 0)");

        const trifactor::ValidationReport report = trifactor::validateLodDag(tri, dag);
        for (const auto& issue : report.issues) AVER_ERROR("  validateLodDag: {} -- {}", issue.where, issue.detail);
        check(report.ok, "validateLodDag passes on the degenerate one-cluster DAG");
    }

    AVER_INFO("=== degenerate input: a mesh too small to cluster (one quad) ===");
    {
        const fmt::OcMeshData quad = makeSingleQuad();
        trifactor::LodDag dag;
        std::string why;
        check(trifactor::buildClusters(quad, dag, &why), "buildClusters on one quad: " + why);
        check(dag.levelCount() == 1 && dag.levels[0].size() == 1, "the whole quad fits in one meshlet");

        check(trifactor::buildLodHierarchy(quad, dag, &why), "buildLodHierarchy does not crash: " + why);
        check(dag.levelCount() == 1, "no bogus level above a single cluster");

        const trifactor::ValidationReport report = trifactor::validateLodDag(quad, dag);
        for (const auto& issue : report.issues) AVER_ERROR("  validateLodDag: {} -- {}", issue.where, issue.detail);
        check(report.ok, "validateLodDag passes");
    }

    if (g_failures == 0) AVER_INFO("=== all {} Trifactor checks passed ===", g_checks);
    else                 AVER_ERROR("=== {} of {} Trifactor checks FAILED ===", g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
