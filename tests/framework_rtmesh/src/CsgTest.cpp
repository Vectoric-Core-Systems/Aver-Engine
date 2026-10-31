// CsgTest -- the BSP constructive solid geometry an importer bakes a CSG tree with (aver/formats/Csg.hpp). Volumes and bounds of simple solids and of
// their unions, intersections and differences (overlapping, disjoint, coplanar-face and nested), the generated primitives, a mirroring transform, and the welded
// triangle soup. Pure arithmetic: no device. Exits 0 or 1.
#include "aver/core/Log.hpp"
#include "aver/formats/Csg.hpp"

#include <cmath>
#include <map>
#include <string>

using namespace aver::fmt::csg;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

static Mesh boxAt(double x, double y, double z, double sx, double sy, double sz, int material = 0) {
    const double size[3] = {sx, sy, sz};
    Mesh m = makeBox(size, material);
    const double t[12] = {1, 0, 0, x + sx / 2, 0, 1, 0, y + sy / 2, 0, 0, 1, z + sz / 2};   // min corner at (x, y, z)
    transform(m, t);
    return m;
}

static void testPrimitives() {
    AVER_INFO("=== primitives ===");
    const double s2[3] = {2, 2, 2};
    Mesh box = makeBox(s2);
    check(near(volume(box), 8.0, 1e-9) && near(surfaceArea(box), 24.0, 1e-9), "a 2 m box: volume 8, area 24");
    const Bounds b = bounds(box);
    check(near(b.lo[0], -1, 1e-12) && near(b.hi[2], 1, 1e-12), "a box is centred on its origin");
    const double s3[3] = {1, 2, 3};
    check(near(volume(makeBox(s3)), 6.0, 1e-9), "an uneven box: 1 x 2 x 3 = 6");

    Mesh sph = makeSphere(1.0, 48, 24, true);
    const double sv = 4.0 / 3.0 * 3.14159265358979323846;
    check(volume(sph) > 0 && near(volume(sph), sv, sv * 0.03), "a 48 x 24 sphere's volume is within 3 percent of 4/3 pi r^3");
    const Bounds sb = bounds(sph);
    check(near(sb.hi[1], 1.0, 1e-9) && near(sb.lo[1], -1.0, 1e-9), "its poles are at +-radius");
    Mesh coarse = makeSphere(0.5, 12, 6, true);
    check(volume(coarse) > 0 && volume(coarse) < 4.0 / 3.0 * 3.14159265358979323846 * 0.125, "a 12 x 6 sphere is a solid inside the true one");

    Mesh cyl = makeCylinder(1.0, 2.0, 64, false, true);
    check(near(volume(cyl), 3.14159265358979323846 * 2.0, 0.07), "a 64-sided cylinder: pi r^2 h");
    const Bounds cb = bounds(cyl);
    check(near(cb.hi[1], 1.0, 1e-9) && near(cb.lo[1], -1.0, 1e-9), "the cylinder is centred along Y");
    Mesh cone = makeCylinder(1.0, 3.0, 64, true, true);
    check(near(volume(cone), 3.14159265358979323846 * 3.0 / 3.0, 0.06), "a cone: one third of pi r^2 h");

    Mesh torus = makeTorus(0.5, 1.5, 24, 48, true);
    const double expect = 2 * 3.14159265358979323846 * 3.14159265358979323846 * 1.0 * 0.25;   // 2 pi^2 R r^2 with R = 1, r = 0.5
    check(volume(torus) > 0 && near(volume(torus), expect, expect * 0.04), "a torus: 2 pi^2 R r^2");
    check(near(bounds(torus).hi[0], 1.5, 0.02), "a torus reaches its outer radius");

    const std::vector<double> lShape = {0, 0, 2, 0, 2, 1, 1, 1, 1, 2, 0, 2};   // three unit squares
    Mesh ext = extrudePolygon(lShape, 2.0);
    check(near(volume(ext), 6.0, 1e-9), "an L polygon (area 3) extruded 2 m: volume 6");
    const Bounds eb = bounds(ext);
    check(near(eb.lo[2], -2.0, 1e-12) && near(eb.hi[2], 0.0, 1e-12), "the extrusion runs from z = 0 to z = -depth");
    std::vector<double> cw(lShape.rbegin(), lShape.rend());
    for (size_t i = 0; i + 1 < cw.size(); i += 2) std::swap(cw[i], cw[i + 1]);   // reverse the points (not the pairs)
    check(volume(extrudePolygon(cw, 2.0)) > 5.99, "a clockwise polygon extrudes to the same solid");

    const std::vector<double> ring = {1, 0, 2, 0, 2, 1, 1, 1};
    Mesh spun = spinPolygon(ring, 360.0, 64);
    const double sp = 3.14159265358979323846 * (4 - 1) * 1.0;
    check(volume(spun) > 0 && near(volume(spun), sp, sp * 0.02), "a square spun about Y: a washer of pi (R^2 - r^2) h");
    Mesh half = spinPolygon(ring, 180.0, 32);
    check(near(volume(half), sp / 2, sp * 0.02), "spun 180 degrees with end caps: half the washer");
}

static void testBooleans() {
    AVER_INFO("=== booleans ===");
    const Mesh a = boxAt(0, 0, 0, 2, 2, 2), b = boxAt(1, 1, 1, 2, 2, 2);
    const Mesh u = combine(a, b, Op::Union);
    const Mesh i = combine(a, b, Op::Intersection);
    const Mesh d = combine(a, b, Op::Subtraction);
    check(near(volume(u), 15.0, 1e-6), "union of two 2 m boxes overlapping by 1 m^3: 8 + 8 - 1 = 15");
    check(near(volume(i), 1.0, 1e-6), "intersection: 1");
    check(near(volume(d), 7.0, 1e-6), "subtraction: 8 - 1 = 7");
    const Bounds ub = bounds(u), ib = bounds(i), db = bounds(d);
    check(near(ub.lo[0], 0, 1e-9) && near(ub.hi[0], 3, 1e-9) && near(ub.hi[2], 3, 1e-9), "the union's bounds span both boxes");
    check(near(ib.lo[0], 1, 1e-9) && near(ib.hi[0], 2, 1e-9) && near(ib.lo[2], 1, 1e-9), "the intersection's bounds are the overlap");
    check(near(db.lo[0], 0, 1e-9) && near(db.hi[0], 2, 1e-9) && near(db.hi[2], 2, 1e-9), "the difference stays inside the first box");
    check(near(surfaceArea(u), 42.0, 1e-6), "the union's skin: 24 + 24 - 3 - 3 square metres of hidden faces = 42");
    check(near(surfaceArea(d), 24.0, 1e-6), "the difference's skin: A's 24 minus 3 cut away plus 3 cut faces = 24");

    const Mesh far = boxAt(10, 10, 10, 1, 1, 1);
    check(near(volume(combine(a, far, Op::Union)), 9.0, 1e-6), "a disjoint union is the sum");
    check(combine(a, far, Op::Intersection).empty() || near(volume(combine(a, far, Op::Intersection)), 0.0, 1e-9), "a disjoint intersection is empty");
    check(near(volume(combine(a, far, Op::Subtraction)), 8.0, 1e-6), "subtracting a far box changes nothing");

    const Mesh inner = boxAt(0.5, 0.5, 0.5, 1, 1, 1);
    check(near(volume(combine(a, inner, Op::Subtraction)), 7.0, 1e-6), "a box with a hole punched inside it: 8 - 1 = 7 (a hollow)");
    check(near(volume(combine(a, inner, Op::Union)), 8.0, 1e-6), "a box swallowing another: unchanged");
    check(near(volume(combine(a, inner, Op::Intersection)), 1.0, 1e-6), "and the intersection is the small one");

    const Mesh halfTop = boxAt(0, 0, 1, 2, 2, 1);   // shares faces with a
    check(near(volume(combine(a, halfTop, Op::Subtraction)), 4.0, 1e-6), "subtracting a box that shares faces: 8 - 4 = 4");
    check(near(volume(combine(a, halfTop, Op::Union)), 8.0, 1e-6), "union with a box on shared faces: 8");
    check(near(volume(combine(a, halfTop, Op::Intersection)), 4.0, 1e-6), "intersection with a box on shared faces: 4");

    // A sphere cut from a box: volume = box - sphere inside it.
    const double s2[3] = {4, 4, 4};
    Mesh big = makeBox(s2);
    Mesh ball = makeSphere(1.0, 32, 16, true);
    const double sv = volume(ball);
    check(near(volume(combine(big, ball, Op::Subtraction)), 64.0 - sv, 1e-3), "a box minus a sphere inside it: 64 minus the sphere's volume");
    // A cylinder bored through a cube.
    const double c2[3] = {2, 2, 2};
    Mesh cube = makeBox(c2);
    Mesh bore = makeCylinder(0.5, 4.0, 32, false, true);
    const double boreInside = volume(bore) * 0.5;   // the 2 m of the bore that lies inside the cube
    check(near(volume(combine(cube, bore, Op::Subtraction)), 8.0 - boreInside, 0.01), "a bored cube: 8 minus the 2 m of bore inside it");

    // Materials survive: the surface cut by B comes from B's polygons.
    const Mesh am = boxAt(0, 0, 0, 2, 2, 2, 0), bm = boxAt(1, 1, 1, 2, 2, 2, 1);
    const Mesh dm = combine(am, bm, Op::Subtraction);
    bool has0 = false, has1 = false;
    for (const Polygon& p : dm.polys) { has0 |= p.material == 0; has1 |= p.material == 1; }
    check(has0 && has1, "a difference keeps A's material outside and B's on the cut faces");

    // Left-to-right chaining, as a CSG tree folds its children.
    const Mesh chain = combine(combine(a, b, Op::Union), boxAt(2, 2, 2, 2, 2, 2), Op::Subtraction);
    check(near(volume(chain), 15.0 - 1.0, 1e-6), "chained operations: (A + B) - C");
}

static void testTransformAndSoup() {
    AVER_INFO("=== transform and soup ===");
    Mesh a = boxAt(0, 0, 0, 1, 2, 3);
    const double mirror[12] = {-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    transform(a, mirror);
    check(near(volume(a), 6.0, 1e-9), "a mirroring transform reverses the winding, so the volume stays positive");
    const Bounds mb = bounds(a);
    check(near(mb.lo[0], -1, 1e-12) && near(mb.hi[0], 0, 1e-12), "and the box moved to x in [-1, 0]");
    int faces = 0;
    bool outward = true;
    for (const Polygon& p : a.polys) {
        bool flat = true;
        for (const Vertex& v : p.v) flat = flat && std::fabs(v.p[0] + 1.0) < 1e-9;
        if (!flat) continue;
        ++faces;
        for (const Vertex& v : p.v) outward = outward && v.n[0] < -0.99;
    }
    check(faces == 1 && outward, "the face now at x = -1 has its normal pointing -X");
    const double scale[12] = {2, 0, 0, 0, 0, 3, 0, 0, 0, 0, 4, 0};
    Mesh s = boxAt(0, 0, 0, 1, 1, 1);
    transform(s, scale);
    check(near(volume(s), 24.0, 1e-9), "a scale transform: 2 x 3 x 4");
    transform(s, scale);
    Mesh flip = boxAt(0, 0, 0, 1, 1, 1);
    invertFaces(flip);
    check(near(volume(flip), -1.0, 1e-9), "invertFaces turns a solid inside out (negative volume)");

    const Mesh bx = boxAt(0, 0, 0, 1, 1, 1);
    const Soup soup = toSoup(bx, 0.001);
    check(soup.pos.size() / 3 >= 8 && soup.idx.size() == 36, "a box soups to 12 triangles");
    check(soup.materials.size() == 1 && soup.rangeCount[0] == 36, "in one material range");
    Mesh two = combine(boxAt(0, 0, 0, 2, 2, 2, 0), boxAt(1, 1, 1, 2, 2, 2, 1), Op::Union);
    const Soup s2 = toSoup(two, 0.001);
    check(s2.materials.size() == 2 && s2.rangeStart[0] == 0 && s2.rangeStart[1] == s2.rangeCount[0], "a two-material result gives two consecutive ranges");
    check(s2.nrm.size() == s2.pos.size() && s2.uv.size() / 2 == s2.pos.size() / 3, "normals and UVs follow the vertices");
    double len2 = 0;
    for (size_t i = 0; i < s2.nrm.size(); i += 3) len2 += std::sqrt(double(s2.nrm[i]) * s2.nrm[i] + double(s2.nrm[i + 1]) * s2.nrm[i + 1] + double(s2.nrm[i + 2]) * s2.nrm[i + 2]);
    check(near(len2 / double(s2.nrm.size() / 3), 1.0, 1e-5), "every soup normal is a unit vector");

    const std::vector<float> pos = {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1};
    const std::vector<uint32_t> idx = {0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3};   // a tetrahedron, outward
    const Mesh tet = fromTriangles(pos, {}, {}, idx);
    check(tet.polys.size() == 4 && near(std::fabs(volume(tet)), 1.0 / 6.0, 1e-9), "fromTriangles builds a tetrahedron of volume 1/6");
}

static void testSweep() {
    AVER_INFO("=== sweeping a polygon along a path ===");
    const std::vector<double> rect = {-1, -0.5, 1, -0.5, 1, 0.5, -1, 0.5};   // 2 x 1, centred
    const std::vector<double> none;

    // Along -Z the fixed frame is extrudePolygon laid on the path.
    Mesh straight = sweepPolygon(rect, {0, 0, 0, 0, 0, -4}, none, SweepFrame::Fixed, false);
    check(near(volume(straight), 8.0, 1e-9), "a 2 x 1 polygon along 4 m of path: volume 8, outward wound");
    Bounds b = bounds(straight);
    check(near(b.lo[2], -4, 1e-9) && near(b.hi[2], 0, 1e-9) && near(b.hi[0], 1, 1e-9) && near(b.hi[1], 0.5, 1e-9), "and it spans the path with the polygon's own orientation");
    check(near(volume(extrudePolygon(rect, 4.0)), volume(straight), 1e-9), "the same solid extrudePolygon gives");
    const std::vector<double> cw = {-1, -0.5, -1, 0.5, 1, 0.5, 1, -0.5};
    check(near(volume(sweepPolygon(cw, {0, 0, 0, 0, 0, -4}, none, SweepFrame::Fixed, false)), 8.0, 1e-9), "a clockwise polygon sweeps to the same solid");

    // Along +X the path frame turns the polygon's x onto world Z and keeps its y up.
    Mesh along = sweepPolygon(rect, {0, 0, 0, 5, 0, 0}, none, SweepFrame::Path, false);
    b = bounds(along);
    check(near(volume(along), 10.0, 1e-9) && near(b.lo[0], 0, 1e-9) && near(b.hi[0], 5, 1e-9), "path frame along +X: volume 10 over 5 m");
    check(near(b.hi[2], 1, 1e-9) && near(b.lo[2], -1, 1e-9) && near(b.hi[1], 0.5, 1e-9), "with the polygon's x on world Z and its y up");
    Mesh reversed = sweepPolygon(rect, {5, 0, 0, 0, 0, 0}, none, SweepFrame::Path, false);
    check(near(volume(reversed), 10.0, 1e-9), "the path walked backwards is the same solid");

    // A quarter turn: area times the length of the centre line, a little under for the mitred corners.
    std::vector<double> arc;
    const double pi = 3.14159265358979323846, R = 5.0;
    for (int i = 0; i <= 32; ++i) { const double a = pi * 0.5 * i / 32; arc.insert(arc.end(), {R * std::sin(a), 0.0, -R * (1.0 - std::cos(a))}); }
    const Mesh bend = sweepPolygon(rect, arc, none, SweepFrame::Path, false);
    check(near(volume(bend), 2.0 * R * pi * 0.5, 2.0 * R * pi * 0.5 * 0.01), "a quarter-circle path of radius 5: area x arc length within 1 percent");

    // A closed loop has no caps: 24 points x 4 edges x 2 triangles.
    std::vector<double> loop;
    for (int i = 0; i < 24; ++i) { const double a = 2 * pi * i / 24; loop.insert(loop.end(), {4.0 * std::cos(a), 0.0, 4.0 * std::sin(a)}); }
    const std::vector<double> sq = {-0.5, -0.5, 0.5, -0.5, 0.5, 0.5, -0.5, 0.5};
    const Mesh ring = sweepPolygon(sq, loop, none, SweepFrame::Path, true);
    check(ring.polys.size() == 24 * 4 * 2, "a closed path is not capped");
    check(near(volume(ring), 2 * pi * 4.0, 2 * pi * 4.0 * 0.02), "a closed 24-point loop of radius 4: area x circumference within 2 percent");
    std::vector<double> loopRepeated = loop;
    loopRepeated.insert(loopRepeated.end(), {loop[0], loop[1], loop[2]});
    check(sweepPolygon(sq, loopRepeated, none, SweepFrame::Path, true).polys.size() == ring.polys.size(), "a last point equal to the first is the same loop");

    // Follow with a chosen up: a path along -Z with +X as up turns the 2 x 1 polygon a quarter turn about the path.
    const std::vector<double> upX = {1, 0, 0, 1, 0, 0};
    b = bounds(sweepPolygon(rect, {0, 0, 0, 0, 0, -4}, upX, SweepFrame::Follow, false));
    check(near(b.hi[0], 0.5, 1e-9) && near(b.hi[1], 1.0, 1e-9) && near(b.lo[2], -4, 1e-9), "path follow takes the up vector it is given");

    check(sweepPolygon(rect, {0, 0, 0}, none, SweepFrame::Path, false).empty(), "one point is no path");
    check(sweepPolygon(rect, {0, 0, 0, 1, 0, 0}, none, SweepFrame::Path, true).empty(), "nor is a closed path of two");
    check(sweepPolygon(rect, {0, 0, 0, 0, 0, 0, 3, 0, 0}, none, SweepFrame::Path, false).polys.size() == sweepPolygon(rect, {0, 0, 0, 3, 0, 0}, none, SweepFrame::Path, false).polys.size(),
          "a repeated point is dropped");
    const Soup soup = toSoup(along, 0.001);
    check(!soup.idx.empty() && soup.uv.size() / 2 == soup.pos.size() / 3, "and the sweep soups like any other solid");
}

int main() {
    testPrimitives();
    testBooleans();
    testTransformAndSoup();
    testSweep();
    AVER_INFO("{} checks, {} failed", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
