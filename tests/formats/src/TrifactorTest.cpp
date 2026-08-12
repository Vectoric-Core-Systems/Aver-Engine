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

// Converts one LOD level of a DAG into the on-disk OcMeshMeshlet shape -- the same conversion
// tests/formats/src/ConvertTool.cpp does, duplicated rather than shared because this file has no
// header of its own to put a shared helper in. `errorBounds` is
// trifactor::computeClusterErrorBounds(dag, scale)'s output, indexed by Cluster::id.
static std::vector<fmt::OcMeshMeshlet> toMeshlets(const trifactor::LodDag& dag, u32 level,
                                                    const std::vector<trifactor::ClusterErrorBounds>& errorBounds) {
    std::vector<fmt::OcMeshMeshlet> out;
    if (level >= dag.levels.size()) return out;
    out.reserve(dag.levels[level].size());
    for (u32 cid : dag.levels[level]) {
        const trifactor::Cluster& c = dag.clusters[cid];
        fmt::OcMeshMeshlet ml;
        ml.vertices     = c.vertices;
        ml.triangles    = c.triangles;
        ml.sphereCenter = c.bounds.sphereCenter;
        ml.sphereRadius = c.bounds.sphereRadius;
        ml.coneApex     = c.bounds.coneApex;
        ml.coneAxis[0]  = c.bounds.coneAxis[0];
        ml.coneAxis[1]  = c.bounds.coneAxis[1];
        ml.coneAxis[2]  = c.bounds.coneAxis[2];
        ml.coneCutoff   = c.bounds.coneCutoff;
        ml.ownError     = errorBounds[cid].ownError;
        ml.parentError  = errorBounds[cid].parentError;
        out.push_back(std::move(ml));
    }
    return out;
}

// Converts one LOD level's clusters into a plain GLOBAL-index triangle list -- the level's own index
// buffer, for OcMeshLod::indices. A cluster's `triangles` are LOCAL indices into its own `vertices`
// (the on-disk MLET shape); this undoes that. Duplicated from tests/formats/src/ConvertTool.cpp's
// own toIndices for the same reason toMeshlets above is duplicated: this file has no header of its
// own to share one from.
static std::vector<u32> toIndices(const trifactor::LodDag& dag, u32 level) {
    std::vector<u32> out;
    if (level >= dag.levels.size()) return out;
    for (u32 cid : dag.levels[level]) {
        const trifactor::Cluster& c = dag.clusters[cid];
        for (usize t = 0; t + 2 < c.triangles.size(); t += 3) {
            out.push_back(c.vertices[c.triangles[t + 0]]);
            out.push_back(c.vertices[c.triangles[t + 1]]);
            out.push_back(c.vertices[c.triangles[t + 2]]);
        }
    }
    return out;
}

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
        // -- would misclassify as two disconnected shells and the eventual routing (not yet built in
        // this stage -- see Cluster::shellId's STAGE STATUS comment in ClusterBuilder.hpp) could drop
        // meshopt_SimplifyLockBorder along a seam that is genuinely part of one continuous surface.
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
        src.meshlets = toMeshlets(dag, 0, errorBounds);
        check(!src.meshlets.empty(), "the fixture actually has meshlets to persist");

        std::vector<u8> bytes;
        check(fmt::writeOcMesh(src, bytes, &why), "writes with meshlets: " + why);

        fmt::OcMeshData back;
        check(fmt::parseOcMesh(bytes.data(), bytes.size(), back, &why), "reads back: " + why);
        check((back.flags & fmt::kOcMeshMeshlets) != 0, "the HasMeshlets flag is set by the writer");
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
        // coarserLods with its own index buffer and its converted screen error ----
        fmt::OcMeshData src = grid;
        src.meshlets = toMeshlets(dag, 0, errorBounds);
        src.coarserLods.clear();
        for (u32 level = 1; level < dag.levelCount(); ++level) {
            fmt::OcMeshLod lod;
            lod.indices  = toIndices(dag, level);
            lod.meshlets = toMeshlets(dag, level, errorBounds);
            f32 rawError = 0.0f;
            for (u32 cid : dag.levels[level]) rawError = std::max(rawError, dag.clusters[cid].error);
            lod.screenErrorThreshold = trifactor::toScreenErrorThreshold(rawError, scale);
            src.coarserLods.push_back(std::move(lod));
        }
        check(!src.coarserLods.empty(), "the fixture actually has coarser LODs to persist");

        std::vector<u8> bytes;
        check(fmt::writeOcMesh(src, bytes, &why), "writes the full hierarchy: " + why);

        fmt::OcMeshData back;
        check(fmt::parseOcMesh(bytes.data(), bytes.size(), back, &why), "reads it back: " + why);
        check(back.lodCount() == src.lodCount(),
              "LOD count survives (" + std::to_string(back.lodCount()) + " vs " + std::to_string(src.lodCount()) + ")");
        check(back.coarserLods.size() == src.coarserLods.size(), "coarser LOD count survives");

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

        // ---- determinism: save -> load -> save is byte-identical, now for a multi-LOD mesh ----
        std::vector<u8> bytes2;
        check(fmt::writeOcMesh(back, bytes2, &why), "re-writes the reloaded multi-LOD mesh: " + why);
        check(bytes == bytes2, "save -> load -> save produces byte-identical output for a multi-LOD mesh (" +
                                    std::to_string(bytes.size()) + " vs " + std::to_string(bytes2.size()) + " bytes)");
    }

    AVER_INFO("=== backward compatibility: an old (MLET chunk version 1, no error fields) file still loads ===");
    {
        // Every .ocmesh with meshlets ever written before this feature has a MLET chunk at version 1
        // (32 B MeshletBounds, no OwnError/ParentError -- see kMlChunkVersionLegacy, OcMesh.cpp).
        // The CURRENT writer always emits version 2 now, so to prove an old file still loads, one is
        // reconstructed here: write a real mesh with today's writer (version 2), then splice the
        // MLET chunk back down to what version 1 actually looked like and re-serialize with the
        // container's own writeAvr1 -- this exercises the real AVR1 header/CRC/offset machinery
        // rather than hand-rolling it, and only touches bytes this test computed itself.
        const fmt::OcMeshData tri = makeSingleTriangle();
        trifactor::LodDag dag;
        std::string why;
        check(trifactor::buildClusters(tri, dag, &why), "buildClusters on one triangle: " + why);
        check(dag.levels[0].size() == 1, "exactly one LOD-0 cluster -- keeps the byte surgery below simple "
                                          "(one MeshletDesc, one MeshletBounds entry)");

        const f32 scale = trifactor::worldExtentScale(tri);
        const std::vector<trifactor::ClusterErrorBounds> errorBounds = trifactor::computeClusterErrorBounds(dag, scale);

        fmt::OcMeshData src = tri;
        src.meshlets = toMeshlets(dag, 0, errorBounds);
        check(src.meshlets.size() == 1, "exactly one meshlet in the MLET chunk");

        std::vector<u8> bytesV2;
        check(fmt::writeOcMesh(src, bytesV2, &why), "writes with today's writer (chunk version 2): " + why);

        fmt::Avr1File container;
        check(fmt::parseAvr1(bytesV2.data(), bytesV2.size(), container, &why), "parses the v2 container: " + why);

        fmt::AvrChunk* mlet = nullptr;
        for (fmt::AvrChunk& c : container.chunks) if (c.id == fmt::avrFourCC("MLET")) mlet = &c;
        check(mlet != nullptr, "the v2 file has an MLET chunk");
        check(mlet && mlet->version == 2, "the writer stamped MLET chunk version 2");
        // Layout for one meshlet, one LOD: MeshletDesc[0..12), MeshletBounds[12..52) (40 B v2:
        // 32 B bounds/cone + 8 B OwnError/ParentError), MeshletVertices/Triangles after that. The two
        // error floats are MeshletBounds' trailing 8 B, at [44, 52).
        check(mlet && mlet->data.size() >= 52, "MLET holds at least one MeshletDesc + one v2 MeshletBounds");
        if (mlet && mlet->data.size() >= 52) {
            mlet->data.erase(mlet->data.begin() + 44, mlet->data.begin() + 52);
            mlet->version = 1;
        }

        std::vector<u8> bytesV1;
        check(fmt::writeAvr1(container, bytesV1, &why), "re-serializes as a version-1-style MLET container: " + why);

        fmt::OcMeshData backV1;
        check(fmt::parseOcMesh(bytesV1.data(), bytesV1.size(), backV1, &why),
              "the reconstructed old (chunk version 1) file still loads: " + why);
        check(backV1.meshlets.size() == 1, "the meshlet survives on the old file");
        if (backV1.meshlets.size() == 1) {
            check(backV1.meshlets[0].vertices == src.meshlets[0].vertices &&
                  backV1.meshlets[0].triangles == src.meshlets[0].triangles,
                  "geometry (vertices/triangles) survives on the old file, unaffected by the newer fields' absence");
            check(backV1.meshlets[0].ownError == 0.0f,
                  "a version-1 file's meshlet defaults ownError to 0.0f (the field was never written)");
            check(backV1.meshlets[0].parentError == std::numeric_limits<f32>::max(),
                  "...and parentError to FLT_MAX -- the same safe 'always drawable, nothing finer needed' "
                  "default a root cluster gets, not whatever this test's v2 cook actually computed");
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
