// W4 PCG vegetation: the biome rule nodes (noise mask, edge falloff, clump, compete), the biome -> graph compiler on a
// synthetic heightfield + weightmap, Generate Vegetation (cell-sorted .ocinst, record, manual overrides, regenerate),
// chunk-edge ownership through GeneratedChunkSource, and the guardrails. No GPU, no window. Exit code = failure count.
#include "aver/core/Log.hpp"
#include "aver/world/ChunkCodec.hpp"
#include "aver/world/ChunkGenerator.hpp"
#include "aver/world/PcgVegetation.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <tuple>

using namespace aver;
using namespace aver::world;

static int g_checks = 0, g_failures = 0;
static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

// z = 100 + 0.2 x (a ~11.3 degree tilt). Layer "grass" weight ramps 1 -> 0 over x in [-1000, 1000]; "rock" is the rest.
struct Hill final : ILandscapeQuery {
    bool heightAt(f64 x, f64, f32& z) const override { z = static_cast<f32>(100.0 + 0.2 * x); return true; }
    f32 layerWeight(std::string_view layer, f64 x, f64) const override {
        const f32 g = static_cast<f32>(std::min(1.0, std::max(0.0, 0.5 - x / 2000.0)));
        return layer == "grass" ? g : layer == "rock" ? 1.0f - g : 0.0f;
    }
};
// Flat for x < 0, a 30 degree slope for x >= 0.
struct Ridge final : ILandscapeQuery {
    bool heightAt(f64 x, f64, f32& z) const override { z = static_cast<f32>(x > 0.0 ? 0.57735 * x : 0.0); return true; }
};
struct Flat final : ILandscapeQuery {
    bool heightAt(f64, f64, f32& z) const override { z = 0.0f; return true; }
    f32 layerWeight(std::string_view layer, f64, f64) const override { return layer == "meadow" ? 1.0f : 0.0f; }
};

struct B {
    fmt::OcGraphData g;
    B() { g.domain = "pcg"; g.name = "t"; }
    B& n(const std::string& id, const std::string& type, std::vector<std::string> p = {}) { g.nodes.push_back(pcgMakeNode(id, type, p)); return *this; }
    B& l(const std::string& a, const std::string& ap, const std::string& b, const std::string& bp) { g.links.push_back({a, ap, b, bp}); return *this; }
};

static PcgContext ctxFor(f32 x0, f32 y0, f32 x1, f32 y1, const ILandscapeQuery* land = nullptr, i32 seed = 5) {
    PcgContext c;
    c.seed = seed;
    c.bounds.min = Vec3{x0, y0, -1e5f};
    c.bounds.max = Vec3{x1, y1, 1e5f};
    c.landscape = land;
    return c;
}

// Grid -> [steps...] -> spawner. Returns the spawner's instances' positions.
struct Step { std::string id, type; std::vector<std::string> params; std::string outPin = "out"; };
static PcgResult runChain(const std::string& gridSpacing, const std::vector<Step>& steps, const PcgContext& c) {
    B b;
    b.n("grid", "CreatePointsGrid", {"spacing=" + gridSpacing, "z=0"});
    std::string tail = "grid", pin = "out";
    for (const Step& s : steps) {
        b.n(s.id, s.type, s.params).l(tail, pin, s.id, "in");
        tail = s.id;
        pin = s.outPin;
    }
    b.n("spawn", "WeightedMeshSpawner", {"meshes=m.ocmesh:1"}).l(tail, pin, "spawn", "in");
    return pcgEvaluate(b.g, c);
}

static std::vector<Vec3> positions(const PcgResult& r) {
    std::vector<Vec3> v;
    for (const auto& g : r.groups) for (const auto& i : g.items) v.push_back(i.xf.position);
    std::sort(v.begin(), v.end(), [](const Vec3& a, const Vec3& b) { return std::tie(a.x, a.y, a.z) < std::tie(b.x, b.y, b.z); });
    return v;
}
static bool samePos(const std::vector<Vec3>& a, const std::vector<Vec3>& b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].z != b[i].z) return false;
    return true;
}
static usize countIn(const std::vector<Vec3>& p, f32 x0, f32 x1) {
    usize n = 0;
    for (const Vec3& q : p) if (q.x >= x0 && q.x < x1) ++n;
    return n;
}
static bool hasError(const PcgResult& r) { for (const auto& d : r.diags) if (d.error) return true; return false; }

static fmt::OcFoliageData treeType() {
    fmt::OcFoliageData t;
    t.meshPath = "Meshes/tree.ocmesh";
    t.material = "M_Tree";
    t.scaleMin = 0.8f; t.scaleMax = 1.2f;
    t.collisionRadiusCm = 300.0f;
    t.alignToNormal = true;
    t.alignMaxAngleDeg = 20.0f;
    t.groundOffsetCm = -15.0f;
    t.cullStartCm = 3000.0f; t.cullEndCm = 8000.0f;
    t.lodScaleBias = 1.5f;
    t.collision = fmt::OcFoliageData::Collision::Trunk;
    return t;
}
static fmt::OcFoliageData grassType() {
    fmt::OcFoliageData t;
    t.meshPath = "Meshes/grass.ocmesh";
    t.scaleMin = 0.5f; t.scaleMax = 1.0f;
    return t;
}

static fmt::OcBiomeData forestBiome() {
    fmt::OcBiomeData b;
    b.name = "Forest";
    b.mask.kind = fmt::OcBiomeMask::Kind::Layer;
    b.mask.name = "grass";
    b.mask.min = 0.5f;
    fmt::OcBiomeEntry tree;
    tree.foliage = "Foliage/tree.ocfoliage";
    tree.densityPer100m2 = 6.0f;
    tree.priority = 1;
    fmt::OcBiomeEntry grass;
    grass.foliage = "Foliage/grass.ocfoliage";
    grass.densityPer100m2 = 20.0f;
    b.entries = {tree, grass};
    return b;
}

int main() {
    const Hill hill;
    const Ridge ridge;
    const Flat flat;

    // ============================== NoiseMask ===========================================================================
    AVER_INFO("=== NoiseMask ===");
    {
        const PcgContext c = ctxFor(0, 0, 8000, 8000, &flat);
        B b;
        b.n("grid", "CreatePointsGrid", {"spacing=100", "z=0"})
            .n("m", "NoiseMask", {"cell=2000", "min=0.45", "max=1"})
            .n("sIn", "WeightedMeshSpawner", {"meshes=in:1"}).n("sOut", "WeightedMeshSpawner", {"meshes=out:1"})
            .l("grid", "out", "m", "in").l("m", "inside", "sIn", "in").l("m", "outside", "sOut", "in");
        const PcgResult r = pcgEvaluate(b.g, c);
        usize in = 0, outN = 0;
        for (const auto& g : r.groups) (g.mesh == "in" ? in : outN) += g.items.size();
        check(in + outN == 80 * 80, "inside and outside together are every point");
        check(in > 800 && outN > 800, "the mask splits the field into patches (" + std::to_string(in) + " in, " + std::to_string(outN) + " out)");
        const PcgResult r2 = pcgEvaluate(b.g, c);
        check(samePos(positions(r), positions(r2)), "deterministic");
        // A window of the field gets the same points as the whole.
        const PcgContext w = ctxFor(2000, 2000, 5000, 5000, &flat);
        const PcgResult rw = pcgEvaluate(b.g, w);
        std::set<std::tuple<f32, f32>> whole;
        for (const auto& g : r.groups) if (g.mesh == "in") for (const auto& i : g.items) whole.insert({i.xf.position.x, i.xf.position.y});
        bool sub = true;
        for (const auto& g : rw.groups) if (g.mesh == "in") for (const auto& i : g.items) sub &= whole.count({i.xf.position.x, i.xf.position.y}) > 0;
        check(sub, "a sub-region keeps exactly the points the whole run keeps");
    }

    // ============================== EdgeFalloff =========================================================================
    AVER_INFO("=== EdgeFalloff ===");
    {
        // grass weight: 1 at x=-1000, 0.5 at x=0, 0 at x=1000. min=0.2 edge=0.6: ramps in over weights 0.2..0.8.
        const PcgContext c = ctxFor(-1600, 0, 1600, 4000, &hill);
        const auto edge = [&](bool invert) {
            return runChain("40", {{"edge", "EdgeFalloff", {"source=layer", "name=grass", "min=0.2", "edge=0.6", invert ? "invert=1" : "invert=0"}, "inside"}}, c);
        };
        const std::vector<Vec3> a = positions(edge(false)), bInv = positions(edge(true));
        const usize total = 80 * 100;   // 3200/40 x 4000/40
        check(a.size() + bInv.size() > total * 9 / 10 && a.size() + bInv.size() < total * 11 / 10,
              "a biome and its inverse together keep about one point per candidate (" + std::to_string(a.size() + bInv.size()) + " of " + std::to_string(total) + ")");
        // Bands across the ramp (x -1000 grass 1.0, -600 -> 0.8, 0 -> 0.5, 600 -> 0.2, 1000 -> 0.0)
        const usize aFull = countIn(a, -1600, -1000), aMid1 = countIn(a, -600, -200), aMid2 = countIn(a, 200, 600), aNone = countIn(a, 1000, 1600);
        check(aFull == 15 * 100, "weight above the ramp keeps every point (" + std::to_string(aFull) + ")");
        check(aNone == 0, "weight below the ramp keeps none");
        check(aMid1 > aMid2 && aMid1 > 0 && aMid2 > 0, "inside the ramp the density falls with the weight (" + std::to_string(aMid1) + " > " + std::to_string(aMid2) + ")");
        const usize bMid1 = countIn(bInv, -600, -200), bMid2 = countIn(bInv, 200, 600);
        check(bMid2 > bMid1, "the inverse rises where the biome falls (" + std::to_string(bMid1) + " < " + std::to_string(bMid2) + ")");
        check(aMid1 > 0 && bMid1 > 0 && aMid2 > 0 && bMid2 > 0, "both biomes are present in the same band: the border interleaves, there is no line");
        // Hard cut.
        const PcgResult hard = runChain("40", {{"edge", "EdgeFalloff", {"source=layer", "name=grass", "min=0.5", "edge=0"}, "inside"}}, c);
        const auto hp = positions(hard);
        bool cut = !hp.empty();
        for (const Vec3& p : hp) cut &= p.x < 0.0f + 40.0f;   // weight >= 0.5 <=> x <= 0 (cell centres at 20 + 40k)
        check(cut, "edge=0 is a hard cut at the threshold");
        // Spline and volume sources.
        PcgContext cs = ctxFor(0, 0, 4000, 4000, &flat);
        cs.splines["river"] = {Vec3{0, 2000, 0}, Vec3{4000, 2000, 0}};
        const auto sp = positions(runChain("50", {{"e", "EdgeFalloff", {"source=spline", "name=river", "half=300", "edge=0"}, "inside"}}, cs));
        bool near = !sp.empty();
        for (const Vec3& p : sp) near &= std::fabs(p.y - 2000.0f) <= 300.0f;
        check(near, "spline mask keeps points within the half width");
        cs.volumes["box"] = PcgBox{Vec3{1000, 1000, -1e5f}, Vec3{3000, 3000, 1e5f}};
        const auto vp = positions(runChain("50", {{"e", "EdgeFalloff", {"source=volume", "volume=box", "edge=0"}, "inside"}}, cs));
        bool inside = !vp.empty();
        for (const Vec3& p : vp) inside &= p.x >= 1000 && p.x <= 3000 && p.y >= 1000 && p.y <= 3000;
        check(inside && vp.size() == 40 * 40, "volume mask keeps the box (" + std::to_string(vp.size()) + ")");
        const PcgResult noLand = runChain("50", {{"e", "EdgeFalloff", {"source=layer", "name=grass"}, "inside"}}, ctxFor(0, 0, 1000, 1000));
        check(hasError(noLand), "a layer mask without a landscape reports an error");
    }

    // ============================== Clump ===============================================================================
    AVER_INFO("=== Clump ===");
    {
        const PcgContext c = ctxFor(0, 0, 6000, 6000, &flat);
        const PcgResult parents = runChain("1000", {}, c);
        const auto pp = positions(parents);
        const PcgResult r = runChain("1000", {{"c", "Clump", {"radius=400", "min=3", "max=6"}}}, c);
        const auto cp = positions(r);
        check(pp.size() == 36, "36 parents");
        check(cp.size() >= 36 * 3 && cp.size() <= 36 * 6, "each parent has 3..6 children (" + std::to_string(cp.size()) + ")");
        bool inRadius = true;
        for (const Vec3& q : cp) {
            f32 best = 1e9f;
            for (const Vec3& p : pp) best = std::min(best, std::hypot(q.x - p.x, q.y - p.y));
            inRadius &= best <= 400.0f + 0.01f;
        }
        check(inRadius, "every child lies within the radius of a parent");
        const auto cp2 = positions(runChain("1000", {{"c", "Clump", {"radius=400", "min=3", "max=6"}}}, c));
        check(samePos(cp, cp2), "deterministic");
        const auto thinned = positions(runChain("1000", {{"c", "Clump", {"radius=400", "min=3", "max=6", "chance=0.5"}}}, c));
        check(thinned.size() < cp.size() && thinned.size() > 0, "chance thins the parents");
        const auto kept = positions(runChain("1000", {{"c", "Clump", {"radius=400", "min=2", "max=2", "keepParent=1"}}}, c));
        check(kept.size() == 36 * 3, "keepParent adds the parent to its clump");
        // Sibling spacing: children of one parent are at least `spacing` apart (parents are 1000 apart, radius 300, so clumps never mix).
        const auto spaced = positions(runChain("1000", {{"c", "Clump", {"radius=300", "min=4", "max=4", "spacing=120"}}}, c));
        bool sepOk = true;
        for (usize i = 0; i < spaced.size(); ++i)
            for (usize j = i + 1; j < spaced.size(); ++j)
                if (std::hypot(spaced[i].x - spaced[j].x, spaced[i].y - spaced[j].y) < 119.9f) sepOk = false;
        check(sepOk && spaced.size() > 36 * 2, "sibling spacing is honoured (" + std::to_string(spaced.size()) + " children)");
        check(hasError(runChain("1000", {{"c", "Clump", {"min=3"}}}, c)), "a clump without a radius is an error");
    }

    // ============================== Compete =============================================================================
    AVER_INFO("=== Compete ===");
    {
        // Three kinds of points on one grid via AttributeSet: even columns are trees (r 300), the rest grass (r 0).
        const PcgContext c = ctxFor(0, 0, 4000, 4000, &flat);
        B b;
        b.n("g1", "CreatePointsGrid", {"spacing=400", "z=0"}).n("t1", "AttributeSet", {"name=radius", "value=150"})
            .n("g2", "CreatePointsGrid", {"spacing=130", "z=0"}).n("t2", "AttributeSet", {"name=radius", "value=0"})
            .n("u", "Union").n("cmp", "Compete", {"maxRadius=150"}).n("s", "WeightedMeshSpawner", {"meshes=m:1"})
            .l("g1", "out", "t1", "in").l("g2", "out", "t2", "in").l("t1", "out", "u", "a").l("t2", "out", "u", "b").l("u", "out", "cmp", "in")
            .l("cmp", "out", "s", "in");
        const PcgResult r = pcgEvaluate(b.g, c);
        check(!hasError(r), "evaluates");
        // Reconstruct who survived: trees are on the 400 lattice (cx = 200 + 400k), grass on the 130 lattice.
        usize trees = 0, grassKept = 0;
        std::vector<Vec3> tp, gp;
        for (const Vec3& p : positions(r)) {
            const f32 fx = std::fmod(p.x - 200.0f, 400.0f), fy = std::fmod(p.y - 200.0f, 400.0f);
            const bool isTree = std::fabs(fx) < 1e-3f && std::fabs(fy) < 1e-3f && p.x > 0;
            (isTree ? tp : gp).push_back(p);
            (isTree ? trees : grassKept)++;
        }
        check(trees == 100, "all 100 trees survive (their radii sum to less than the lattice spacing) (" + std::to_string(trees) + ")");
        bool clear = true;
        for (const Vec3& g : gp) for (const Vec3& t : tp) if (std::hypot(g.x - t.x, g.y - t.y) < 150.0f) clear = false;
        check(clear && grassKept > 0, "no grass survives inside a trunk's radius, grass elsewhere does (" + std::to_string(grassKept) + ")");

        // Two radii on one point set: the larger wins, a higher priority beats a larger radius.
        const auto two = [&](f32 prioSmall) {
            B t;
            t.n("a", "CreatePointsGrid", {"spacing=1000", "z=0"}).n("ar", "AttributeSet", {"name=radius", "value=400"})
                .n("b", "CreatePointsGrid", {"spacing=1000", "z=0"}).n("br", "AttributeSet", {"name=radius", "value=250"})
                .n("bp", "AttributeSet", {"name=priority", "value=" + std::to_string(prioSmall)})
                .n("bo", "TransformPoints", {"posMin=300,0,0", "posMax=300,0,0"})
                .n("u", "Union").n("cmp", "Compete", {"maxRadius=400"}).n("s", "WeightedMeshSpawner", {"meshes=m:1"})
                .l("a", "out", "ar", "in").l("b", "out", "bo", "in").l("bo", "out", "br", "in").l("br", "out", "bp", "in")
                .l("ar", "out", "u", "a").l("bp", "out", "u", "b").l("u", "out", "cmp", "in").l("cmp", "out", "s", "in");
            return positions(pcgEvaluate(t.g, ctxFor(0, 0, 3000, 3000, &flat)));
        };
        const auto big = two(0.0f);
        bool allBigGrid = !big.empty();
        for (const Vec3& p : big) allBigGrid &= std::fabs(std::fmod(p.x - 500.0f, 1000.0f)) < 1e-3f;
        check(allBigGrid && big.size() == 9, "equal priority: the larger radius wins every overlap (" + std::to_string(big.size()) + ")");
        const auto prio = two(5.0f);
        bool allSmall = !prio.empty();
        for (const Vec3& p : prio) allSmall &= std::fabs(std::fmod(p.x - 800.0f, 1000.0f)) < 1e-3f;
        check(allSmall, "a higher priority beats a larger radius");

        // Pairwise: no two kept points with radii are closer than the sum.
        const auto many = [&](const char* mode) {
            B t;
            t.n("g", "CreatePointsGrid", {"spacing=90", "z=0"}).n("r", "AttributeSet", {"name=radius", "min=40", "max=160"})
                .n("cmp", "Compete", {"maxRadius=160", std::string("mode=") + mode}).n("s", "WeightedMeshSpawner", {"meshes=m:1"})
                .l("g", "out", "r", "in").l("r", "out", "cmp", "in").l("cmp", "out", "s", "in");
            return pcgEvaluate(t.g, ctxFor(0, 0, 2000, 2000, &flat));
        };
        const PcgResult mg = many("greedy");
        check(!mg.groups.empty() && !hasError(mg), "greedy compete runs");
        // Recompute radii from the attribute to check spacing: radius is a function of the point seed; read it from a second pass.
        B pass;
        pass.n("g", "CreatePointsGrid", {"spacing=90", "z=0"}).n("r", "AttributeSet", {"name=radius", "min=40", "max=160"})
            .n("cmp", "Compete", {"maxRadius=160"}).n("f", "AttributeFilter", {"attr=radius", "op=>=", "value=0"}).n("s", "WeightedMeshSpawner", {"meshes=m:1"})
            .l("g", "out", "r", "in").l("r", "out", "cmp", "in").l("cmp", "out", "f", "in").l("f", "inside", "s", "in");
        check(samePos(positions(pcgEvaluate(pass.g, ctxFor(0, 0, 2000, 2000, &flat))), positions(pcgEvaluate(pass.g, ctxFor(0, 0, 2000, 2000, &flat)))), "deterministic");
        check(!positions(many("local")).empty(), "local mode runs");
    }

    // ============================== biome graph =========================================================================
    AVER_INFO("=== biome graph ===");
    {
        fmt::OcBiomeData forest = forestBiome();
        std::vector<PcgBiomeSource> src(1);
        src[0].biome = &forest;
        src[0].types = {treeType(), grassType()};
        fmt::OcGraphData g;
        std::string why;
        check(pcgBuildBiomeGraph(src, g, &why), "compiles (" + why + ")");
        check(pcgIsGraph(g) && pcgValidate(g).empty(), "a valid DOMAIN pcg graph");
        check(pcgGraphHash(g) != 0, "and hashes");

        fmt::OcBiomeData wrong = forest;
        wrong.entries[0].densityPer100m2 = 0.0f;
        std::vector<PcgBiomeSource> bad(1);
        bad[0].biome = &wrong;
        bad[0].types = {treeType(), grassType()};
        check(!pcgBuildBiomeGraph(bad, g, &why), "an invalid biome is refused (" + why + ")");
        bad[0].biome = &forest;
        bad[0].types = {treeType()};
        check(!pcgBuildBiomeGraph(bad, g, &why), "so is a type list that does not match the entries");

        // Density: one tree entry with no mask over a 100 m x 100 m area (100 x density).
        fmt::OcBiomeData plain;
        plain.name = "Plain";
        fmt::OcBiomeEntry e;
        e.foliage = "g";
        e.densityPer100m2 = 8.0f;
        plain.entries = {e};
        std::vector<PcgBiomeSource> ps(1);
        ps[0].biome = &plain;
        ps[0].types = {grassType()};
        fmt::OcGraphData pg;
        pcgBuildBiomeGraph(ps, pg);
        const PcgResult flatRun = pcgEvaluate(pg, ctxFor(0, 0, 10000, 10000, &flat));
        const usize n = flatRun.instanceCount();
        check(n > 800 * 0.75 && n < 800 * 1.25, "instance count follows the density (" + std::to_string(n) + " of ~800)");
        const PcgResult again = pcgEvaluate(pg, ctxFor(0, 0, 10000, 10000, &flat));
        check(samePos(positions(again), positions(flatRun)), "deterministic");

        // Slope: 30 degrees for x > 0, flat before.
        plain.entries[0].slopeMaxDeg = 10.0f;
        fmt::OcGraphData sg;
        pcgBuildBiomeGraph(ps, sg);
        const auto sp = positions(pcgEvaluate(sg, ctxFor(-3000, 0, 3000, 3000, &ridge)));
        bool flatOnly = !sp.empty();
        for (const Vec3& p : sp) flatOnly &= p.x < 60.0f;
        check(flatOnly, "a slope limit of 10 degrees keeps off the 30 degree ground");
        plain.entries[0].slopeMinDeg = 20.0f; plain.entries[0].slopeMaxDeg = 40.0f;
        pcgBuildBiomeGraph(ps, sg);
        const auto st = positions(pcgEvaluate(sg, ctxFor(-3000, 0, 3000, 3000, &ridge)));
        bool steepOnly = !st.empty();
        for (const Vec3& p : st) steepOnly &= p.x > -60.0f;
        check(steepOnly, "and a 20..40 band keeps to it");
        // Altitude.
        plain.entries[0].slopeMinDeg = 0.0f; plain.entries[0].slopeMaxDeg = 90.0f;
        plain.entries[0].heightMinCm = 500.0f; plain.entries[0].heightMaxCm = 1500.0f;
        pcgBuildBiomeGraph(ps, sg);
        const auto hs = positions(pcgEvaluate(sg, ctxFor(-3000, 0, 6000, 3000, &ridge)));
        bool band = !hs.empty();
        for (const Vec3& p : hs) band &= p.z >= 499.0f && p.z <= 1501.0f;
        check(band, "an altitude band keeps to its heights");
        // Layer gate.
        plain.entries[0].heightMinCm = -1e9f; plain.entries[0].heightMaxCm = 1e9f;
        plain.entries[0].layer = "grass"; plain.entries[0].layerMin = 0.75f;
        pcgBuildBiomeGraph(ps, sg);
        const auto lg = positions(pcgEvaluate(sg, ctxFor(-3000, 0, 3000, 3000, &hill)));
        bool onGrass = !lg.empty();
        for (const Vec3& p : lg) onGrass &= p.x <= -500.0f + 50.0f;   // weight >= 0.75 <=> x <= -500
        check(onGrass, "a layer gate keeps to the painted ground");

        // Forest: tree beats grass; grass is kept off trunks; mask follows the layer.
        fmt::OcGraphData fg;
        pcgBuildBiomeGraph(src, fg);
        const PcgResult fr = pcgEvaluate(fg, ctxFor(-3000, 0, 3000, 6000, &hill));
        check(!hasError(fr) && fr.groups.size() == 2, "the forest evaluates to a tree group and a grass group");
        std::vector<Vec3> trees, grass;
        for (const auto& grp : fr.groups) for (const auto& i : grp.items) (grp.mesh == "Meshes/tree.ocmesh" ? trees : grass).push_back(i.xf.position);
        check(trees.size() > 15 && grass.size() > trees.size(), "trees " + std::to_string(trees.size()) + ", grass " + std::to_string(grass.size()));
        bool overlap = false;
        for (usize i = 0; i < trees.size() && !overlap; ++i)
            for (usize j = i + 1; j < trees.size(); ++j) {
                const f32 dx = trees[i].x - trees[j].x, dy = trees[i].y - trees[j].y;
                // scales are 0.8..1.2 of a 300 cm radius: the declared spacing check uses the scaled sum
                if (std::sqrt(dx * dx + dy * dy) < 2.0f * 300.0f * 0.8f - 1.0f) { overlap = true; break; }
            }
        check(!overlap, "no two trees overlap their (scaled) radii");
        bool grassOnTrunk = false;
        for (const Vec3& gp : grass) for (const Vec3& tp : trees) if (std::hypot(gp.x - tp.x, gp.y - tp.y) < 300.0f * 0.8f - 1.0f) grassOnTrunk = true;
        check(!grassOnTrunk, "no grass inside a trunk's radius");
        usize treesRight = 0;
        for (const Vec3& t : trees) if (t.x > 500.0f) ++treesRight;
        check(treesRight == 0, "the forest mask keeps trees off the rock side (layer weight < 0.5)");
        // The type's authored fields reached the group.
        const PcgInstanceGroup* tg = nullptr;
        for (const auto& grp : fr.groups) if (grp.mesh == "Meshes/tree.ocmesh") tg = &grp;
        check(tg && tg->cullStartCm == 3000.0f && tg->cullEndCm == 8000.0f && tg->lodScaleBias == 1.5f &&
                  tg->flags == fmt::kOcInstanceFlagCollisionTrunk,
              "the tree group carries cull range, LOD bias and the collision flag");
        // Ground offset and align cap: the tree is sunk 15 cm and tilted at most 20 degrees on an 11 degree slope (all of it).
        bool sunk = true;
        for (const auto& i : tg->items) {
            const f32 surface = 100.0f + 0.2f * i.xf.position.x;
            sunk &= std::fabs(i.xf.position.z - (surface - 15.0f)) < 0.5f;
        }
        check(sunk, "groundOffsetCm lowers the instance below the surface");
        // Two biomes (one inverted) share the border.
        fmt::OcBiomeData meadow;
        meadow.name = "Meadow";
        meadow.mask = forest.mask;
        meadow.mask.invert = true;
        meadow.mask.edge = 0.0f;
        meadow.entries = {plain.entries[0]};
        meadow.entries[0].layer.clear();
        meadow.entries[0].densityPer100m2 = 8.0f;
        fmt::OcBiomeData soft = forest;
        soft.mask.min = 0.2f; soft.mask.edge = 0.6f;
        meadow.mask.min = 0.2f; meadow.mask.edge = 0.6f;
        std::vector<PcgBiomeSource> two(2);
        two[0].biome = &soft; two[0].types = {treeType(), grassType()};
        two[1].biome = &meadow; two[1].types = {grassType()};
        fmt::OcGraphData tg2;
        check(pcgBuildBiomeGraph(two, tg2, &why), "two biomes compile together (" + why + ")");
        const PcgResult tr = pcgEvaluate(tg2, ctxFor(-1600, 0, 1600, 3000, &hill));
        check(!hasError(tr) && tr.instanceCount() > 40, "and evaluate (" + std::to_string(tr.instanceCount()) + " instances)");
    }

    // ============================== cells equal the whole ===============================================================
    AVER_INFO("=== chunk-edge ownership ===");
    {
        fmt::OcBiomeData forest = forestBiome();
        forest.entries[0].clumpRadiusCm = 500.0f;
        forest.entries[1].densityPer100m2 = 60.0f;
        forest.mask.min = 0.2f; forest.mask.edge = 0.6f;
        std::vector<PcgBiomeSource> src(1);
        src[0].biome = &forest;
        src[0].types = {treeType(), grassType()};
        auto prog = std::make_shared<PcgProgram>();
        fmt::OcGraphData g;
        pcgBuildBiomeGraph(src, g);
        prog->graph = std::make_shared<const fmt::OcGraphData>(g);
        prog->seed = 21;
        prog->landscape = std::make_shared<Hill>();
        constexpr i32 kCell = 1600;   // a chunk size chunkSizeValid accepts
        const PcgCellGenerator gen(prog, kCell);
        check(gen.margin() > 600.0f, "the cell margin covers the clump, jitter and compete reach (" + std::to_string(gen.margin()) + ")");

        using Key = std::tuple<f32, f32, i32>;
        const auto keysOf = [](const PcgResult& r, const PcgBox* own = nullptr) {
            std::vector<Key> v;
            for (const auto& grp : r.groups) for (const auto& i : grp.items)
                if (!own || own->containsXY(i.xf.position)) v.emplace_back(i.xf.position.x, i.xf.position.y, i.seed);
            std::sort(v.begin(), v.end());
            return v;
        };
        const PcgBox area{Vec3{-kCell, 0, -1e5f}, Vec3{kCell, 2 * kCell, 1e5f}};
        PcgBox big = area;
        big.min.x -= gen.margin(); big.min.y -= gen.margin(); big.max.x += gen.margin(); big.max.y += gen.margin();
        const std::vector<Key> whole = keysOf(gen.generateBox(big), &area);
        std::vector<Key> got;
        bool disjoint = true;
        for (i32 cy = 1; cy >= 0; --cy)
            for (i32 cx = 0; cx >= -1; --cx) {
                const PcgResult r = gen.generate(cx, cy);
                const PcgBox own = gen.cellBox(cx, cy);
                for (const Key& k : keysOf(r)) {
                    disjoint &= own.containsXY(Vec3{std::get<0>(k), std::get<1>(k), 0});
                    got.push_back(k);
                }
            }
        std::sort(got.begin(), got.end());
        check(whole.size() > 150, "a populated area (" + std::to_string(whole.size()) + " instances)");
        check(disjoint, "every instance a cell returns lies inside that cell");
        check(std::adjacent_find(got.begin(), got.end()) == got.end(), "no instance is produced by two cells");
        check(got == whole, "four neighbouring cells equal one whole-area run, instance for instance");

        // Through GeneratedChunkSource: the entities of four chunks are the same set, each in exactly one chunk.
        GeneratorSettings gs;
        gs.chunkSizeCm = kCell;
        gs.worldSeed = 3;
        gs.pcgProgram = prog;
        gs.generatorVersion = pcgGeneratorVersion(*prog);
        GeneratedChunkSource cs(gs);
        std::set<std::tuple<i64, i64, std::string>> seen;
        usize entities = 0;
        bool local = true, unique = true;
        for (i32 cy = 0; cy <= 1; ++cy)
            for (i32 cx = -1; cx <= 0; ++cx) {
                ChunkPayload p;
                std::string why;
                if (!cs.load(ChunkCoord{cx, cy, 0}, p, &why)) { check(false, "chunk loads: " + why); continue; }
                for (const auto& e : p.entities) {
                    local &= e.local.position.x >= 0 && e.local.position.x < kCell && e.local.position.y >= 0 && e.local.position.y < kCell;
                    unique &= seen.insert({cx, cy, e.name}).second;
                    ++entities;
                }
            }
        check(local && unique, "chunk-local positions, unique names");
        check(entities == whole.size(), "the four chunks hold exactly the whole run's instances (" + std::to_string(entities) + ")");
        std::set<std::string> names;
        for (const auto& s : seen) names.insert(std::get<2>(s));
        check(names.size() == seen.size(), "no instance name appears in two chunks (nothing duplicated across an edge)");
        // Instances close to a shared edge belong to exactly one side.
        usize nearEdge = 0;
        for (const Key& k : whole) if (std::fabs(std::get<0>(k)) < 40.0f) ++nearEdge;
        check(nearEdge > 0, "the fixture puts instances within 40 cm of the shared edge (" + std::to_string(nearEdge) + ")");
    }

    // ============================== Generate Vegetation =================================================================
    AVER_INFO("=== Generate Vegetation ===");
    {
        fmt::OcBiomeData forest = forestBiome();
        forest.entries[1].densityPer100m2 = 60.0f;
        VegetationInputs in;
        PcgBiomeSource s;
        s.biome = &forest;
        s.types = {treeType(), grassType()};
        in.sources = {s};
        in.landscape = std::make_shared<Hill>();
        in.knownLayers = {"grass", "rock"};
        fmt::OcVegetationRecord rec;
        rec.biomes = {"Biomes/Forest.ocbiome"};
        rec.seed = 9;
        rec.areaMin[0] = -6400; rec.areaMin[1] = 0; rec.areaMax[0] = 1600; rec.areaMax[1] = 8000;
        rec.cellCm = 1600;
        rec.output = "Foliage/Hill.ocinst";

        VegetationGenerator gen(in, rec);
        const VegetationResult a = gen.run();
        check(a.ok, "generates (" + a.error + ")");
        check(a.table.valid() && !a.table.cells.empty() && a.table.cellCm == 1600.0f, "a valid cell-sorted .ocinst");
        const u32 rows = static_cast<u32>(a.table.transforms.size() / 12);
        check(rows == gen.record().count && rows == a.generated && rows > 1000, "the record counts the rows (" + std::to_string(rows) + ")");
        check(gen.record().inputHash != 0 && gen.record().inputHash == gen.currentInputHash(), "and the hash of what went in");
        bool cull = false, flags = false;
        for (const auto& grp : a.table.groups) {
            if (grp.asset == "Meshes/tree.ocmesh") {
                cull = grp.cullStartCm == 3000.0f && grp.cullEndCm == 8000.0f && grp.lodScaleBias == 1.5f;
                flags = (grp.flags & fmt::kOcInstanceFlagCollisionTrunk) != 0 && (grp.flags & fmt::kOcInstanceFlagCastShadow) != 0;
            }
        }
        check(cull && flags, "the table's tree group carries the cull range and collision flag");
        std::vector<u8> bytes;
        fmt::OcInstanceData back;
        std::string why;
        check(fmt::writeOcInstances(a.table, bytes, &why) && fmt::parseOcInstances(bytes.data(), bytes.size(), back, &why) &&
                  back.groups.size() == a.table.groups.size() && back.groups[0].cullEndCm == a.table.groups[0].cullEndCm,
              "it round-trips through the .ocinst writer (" + why + ")");

        // Repeat: identical, and every cell reused.
        const VegetationResult b = gen.run();
        check(b.ok && b.table.transforms == a.table.transforms && b.cellsRun == 0 && b.cellsReused == 25,
              "a repeat run reuses all 25 cells and gives the same table (" + std::to_string(b.cellsRun) + " run)");
        VegetationGenerator fresh(in, rec);
        check(fresh.run().table.transforms == a.table.transforms, "a fresh generator gives the same table");

        // Landscape edit: only the cells the box reaches regenerate.
        PcgBox box{Vec3{-6300, 100, -1e5f}, Vec3{-6200, 200, 1e5f}};
        const u32 marked = gen.invalidate(box);
        const VegetationResult c = gen.run();
        check(marked >= 1 && marked < 25 && c.cellsRun == marked && c.cellsReused == 25 - marked,
              "invalidate marks only the reachable cells (" + std::to_string(marked) + ") and the next run regenerates just those");
        check(c.table.transforms == a.table.transforms, "which regenerate to the same result");

        // Manual overrides: erase a stamp, add a rock; they survive a regenerate.
        const Vec3 victim = [&] {
            for (usize i = 0; i + 12 <= a.table.transforms.size(); i += 12)
                return Vec3{a.table.transforms[i + 9], a.table.transforms[i + 10], a.table.transforms[i + 11]};
            return Vec3{};
        }();
        gen.eraseAt(victim, 250.0f);
        Transform rock;
        rock.position = Vec3{10, 20, 30};
        gen.addInstance("Meshes/rock.ocmesh", rock);
        const VegetationResult d = gen.run();
        check(d.ok && d.erasedByOverride >= 1 && d.addedByOverride == 1, "an erase stamp and an addition apply (" + std::to_string(d.erasedByOverride) + " erased)");
        check(d.table.transforms.size() / 12 == a.table.transforms.size() / 12 - d.erasedByOverride + 1, "the row count follows");
        bool victimGone = true;
        for (usize i = 0; i + 12 <= d.table.transforms.size(); i += 12) {
            const f32 dx = d.table.transforms[i + 9] - victim.x, dy = d.table.transforms[i + 10] - victim.y;
            if (dx * dx + dy * dy <= 250.0f * 250.0f) victimGone = false;
        }
        check(victimGone, "nothing generated remains inside the stamp");
        gen.bumpEnvironment();
        const VegetationResult e2 = gen.run();
        check(e2.ok && e2.cellsRun == 25 && e2.table.transforms == d.table.transforms, "a full regenerate keeps the overrides on the new baseline");
        // The record round-trips with its override set.
        std::string text;
        fmt::OcVegetationRecord rb;
        check(fmt::writeOcVegetation(gen.record(), text, &why) && fmt::parseOcVegetation(text, rb, &why), "the .ocveg record writes and parses (" + why + ")");
        check(rb.erased.size() == 1 && rb.added.size() == 1 && rb.inputHash == gen.record().inputHash && rb.count == gen.record().count &&
                  rb.areaMax[1] == 8000.0f && rb.cellCm == 1600 && rb.added[0].asset == "Meshes/rock.ocmesh",
              "with the override set, hash and count");
        std::string text2;
        check(fmt::writeOcVegetation(rb, text2, &why) && text == text2, "and re-writes byte for byte");
        VegetationGenerator restored(in, rb);
        check(restored.run().table.transforms == d.table.transforms, "a generator rebuilt from the saved record reproduces the table");

        // Editing a biome entry makes every cell stale (the graph hash changes) and the table different.
        const u64 before = gen.currentInputHash();
        forest.entries[1].densityPer100m2 = 120.0f;
        check(gen.currentInputHash() != before, "editing a biome changes the input hash (the saved table is detectably stale)");
        const VegetationResult f = gen.run();
        check(f.ok && f.cellsRun == 25 && f.generated > a.generated, "and the next run regenerates every cell");
    }

    // ============================== guardrails ==========================================================================
    AVER_INFO("=== guardrails ===");
    {
        fmt::OcBiomeData dense;
        dense.name = "Dense";
        fmt::OcBiomeEntry e;
        e.foliage = "g";
        e.densityPer100m2 = 400.0f;
        dense.entries = {e};
        fmt::OcFoliageData noCull = grassType();
        std::vector<PcgBiomeSource> src(1);
        src[0].biome = &dense;
        src[0].types = {noCull};
        PcgGuardrails caps;
        caps.maxInstancesPerCell = 10000;
        const auto lods = +[](const std::string&, void*) { return false; };
        const PcgBiomeEstimate est = pcgEstimateBiome(src, 6400.0f, caps, lods, nullptr, {"grass"});
        check(est.candidatesPerCell > 1.0e4 && est.overCap, "a 6400 cm cell at 400 per 100 m^2 is over the cap before anything is generated");
        const auto has = [&](const char* needle) {
            for (const auto& w : est.warnings) if (w.find(needle) != std::string::npos) return true;
            return false;
        };
        check(has("no cullEndCm"), "warns that the type has no cullEndCm");
        check(has("no LODs"), "warns that the mesh has no LODs");
        check(has("over the cap"), "and states the cap");
        dense.entries[0].layer = "swamp";
        const PcgBiomeEstimate est2 = pcgEstimateBiome(src, 100.0f, caps, nullptr, nullptr, {"grass"});
        bool layerWarn = false;
        for (const auto& w : est2.warnings) layerWarn |= w.find("swamp") != std::string::npos;
        check(layerWarn && !est2.overCap, "an unknown paint layer is named, and a small cell is within the cap");

        // The run is refused before it generates; with ignoreCaps it runs and caps each cell.
        VegetationInputs in;
        in.sources = src;
        in.landscape = std::make_shared<Flat>();
        dense.entries[0].layer.clear();
        dense.entries[0].densityPer100m2 = 600.0f;
        fmt::OcVegetationRecord rec;
        rec.areaMin[0] = 0; rec.areaMin[1] = 0; rec.areaMax[0] = 3200; rec.areaMax[1] = 3200;
        rec.cellCm = 1600;
        VegetationGenerator refuse(in, rec, PcgGuardrails{1000, 64u << 20});
        const VegetationResult r1 = refuse.run();
        check(!r1.ok && r1.error.find("refused") != std::string::npos, "an over-cap setup is refused before generating (" + r1.error + ")");
        const VegetationResult r2 = refuse.run(true);
        check(r2.ok && r2.cellsCapped == 4, "with the guardrail overridden every cell is capped (" + std::to_string(r2.cellsCapped) + ")");
        check(r2.table.transforms.size() / 12 <= 4 * 1000, "to the per-cell instance cap (" + std::to_string(r2.table.transforms.size() / 12) + ")");
        bool warned = false;
        for (const auto& w : r2.warnings) warned |= w.find("capped to 1000") != std::string::npos;
        check(warned, "and says so");
        VegetationGenerator again(in, rec, PcgGuardrails{1000, 64u << 20});
        check(again.run(true).table.transforms == r2.table.transforms, "the cap keeps the same instances every time");
    }

    AVER_INFO("PcgBiomeTest: {} of {} checks passed", g_checks - g_failures, g_checks);
    return g_failures ? 1 : 0;
}
