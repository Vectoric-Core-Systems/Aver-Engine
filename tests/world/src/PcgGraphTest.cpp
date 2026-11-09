// PCG graph evaluator: every node on its own (landscape through a fake heightfield), then the evaluator itself --
// caching, cancellation, time budget, cycles, validation, Subgraph and Loop, the .ocgraph profile round trip.
// Exit code = failure count.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/world/PcgVolumeRunner.hpp"

#include <algorithm>
#include <atomic>
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

// z = 100 + 0.2 x: a plane tilted ~11.3 degrees about Y. Layer "grass" is painted on x < 0. No surface for y > 5000.
struct FakeLand final : ILandscapeQuery {
    bool heightAt(f64 x, f64 y, f32& z) const override {
        if (y > 5000.0) return false;
        z = static_cast<f32>(100.0 + 0.2 * x);
        return true;
    }
    f32 layerWeight(std::string_view layer, f64 x, f64) const override { return layer == "grass" && x < 0.0 ? 1.0f : 0.0f; }
};

// ---- tiny graph builder -------------------------------------------------------------------------------------------------
struct B {
    fmt::OcGraphData g;
    B() { g.domain = "pcg"; g.name = "t"; }
    B& n(const std::string& id, const std::string& type, std::vector<std::string> p = {}) {
        g.nodes.push_back(pcgMakeNode(id, type, p));
        return *this;
    }
    B& l(const std::string& a, const std::string& ap, const std::string& b, const std::string& bp) {
        g.links.push_back({a, ap, b, bp});
        return *this;
    }
};

static PcgContext ctxFor(f32 x0, f32 y0, f32 x1, f32 y1, i32 seed = 7) {
    PcgContext c;
    c.seed = seed;
    c.bounds.min = Vec3{x0, y0, -10000};
    c.bounds.max = Vec3{x1, y1, 10000};
    return c;
}

// Evaluates graph "source -> [nodes...] -> out" and returns the points reaching a WeightedMeshSpawner at the end.
static PcgResult run(B& b, const PcgContext& c) { return pcgEvaluate(b.g, c); }

static std::vector<Vec3> positions(const PcgResult& r) {
    std::vector<Vec3> v;
    for (const auto& g : r.groups) for (const auto& i : g.items) v.push_back(i.xf.position);
    std::sort(v.begin(), v.end(), [](const Vec3& a, const Vec3& b) {
        return std::tie(a.x, a.y, a.z) < std::tie(b.x, b.y, b.z);
    });
    return v;
}
static bool samePos(const std::vector<Vec3>& a, const std::vector<Vec3>& b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].z != b[i].z) return false;
    return true;
}
static bool hasError(const PcgResult& r, const std::string& needle) {
    for (const auto& d : r.diags) if (d.error && d.message.find(needle) != std::string::npos) return true;
    return false;
}

// A source node feeding a spawner, optionally through filter nodes given as {id, type, params, outPin}.
struct Step { std::string id, type; std::vector<std::string> params; std::string outPin = "out"; };
static B chain(const Step& source, const std::vector<Step>& mid = {}) {
    B b;
    b.n(source.id, source.type, source.params);
    std::string prev = source.id, prevPin = source.outPin;
    for (const Step& s : mid) {
        b.n(s.id, s.type, s.params).l(prev, prevPin, s.id, "in");
        prev = s.id; prevPin = s.outPin;
    }
    b.n("sp", "WeightedMeshSpawner", {"meshes=Meshes/a.ocmesh"}).l(prev, prevPin, "sp", "in");
    return b;
}

int main() {
    FakeLand land;

    // ---- CreatePointsGrid -----------------------------------------------------------------------------------------------
    {
        B b = chain({"g", "CreatePointsGrid", {"spacing=100", "z=5"}});
        const PcgResult r = run(b, ctxFor(0, 0, 1000, 500));
        const auto p = positions(r);
        check(p.size() == 50, "a 1000 x 500 box at 100 cm spacing holds 10 x 5 grid points");
        check(p.front().x == 50.0f && p.front().y == 50.0f && p.front().z == 5.0f, "points sit at cell centres at the requested z");

        // The same lattice point has the same position AND seed whatever region is evaluated.
        B b2 = chain({"g", "CreatePointsGrid", {"spacing=100", "z=5"}});
        const PcgResult sub = run(b2, ctxFor(200, 100, 600, 300));
        auto subP = positions(sub);
        size_t matched = 0;
        for (const Vec3& s : subP) matched += std::count_if(p.begin(), p.end(), [&](const Vec3& q) { return q.x == s.x && q.y == s.y; });
        check(subP.size() == 8 && matched == 8, "a sub-region yields exactly the matching subset of the full grid");
        std::set<i32> seeds;
        for (const auto& g : r.groups) for (const auto& i : g.items) seeds.insert(i.seed);
        check(seeds.size() == 50, "every grid point has its own seed");
        const PcgResult shifted = run(b, ctxFor(-1000, -1000, -900, -900));
        check(positions(shifted).size() == 1 && positions(shifted)[0].x == -950.0f, "negative coordinates sit on the same world lattice");
        B bad = chain({"g", "CreatePointsGrid", {"spacing=0"}});
        check(hasError(run(bad, ctxFor(0, 0, 100, 100)), "spacing"), "a non-positive spacing is an error, not a hang");
        PcgContext capped = ctxFor(0, 0, 100000, 100000);
        capped.maxPoints = 1000;
        B big = chain({"g", "CreatePointsGrid", {"spacing=10"}});
        const PcgResult cr = run(big, capped);
        check(hasError(cr, "cap") && cr.instanceCount() == 0 && !cr.ok, "a node that would exceed maxPoints refuses and reports");
    }

    // ---- CreatePointsRandom ---------------------------------------------------------------------------------------------
    {
        B b = chain({"r", "CreatePointsRandom", {"count=40", "scaleMin=0.5", "scaleMax=2"}});
        const PcgResult r = run(b, ctxFor(0, 0, 1000, 1000, 5));
        check(r.instanceCount() == 40, "count points come out of a count-based scatter");
        const PcgResult again = run(b, ctxFor(0, 0, 1000, 1000, 5));
        check(samePos(positions(again), positions(r)), "and the same seed gives the same points");
        PcgContext part = ctxFor(0, 0, 1000, 1000, 5);
        part.partitioned = true;
        bool warned = false;
        for (const auto& d : run(b, part).diags) warned |= !d.error && d.message.find("evaluated region") != std::string::npos;
        check(warned, "a partitioned run warns that count-based scatter is region-dependent");
    }

    // ---- SurfaceSampler / ProjectToSurface -------------------------------------------------------------------------------
    {
        B b = chain({"s", "SurfaceSampler", {"spacing=200", "align=1"}});
        PcgContext c = ctxFor(0, 0, 1000, 400);
        c.landscape = &land;
        const PcgResult r = run(b, c);
        check(r.instanceCount() == 10, "a surface sampler lays one point per lattice cell");
        bool onSurface = true, aligned = true;
        for (const auto& i : r.groups[0].items) {
            onSurface &= std::fabs(i.xf.position.z - (100.0f + 0.2f * i.xf.position.x)) < 1e-2f;
            const Vec3 up = i.xf.rotation.rotate(Vec3{0, 0, 1});
            aligned &= up.x < -0.15f && up.z > 0.95f;   // the up vector leans into the slope (normal is (-0.2, 0, 1)/|.|)
        }
        check(onSurface, "points sit on the landscape height");
        check(aligned, "align=1 rotates +Z onto the surface normal");
        B none = chain({"s", "SurfaceSampler", {"spacing=200"}});
        check(hasError(run(none, ctxFor(0, 0, 400, 400)), "landscape"), "without a landscape the sampler reports an error");
        PcgContext holes = ctxFor(0, 4800, 400, 5400);
        holes.landscape = &land;
        B b3 = chain({"s", "SurfaceSampler", {"spacing=200"}});
        check(run(b3, holes).instanceCount() == 2 * 1, "cells with no surface are skipped (heightAt false)");

        B pr = chain({"g", "CreatePointsGrid", {"spacing=200", "z=0"}}, {{"p", "ProjectToSurface", {"zOffset=10"}}});
        PcgContext c2 = ctxFor(0, 0, 400, 400);
        c2.landscape = &land;
        const PcgResult pr1 = run(pr, c2);
        check(pr1.instanceCount() == 4 && std::fabs(pr1.groups[0].items[0].xf.position.z - (100.0f + 0.2f * 100.0f + 10.0f)) < 1e-2f,
              "ProjectToSurface snaps z to height + offset");
        PcgContext c3 = ctxFor(0, 4800, 400, 5400);
        c3.landscape = &land;
        check(run(pr, c3).instanceCount() == 2, "ProjectToSurface drops points over no surface by default");
    }

    // ---- SplineSampler ---------------------------------------------------------------------------------------------------
    {
        B b = chain({"sp0", "SplineSampler", {"pts=0,0,0|1000,0,0", "spacing=250"}});
        const PcgResult r = run(b, ctxFor(-100, -100, 5000, 100));
        auto p = positions(r);
        check(p.size() == 4 && p[0].x == 0.0f && p[1].x == 250.0f && p[3].x == 750.0f, "points are spaced along the line from its start");
        const Vec3 fwd = r.groups[0].items[0].xf.rotation.rotate(Vec3{1, 0, 0});
        check(fwd.x > 0.99f, "yaw follows the tangent");
        // Clipping to the evaluation box must not change which index a point has.
        const PcgResult clipped = run(b, ctxFor(400, -100, 800, 100));
        std::set<i32> all, part;
        for (const auto& i : r.groups[0].items) all.insert(i.seed);
        for (const auto& i : clipped.groups[0].items) part.insert(i.seed);
        check(clipped.instanceCount() == 2 && std::includes(all.begin(), all.end(), part.begin(), part.end()),
              "a clipped run keeps the same points (and seeds) as the full one");
        PcgContext named = ctxFor(-100, -100, 5000, 100);
        named.splines["road"] = {Vec3{0, 0, 0}, Vec3{0, 600, 0}};
        B nb = chain({"sp0", "SplineSampler", {"name=road", "spacing=300"}});
        named.bounds.max.y = 5000;
        check(run(nb, named).instanceCount() == 2, "a named spline comes from the context");
        check(hasError(run(nb, ctxFor(0, 0, 10, 10)), "unknown spline"), "an unknown spline name is reported");
        B jit = chain({"sp0", "SplineSampler", {"pts=0,0,0|1000,0,0", "spacing=250", "jitter=30"}});
        const auto jp = positions(run(jit, ctxFor(-100, -100, 5000, 100)));
        bool offAxis = false;
        for (const Vec3& q : jp) offAxis |= std::fabs(q.y) > 0.01f && std::fabs(q.y) <= 30.0f;
        check(offAxis, "jitter moves points sideways within its range");
    }

    // ---- VolumeSampler / BoundsFilter ------------------------------------------------------------------------------------
    {
        B b = chain({"v", "VolumeSampler", {"min=0,0,0", "max=400,400,400", "spacing=200"}});
        check(run(b, ctxFor(-1000, -1000, 1000, 1000)).instanceCount() == 8, "a 400 cm box at 200 cm spacing is a 2 x 2 x 2 lattice");
        check(run(b, ctxFor(0, 0, 200, 400)).instanceCount() == 4, "the evaluation region clips the lattice in XY");
        PcgContext named = ctxFor(-1000, -1000, 1000, 1000);
        named.volumes["cave"] = PcgBox{Vec3{0, 0, 0}, Vec3{200, 200, 200}};
        B nb = chain({"v", "VolumeSampler", {"volume=cave", "spacing=100"}});
        check(run(nb, named).instanceCount() == 8, "a named volume resolves from the context");

        B f = chain({"g", "CreatePointsGrid", {"spacing=100"}}, {{"bf", "BoundsFilter", {"min=0,0,-10", "max=300,200,10"}, "inside"}});
        check(run(f, ctxFor(0, 0, 1000, 1000)).instanceCount() == 6, "BoundsFilter inside keeps the points in the box");
        B f2 = chain({"g", "CreatePointsGrid", {"spacing=100"}}, {{"bf", "BoundsFilter", {"min=0,0,-10", "max=300,200,10"}, "outside"}});
        check(run(f2, ctxFor(0, 0, 1000, 1000)).instanceCount() == 94, "and outside keeps the rest");
    }

    // ---- density nodes ----------------------------------------------------------------------------------------------------
    {
        B b = chain({"g", "CreatePointsGrid", {"spacing=100"}}, {{"n", "DensityNoise", {"cell=500", "octaves=2"}}, {"d", "DensityFilter", {"min=0.5"}, "inside"}});
        const PcgResult r = run(b, ctxFor(0, 0, 3000, 3000));
        check(r.nodePointCount.at("n") == 900, "DensityNoise keeps every point");
        const u32 kept = r.nodePointCount.at("d");
        check(kept > 100 && kept < 800, "a 0.5 density threshold keeps a fraction of a noise field");
        check(run(b, ctxFor(0, 0, 3000, 3000)).nodePointCount.at("d") == kept, "noise is deterministic");
        B other = b;
        other.g.nodes[1].extraTokens = {"cell=500", "octaves=2", "seed=9"};
        check(run(other, ctxFor(0, 0, 3000, 3000)).nodePointCount.at("d") != kept, "a different noise seed gives a different field");

        B rm = chain({"g", "CreatePointsGrid", {"spacing=100"}},
                     {{"n", "DensityNoise", {"cell=500"}}, {"m", "DensityRemap", {"inMin=0", "inMax=1", "outMin=10", "outMax=20"}},
                      {"d", "DensityFilter", {"min=10", "max=20"}, "inside"}});
        const PcgResult rr = run(rm, ctxFor(0, 0, 3000, 3000));
        check(rr.nodePointCount.at("d") == 900, "DensityRemap maps [0,1] into [10,20]");
        B mul = chain({"g", "CreatePointsGrid", {"spacing=100"}},
                      {{"a", "AttributeSet", {"name=density", "value=0.5"}}, {"n", "DensityNoise", {"mode=mul"}}, {"d", "DensityFilter", {"min=0.5"}, "inside"}});
        check(run(mul, ctxFor(0, 0, 1000, 1000)).nodePointCount.at("d") == 0, "mode=mul scales the existing density (0.5 x [0,1) never reaches 0.5)");
    }

    // ---- attributes -----------------------------------------------------------------------------------------------------
    {
        B b = chain({"g", "CreatePointsGrid", {"spacing=100"}},
                    {{"s", "AttributeSet", {"name=moisture", "min=0", "max=1"}}, {"m", "AttributeMath", {"dst=wet", "a=moisture", "op=mul", "value=2"}},
                     {"f", "AttributeFilter", {"attr=wet", "op=>=", "value=1"}, "inside"}});
        const PcgResult r = run(b, ctxFor(0, 0, 4000, 4000));
        const u32 kept = r.nodePointCount.at("f");
        check(kept > 560 && kept < 1040, "a random range filtered at its midpoint keeps about half");
        check(run(b, ctxFor(0, 0, 4000, 4000)).nodePointCount.at("f") == kept, "per-point random draws are deterministic");
        // A point's random value depends on the point, not on how many points came before it.
        B b2 = b;
        const PcgResult sub = run(b2, ctxFor(1000, 1000, 2000, 2000));
        B only = chain({"g", "CreatePointsGrid", {"spacing=100"}}, {{"s", "AttributeSet", {"name=moisture", "min=0", "max=1"}}, {"f", "AttributeFilter", {"attr=moisture", "op=>=", "value=0.5"}, "inside"}});
        B all = only;
        const PcgResult a = run(only, ctxFor(0, 0, 4000, 4000)), c = run(all, ctxFor(1000, 1000, 2000, 2000));
        size_t inBoth = 0;
        const auto pa = positions(a);
        for (const Vec3& p : positions(c)) inBoth += std::binary_search(pa.begin(), pa.end(), p, [](const Vec3& l, const Vec3& r2) { return std::tie(l.x, l.y, l.z) < std::tie(r2.x, r2.y, r2.z); });
        check(inBoth == positions(c).size() && !sub.groups.empty(), "the same lattice point makes the same keep/drop decision in a sub-region");
        B ops = chain({"g", "CreatePointsGrid", {"spacing=100"}},
                      {{"a", "AttributeSet", {"name=v", "value=3"}}, {"m", "AttributeMath", {"dst=v", "a=v", "op=add", "value=4"}}, {"f", "AttributeFilter", {"attr=v", "op===", "value=7"}, "inside"}});
        check(run(ops, ctxFor(0, 0, 300, 100)).nodePointCount.at("f") == 3, "add and == work (3 + 4 == 7)");
        B miss = chain({"g", "CreatePointsGrid", {"spacing=100"}}, {{"f", "AttributeFilter", {"attr=nope", "op=>", "value=0"}, "inside"}});
        bool warn = false;
        for (const auto& d : run(miss, ctxFor(0, 0, 300, 100)).diags) warn |= !d.error && d.message.find("nope") != std::string::npos;
        check(warn, "filtering on an attribute that does not exist warns");
    }

    // ---- TransformPoints ------------------------------------------------------------------------------------------------
    {
        B b = chain({"g", "CreatePointsGrid", {"spacing=100", "z=0"}},
                    {{"t", "TransformPoints", {"posMin=-20,-20,0", "posMax=20,20,50", "rotMin=0,0,0", "rotMax=0,0,360", "scaleMin=0.5,0.5,0.5", "scaleMax=2,2,2"}}});
        const PcgResult r = run(b, ctxFor(0, 0, 1000, 1000));
        bool inRange = true, varied = false;
        f32 s0 = r.groups[0].items[0].xf.scale.x;
        for (const auto& i : r.groups[0].items) {
            const Vec3 p = i.xf.position;
            const f32 cx = std::floor(p.x / 100.0f) * 100.0f + 50.0f, cy = std::floor(p.y / 100.0f) * 100.0f + 50.0f;
            inRange &= std::fabs(p.x - cx) <= 20.5f && std::fabs(p.y - cy) <= 20.5f && p.z >= 0.0f && p.z <= 50.0f;
            inRange &= i.xf.scale.x >= 0.5f && i.xf.scale.x <= 2.0f && i.xf.scale.x == i.xf.scale.y;
            varied |= i.xf.scale.x != s0;
        }
        check(inRange, "jitter, rotation and uniform scale stay inside their ranges");
        check(varied, "and actually vary from point to point");
        check(samePos(positions(run(b, ctxFor(0, 0, 1000, 1000))), positions(r)), "TransformPoints is deterministic");
        B ns = chain({"g", "CreatePointsGrid", {"spacing=100"}}, {{"t", "TransformPoints", {"scaleMin=1,1,1", "scaleMax=3,3,3", "uniformScale=0"}}});
        bool nonUniform = false;
        const PcgResult nr = run(ns, ctxFor(0, 0, 1000, 1000));
        for (const auto& i : nr.groups[0].items) nonUniform |= i.xf.scale.x != i.xf.scale.y;
        check(nonUniform, "uniformScale=0 draws each axis separately");
    }

    // ---- SelfPrune ------------------------------------------------------------------------------------------------------
    {
        const auto minDist = [](const PcgResult& r) {
            f32 best = 1e30f;
            const auto p = positions(r);
            for (usize i = 0; i < p.size(); ++i)
                for (usize j = i + 1; j < p.size(); ++j) best = std::min(best, std::sqrt((p[i].x - p[j].x) * (p[i].x - p[j].x) + (p[i].y - p[j].y) * (p[i].y - p[j].y)));
            return best;
        };
        B b = chain({"g", "CreatePointsGrid", {"spacing=20"}},
                    {{"j", "TransformPoints", {"posMin=-9,-9,0", "posMax=9,9,0"}}, {"n", "DensityNoise", {"cell=300"}}, {"p", "SelfPrune", {"radius=60"}}});
        const PcgResult r = run(b, ctxFor(0, 0, 1000, 1000));
        check(r.nodePointCount.at("p") < r.nodePointCount.at("n") && r.nodePointCount.at("p") > 50, "pruning removes points but not all of them");
        check(minDist(r) >= 60.0f, "no two surviving points are closer than the radius (greedy)");
        B loc = b;
        loc.g.nodes[3].extraTokens = {"radius=60", "mode=local"};
        const PcgResult lr = run(loc, ctxFor(0, 0, 1000, 1000));
        check(minDist(lr) >= 60.0f && lr.nodePointCount.at("p") <= r.nodePointCount.at("p"), "local mode also separates, and is never denser than greedy");

        // Priority: of two points 30 cm apart the denser one survives.
        B pr = chain({"a", "CreatePointsGrid", {"spacing=30"}}, {{"d", "AttributeSet", {"name=density", "min=0", "max=1"}}, {"p", "SelfPrune", {"radius=100"}}});
        const PcgResult prr = run(pr, ctxFor(0, 0, 60, 30));
        check(prr.instanceCount() == 1, "two points within the radius collapse to one");
        // Cells evaluated apart equal the whole in local mode (the margin is exactly the radius).
        B part = chain({"g", "CreatePointsGrid", {"spacing=20"}}, {{"n", "DensityNoise", {"cell=300"}}, {"p", "SelfPrune", {"radius=60", "mode=local"}}});
        const PcgResult whole = run(part, ctxFor(0, 0, 1000, 1000));
        PcgContext cl = ctxFor(0, 0, 560, 1000);   // grown by the radius on the open side only: the whole run has no points beyond 0..1000
        const PcgResult left = run(part, cl);
        size_t ownedL = 0, wholeL = 0;
        const auto lp = positions(left);
        const auto wp = positions(whole);
        for (const Vec3& p : wp) wholeL += p.x < 500;
        for (const Vec3& p : lp) ownedL += p.x >= 0 && p.x < 500 && p.y >= 0 && p.y < 1000;
        bool same = true;
        for (const Vec3& p : wp) if (p.x < 500) same &= std::binary_search(lp.begin(), lp.end(), p, [](const Vec3& a, const Vec3& b2) { return std::tie(a.x, a.y, a.z) < std::tie(b2.x, b2.y, b2.z); });
        check(ownedL == wholeL && same, "local-mode pruning of a region grown by the radius equals the whole run inside the region");
    }

    // ---- set operations --------------------------------------------------------------------------------------------------
    {
        // A = grid 100, B = grid 200 (B points are a subset of A positions? 100-grid centres 50,150,...; 200-grid 100,300,...) so shift via z? Use radius.
        B b;
        b.n("a", "CreatePointsGrid", {"spacing=100"}).n("c", "CreatePointsGrid", {"spacing=300"});
        b.n("d", "Difference", {"radius=110"}).l("a", "out", "d", "a").l("c", "out", "d", "b");
        b.n("sp", "WeightedMeshSpawner", {"meshes=m"}).l("d", "out", "sp", "in");
        const PcgResult r = run(b, ctxFor(0, 0, 600, 600));
        check(r.nodePointCount.at("a") == 36 && r.nodePointCount.at("c") == 4, "fixture sizes");
        check(r.nodePointCount.at("d") < 36 && r.nodePointCount.at("d") > 10, "Difference removes the A points near B");
        B i2;
        i2.n("a", "CreatePointsGrid", {"spacing=100"}).n("c", "CreatePointsGrid", {"spacing=300"});
        i2.n("x", "Intersect", {"radius=110"}).l("a", "out", "x", "a").l("c", "out", "x", "b");
        i2.n("sp", "WeightedMeshSpawner", {"meshes=m"}).l("x", "out", "sp", "in");
        const PcgResult ir = run(i2, ctxFor(0, 0, 600, 600));
        check(ir.nodePointCount.at("x") + r.nodePointCount.at("d") == 36, "Intersect and Difference partition A exactly");
        B u;
        u.n("a", "CreatePointsGrid", {"spacing=100"}).n("c", "CreatePointsGrid", {"spacing=100"});
        u.n("u", "Union", {}).l("a", "out", "u", "a").l("c", "out", "u", "b");
        u.n("sp", "WeightedMeshSpawner", {"meshes=m"}).l("u", "out", "sp", "in");
        check(run(u, ctxFor(0, 0, 600, 600)).nodePointCount.at("u") == 72, "Union concatenates");
        u.g.nodes[2].extraTokens = {"dedup=1"};
        check(run(u, ctxFor(0, 0, 600, 600)).nodePointCount.at("u") == 36, "Union with dedup drops coincident points");
    }

    // ---- landscape filters -----------------------------------------------------------------------------------------------
    {
        PcgContext c = ctxFor(-1000, 0, 1000, 400);
        c.landscape = &land;
        B s = chain({"s", "SurfaceSampler", {"spacing=200"}}, {{"f", "SlopeFilter", {"min=0", "max=5"}, "inside"}});
        check(run(s, c).nodePointCount.at("f") == 0, "an 11 degree surface fails a 0-5 degree slope filter");
        B s2 = chain({"s", "SurfaceSampler", {"spacing=200"}}, {{"f", "SlopeFilter", {"min=10", "max=13"}, "inside"}});
        check(run(s2, c).nodePointCount.at("f") == 20, "and passes 10-13");
        B s3 = chain({"g", "CreatePointsGrid", {"spacing=200"}}, {{"f", "SlopeFilter", {"min=10", "max=13", "source=landscape"}, "inside"}});
        check(run(s3, c).nodePointCount.at("f") == 20, "source=landscape recomputes the slope at the point");
        B h = chain({"s", "SurfaceSampler", {"spacing=200"}}, {{"f", "HeightFilter", {"min=100"}, "inside"}});
        check(run(h, c).nodePointCount.at("f") == 10, "HeightFilter keeps the points at or above z = 100 (x >= 0)");
        B lw = chain({"s", "SurfaceSampler", {"spacing=200"}}, {{"f", "LayerWeightFilter", {"layer=grass", "min=0.5"}, "inside"}});
        check(run(lw, c).nodePointCount.at("f") == 10, "LayerWeightFilter keeps points on the painted layer (x < 0)");
        B lw2 = chain({"g", "CreatePointsGrid", {"spacing=200"}}, {{"f", "LayerWeightFilter", {"layer=grass"}, "inside"}});
        check(hasError(run(lw2, ctxFor(0, 0, 400, 400)), "landscape"), "layer filters need a landscape");

        PcgContext d = ctxFor(0, -500, 1000, 500);
        d.splines["river"] = {Vec3{0, 0, 0}, Vec3{1000, 0, 0}};
        B ds = chain({"g", "CreatePointsGrid", {"spacing=100"}}, {{"f", "DistanceToSplineFilter", {"name=river", "max=100"}, "inside"}});
        check(run(ds, d).nodePointCount.at("f") == 20, "DistanceToSplineFilter keeps the two rows within 100 cm of the spline");
        B ds2 = chain({"g", "CreatePointsGrid", {"spacing=100"}}, {{"f", "DistanceToSplineFilter", {"name=river", "min=150"}, "inside"}});
        check(run(ds2, d).nodePointCount.at("f") == 80, "and min= keeps the far rows");
        PcgContext a = ctxFor(0, 0, 1000, 1000);
        a.actors["tower"] = {Vec3{500, 500, 0}};
        B da = chain({"g", "CreatePointsGrid", {"spacing=100"}}, {{"f", "DistanceToActorFilter", {"tag=tower", "max=120"}, "inside"}});
        check(run(da, a).nodePointCount.at("f") == 4, "DistanceToActorFilter keeps the four points around a tagged actor");
    }

    // ---- spawners -------------------------------------------------------------------------------------------------------
    {
        B b;
        b.n("g", "CreatePointsGrid", {"spacing=20"}).n("sp", "WeightedMeshSpawner", {"meshes=Meshes/a.ocmesh:3,Meshes/b.ocmesh:1", "material=M_Leaf"}).l("g", "out", "sp", "in");
        const PcgResult r = run(b, ctxFor(0, 0, 1000, 800));   // 2000 points
        check(r.instanceCount() == 2000 && r.groups.size() == 2, "a weighted spawner splits points into one group per mesh");
        const f64 share = static_cast<f64>(r.groups[0].items.size()) / 2000.0;
        check(r.groups[0].mesh == "Meshes/a.ocmesh" && r.groups[0].material == "M_Leaf" && share > 0.70 && share < 0.80, "3:1 weights give about 75% / 25%");
        check(run(b, ctxFor(0, 0, 1000, 800)).groups[0].items.size() == r.groups[0].items.size(), "the pick depends on the point seed only");
        B none;
        none.n("g", "CreatePointsGrid", {}).n("sp", "WeightedMeshSpawner", {}).l("g", "out", "sp", "in");
        check(hasError(run(none, ctxFor(0, 0, 400, 400)), "meshes"), "a spawner with no meshes reports it");

        B act;
        act.n("g", "CreatePointsGrid", {"spacing=250"}).n("sp", "ActorSpawner", {"class=Crate"}).l("g", "out", "sp", "in");
        const PcgResult ar = run(act, ctxFor(0, 0, 1000, 500));
        check(ar.actors.size() == 8 && ar.actors[0].actorClass == "Crate" && ar.groups.empty(), "an actor spawner emits one spawn record per point");
    }

    // ---- evaluator: cache, cancel, budget, cycles, validation ----------------------------------------------------------------
    {
        B b = chain({"g", "CreatePointsGrid", {"spacing=100"}}, {{"n", "DensityNoise", {"cell=500"}}, {"d", "DensityFilter", {"min=0.4"}, "inside"}});
        PcgCache cache;
        PcgContext c = ctxFor(0, 0, 2000, 2000);
        c.cache = &cache;
        const PcgResult first = run(b, c);
        check(first.nodesRun == 4 && first.nodesCached == 0, "the first run executes every node");
        const PcgResult second = run(b, c);
        check(second.nodesRun == 0 && second.nodesCached == 4 && samePos(positions(second), positions(first)), "an unchanged second run is served wholly from the cache, identically");
        b.g.nodes[2].extraTokens = {"min=0.6"};
        const PcgResult third = run(b, c);
        check(third.nodesRun == 2 && third.nodesCached == 2, "editing a node re-runs only it and what follows it");
        c.environmentStamp = 1;
        check(run(b, c).nodesCached == 0, "a changed environment stamp invalidates every entry");
        PcgContext other = ctxFor(0, 0, 1000, 1000);
        other.cache = &cache;
        check(run(b, other).nodesCached == 0, "a different region does not hit the cache");

        std::atomic<bool> stop{true};
        PcgContext cc = ctxFor(0, 0, 2000, 2000);
        cc.cancel = &stop;
        const PcgResult cr = run(b, cc);
        check(cr.cancelled && !cr.ok && cr.instanceCount() == 0, "a raised cancel flag stops evaluation and yields nothing");
        PcgContext tb = ctxFor(0, 0, 2000, 2000);
        tb.budgetMs = 1e-9;
        const PcgResult tr = run(b, tb);
        check(tr.timedOut && !tr.ok, "an exhausted time budget stops evaluation and says so");

        B cyc;
        cyc.n("a", "DensityRemap").n("b", "DensityRemap").n("sp", "WeightedMeshSpawner", {"meshes=m"});
        cyc.l("a", "out", "b", "in").l("b", "out", "a", "in").l("b", "out", "sp", "in");
        check(hasError(run(cyc, ctxFor(0, 0, 10, 10)), "cycle"), "a cycle is reported, not followed");
        const auto v = pcgValidate(cyc.g);
        bool cyclic = false;
        for (const auto& d : v) cyclic |= d.error && d.message.find("cycle") != std::string::npos;
        check(cyclic, "pcgValidate finds the cycle");

        B bad;
        bad.n("x", "NoSuchNode").n("g", "CreatePointsGrid").n("f", "DensityFilter").l("g", "nope", "f", "in").l("g", "out", "f", "in").l("g", "out", "f", "in");
        const auto vd = pcgValidate(bad.g);
        bool unk = false, pin = false, dbl = false, nosink = false;
        for (const auto& d : vd) {
            unk |= d.message.find("unknown node type") != std::string::npos;
            pin |= d.message.find("no output pin") != std::string::npos;
            dbl |= d.message.find("more than one link") != std::string::npos;
            nosink |= !d.error && d.message.find("no spawner") != std::string::npos;
        }
        check(unk && pin && dbl && nosink, "validation names an unknown type, a bad pin, a doubled input and a missing spawner");
        check(pcgValidate(b.g).empty(), "a well-formed graph validates clean");
        fmt::OcGraphData notPcg = b.g;
        notPcg.domain = "gameplay";
        check(!pcgEvaluate(notPcg, ctxFor(0, 0, 10, 10)).ok, "a gameplay graph is refused");
        check(pcgGraphHash(b.g) == pcgGraphHash(b.g) && pcgGraphHash(b.g) != pcgGraphHash(bad.g), "the graph hash tells graphs apart");
        fmt::OcGraphData moved = b.g;
        for (auto& n : moved.nodes) { n.x = 5; n.y = 6; n.hasPosition = true; }
        check(pcgGraphHash(moved) == pcgGraphHash(b.g), "and ignores canvas positions");
    }

    // ---- Subgraph / Loop ------------------------------------------------------------------------------------------------
    {
        fmt::OcGraphData lift;
        {
            B s;
            s.n("in", "Input").n("t", "TransformPoints", {"posMin=0,0,100", "posMax=0,0,100"}).n("out", "Output");
            s.l("in", "out", "t", "in").l("t", "out", "out", "in");
            lift = s.g;
        }
        fmt::OcGraphData selfRef;
        {
            B s;
            s.n("in", "Input").n("sub", "Subgraph", {"graph=self"}).n("out", "Output");
            s.l("in", "out", "sub", "in").l("sub", "out", "out", "in");
            selfRef = s.g;
        }
        PcgContext c = ctxFor(0, 0, 400, 400);
        c.graphLoader = [&](const std::string& n) -> const fmt::OcGraphData* { return n == "lift" ? &lift : n == "self" ? &selfRef : nullptr; };
        B b = chain({"g", "CreatePointsGrid", {"spacing=100"}}, {{"s", "Subgraph", {"graph=lift"}}});
        const PcgResult r = run(b, c);
        bool lifted = r.instanceCount() == 16;
        for (const auto& i : r.groups[0].items) lifted &= i.xf.position.z == 100.0f;
        check(lifted, "a Subgraph runs its graph on the input points and returns its Output");
        B lp = chain({"g", "CreatePointsGrid", {"spacing=100"}}, {{"s", "Loop", {"graph=lift"}}});
        const PcgResult lr = run(lp, c);
        bool lz = lr.instanceCount() == 16;
        for (const auto& i : lr.groups[0].items) lz &= i.xf.position.z == 100.0f;
        check(lz, "a Loop runs its graph once per input point and merges the results");
        lp.g.nodes[1].extraTokens = {"graph=lift", "max=5"};
        const PcgResult capped = run(lp, c);
        bool warned = false;
        for (const auto& d : capped.diags) warned |= !d.error && d.message.find("truncated") != std::string::npos;
        check(capped.instanceCount() == 5 && warned, "a Loop is capped by max= and says so");
        B missing = chain({"g", "CreatePointsGrid", {"spacing=100"}}, {{"s", "Subgraph", {"graph=ghost"}}});
        check(hasError(run(missing, c), "unknown graph"), "an unknown subgraph name is an error");
        B rec = chain({"g", "CreatePointsGrid", {"spacing=100"}}, {{"s", "Subgraph", {"graph=self"}}});
        check(hasError(run(rec, c), "too deep"), "a recursive subgraph stops at the nesting limit");
    }

    // ---- the .ocgraph profile -------------------------------------------------------------------------------------------
    {
        const std::string text =
            "OCGRAPH 1\n"
            "DOMAIN pcg\n"
            "NAME PG_Test\n"
            "NODE g CreatePointsGrid 0 0 spacing=100 z=0\n"
            "PIN g out out points\n"
            "NODE sp WeightedMeshSpawner 200 0 meshes=Meshes/a.ocmesh:1\n"
            "PIN sp in in points\n"
            "LINK g.out sp.in\n";
        fmt::OcGraphData g;
        std::string err;
        check(fmt::parseOcgraph(text, g, &err), "a DOMAIN pcg graph parses with the existing reader: " + err);
        check(fmt::ocGraphDomainOf(g) == fmt::OcGraphDomain::Pcg && pcgIsGraph(g), "and is classified as the pcg domain");
        check(fmt::writeOcgraph(g, text) == text, "and survives a load/save byte for byte, params included");
        PcgContext c = ctxFor(0, 0, 300, 200);
        check(pcgEvaluate(g, c).instanceCount() == 6 && pcgValidate(g).empty(), "and evaluates");
        check(fmt::ocGraphDomainName(fmt::OcGraphDomain::Pcg) == "pcg", "ocGraphDomainName knows it");
    }

    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else AVER_ERROR("=== {} assertions, {} failed ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
