// EditMeshTest -- aver/formats/EditMesh.hpp: the editable polygon mesh behind the Modeling mode. Validation, triangulation, conversions to csg and .ocmesh data,
// primitives, every poly edit operation with a topology or volume invariant, booleans with known volumes, UV tools and the authored-mesh save / reopen pair.
// Pure arithmetic and temp files: no device. Exits 0 or 1.
#include "aver/core/Log.hpp"
#include "aver/formats/EditMesh.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <set>
#include <string>

using namespace aver::fmt::em;
using aver::fmt::OcMeshData;

static int g_checks = 0, g_failures = 0;
static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}
static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

static int eulerChar(const EditMesh& m) {
    std::set<aver::u32> vs;
    std::set<Edge> es;
    for (const Face& f : m.faces)
        for (size_t i = 0; i < f.v.size(); ++i) { vs.insert(f.v[i]); es.insert(makeEdge(f.v[i], f.v[(i + 1) % f.v.size()])); }
    return int(vs.size()) - int(es.size()) + int(m.faces.size());
}
static bool closedSolid(const EditMesh& m, std::string* msg = nullptr) {
    const Validation v = validate(m);
    if (msg) *msg = v.message;
    return v.closed();
}
static EditMesh cube(double s = 100.0) { PrimitiveOptions o; return makeBox({s, s, s}, o); }
static aver::u32 topFace(const EditMesh& m) {
    aver::u32 best = 0; double bz = -1e300;
    for (aver::u32 i = 0; i < m.faces.size(); ++i) { const V3 n = faceNormal(m, i); const double z = faceCentroid(m, i).z; if (n.z > 0.99 && z > bz) { bz = z; best = i; } }
    return best;
}
static aver::u32 findVertex(const EditMesh& m, const V3& p) {
    for (aver::u32 i = 0; i < m.pos.size(); ++i) if (length(m.pos[i] - p) < 1e-6) return i;
    return 0xFFFFFFFFu;
}
static EditMesh moved(EditMesh m, const V3& d) { for (V3& p : m.pos) p += d; return m; }

static void testCore() {
    AVER_INFO("=== core: validation, triangulation, conversions ===");
    EditMesh b = cube();
    std::string msg;
    check(closedSolid(b, &msg), "a box is a closed manifold");
    check(near(volume(b), 1e6, 1e-3) && near(surfaceArea(b), 6e4, 1e-3), "box volume 1e6, area 6e4");
    check(eulerChar(b) == 2, "box Euler characteristic 2");
    const Bounds3 bb = bounds(b);
    check(near(bb.lo.x, -50, 1e-9) && near(bb.hi.z, 50, 1e-9), "bounds");

    EditMesh open = b;
    open.faces.pop_back();
    const Validation vo = validate(open);
    check(vo.openEdges == 4 && !vo.closed() && vo.manifold(), "a box with a face missing has 4 open edges and is still manifold");
    check(vo.message.find("open edges") != std::string::npos, "the message names the open edges");

    EditMesh mixed = b;
    flipFaces(mixed, {0});
    check(validate(mixed).inconsistentEdges > 0 && !validate(mixed).closed(), "one flipped face is mixed winding");

    EditMesh nm = b;
    nm.addFace({0, 1, 2});
    check(validate(nm).nonManifoldEdges + validate(nm).inconsistentEdges > 0, "a third face on an edge is not manifold");

    EditMesh dg = b;
    dg.faces[0].v[1] = dg.faces[0].v[0];
    check(validate(dg).degenerateFaces == 1, "a repeated corner is a degenerate face");
    check(removeDegenerateFaces(dg) == 0 || dg.faces[0].v.size() == 3, "removeDegenerateFaces collapses the repeat to a triangle");

    // an L-shaped (concave) face triangulates to its area
    EditMesh l;
    const double pts[6][2] = {{0, 0}, {20, 0}, {20, 10}, {10, 10}, {10, 20}, {0, 20}};
    std::vector<aver::u32> ids;
    for (const auto& p : pts) ids.push_back(l.addVertex({p[0], p[1], 0}));
    l.addFace(ids);
    check(triangulate(l).size() == 4, "the L face is four triangles");
    check(near(surfaceArea(l), 300.0, 1e-9) && near(faceArea(l, 0), 300.0, 1e-9), "the L face keeps its area 300 (a fan would give 350)");
    for (const Tri& t : triangulate(l)) {
        const V3 a = l.pos[l.faces[0].v[t.c[0]]], c1 = l.pos[l.faces[0].v[t.c[1]]], c2 = l.pos[l.faces[0].v[t.c[2]]];
        check(cross(c1 - a, c2 - a).z > 0, "every L triangle keeps the winding");
    }

    // csg round trip
    const aver::fmt::csg::Mesh c = toCsg(b);
    check(c.polys.size() == 6 && near(aver::fmt::csg::volume(c), 1e6, 1e-3), "toCsg keeps the six quads and the volume");
    EditMesh back = fromCsg(c);
    check(back.faces.size() == 6 && back.pos.size() == 8 && closedSolid(back) && near(volume(back), 1e6, 1e-3), "fromCsg welds back to 8 vertices, 6 faces, closed");
    check(toCsg(l).polys.size() == 4, "a concave face reaches csg as triangles");

    // .ocmesh round trip
    const OcMeshData d = toOcMesh(b);
    check(d.valid() && d.indices.size() == 36 && d.vertexCount() == 24, "toOcMesh: 12 triangles, 24 vertices (flat normals) valid=" + std::to_string(d.valid()) + " idx=" + std::to_string(d.indices.size()) + " v=" + std::to_string(d.vertexCount()));
    check(aver::fmt::submeshesPartitionIndices(d) && d.hasTangent(), "submeshes cover the indices; tangents exist");
    check(near(d.boundsMax.z, 50, 1e-4) && near(d.boundsMin.x, -50, 1e-4), "ocmesh bounds");
    std::vector<aver::u8> bytes;
    OcMeshData parsed;
    std::string why;
    check(aver::fmt::writeOcMesh(d, bytes, &why) && aver::fmt::parseOcMesh(bytes.data(), bytes.size(), parsed, &why), "the .ocmesh writes and parses");
    EditMesh fromOc;
    check(fromOcMesh(parsed, fromOc, &why), "fromOcMesh accepts it");
    check(fromOc.faces.size() == 12 && fromOc.pos.size() == 8 && closedSolid(fromOc) && near(volume(fromOc), 1e6, 1.0), "welded back to 8 vertices, 12 triangles, closed, volume kept");

    EditMesh sph = makeSphere(50, 16, 8, {});
    const OcMeshData ds = toOcMesh(sph);
    bool unit = true;
    for (aver::usize i = 0; i < ds.normals.size(); i += 3) unit &= near(std::sqrt(ds.normals[i] * ds.normals[i] + ds.normals[i + 1] * ds.normals[i + 1] + ds.normals[i + 2] * ds.normals[i + 2]), 1.0, 1e-3);
    check(unit, "smooth normals are unit length");
    check(ds.vertexCount() < ds.indices.size() / 2, "smooth vertices are shared between triangles");

    // two materials become two submeshes in slot order
    EditMesh two = cube();
    two.materials = {"stone", "wood"};
    two.faces[0].material = 1;
    two.faces[1].material = 1;
    const OcMeshData dt = toOcMesh(two);
    check(dt.submeshes.size() == 2 && dt.submeshes[0].name == "stone" && dt.submeshes[1].name == "wood" && dt.submeshes[1].indexCount == 12 && dt.materialSlots.size() == 2,
          "materials become submeshes by slot");
    EditMesh twoBack;
    check(fromOcMesh(dt, twoBack) && twoBack.materials.size() == 2, "and come back as materials");
    int onWood = 0;
    for (const Face& f : twoBack.faces) onWood += f.material == 1;
    check(onWood == 4, "the two quads on slot 1 are four triangles");

    OcMeshData skinned = d;
    skinned.joints.assign(skinned.vertexCount() * 4, 0);
    skinned.weights.assign(skinned.vertexCount() * 4, 0.25f);
    EditMesh sk;
    check(!fromOcMesh(skinned, sk, &why) && why.find("skinned") != std::string::npos, "a skinned mesh is refused with a message");
}

static void testPrimitives() {
    AVER_INFO("=== primitives ===");
    const double pi = 3.14159265358979323846;
    PrimitiveOptions base; base.pivotAtBase = true;
    EditMesh b = makeBox({20, 30, 40}, base);
    check(closedSolid(b) && near(volume(b), 24000, 1e-6), "box 20 x 30 x 40");
    check(near(bounds(b).lo.z, 0, 1e-9) && near(bounds(b).hi.z, 40, 1e-9) && near(bounds(b).lo.x, -10, 1e-9), "pivot at base: min Z = 0, centred in XY");
    PrimitiveOptions ctr;
    check(near(bounds(makeBox({20, 30, 40}, ctr)).lo.z, -20, 1e-9), "pivot at centre otherwise");
    {   // UV: tile 100 -> a 20 x 30 side spans 0.2 x 0.3 of a tile, and no face is mirrored
        double uvArea = 0;
        for (const Face& f : b.faces) { double a = 0; for (size_t i = 0; i < 4; ++i) { const V2& p = f.uv[i]; const V2& q = f.uv[(i + 1) % 4]; a += p.u * q.v - q.u * p.v; } uvArea += a / 2; }
        check(uvArea > 0 && near(uvArea, surfaceArea(b) / 1e4, 1e-9), "box UVs are world-scaled and keep the winding (positive area everywhere)");
    }
    EditMesh sph = makeSphere(50, 32, 16, {});
    const double sv = 4.0 / 3.0 * pi * 125000;
    check(closedSolid(sph) && near(volume(sph), sv, sv * 0.03), "sphere closed, volume within 3 percent");
    check(near(bounds(sph).hi.z, 50, 1e-6), "sphere poles on +-Z");
    check(near(bounds(makeSphere(50, 32, 16, base)).lo.z, 0, 1e-6), "sphere pivot at base");
    EditMesh cyl = makeCylinder(20, 100, 48, base);
    check(closedSolid(cyl) && near(volume(cyl), pi * 400 * 100, pi * 400 * 100 * 0.01) && near(bounds(cyl).hi.z, 100, 1e-6), "cylinder along Z");
    EditMesh cone = makeCone(20, 90, 48, base);
    check(closedSolid(cone) && near(volume(cone), pi * 400 * 90 / 3, pi * 400 * 30 * 0.01) && near(bounds(cone).hi.z, 90, 1e-6), "cone along Z");
    EditMesh tor = makeTorus(10, 30, 16, 32, {});
    const double tv = 2 * pi * pi * 20 * 100;
    check(closedSolid(tor) && near(volume(tor), tv, tv * 0.04) && eulerChar(tor) == 0, "torus: closed, 2 pi^2 R r^2, Euler 0");
    EditMesh pl = makePlane(100, 60, 4, 3, {});
    check(pl.faces.size() == 12 && near(surfaceArea(pl), 6000, 1e-6) && validate(pl).openEdges == 14 && near(faceNormal(pl, 0).z, 1.0, 1e-9), "plane 4 x 3 faces up, 14 boundary edges");
    EditMesh st = makeStairs(100, 200, 90, 5, base);
    check(closedSolid(st) && near(volume(st), 100 * 200 * 90 * 6.0 / 10.0, 1e-6) && near(bounds(st).lo.z, 0, 1e-9), "stairs: W * D * H * (n+1) / 2n");
    EditMesh arch = makeArch(200, 150, 30, 40, 32, base);
    const double R = 100, Ri = 70, legs = 2 * 30 * 150, ring = 0.5 * pi * (R * R - Ri * Ri);
    check(closedSolid(arch) && near(volume(arch), (legs + ring) * 40, (legs + ring) * 40 * 0.01), "arch: two legs plus the half ring, times depth");
    check(near(bounds(arch).lo.z, 0, 1e-9) && near(bounds(arch).hi.z, 250, 1.5), "arch height = leg height + outer radius");
    check(arch.faces.size() == 2 + arch.pos.size() / 2, "arch is two n-gon caps and quad sides");
    PrimitiveOptions mat; mat.material = 3;
    check(makeBox({1, 1, 1}, mat).faces[0].material == 3 && makeSphere(1, 8, 4, mat).faces[0].material == 3, "the material option reaches every face");
}

static void testOps() {
    AVER_INFO("=== poly edit operations ===");
    // move / rotate / scale
    {
        EditMesh m = cube();
        std::vector<aver::u32> all;
        for (aver::u32 i = 0; i < m.pos.size(); ++i) all.push_back(i);
        moveVertices(m, all, {10, 0, 0});
        check(near(bounds(m).lo.x, -40, 1e-9) && near(volume(m), 1e6, 1e-3), "move keeps the volume");
        EditMesh m2 = makeBox({100, 40, 10}, {});
        rotateVertices(m2, all, {0, 0, 0}, {0, 0, 1}, 3.14159265358979323846 / 2);
        check(near(bounds(m2).hi.x, 20, 1e-9) && near(bounds(m2).hi.y, 50, 1e-9) && near(volume(m2), 40000, 1e-6), "rotate 90 degrees about Z swaps the footprint, same volume");
        scaleVertices(m, all, {0, 0, 0}, {2, 1, 1});
        check(near(volume(m), 2e6, 1e-2), "scale X by 2 doubles the volume");
    }
    // extrude
    {
        EditMesh m = cube();
        const aver::u32 top = topFace(m);
        const OpResult r = extrudeFaces(m, {top}, 50);
        check(r.ok && m.faces.size() == 10 && closedSolid(m), "extrude top face: 4 side quads, still closed");
        check(near(volume(m), 100.0 * 100 * 150, 1e-3), "volume = base * (height + 50)");
        check(near(bounds(m).hi.z, 100, 1e-9) && near(faceCentroid(m, top).z, 100, 1e-9), "the cap kept its index and moved to z = 100");
        check(eulerChar(m) == 2 && r.verts.size() == 4, "Euler 2; four new vertices");
        EditMesh p = cube();
        check(extrudeFaces(p, {topFace(p)}, -30).ok && closedSolid(p) && near(volume(p), 1e6 - 100.0 * 100 * 30, 1e-3), "a negative extrude cuts a pocket of area * depth");
        EditMesh q = cube();
        check(!extrudeFaces(q, {9999}, 1).ok && q.faces.size() == 6, "a bad face index is refused and nothing changes");
        // a region of two coplanar faces extrudes together
        EditMesh g = cube();
        subdivideFlat(g, {topFace(g)});
        std::vector<aver::u32> topFaces;
        for (aver::u32 i = 0; i < g.faces.size(); ++i) if (faceNormal(g, i).z > 0.99 && near(faceCentroid(g, i).z, 50, 1e-9)) topFaces.push_back(i);
        const double beforeV = volume(g);
        check(topFaces.size() == 4 && extrudeFaces(g, topFaces, 20).ok && closedSolid(g) && near(volume(g), beforeV + 1e4 * 20, 1e-3), "a region of four faces extrudes as one block");
        EditMesh w = cube();
        w.faces[topFace(w)].material = 2;
        extrudeFaces(w, {topFace(w)}, 10);
        check(w.faces.back().material == 2, "side quads take the material of the face they came from");
    }
    // inset
    {
        EditMesh m = cube();
        const double area0 = surfaceArea(m);
        const aver::u32 top = topFace(m);
        const OpResult r = insetFaces(m, {top}, 10);
        check(r.ok && m.faces.size() == 10 && closedSolid(m), "inset: ring of 4 quads");
        check(near(faceArea(m, top), 80.0 * 80, 1e-6), "the inner face is (100 - 2 * 10)^2");
        check(near(volume(m), 1e6, 1e-3) && near(surfaceArea(m), area0, 1e-3), "volume and total area are unchanged (the ring is coplanar)");
        check(near(m.faces[top].uv[0].u, 0.0, 10) , "inset cap keeps UVs mapped");
        {   // UV continuity: the cap's uv at a corner equals what the old face's mapping gives there
            const EditMesh ref = cube();
            const Face& old = ref.faces[topFace(ref)];
            const V3 p = m.pos[m.faces[top].v[0]];
            const V3 p0 = ref.pos[old.v[0]], p1 = ref.pos[old.v[1]], p3 = ref.pos[old.v[3]];
            const double su = (old.uv[1].u - old.uv[0].u) / length(p1 - p0), sv = (old.uv[3].v - old.uv[0].v) / length(p3 - p0);
            const V3 d = p - p0;
            const double eu = old.uv[0].u + dot(d, normalized(p1 - p0)) * su, ev = old.uv[0].v + dot(d, normalized(p3 - p0)) * sv;
            check(near(m.faces[top].uv[0].u, eu, 1e-9) && near(m.faces[top].uv[0].v, ev, 1e-9), "inset UVs continue the face's own mapping");
        }
        EditMesh big = cube();
        const OpResult bigR = insetFaces(big, {topFace(big)}, 60);
        check(!bigR.ok && big.faces.size() == 6 && near(volume(big), 1e6, 1e-3), "[" + bigR.message + "] an inset past the middle is refused and changes nothing");
        EditMesh frame = cube();
        const aver::u32 t2 = topFace(frame);
        insetFaces(frame, {t2}, 20);
        extrudeFaces(frame, {t2}, -40);
        check(closedSolid(frame) && near(volume(frame), 1e6 - 60.0 * 60 * 40, 1e-3), "inset then push in: a 60 x 60 x 40 pocket");
    }
    // extrude edges
    {
        EditMesh pl = makePlane(100, 100, 2, 2, {});
        std::vector<Edge> boundary;
        const Validation v0 = validate(pl);
        (void)v0;
        const aver::u32 a = findVertex(pl, {-50, -50, 0}), b = findVertex(pl, {0, -50, 0});
        const double area0 = surfaceArea(pl);
        const OpResult r = extrudeEdges(pl, {makeEdge(a, b)}, {0, 0, 10});
        check(r.ok && pl.faces.size() == 5 && near(surfaceArea(pl), area0 + 50.0 * 10, 1e-9), "edge extrude adds a quad of length * distance");
        check(validate(pl).manifold(), "still manifold");
        check(near(faceNormal(pl, r.faces[0]).y, 1.0, 1e-9), "the new quad folds up from the sheet with consistent winding (normal +Y over the -Y edge)");
        const aver::u32 c = findVertex(pl, {0, 0, 0}), d = findVertex(pl, {50, 0, 0});
        EditMesh pl2 = makePlane(100, 100, 2, 2, {});
        const OpResult bad = extrudeEdges(pl2, {makeEdge(findVertex(pl2, {0, 0, 0}), findVertex(pl2, {50, 0, 0}))}, {0, 0, 10});
        (void)c; (void)d;
        check(!bad.ok && bad.message.find("single face") != std::string::npos && pl2.faces.size() == 4, "an interior edge is refused with a message");
    }
    // bevel
    {
        EditMesh m = cube();
        const aver::u32 a = findVertex(m, {50, 50, -50}), b = findVertex(m, {50, 50, 50});
        const OpResult r = bevelEdges(m, {makeEdge(a, b)}, 10, 1);
        std::string msg;
        check(r.ok && closedSolid(m, &msg) && eulerChar(m) == 2, "chamfer one edge: closed, Euler 2 " + msg + r.message);
        check(near(volume(m), 1e6 - 50.0 * 100, 1e-3), "volume = cube - (w^2 / 2) * length");
        check(m.faces.size() == 7, "one strip face");
        EditMesh m4 = cube();
        const OpResult r4 = bevelEdges(m4, {makeEdge(a, b)}, 10, 4);
        check(r4.ok && closedSolid(m4) && eulerChar(m4) == 2 && m4.faces.size() == 10, "four segments: a four-quad strip");
        check(volume(m4) > volume(m) && volume(m4) < 1e6, "a rounded bevel removes less than a chamfer, but something");
        // all edges
        EditMesh all = cube();
        std::vector<Edge> es = edgesOfFaces(all, [&] { std::vector<aver::u32> f; for (aver::u32 i = 0; i < all.faces.size(); ++i) f.push_back(i); return f; }());
        const OpResult ra = bevelEdges(all, es, 10, 1);
        check(es.size() == 12 && ra.ok && closedSolid(all, &msg) && eulerChar(all) == 2, "all 12 edges: closed, Euler 2 " + msg + ra.message);
        const double expect = 80.0 * 80 * 80 + 6 * 80.0 * 80 * 10 + 12 * 80.0 * 50 + 8 * 1000.0 / 6.0;
        check(near(volume(all), expect, 1e-3), "all edges chamfered: core + slabs + edge prisms + corner tetra caps");
        check(all.faces.size() == 6 + 12 + 8, "6 faces, 12 strips, 8 corner triangles");
        EditMesh all3 = cube();
        const OpResult r3 = bevelEdges(all3, es, 10, 3);
        check(r3.ok && closedSolid(all3) && eulerChar(all3) == 2 && volume(all3) > volume(all) && volume(all3) < 1e6, "all edges, three segments: closed, rounder than the chamfer");
        // refusals leave the mesh alone
        EditMesh two = cube();
        const aver::u32 c = findVertex(two, {50, -50, 50});
        const OpResult bad = bevelEdges(two, {makeEdge(a, b), makeEdge(b, c)}, 10, 1);
        check(!bad.ok && bad.message.find("one edge or all") != std::string::npos && two.faces.size() == 6, "two of three edges at a corner is refused");
        EditMesh wide = cube();
        check(!bevelEdges(wide, {makeEdge(a, b)}, 200, 1).ok && wide.faces.size() == 6, "a bevel wider than the neighbouring edge is refused");
        EditMesh planeM = makePlane(10, 10, 1, 1, {});
        check(!bevelEdges(planeM, {makeEdge(0, 1)}, 1, 1).ok, "an edge of an open sheet is refused");
        // vertex of valence four: a cone's apex edge
        EditMesh cone = makeCone(40, 80, 4, {});
        aver::u32 apex = 0;
        for (aver::u32 i = 0; i < cone.pos.size(); ++i) if (cone.pos[i].z > cone.pos[apex].z) apex = i;
        aver::u32 other = 0xFFFFFFFFu;
        for (const Face& f : cone.faces) if (f.v.size() == 3) for (size_t i = 0; i < 3; ++i) if (f.v[i] == apex) other = f.v[(i + 1) % 3];
        const double v0 = volume(cone);
        const OpResult rc = bevelEdges(cone, {makeEdge(apex, other)}, 3, 2);
        check(rc.ok && closedSolid(cone, &msg) && eulerChar(cone) == 2 && volume(cone) < v0, "bevel from a four-valent apex: closed, Euler 2, volume down " + msg + rc.message);
    }
    // delete
    {
        EditMesh m = cube();
        check(deleteFaces(m, {topFace(m)}).ok && m.faces.size() == 5 && validate(m).openEdges == 4 && near(surfaceArea(m), 5e4, 1e-3), "delete a face: 5 left, 4 open edges");
        EditMesh v = cube();
        const aver::u32 corner = findVertex(v, {50, 50, 50});
        check(deleteVertices(v, {corner}).ok && v.faces.size() == 3 && validate(v).unusedVertices == 1, "delete a vertex removes its three faces");
        compact(v);
        check(v.pos.size() == 7, "compact drops the unused vertex");
        EditMesh e = cube();
        check(deleteEdges(e, {makeEdge(findVertex(e, {50, 50, -50}), findVertex(e, {50, 50, 50}))}).ok && e.faces.size() == 4, "delete an edge removes its two faces");
        EditMesh none = cube();
        check(!deleteFaces(none, {}).ok, "deleting nothing says so");
    }
    // merge / weld
    {
        EditMesh m = cube();
        const aver::u32 a = findVertex(m, {50, 50, 50}), b = findVertex(m, {50, 50, -50});
        check(mergeVertices(m, {a, b}).ok && m.faces.size() == 6 && validate(m).manifold(), "merging two corners of a cube keeps six faces (two become triangles)");
        check(near(m.pos[std::min(a, b)].z, 0, 1e-9) && validate(m).closed(), "at their centre; still closed");
        EditMesh dupe = cube();
        const EditMesh copy = moved(cube(), {0, 0, 0});
        const aver::u32 n0 = static_cast<aver::u32>(dupe.pos.size());
        for (const V3& p : copy.pos) dupe.addVertex(p);
        for (Face f : copy.faces) { for (aver::u32& i : f.v) i += n0; dupe.faces.push_back(f); }
        check(weldVertices(dupe, 1e-6) == 8 && dupe.faces.size() == 12, "welding coincident copies merges 8 vertices");
        EditMesh near2 = cube();
        near2.pos[0] += V3{0.0005, 0, 0};
        const aver::u32 extra = near2.addVertex(near2.pos[0] + V3{0.0002, 0, 0});
        near2.addFace({extra, 1, 2});
        check(weldVertices(near2, 0.001) == 1, "weld within a tolerance");
    }
    // flip
    {
        EditMesh m = cube();
        flipFaces(m, {});
        check(near(volume(m), -1e6, 1e-3) && closedSolid(m), "flipping every face inverts the volume");
        flipFaces(m, {});
        check(near(volume(m), 1e6, 1e-3), "and back");
        EditMesh p = cube();
        flipFaces(p, {0});
        check(!closedSolid(p), "flipping one face breaks the winding");
        EditMesh q = cube();
        q.faces[0].uv = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        flipFaces(q, {0});
        check(near(q.faces[0].uv[0].u, 0, 0) && near(q.faces[0].uv[0].v, 1, 0), "UVs follow the corner order");
    }
    // subdivide
    {
        EditMesh m = cube();
        const OpResult r = subdivideFlat(m, {});
        check(r.ok && m.faces.size() == 24 && closedSolid(m) && eulerChar(m) == 2, "subdivide all: 24 quads, closed");
        check(near(volume(m), 1e6, 1e-3) && near(surfaceArea(m), 6e4, 1e-3), "volume and area are unchanged");
        check(m.pos.size() == 8 + 12 + 6, "8 corners + 12 edge midpoints + 6 face centres");
        EditMesh one = cube();
        const OpResult r1 = subdivideFlat(one, {topFace(one)});
        check(r1.ok && one.faces.size() == 5 + 4 && closedSolid(one) && near(volume(one), 1e6, 1e-3), "one face: neighbours take the midpoints and stay watertight");
        EditMesh tri = makeCone(30, 60, 5, {});
        const size_t nf = tri.faces.size();
        subdivideFlat(tri, {});
        check(closedSolid(tri) && tri.faces.size() > nf, "triangles and a pentagon subdivide too");
    }
    // mirror
    {
        EditMesh half = makeBox({100, 100, 100}, {});
        for (V3& p : half.pos) p.x += 50;            // spans x 0..100
        aver::u32 face0 = 0;
        for (aver::u32 i = 0; i < half.faces.size(); ++i) if (faceNormal(half, i).x < -0.99) face0 = i;
        deleteFaces(half, {face0});
        const OpResult r = mirror(half, 0, 0.0, 1e-6);
        compact(half);
        check(r.ok && closedSolid(half) && near(volume(half), 2e6, 1e-3), "a half box mirrored across its open side is a closed box of twice the volume");
        check(half.pos.size() == 12 && half.faces.size() == 10 && near(bounds(half).lo.x, -100, 1e-9), "[" + std::to_string(half.pos.size()) + "/" + std::to_string(half.faces.size()) + "] welded on the plane: 12 vertices, 10 faces");
        EditMesh solid = cube();
        mirror(solid, 2, 200, 1e-6);
        check(solid.faces.size() == 12 && near(bounds(solid).hi.z, 450, 1e-9) && near(volume(solid), 2e6, 1e-3), "a copy lands on the far side of the plane with its winding flipped");
    }
}

static void testBooleans() {
    AVER_INFO("=== booleans ===");
    EditMesh a = moved(makeBox({2, 2, 2}, {}), {1, 1, 1});          // 0..2
    EditMesh b = moved(makeBox({2, 2, 2}, {}), {2, 2, 2});          // 1..3
    EditMesh out;
    std::string why;
    check(boolean(a, b, aver::fmt::csg::Op::Union, out, &why) && near(volume(out), 15.0, 1e-6), "union of boxes overlapping by 1: 8 + 8 - 1");
    check(closedSolid(out, &why), "the union is watertight " + why);
    check(boolean(a, b, aver::fmt::csg::Op::Intersection, out, &why) && near(volume(out), 1.0, 1e-6) && closedSolid(out), "intersection 1");
    check(boolean(a, b, aver::fmt::csg::Op::Subtraction, out, &why) && near(volume(out), 7.0, 1e-6) && closedSolid(out), "difference 7");
    EditMesh far = moved(makeBox({2, 2, 2}, {}), {10, 0, 0});
    check(boolean(a, far, aver::fmt::csg::Op::Union, out, &why) && near(volume(out), 16.0, 1e-6), "disjoint union is the sum");
    check(boolean(a, far, aver::fmt::csg::Op::Intersection, out, &why) && out.faces.empty() && why.find("empty") != std::string::npos, "disjoint intersection is empty, and says so");
    // a hole through the middle
    EditMesh cyl = moved(makeCylinder(0.5, 6, 32, {}), {1, 1, 1});
    check(boolean(a, cyl, aver::fmt::csg::Op::Subtraction, out, &why), "drill a cylinder through a box");
    const double hole = 3.14159265358979323846 * 0.25 * 2 * (1 - 1e-2);
    check(near(volume(out), 8 - hole, 0.03), "[" + std::to_string(volume(out)) + "] box minus the cylinder's 2 long section");
    // materials
    EditMesh ma = a, mb = b;
    ma.materials = {"stone"};
    mb.materials = {"metal"};
    check(boolean(ma, mb, aver::fmt::csg::Op::Subtraction, out, &why) && out.materials.size() == 2 && out.materials[1] == "metal", "materials of both sources are kept by name");
    bool anyMetal = false, anyStone = false;
    for (const Face& f : out.faces) { anyMetal |= f.material == 1; anyStone |= f.material == 0; }
    check(anyMetal && anyStone, "cut faces carry the cutter's material, the rest the original's");
    // refusals
    EditMesh openMesh = cube();
    openMesh.faces.pop_back();
    EditMesh keep = out;
    check(!boolean(openMesh, a, aver::fmt::csg::Op::Union, out, &why) && why.find("not a closed solid") != std::string::npos && why.find("open edges") != std::string::npos, "an open first mesh is refused with a message");
    check(!boolean(a, openMesh, aver::fmt::csg::Op::Union, out, &why) && why.find("second") != std::string::npos, "an open second mesh is named as such");
    check(out.faces.size() == keep.faces.size(), "the output is untouched on refusal");
    EditMesh inside = a;
    flipFaces(inside, {});
    check(!boolean(inside, b, aver::fmt::csg::Op::Union, out, &why) && why.find("inside out") != std::string::npos, "an inside-out mesh is refused with a message");
    check(!boolean(EditMesh{}, b, aver::fmt::csg::Op::Union, out, &why), "an empty mesh is refused");
    // a boolean result is editable again
    boolean(a, b, aver::fmt::csg::Op::Union, out, &why);
    check(extrudeFaces(out, {topFace(out)}, 1).ok, "and can be edited further");
}

static void testUv() {
    AVER_INFO("=== UV tools ===");
    EditMesh m = cube();
    UvProjectionParams p;
    p.tile = 100;
    projectUv(m, {}, UvProjection::Box, p);
    const aver::u32 top = topFace(m);
    const Face& tf = m.faces[top];
    double umin = 1e9, umax = -1e9;
    for (const V2& t : tf.uv) { umin = std::min(umin, t.u); umax = std::max(umax, t.u); }
    check(near(umax - umin, 1.0, 1e-9), "box projection: a 100 cm face spans one tile");
    check(near(texelDensity(m, {}, 512), 5.12, 1e-9), "texel density: 512 texels over 100 cm = 5.12 per cm");
    UvProjectionParams q;
    q.tile = 50;
    q.offset = {0.25, 0};
    projectUv(m, {top}, UvProjection::Planar, q);
    umin = 1e9; umax = -1e9;
    for (const V2& t : m.faces[top].uv) { umin = std::min(umin, t.u); umax = std::max(umax, t.u); }
    check(near(umax - umin, 2.0, 1e-9) && near(texelDensity(m, {top}, 512), 10.24, 1e-9), "planar at tile 50 doubles the density; the offset applies");
    UvProjectionParams sc; sc.tile = 100; sc.scale = {2, 2};
    projectUv(m, {top}, UvProjection::Planar, sc);
    check(near(texelDensity(m, {top}, 512), 10.24, 1e-9), "scale 2 doubles the density");
    transformUv(m, {top}, {0, 0}, 0, {0.5, 0.5});
    check(near(texelDensity(m, {top}, 512), 5.12, 1e-9), "per-face transform: scale 0.5 halves it");
    const double u0 = m.faces[top].uv[0].u;
    transformUv(m, {top}, {0.5, 0}, 0, {1, 1});
    check(near(m.faces[top].uv[0].u, u0 + 0.5, 1e-12), "per-face offset");
    transformUv(m, {top}, {0, 0}, 90, {1, 1});
    double area = 0;
    for (size_t i = 0; i < 4; ++i) { const V2& a = m.faces[top].uv[i]; const V2& b = m.faces[top].uv[(i + 1) % 4]; area += a.u * b.v - b.u * a.v; }
    check(near(texelDensity(m, {top}, 512), 10.24 / 2, 1e-9) && near(std::fabs(area) / 2, 1.0, 1e-9), "rotation keeps the area");

    EditMesh cyl = makeCylinder(30, 100, 24, {});
    UvProjectionParams cp;
    cp.tile = 100;
    projectUv(cyl, {}, UvProjection::Cylindrical, cp);
    double lo = 1e9, hi = -1e9;
    bool seamOk = true;
    for (const Face& f : cyl.faces) {
        if (std::fabs(faceNormal(cyl, aver::u32(&f - cyl.faces.data())).z) > 0.9) continue;   // caps wind once round the axis by nature
        double fl = 1e9, fh = -1e9;
        for (const V2& t : f.uv) { fl = std::min(fl, t.u); fh = std::max(fh, t.u); lo = std::min(lo, t.u); hi = std::max(hi, t.u); }
        seamOk &= (fh - fl) < 0.5;
    }
    check(seamOk && hi - lo > 0.9 && hi - lo < 1.6, "cylindrical: u wraps once around, no face straddles the seam");
    EditMesh sph = makeSphere(40, 16, 8, {});
    projectUv(sph, {}, UvProjection::Spherical, cp);
    double vlo = 1e9, vhi = -1e9;
    for (const Face& f : sph.faces) for (const V2& t : f.uv) { vlo = std::min(vlo, t.v); vhi = std::max(vhi, t.v); }
    check(near(vlo, 0, 1e-6) && near(vhi, 1, 1e-6), "spherical: v runs pole to pole");

    // lightmap UV1
    OcMeshData d = toOcMesh(cube());
    const size_t idx0 = d.indices.size();
    std::string err;
    check(addLightmapUv1(d, 256, &err) && d.hasUv1() && d.valid() && d.indices.size() == idx0 && aver::fmt::submeshesPartitionIndices(d), "UV1 from LightmapUnwrap: valid, same triangles");
    bool in01 = true;
    for (float t : d.uvs1) in01 &= t >= 0.0f && t <= 1.0f;
    check(in01 && d.vertexCount() >= 24, "UV1 inside the atlas");
    std::vector<aver::u8> bytes;
    OcMeshData back;
    check(aver::fmt::writeOcMesh(d, bytes, &err) && aver::fmt::parseOcMesh(bytes.data(), bytes.size(), back, &err) && back.hasUv1(), "and survives the file");
}

static void testSave() {
    AVER_INFO("=== save / reopen ===");
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "aver_editmesh_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const std::string path = (dir / "crate.ocmesh").string();
    EditMesh m = cube();
    m.materials = {"wood"};
    extrudeFaces(m, {topFace(m)}, 20);
    m.faces[0].smoothing = 2;
    std::string why;
    check(saveAuthoredMesh(path, m, {}, &why), "save " + why);
    check(fs::exists(path) && fs::exists(path + ".edit"), "the mesh and its edit sidecar are written");
    OcMeshData file;
    check(aver::fmt::loadOcMesh(path, file, &why) && file.valid(), "the .ocmesh is a normal mesh");
    EditMesh re;
    bool side = false;
    check(loadAuthoredMesh(path, re, &side, &why) && side, "reopen uses the sidecar");
    check(re.faces.size() == m.faces.size() && re.pos.size() == m.pos.size() && re.faces[0].smoothing == 2 && re.materials == m.materials, "quads, smoothing and materials come back");
    bool same = true;
    for (size_t i = 0; i < m.faces.size(); ++i) {
        same &= re.faces[i].v == m.faces[i].v;
        for (size_t c = 0; c < m.faces[i].uv.size(); ++c) same &= near(re.faces[i].uv[c].u, m.faces[i].uv[c].u, 1e-6) && near(re.faces[i].uv[c].v, m.faces[i].uv[c].v, 1e-6);
    }
    for (size_t i = 0; i < m.pos.size(); ++i) same &= length(re.pos[i] - m.pos[i]) < 1e-12;
    check(same && near(volume(re), volume(m), 1e-6), "identical topology, UVs and exact positions");

    // a mesh file changed by anything else opens as triangles
    EditMesh other = makeSphere(20, 8, 4, {});
    check(aver::fmt::saveOcMesh(path, toOcMesh(other), &why), "overwrite the .ocmesh behind the sidecar's back");
    EditMesh tri;
    side = true;
    check(loadAuthoredMesh(path, tri, &side, &why) && !side && tri.faces.size() > 0, "a stale sidecar is ignored: opens as triangles");
    bool allTri = true;
    for (const Face& f : tri.faces) allTri &= f.v.size() == 3;
    check(allTri, "all triangles");
    // no sidecar
    check(saveAuthoredMesh(path, m, {}, &why), "save again");
    fs::remove(path + ".edit", ec);
    EditMesh noSide;
    check(loadAuthoredMesh(path, noSide, &side, &why) && !side && noSide.faces.size() == 10 * 2 && closedSolid(noSide), "without a sidecar an authored mesh is imported as triangles (welded, closed)");
    // options: UV1 and the cook hook
    int cooked = 0;
    SaveOptions o;
    o.lightmapAtlasEdge = 128;
    o.cook = [&](OcMeshData& d, std::string*) { ++cooked; return d.hasUv1(); };
    check(saveAuthoredMesh(path, m, o, &why) && cooked == 1, "the cook hook runs after the UV1 step");
    OcMeshData withUv1;
    check(aver::fmt::loadOcMesh(path, withUv1, &why) && withUv1.hasUv1(), "the saved mesh has UV1");
    EditMesh re2;
    check(loadAuthoredMesh(path, re2, &side, &why) && side && re2.faces.size() == m.faces.size(), "and still reopens as quads");
    o.cook = [](OcMeshData&, std::string* w) { if (w) *w = "cook failed"; return false; };
    check(!saveAuthoredMesh(path, m, o, &why) && why == "cook failed", "a failing cook aborts the save with its message");
    EditMesh empty;
    check(!saveAuthoredMesh(path, empty, {}, &why) && !why.empty(), "an empty mesh is not saved");
    // sidecar text
    EditMesh parsed;
    std::string text = serializeEditMesh(m, 0x1234);
    aver::u64 h = 0;
    check(parseEditMesh(text, parsed, &h) && h == 0x1234 && parsed.faces.size() == m.faces.size(), "the sidecar text parses");
    check(!parseEditMesh("garbage", parsed), "garbage is rejected");
    check(!parseEditMesh("EDITMESH 1\nf 0 0 3 0 0 0 1 0 0 2 0 0\n", parsed), "a face past the vertices is rejected");
    fs::remove_all(dir, ec);
}

int main() {
    testCore();
    testPrimitives();
    testOps();
    testBooleans();
    testUv();
    testSave();
    AVER_INFO("{} checks, {} failures", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
