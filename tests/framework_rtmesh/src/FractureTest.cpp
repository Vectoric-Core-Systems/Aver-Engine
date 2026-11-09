// FractureTest -- Voronoi brittle fracture (aver/formats/Fracture.hpp) and the .ocfrac asset (aver/formats/OcFrac.hpp). Pure geometry: no device, no physics.
// Checks are invariants worked out without the code under test: the pieces' volumes sum to the source volume, every piece is closed, pieces do not overlap,
// a brick lattice gives the contact graph of a 3D grid with exact face areas, the same seed gives the same pieces, and the asset round-trips. Also times a
// fracture of a few-thousand-polygon mesh. Exits 0 or 1.
#include "aver/core/Log.hpp"
#include "aver/formats/Csg.hpp"
#include "aver/formats/Fracture.hpp"
#include "aver/formats/OcFrac.hpp"

#include <chrono>
#include <cmath>
#include <string>

using namespace aver;
using namespace aver::fmt;
namespace fr = aver::fmt::fracture;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static std::string num(double v) { char b[64]; std::snprintf(b, sizeof b, "%.6g", v); return b; }

static csg::Mesh boxAt(double x, double y, double z, double sx, double sy, double sz) {
    const double size[3] = {sx, sy, sz};
    csg::Mesh m = csg::makeBox(size, 0);
    const double t[12] = {1, 0, 0, x + sx / 2, 0, 1, 0, y + sy / 2, 0, 0, 1, z + sz / 2};
    csg::transform(m, t);
    return m;
}

static double sumVolume(const fr::Result& r) {
    double s = 0;
    for (const fr::Piece& p : r.pieces) s += p.volume;
    return s;
}

static void checkPieces(const fr::Result& r, double srcVol, const std::string& what) {
    check(!r.pieces.empty(), what + ": produced pieces");
    check(std::fabs(sumVolume(r) - srcVol) <= srcVol * 1e-6, what + ": volumes sum to the source (" + num(sumVolume(r)) + " vs " + num(srcVol) + ")");
    bool closed = true, positive = true, hasInside = true;
    for (const fr::Piece& p : r.pieces) {
        closed = closed && fr::isClosed(p.mesh);
        positive = positive && p.volume > 0 && std::fabs(csg::volume(p.mesh) - p.volume) < 1e-9 * srcVol + 1e-12;
        bool in = false;
        for (const csg::Polygon& q : p.mesh.polys) if (q.material == r.insideMaterial) { in = true; break; }
        hasInside = hasInside && in;
    }
    check(closed, what + ": every piece is a closed surface");
    check(positive, what + ": every piece has positive volume");
    check(hasInside, what + ": every piece has inside faces");
}

// pairs whose boxes overlap: their intersection volume must vanish
static void checkNoOverlap(const fr::Result& r, double srcVol, const std::string& what) {
    int tested = 0;
    double worst = 0;
    for (size_t i = 0; i < r.pieces.size() && tested < 120; ++i) {
        const csg::Bounds bi = csg::bounds(r.pieces[i].mesh);
        for (size_t j = i + 1; j < r.pieces.size() && tested < 120; ++j) {
            const csg::Bounds bj = csg::bounds(r.pieces[j].mesh);
            bool hit = true;
            for (int k = 0; k < 3; ++k) if (bi.lo[k] > bj.hi[k] - 1e-9 || bj.lo[k] > bi.hi[k] - 1e-9) hit = false;
            if (!hit) continue;
            ++tested;
            worst = std::max(worst, std::fabs(csg::volume(csg::combine(r.pieces[i].mesh, r.pieces[j].mesh, csg::Op::Intersection))));
        }
    }
    check(tested > 0 && worst <= srcVol * 1e-6, what + ": " + std::to_string(tested) + " box-overlapping pairs share no volume (worst " + num(worst) + ")");
}

static void testUniformBox() {
    AVER_INFO("=== uniform seeds in a box ===");
    const csg::Mesh src = boxAt(0, 0, 0, 10, 8, 6);
    const double vol = 480;
    const auto seeds = fr::seedsUniform(src, 24, 7);
    check(seeds.size() == 24, "24 seeds placed inside the box");
    const fr::Result r = fr::voronoi(src, seeds);
    checkPieces(r, vol, "box");
    check(r.pieces.size() == 24, "one piece per seed, got " + std::to_string(r.pieces.size()));
    check(r.insideMaterial == 1, "inside material is one past the source's");
    checkNoOverlap(r, vol, "box");

    bool uv = false;
    for (const csg::Polygon& q : r.pieces[0].mesh.polys)
        if (q.material == r.insideMaterial) for (const csg::Vertex& v : q.v) if (v.uv[0] != 0 || v.uv[1] != 0) uv = true;
    check(uv, "inside faces carry planar UVs");
    const fr::Result r2 = fr::voronoi(src, seeds, [] { fr::Options o; o.insideMaterial = 5; return o; }());
    bool five = false;
    for (const csg::Polygon& q : r2.pieces[0].mesh.polys) if (q.material == 5) five = true;
    check(r2.insideMaterial == 5 && five, "Options::insideMaterial is honoured");
}

static void testDeterministic() {
    AVER_INFO("=== determinism ===");
    const csg::Mesh src = boxAt(0, 0, 0, 10, 10, 10);
    const fr::Result a = fr::voronoi(src, fr::seedsUniform(src, 30, 11));
    const fr::Result b = fr::voronoi(src, fr::seedsUniform(src, 30, 11));
    const fr::Result c = fr::voronoi(src, fr::seedsUniform(src, 30, 12));
    bool same = a.pieces.size() == b.pieces.size() && a.links.size() == b.links.size();
    for (size_t i = 0; same && i < a.pieces.size(); ++i)
        same = a.pieces[i].volume == b.pieces[i].volume && a.pieces[i].mesh.polys.size() == b.pieces[i].mesh.polys.size();
    check(same, "the same seed gives bit-identical piece volumes and polygon counts");
    check(!c.pieces.empty() && c.pieces[0].volume != a.pieces[0].volume, "a different seed gives different pieces");
}

static void testBrickGraph() {
    AVER_INFO("=== brick lattice: contact graph of a 3x3x3 grid ===");
    const csg::Mesh src = boxAt(0, 0, 0, 9, 9, 9);
    const auto seeds = fr::seedsBrick(csg::bounds(src), {3, 3, 3}, 0.0, 0.0, 1);
    check(seeds.size() == 27, "27 brick seeds");
    const fr::Result r = fr::voronoi(src, seeds);
    checkPieces(r, 729, "bricks");
    check(r.pieces.size() == 27, "27 pieces");
    check(r.links.size() == 54, "54 links = 3 * (2*3*3), got " + std::to_string(r.links.size()));
    bool areas = true;
    for (const fr::Link& l : r.links) areas = areas && std::fabs(l.area - 9.0) < 1e-6;
    check(areas, "every link is a 3x3 face of area 9");
    for (const fr::Piece& p : r.pieces) if (std::fabs(p.volume - 27.0) > 1e-6) { check(false, "each brick has volume 27"); break; }

    // anchors: the bottom layer (z = 0) rests on the ground
    fr::Graph g = fr::makeGraph(r);
    fr::anchorByPlane(g, r, {0, 0, 1}, 0.0, 1e-6);
    int anchored = 0;
    for (u8 a : g.anchor) anchored += a;
    check(anchored == 9, "plane z=0 anchors the 9 bottom bricks, got " + std::to_string(anchored));

    u32 n = 0;
    std::vector<u8> none(g.links.size(), 0);
    fr::components(g.count, g.links, none, &n);
    check(n == 1, "unbroken, the whole lattice is one component");
    // break every link of the brick that owns the centre seed (index 13): it becomes its own component
    std::vector<u8> broken(g.links.size(), 0);
    for (size_t i = 0; i < g.links.size(); ++i) if (g.links[i].a == 13 || g.links[i].b == 13) broken[i] = 1;
    const auto comp = fr::components(g.count, g.links, broken, &n);
    check(n == 2, "cutting the centre brick free leaves 2 components");
    int alone = 0;
    for (u32 i = 0; i < g.count; ++i) if (comp[i] == comp[13]) ++alone;
    check(alone == 1, "and the centre brick is alone in its component");
    // cut a layer: a horizontal slab split off
    std::vector<u8> cut(g.links.size(), 0);
    for (size_t i = 0; i < g.links.size(); ++i) {
        const u32 za = g.links[i].a / 9, zb = g.links[i].b / 9;
        if (za != zb && std::min(za, zb) == 1) cut[i] = 1;
    }
    fr::components(g.count, g.links, cut, &n);
    check(n == 2, "cutting between layers 1 and 2 gives 2 components");
}

static void testNeighbourSearch() {
    AVER_INFO("=== bounded neighbour search at a few hundred seeds ===");
    const csg::Mesh src = boxAt(0, 0, 0, 20, 20, 20);
    const auto seeds = fr::seedsUniform(src, 300, 3);
    const auto t0 = std::chrono::steady_clock::now();
    const fr::Result r = fr::voronoi(src, seeds);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    AVER_INFO("  300 seeds in a 12-polygon box: {} pieces in {} ms", r.pieces.size(), ms);
    check(std::fabs(sumVolume(r) - 8000.0) <= 8000.0 * 1e-6, "300 cells tile the box exactly (a missed neighbour would overlap): " + num(sumVolume(r)));
    check(r.pieces.size() == 300, "all 300 cells survive");
    // Euler-ish sanity: a connected tiling has at least n-1 links
    check(r.links.size() >= r.pieces.size() - 1, "contact graph is at least a spanning structure (" + std::to_string(r.links.size()) + " links)");
    u32 n = 0;
    fr::components(static_cast<u32>(r.pieces.size()), r.links, {}, &n);
    check(n == 1, "and it is one connected component");
}

static void testPatterns() {
    AVER_INFO("=== seed patterns ===");
    const csg::Mesh src = boxAt(-5, -5, -5, 10, 10, 10);
    const auto rad = fr::seedsRadial({0, 0, 0}, 4.0, 3, 6, 0.3, 5);
    check(rad.size() == 1 + 6 * (1 + 2 + 3), "radial: centre + 6*k per shell = 37, got " + std::to_string(rad.size()));
    const fr::Result rr = fr::voronoi(src, rad);
    checkPieces(rr, 1000, "radial");
    // cells near the centre are smaller than the ones near the rim
    double centre = 1e300, rim = 0;
    for (const fr::Piece& p : rr.pieces) {
        const double d = std::sqrt(p.centroid[0] * p.centroid[0] + p.centroid[1] * p.centroid[1] + p.centroid[2] * p.centroid[2]);
        if (d < 1.5) centre = std::min(centre, p.volume);
        if (d > 4.0) rim = std::max(rim, p.volume);
    }
    check(rim > centre, "radial pattern: outer pieces are larger than the centre ones");

    const auto clu = fr::seedsClustered(src, {0, 0, 0}, 40, 9);
    check(clu.size() == 40, "clustered: 40 seeds inside");
    int near = 0;
    for (const auto& s : clu) if (std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]) < 4.0) ++near;
    check(near > 20, "clustered: most seeds sit near the centre, " + std::to_string(near) + "/40");
    checkPieces(fr::voronoi(src, clu), 1000, "clustered");

    const auto stag = fr::seedsBrick(csg::bounds(src), {5, 5, 5}, 0.5, 0.1, 2);
    check(stag.size() == 8, "staggered bricks: 2x2x2");
    const fr::Result sr = fr::voronoi(src, stag);
    checkPieces(sr, 1000, "staggered bricks");
    check(fr::seedsUniform(src, 10, 1) == fr::seedsUniform(src, 10, 1), "uniform seeds repeat for the same seed");
}

static void testNonConvex() {
    AVER_INFO("=== a non-convex source (L-shaped extrusion) ===");
    const std::vector<double> L = {0, 0, 6, 0, 6, 2, 2, 2, 2, 6, 0, 6};   // area 6*2 + 2*4 = 20
    csg::Mesh src = csg::extrudePolygon(L, 3.0, 0);
    const double vol = std::fabs(csg::volume(src));
    check(std::fabs(vol - 60.0) < 1e-6, "L extrusion volume 60");
    const fr::Result r = fr::voronoi(src, fr::seedsUniform(src, 18, 21));
    checkPieces(r, vol, "L shape");
    checkNoOverlap(r, vol, "L shape");
}

static void testTimingAndAsset() {
    AVER_INFO("=== a few-thousand-polygon mesh: timing, then the asset ===");
    const csg::Mesh src = csg::makeSphere(10.0, 72, 48, true, 0);
    check(src.polys.size() >= 3000, "sphere has " + std::to_string(src.polys.size()) + " polygons");
    const double vol = csg::volume(src);
    for (int count : {32, 96}) {
        const auto seeds = fr::seedsUniform(src, count, 5);
        const auto t0 = std::chrono::steady_clock::now();
        const fr::Result r = fr::voronoi(src, seeds);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        AVER_INFO("  FRACTURE TIME: {} polygons, {} seeds -> {} pieces, {} links: {} ms", src.polys.size(), count, r.pieces.size(), r.links.size(), ms);
        check(std::fabs(sumVolume(r) - vol) <= vol * 1e-6, std::to_string(count) + " seeds: volumes sum to the sphere's");
        if (count == 32) {
            fr::Graph g = fr::makeGraph(r);
            fr::anchorByPlane(g, r, {0, 0, 1}, -9.5, 0.0);
            OcFracBuild bo;
            bo.snap = 1e-5;
            bo.toughness = 2.0;
            const OcFracData d = buildOcFrac(r, g, bo);
            std::string why;
            check(validateOcFrac(d, &why), "built asset validates " + why);
            check(d.pieces.size() == r.pieces.size() && d.links.size() == r.links.size(), "piece and link counts carry over");
            check(std::fabs(d.links[0].threshold - 2.0 * d.links[0].area) < 1e-4, "link threshold = toughness * area");
            std::vector<u8> bytes;
            check(writeOcFrac(d, bytes, &why), "writes " + why);
            OcFracData back;
            check(parseOcFrac(bytes.data(), bytes.size(), back, &why), "parses back " + why);
            check(back.verts == d.verts && back.indices == d.indices && back.hull == d.hull && back.anchors == d.anchors, "pools round-trip bit-exact");
            bool same = back.pieces.size() == d.pieces.size() && back.links.size() == d.links.size() && back.ranges.size() == d.ranges.size();
            for (size_t i = 0; same && i < d.pieces.size(); ++i)
                same = back.pieces[i].volume == d.pieces[i].volume && back.pieces[i].hullStart == d.pieces[i].hullStart && back.pieces[i].rangeCount == d.pieces[i].rangeCount;
            for (size_t i = 0; same && i < d.links.size(); ++i)
                same = back.links[i].a == d.links[i].a && back.links[i].threshold == d.links[i].threshold;
            check(same && back.insideMaterial == d.insideMaterial, "records round-trip");
            double tri = 0;
            for (const OcFracPiece& p : d.pieces) tri += p.volume;
            check(std::fabs(tri - d.sourceVolume) < 1e-3 * vol, "source volume recorded");

            // validation
            OcFracData bad = d; bad.indices[0] = d.vertexCount() + 3;
            check(!validateOcFrac(bad, &why), "index past the pool is refused: " + why);
            bad = d; bad.links[0].b = bad.links[0].a;
            check(!validateOcFrac(bad, &why), "self link is refused: " + why);
            bad = d; bad.anchors.pop_back();
            check(!validateOcFrac(bad, &why), "anchor count mismatch is refused: " + why);
            bad = d; bad.pieces[0].hullCount = 2;
            check(!validateOcFrac(bad, &why), "a 2-point hull is refused: " + why);
            std::vector<u8> cut(bytes.begin(), bytes.begin() + static_cast<long>(bytes.size() / 2));
            check(!parseOcFrac(cut.data(), cut.size(), back, &why), "a truncated file is refused");
            std::vector<u8> junk(bytes);
            junk[0] ^= 0xFF;
            check(!parseOcFrac(junk.data(), junk.size(), back, &why), "a corrupt header is refused");
        }
    }
}

int main() {
    testUniformBox();
    testDeterministic();
    testBrickGraph();
    testNeighbourSearch();
    testPatterns();
    testNonConvex();
    testTimingAndAsset();
    AVER_INFO("{} checks, {} failed", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
