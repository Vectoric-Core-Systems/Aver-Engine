// Group-shared LOD spheres, the cut oracle, and the material-pure cook (Aver.Trifactor).
//
//  1. deriveLodSpheres / validateLodSpheres: every group sphere contains its children's bounds and
//     lod spheres and its outputs' bounds; a corrupted table is caught.
//  2. The cut over random cameras is a valid antichain cut: no cluster is drawn under a drawn
//     ancestor, every LOD-0 cluster is covered, and all outputs of one group agree on whether they
//     are covered (the hole the per-cluster-sphere rule left). A hand-built DAG shows the old rule
//     failing that and the shared rule passing.
//  3. Material-pure cook: buildClusters cuts per submesh, groups never mix slots, packLodDag fills
//     SubmeshRange meshlet spans, MLET v4 round-trips, and an older file keeps loading.
//
// Pure CPU, meshes synthesised in memory.
#include "TrifactorFixtures.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/Avr1.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/trifactor/ClusterAdapt.hpp"
#include "aver/trifactor/ClusterBuilder.hpp"
#include "aver/trifactor/ClusterSelect.hpp"

#include <cstdio>
#include <utility>
#include <string>
#include <vector>

using namespace aver;

static int g_checks = 0, g_failures = 0;
static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    AVER_ERROR("  FAIL  {}", what);
    ++g_failures;
}

struct Cooked {
    fmt::OcMeshData mesh;   // source + packed ladder
    trifactor::LodDag dag;
    bool ok = false;
};

static Cooked cook(fmt::OcMeshData src) {
    Cooked c;
    std::string why;
    c.mesh = std::move(src);
    c.ok = trifactor::buildClusters(c.mesh, c.dag, &why) && trifactor::buildLodHierarchy(c.mesh, c.dag, &why) &&
           trifactor::validateLodDag(c.mesh, c.dag).ok && trifactor::packLodDag(c.dag, c.mesh, &why);
    if (!c.ok) AVER_ERROR("cook failed: {}", why);
    return c;
}

// ---- random cameras ----------------------------------------------------------------------------
struct Rng {
    u64 s = 0x9E3779B97F4A7C15ull;
    u32 next() { s = s * 6364136223846793005ull + 1442695040888963407ull; return u32(s >> 33); }
    f32 unit() { return f32(next() & 0xFFFFFF) / f32(0x1000000); }
    f32 range(f32 a, f32 b) { return a + (b - a) * unit(); }
};

static trifactor::View makeView(const Vec3& eye, f32 viewportPx, f32 fovDeg) {
    trifactor::View v;
    v.eye = eye;
    v.viewportHeightPx = viewportPx;
    v.verticalFovRadians = fovDeg * kDegToRad;
    return v;
}

// Property check of one cut. Returns the number of violations; `why` names the first.
struct CutVerdict { u32 overlaps = 0, holes = 0, splitGroups = 0; };
static CutVerdict judgeCut(const std::vector<trifactor::ClusterView>& cl, const std::vector<u32>& drawnIds) {
    const u32 n = static_cast<u32>(cl.size());
    std::vector<u8> drawn(n, 0), covered(n, 0), coveredAbove(n, 0);
    for (u32 id : drawnIds) drawn[id] = 1;
    CutVerdict v;
    // ids are level-major, so walking down from the top visits every parent before its children.
    for (u32 i = n; i-- > 0;) {
        bool above = false;
        for (u32 p : cl[i].parents) if (covered[p]) above = true;
        coveredAbove[i] = above;
        covered[i] = drawn[i] || above;
        if (drawn[i] && above) ++v.overlaps;                       // a drawn cluster under a drawn ancestor
    }
    for (u32 i = 0; i < n; ++i)
        if (cl[i].level == 0 && !covered[i]) ++v.holes;            // an LOD-0 cluster nothing draws
    // outputs of one group share the same children list; they must all be covered or all uncovered
    for (u32 i = 0; i < n; ++i) {
        if (cl[i].children.empty()) continue;
        for (u32 j = i + 1; j < n && cl[j].level == cl[i].level; ++j)
            if (cl[j].children == cl[i].children && covered[j] != covered[i]) { ++v.splitGroups; break; }
    }
    return v;
}

static void runCameras(const char* name, const fmt::OcMeshData& mesh, bool expectClean) {
    std::vector<trifactor::ClusterView> shared;
    std::string why;
    if (!trifactor::buildClusterDagViews(mesh, shared, &why)) { check(false, std::string(name) + ": DAG views: " + why); return; }
    std::vector<trifactor::ClusterView> legacy = shared;
    for (auto& c : legacy) c.lodRadius = -1.0f;   // per-cluster own spheres, the old rule

    f32 ext = 0.0f;
    for (const auto& c : shared) ext = std::max(ext, c.sphereCenter.size() + c.sphereRadius);
    Rng rng;
    CutVerdict sum, sumLegacy;
    u32 cameras = 0, inside = 0;
    for (u32 k = 0; k < 400; ++k) {
        Vec3 eye;
        if (k % 5 == 0) {   // inside / on a random cluster's sphere, the sentinel case
            const auto& c = shared[rng.next() % shared.size()];
            eye = c.sphereCenter + Vec3{rng.range(-1, 1), rng.range(-1, 1), rng.range(-1, 1)} * (c.sphereRadius * 0.5f);
            ++inside;
        } else {
            eye = Vec3{rng.range(-2, 2), rng.range(-2, 2), rng.range(-2, 2)} * ext * rng.range(0.05f, 3.0f);
        }
        const f32 px = std::vector<f32>{0.25f, 1.0f, 2.0f, 4.0f, 8.0f, 32.0f}[rng.next() % 6];
        const trifactor::View view = makeView(eye, rng.range(480.0f, 2160.0f), rng.range(30.0f, 100.0f));
        const CutVerdict a = judgeCut(shared, trifactor::selectCutIds(shared, px, view));
        const CutVerdict b = judgeCut(legacy, trifactor::selectCutIds(legacy, px, view));
        sum.overlaps += a.overlaps; sum.holes += a.holes; sum.splitGroups += a.splitGroups;
        sumLegacy.overlaps += b.overlaps; sumLegacy.holes += b.holes; sumLegacy.splitGroups += b.splitGroups;
        ++cameras;
    }
    AVER_INFO("  {}: {} cameras ({} inside spheres) | shared spheres: overlap {} hole {} split {} | own spheres (old rule): overlap {} hole {} split {}",
              name, cameras, inside, sum.overlaps, sum.holes, sum.splitGroups,
              sumLegacy.overlaps, sumLegacy.holes, sumLegacy.splitGroups);
    if (expectClean)
        check(sum.overlaps == 0 && sum.holes == 0 && sum.splitGroups == 0,
              std::string(name) + ": the cut is a valid antichain cover over random cameras");
}

// ---- a hand-built DAG: two children, one group of two outputs with different own spheres ------------
static void syntheticGroupSplit() {
    std::vector<trifactor::ClusterView> v(4);
    for (u32 i = 0; i < 4; ++i) v[i].id = i;
    v[0].level = v[1].level = 0;
    v[2].level = v[3].level = 1;
    v[0].sphereCenter = {0, 0, 0};  v[0].sphereRadius = 10.0f;
    v[1].sphereCenter = {20, 0, 0}; v[1].sphereRadius = 10.0f;
    // the group's two outputs sit at very different distances from the camera at x = 0
    v[2].sphereCenter = {0, 0, 0};      v[2].sphereRadius = 10.0f;
    v[3].sphereCenter = {5000, 0, 0};   v[3].sphereRadius = 10.0f;
    v[2].error = v[3].error = 3.0f;
    v[0].parents = v[1].parents = {2, 3};
    v[2].children = v[3].children = {0, 1};
    const trifactor::View view = makeView({0, 0, 400}, 1080.0f, 60.0f);
    const f32 thresholdPx = 4.0f;

    const CutVerdict oldRule = judgeCut(v, trifactor::selectCutIds(v, thresholdPx, view));
    check(oldRule.splitGroups > 0, "hand-built DAG: per-cluster spheres split one group (one output passes, its sibling fails)");

    // The shared rule: both outputs measured at one sphere that contains both children.
    for (u32 i : {2u, 3u}) { v[i].lodCenter = {10, 0, 0}; v[i].lodRadius = 30.0f; }
    const CutVerdict shared = judgeCut(v, trifactor::selectCutIds(v, thresholdPx, view));
    check(shared.overlaps == 0 && shared.holes == 0 && shared.splitGroups == 0,
          "hand-built DAG: one shared sphere per group makes the same cut consistent");
}

static std::vector<std::pair<std::string, fmt::OcMeshData>> corpus() {
    std::vector<std::pair<std::string, fmt::OcMeshData>> c;
    c.push_back({"grid48", trifix::makeGrid(48)});
    c.push_back({"sphere 64x32", trifix::makeSphere(64, 32)});
    {
        auto m = trifix::makeSphere(64, 32);
        trifix::splitByMaterial(m, 3, [](u32 t) { return t * 3 / 3900; });
        c.push_back({"sphere 3 bands", std::move(m)});
    }
    {
        auto m = trifix::makeGrid(40);
        trifix::addQuadFragments(m, 120);
        const u32 base = 39 * 39 * 2;
        trifix::splitByMaterial(m, 2, [&](u32 t) { return t < base ? 0u : 1u; });
        c.push_back({"grid40 + 120 cards, 2 mats", std::move(m)});
    }
    return c;
}

static void lodSphereSection() {
    AVER_INFO("=== T-CPU-1: group LOD spheres ===");
    for (auto& [name, src] : corpus()) {
        Cooked c = cook(src);
        check(c.ok, name + ": cooks and packs");
        if (!c.ok) continue;
        trifactor::LodSphereTable t;
        std::string why;
        check(trifactor::deriveLodSpheres(c.mesh, t, &why), name + ": deriveLodSpheres: " + why);
        check(t.hasTopology, name + ": the packed ladder carries group topology");
        check(trifactor::validateLodSpheres(c.mesh, t, &why), name + ": validateLodSpheres: " + why);

        // a group sphere shrunk to nothing must be caught
        if (t.hasTopology) {
            trifactor::LodSphereTable bad = t;
            bad.group[1][0].radius *= 0.01f;
            for (usize i = 0; i < c.mesh.coarserLods[0].meshlets.size(); ++i)
                if (c.mesh.coarserLods[0].meshlets[i].ownerGroupId == 0) bad.cluster[1][i] = bad.group[1][0];
            check(!trifactor::validateLodSpheres(c.mesh, bad, &why), name + ": a shrunken group sphere fails validation");
        }

        // survives the file
        std::vector<u8> bytes;
        fmt::OcMeshData back;
        check(fmt::writeOcMesh(c.mesh, bytes, &why) && fmt::parseOcMesh(bytes.data(), bytes.size(), back, &why), name + ": file round trip: " + why);
        trifactor::LodSphereTable t2;
        check(trifactor::deriveLodSpheres(back, t2, &why) && trifactor::validateLodSpheres(back, t2, &why), name + ": spheres derive and validate from the loaded file");

        // no topology (an older file): falls back to own bounds, still valid, no DAG views
        fmt::OcMeshData flat = c.mesh;
        for (auto& lod : flat.coarserLods) { lod.groupNodes.clear(); lod.groupChildren.clear(); }
        trifactor::LodSphereTable t3;
        check(trifactor::deriveLodSpheres(flat, t3, &why) && !t3.hasTopology && trifactor::validateLodSpheres(flat, t3, &why),
              name + ": without group nodes the table falls back to own bounds");
        std::vector<trifactor::ClusterView> views;
        check(!trifactor::buildClusterDagViews(flat, views, &why), name + ": no DAG views without group topology");
    }
}

static void cutSection() {
    AVER_INFO("=== T-CPU-2: the cut is an antichain cover ===");
    syntheticGroupSplit();
    for (auto& [name, src] : corpus()) {
        Cooked c = cook(src);
        if (!c.ok) { check(false, name + ": cooks"); continue; }
        runCameras(name.c_str(), c.mesh, /*expectClean=*/true);
    }
}

static void materialSection() {
    AVER_INFO("=== T-CPU-3: material-pure cook ===");
    for (auto& [name, src] : corpus()) {
        Cooked c = cook(src);
        if (!c.ok) { check(false, name + ": cooks"); continue; }
        const fmt::OcMeshData& m = c.mesh;
        check(m.builderVersion == trifactor::kBuilderVersion && trifactor::kBuilderVersion == 3, name + ": stamped builder version 3");
        check(m.meshletsMaterialPure, name + ": flagged material-pure");

        // SubmeshRange spans tile LOD 0's meshlets, in order, one slot each
        u32 next = 0;
        bool tile = true, slotsOk = true;
        for (const fmt::OcMeshSubmesh& sm : m.submeshes) {
            if (sm.meshletStart != next) tile = false;
            next = sm.meshletStart + sm.meshletCount;
            for (u32 i = sm.meshletStart; i < sm.meshletStart + sm.meshletCount && i < m.meshlets.size(); ++i)
                if (m.meshlets[i].materialSlot != sm.materialSlot) slotsOk = false;
        }
        check(tile && next == m.meshlets.size(), name + ": submesh meshlet spans tile LOD 0 exactly");
        check(slotsOk, name + ": every LOD-0 meshlet carries its submesh's slot");
        for (const fmt::OcMeshSubmesh& sm : m.submeshes)
            check(sm.meshletCount > 0, name + ": submesh '" + sm.name + "' has meshlets");

        // every coarser meshlet shares the slot of its group's children
        bool grouped = true;
        for (usize L = 0; L < m.coarserLods.size(); ++L) {
            const fmt::OcMeshLod& lod = m.coarserLods[L];
            const auto& kids = L == 0 ? m.meshlets : m.coarserLods[L - 1].meshlets;
            for (const auto& g : lod.groupNodes) {
                for (u32 k = 0; k < g.childClusterCount; ++k)
                    for (u32 o = 0; o < g.ownClusterCount; ++o)
                        if (kids[lod.groupChildren[g.childClusterStart + k]].materialSlot != lod.meshlets[g.ownClusterStart + o].materialSlot) grouped = false;
            }
        }
        check(grouped, name + ": no group mixes material slots");

        // the file keeps it
        std::vector<u8> bytes;
        fmt::OcMeshData back;
        std::string why;
        check(fmt::writeOcMesh(m, bytes, &why) && fmt::parseOcMesh(bytes.data(), bytes.size(), back, &why), name + ": round trip: " + why);
        check(back.meshletsMaterialPure, name + ": the loaded file is material-pure");
        bool same = back.meshlets.size() == m.meshlets.size() && back.submeshes.size() == m.submeshes.size();
        for (usize i = 0; same && i < m.meshlets.size(); ++i) same = back.meshlets[i].materialSlot == m.meshlets[i].materialSlot;
        for (usize i = 0; same && i < m.submeshes.size(); ++i)
            same = back.submeshes[i].meshletStart == m.submeshes[i].meshletStart && back.submeshes[i].meshletCount == m.submeshes[i].meshletCount;
        check(same, name + ": slots and spans survive the round trip");

        // a v3 file (same ladder, written without the material flag) still loads, with the fields absent
        fmt::OcMeshData v3 = m;
        v3.meshletsMaterialPure = false;
        std::vector<u8> bytes3;
        fmt::OcMeshData back3;
        check(fmt::writeOcMesh(v3, bytes3, &why) && fmt::parseOcMesh(bytes3.data(), bytes3.size(), back3, &why), name + ": v3 round trip: " + why);
        bool absent = !back3.meshletsMaterialPure && back3.meshlets.size() == m.meshlets.size();
        for (const auto& sm : back3.submeshes) absent = absent && sm.meshletStart == 0 && sm.meshletCount == 0;
        for (const auto& ml : back3.meshlets) absent = absent && ml.materialSlot == 0;
        check(absent, name + ": an older (v3) file loads with the material fields absent");
        check(back3.coarserLods.size() == m.coarserLods.size() && !back3.coarserLods.empty() && !back3.coarserLods[0].groupNodes.empty(),
              name + ": the v3 file keeps its group topology");
    }

    // a one-material mesh is a single range and has no slot to split on
    {
        Cooked c = cook(trifix::makeGrid(48));
        check(c.ok && c.mesh.submeshes.size() == 1 && c.mesh.submeshes[0].meshletStart == 0 &&
                  c.mesh.submeshes[0].meshletCount == c.mesh.meshlets.size(),
              "one material: the single submesh spans every LOD-0 meshlet");
    }

    // a small shell split across two submeshes is treated as large (borders stay locked)
    {
        auto m = trifix::makeGrid(8);   // 98 triangles, 64 vertices: one meshlet, one small shell
        trifix::splitByMaterial(m, 2, [](u32 t) { return t < 49 ? 0u : 1u; });
        trifactor::LodDag dag;
        std::string why;
        check(trifactor::buildClusters(m, dag, &why), "split small shell: buildClusters: " + why);
        bool anySmall = false;
        for (u8 s : dag.smallShells) anySmall = anySmall || s;
        check(!anySmall, "a small shell spanning two submeshes is demoted to large");
        check(dag.levels[0].size() == 2 && dag.clusters[dag.levels[0][0]].materialSlot != dag.clusters[dag.levels[0][1]].materialSlot,
              "and gets one cluster per submesh");
        check(trifactor::buildLodHierarchy(m, dag, &why) && trifactor::validateLodDag(m, dag).ok, "and the ladder validates");
    }

    // a table that does not partition the index buffer is refused
    {
        auto m = trifix::makeGrid(8);
        trifix::splitByMaterial(m, 2, [](u32 t) { return t < 49 ? 0u : 1u; });
        m.submeshes[1].indexCount -= 3;
        trifactor::LodDag dag;
        std::string why;
        check(!trifactor::buildClusters(m, dag, &why), "a submesh table that leaves triangles out is refused: " + why);
    }

    // packLodDag refuses a table of another length than the one the dag was built from
    {
        auto m = trifix::makeGrid(8);
        trifix::splitByMaterial(m, 2, [](u32 t) { return t < 49 ? 0u : 1u; });
        trifactor::LodDag dag;
        std::string why;
        trifactor::buildClusters(m, dag, &why);
        trifactor::buildLodHierarchy(m, dag, &why);
        fmt::OcMeshData other = m;
        other.submeshes.push_back(other.submeshes.back());
        check(!trifactor::packLodDag(dag, other, &why), "packLodDag refuses a mesh with a different submesh count");
    }

    // validateLodDag catches a mislabelled cluster
    {
        auto m = trifix::makeGrid(30);
        trifix::splitByMaterial(m, 2, [](u32 t) { return t % 2; });
        trifactor::LodDag dag;
        std::string why;
        trifactor::buildClusters(m, dag, &why);
        check(trifactor::validateLodDag(m, dag).ok, "interleaved two-material grid validates");
        dag.clusters[dag.levels[0][0]].submesh ^= 1u;
        check(!trifactor::validateLodDag(m, dag).ok, "a cluster naming the wrong submesh fails validation");
    }
}

int main() {
    lodSphereSection();
    cutSection();
    materialSection();
    if (g_failures) { AVER_ERROR("=== {} of {} cluster-cut checks FAILED ===", g_failures, g_checks); return 1; }
    AVER_INFO("=== all {} cluster-cut checks passed ===", g_checks);
    return 0;
}
