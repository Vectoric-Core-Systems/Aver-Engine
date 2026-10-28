#pragma once
// Constructive solid geometry on convex polygons (G4 R7): union, intersection and subtraction by BSP trees, the way a converted Godot level's CSG tree is baked
// into a static mesh at convert time. Plain doubles, no engine type, no axis: the caller picks the space (the converter works in Godot metres, +Y up) and
// maps the result. A solid is a closed set of polygons wound counter-clockwise seen from outside; an open surface is carried but gives no meaningful boolean.
// Contract: docs/plans/fabric/G4_RENDER.md R7.
#include <cstdint>
#include <vector>

namespace aver::fmt::csg {

struct Vertex {
    double p[3] = {0, 0, 0};
    double n[3] = {0, 0, 0};   // the vertex normal as supplied; zero = use the polygon's plane normal
    double uv[2] = {0, 0};
};

struct Polygon {
    std::vector<Vertex> v;     // convex, planar, at least 3
    int material = 0;          // a caller's surface index, kept through every split
};

struct Mesh {
    std::vector<Polygon> polys;
    bool empty() const { return polys.empty(); }
};

enum class Op { Union = 0, Intersection = 1, Subtraction = 2 };   // Godot's CSGShape3D.Operation numbers

// a OP b. Both are copied; neither is modified.
Mesh combine(const Mesh& a, const Mesh& b, Op op);

// Row-vector convention is NOT used here: m is a 3x4 column-vector matrix, rows {r0, r1, r2} each (x, y, z, translation): p' = M * (p, 1). Normals use the
// inverse transpose of the 3x3 part; a mirroring matrix reverses the winding.
void transform(Mesh& m, const double m34[12]);
// Reverses every polygon (inside out) and negates its normals.
void invertFaces(Mesh& m);

struct Bounds { double lo[3], hi[3]; bool valid = false; };
Bounds bounds(const Mesh& m);
// The enclosed volume of a closed outward-wound mesh: the sum of signed tetrahedra against the origin.
double volume(const Mesh& m);
double surfaceArea(const Mesh& m);

// ---- solids (Godot space, centred as Godot's CSG nodes are) ----
Mesh makeBox(const double size[3], int material = 0);
// radialSegments around Y, rings + 1 latitude bands (CSGSphere3D's radial_segments and rings); smooth = vertex normals follow the sphere.
Mesh makeSphere(double radius, int radialSegments, int rings, bool smooth, int material = 0);
// Along Y, centred; topRadius 0 is a cone. `sides` around.
Mesh makeCylinder(double radius, double height, int sides, bool cone, bool smooth, int material = 0);
// A torus about Y: `sides` edges per cross-section ring, `ringSides` slices around the axis (CSGTorus3D's sides and ring_sides).
Mesh makeTorus(double innerRadius, double outerRadius, int sides, int ringSides, bool smooth, int material = 0);
// A polygon in the XY plane (any winding, simple) extruded from z = 0 to z = -depth (CSGPolygon3D depth mode).
Mesh extrudePolygon(const std::vector<double>& xy, double depth, int material = 0);
// The same polygon spun about Y by `degrees` in `steps` steps (CSGPolygon3D spin mode); 360 closes the ring.
Mesh spinPolygon(const std::vector<double>& xy, double degrees, int steps, int material = 0);
// A triangle list (positions 3 per vertex, optional normals 3 and uvs 2 per vertex) as polygons; groups[k] gives triangle k's material (empty = all 0).
Mesh fromTriangles(const std::vector<float>& pos, const std::vector<float>& nrm, const std::vector<float>& uv, const std::vector<uint32_t>& idx,
                   const std::vector<int>& groups = {});

// ---- out: a welded triangle soup per material ----
struct Soup {
    std::vector<float> pos, nrm, uv;     // 3, 3 and 2 per vertex
    std::vector<uint32_t> idx;           // one range per material, in material order
    std::vector<int> materials;          // the material of each range
    std::vector<uint32_t> rangeStart, rangeCount;
};
// Triangulates (fans) every polygon, drops slivers, snaps positions to `snap` (0 = none; Godot's default is 0.001) and welds equal vertices. A vertex with
// a zero normal gets its polygon's plane normal.
Soup toSoup(const Mesh& m, double snap);

}  // namespace aver::fmt::csg
