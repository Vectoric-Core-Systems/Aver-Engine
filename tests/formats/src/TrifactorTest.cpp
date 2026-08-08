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
#include "aver/core/Log.hpp"

#include <algorithm>
#include <array>
#include <cmath>
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

// Converts one LOD level of a DAG into the on-disk OcMeshMeshlet shape -- the same conversion
// tests/formats/src/ConvertTool.cpp does, duplicated rather than shared because this file has no
// header of its own to put a shared helper in, and the conversion is eight lines.
static std::vector<fmt::OcMeshMeshlet> toMeshlets(const trifactor::LodDag& dag, u32 level) {
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

        fmt::OcMeshData src = grid;
        src.meshlets = toMeshlets(dag, 0);
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

        // ---- build the on-disk shape: LOD 0 into meshlets (as ever), every coarser level into
        // coarserLods with its own index buffer and its converted screen error ----
        fmt::OcMeshData src = grid;
        src.meshlets = toMeshlets(dag, 0);
        src.coarserLods.clear();
        for (u32 level = 1; level < dag.levelCount(); ++level) {
            fmt::OcMeshLod lod;
            lod.indices  = toIndices(dag, level);
            lod.meshlets = toMeshlets(dag, level);
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
        f32 prevError = 0.0f;   // LOD 0's error is 0 by definition
        for (usize i = 0; i < src.coarserLods.size() && everyLevelExact; ++i) {
            const fmt::OcMeshLod& a = src.coarserLods[i];
            const fmt::OcMeshLod& b = back.coarserLods[i];
            everyLevelExact = everyLevelExact && a.indices == b.indices && a.meshlets.size() == b.meshlets.size();
            for (usize mi = 0; mi < a.meshlets.size() && everyLevelExact; ++mi)
                everyLevelExact = everyLevelExact && a.meshlets[mi].vertices == b.meshlets[mi].vertices &&
                                   a.meshlets[mi].triangles == b.meshlets[mi].triangles;

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

        // ---- determinism: save -> load -> save is byte-identical, now for a multi-LOD mesh ----
        std::vector<u8> bytes2;
        check(fmt::writeOcMesh(back, bytes2, &why), "re-writes the reloaded multi-LOD mesh: " + why);
        check(bytes == bytes2, "save -> load -> save produces byte-identical output for a multi-LOD mesh (" +
                                    std::to_string(bytes.size()) + " vs " + std::to_string(bytes2.size()) + " bytes)");
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
