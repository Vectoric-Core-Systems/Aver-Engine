#pragma once
// EditMesh -- an editable polygon mesh for the in-engine Modeling mode (docs/plans/WAVE2_UE.md W5), beside fmt::csg. Indexed vertices, n-gon faces wound
// counter-clockwise seen from outside, a UV per face corner, a material and a smoothing group per face. Doubles, engine space (centimetres, +Z up).
// Headless: validation, triangulation, conversions to .ocmesh data and to csg, primitives, poly edit operations, booleans (csg::combine), UV tools and the
// save/reopen pair (.ocmesh plus an edit-metadata sidecar so an authored mesh reopens as its quads). No device, no UI.
// Every operation that can refuse works on a copy and leaves the mesh untouched when it returns ok = false (message says why).
#include "aver/core/Types.hpp"
#include "aver/formats/Csg.hpp"
#include "aver/formats/OcMesh.hpp"

#include <cmath>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace aver::fmt::em {

struct V3 {
    double x = 0, y = 0, z = 0;
    V3() = default;
    V3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}
    V3 operator+(const V3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    V3 operator-(const V3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    V3 operator*(double s) const { return {x * s, y * s, z * s}; }
    V3& operator+=(const V3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    double& operator[](int i) { return i == 0 ? x : (i == 1 ? y : z); }
    double operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
};
struct V2 { double u = 0, v = 0; };

inline double dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(const V3& a, const V3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline double length(const V3& a) { return std::sqrt(dot(a, a)); }
inline V3 normalized(const V3& a) { const double l = length(a); return l > 1e-300 ? a * (1.0 / l) : V3{}; }

struct Face {
    std::vector<u32> v;      // vertex indices, >= 3, counter-clockwise from outside
    std::vector<V2> uv;      // one per corner (same size as v)
    int material = 0;        // index into EditMesh::materials
    u32 smoothing = 0;       // 0 = flat; faces sharing a vertex and a non-zero group share one averaged normal there
};

struct EditMesh {
    std::vector<V3> pos;
    std::vector<Face> faces;
    std::vector<std::string> materials;   // slot names; a face's material indexes this (missing names are made up on export)

    bool empty() const { return faces.empty(); }
    u32 addVertex(const V3& p) { pos.push_back(p); return static_cast<u32>(pos.size() - 1); }
    // Appends a face (UVs zero). Returns its index.
    u32 addFace(std::vector<u32> verts, int material = 0, u32 smoothing = 0);
};

using Edge = std::pair<u32, u32>;                  // normalised: first < second
inline Edge makeEdge(u32 a, u32 b) { return a < b ? Edge{a, b} : Edge{b, a}; }

// What an operation did. faces / verts name the new or moved elements in the mesh after the op.
struct OpResult {
    bool ok = false;
    std::string message;
    std::vector<u32> faces;
    std::vector<u32> verts;
};

// ---- core (step 1) ----
V3 faceNormal(const EditMesh& m, u32 face);        // unit; zero for a degenerate face
double faceArea(const EditMesh& m, u32 face);
V3 faceCentroid(const EditMesh& m, u32 face);      // mean of the corners

struct Validation {
    u32 badIndex = 0;             // faces with a corner past the vertices or fewer than 3 corners
    u32 degenerateFaces = 0;      // repeated vertex in a face, or zero area
    u32 nonPlanarFaces = 0;       // n-gons whose corners leave their plane by more than the tolerance (informational: triangulation handles them)
    u32 openEdges = 0;            // edges used by one face
    u32 nonManifoldEdges = 0;     // edges used by more than two faces
    u32 inconsistentEdges = 0;    // edges two faces traverse the same way (mixed winding)
    u32 unusedVertices = 0;       // informational
    std::string message;          // empty when closed
    bool manifold() const { return badIndex == 0 && degenerateFaces == 0 && nonManifoldEdges == 0 && inconsistentEdges == 0; }
    bool closed() const { return manifold() && openEdges == 0; }
};
Validation validate(const EditMesh& m);

struct Tri { u32 face; u32 c[3]; };                // corner indices inside the face
// Convex faces fan, others are ear-clipped in the face's plane; a face that cannot be clipped falls back to a fan.
void triangulateFace(const EditMesh& m, u32 face, std::vector<Tri>& out);
std::vector<Tri> triangulate(const EditMesh& m);

struct Bounds3 { V3 lo, hi; bool valid = false; };
Bounds3 bounds(const EditMesh& m);                 // of the vertices faces use
double volume(const EditMesh& m);                  // signed, outward-wound closed mesh = positive
double surfaceArea(const EditMesh& m);
// Drops vertices no face uses (remap, if given, maps old index -> new or ~0u). Face order is kept.
void compact(EditMesh& m, std::vector<u32>* remap = nullptr);

// ---- conversions ----
// csg works on convex planar polygons: planar convex faces pass through, others are triangulated. Smoothing groups are not carried.
csg::Mesh toCsg(const EditMesh& m);
// Welds corners closer than `weldTol` (0 = 1e-6 of the bounds diagonal), drops degenerate polygons, repairs T-junctions. Materials: indices are kept as given.
EditMesh fromCsg(const csg::Mesh& c, double weldTol = 0.0);
// Triangulates (see above), one vertex per distinct (position, normal, UV, material); submeshes per used material slot in slot order; tangents generated.
// Normals follow smoothing groups. Float precision, so cm positions are exact to ~1e-5 at 100 m.
OcMeshData toOcMesh(const EditMesh& m);
// Welds equal positions, one triangle per face, UV0 per corner, a smoothing group 1 where the stored normal differs from the face normal. Refuses skinned and
// morphed meshes (message in *why).
bool fromOcMesh(const OcMeshData& d, EditMesh& out, std::string* why = nullptr);

// ---- topology cleanup (step 3 helpers) ----
// Merges vertices closer than tol; faces that collapse are removed. Returns the number of vertices merged away.
u32 weldVertices(EditMesh& m, double tol);
// Removes repeated consecutive corners and faces with fewer than 3 corners or no area. Returns faces removed.
u32 removeDegenerateFaces(EditMesh& m);
// Inserts a vertex that lies on another face's edge into that face (what BSP results need to become watertight). Returns insertions.
u32 repairTJunctions(EditMesh& m, double tol);

// ---- primitives (step 2) ----
struct PrimitiveOptions {
    bool pivotAtBase = false;    // origin at the centre of the bottom face (min Z = 0); otherwise the centre of the bounds
    double uvTile = 100.0;       // world units per 1.0 of UV on flat-mapped primitives
    int material = 0;
};
EditMesh makeBox(const V3& size, const PrimitiveOptions& o = {});
EditMesh makeSphere(double radius, int segments, int rings, const PrimitiveOptions& o = {});
EditMesh makeCylinder(double radius, double height, int sides, const PrimitiveOptions& o = {});
EditMesh makeCone(double radius, double height, int sides, const PrimitiveOptions& o = {});
EditMesh makeTorus(double innerRadius, double outerRadius, int sides, int ringSides, const PrimitiveOptions& o = {});
EditMesh makePlane(double sizeX, double sizeY, int segX, int segY, const PrimitiveOptions& o = {});   // faces +Z
// Steps climb along +X over `depth`, width along Y, total `height` Z.
EditMesh makeStairs(double width, double depth, double height, int steps, const PrimitiveOptions& o = {});
// An arch standing in the XZ plane: `width` outside, straight legs of `legHeight` under a half-ring of `wall` thickness, `depth` along Y.
EditMesh makeArch(double width, double legHeight, double wall, double depth, int arcSegments, const PrimitiveOptions& o = {});

// ---- poly edit operations (step 3). Index lists are faces / vertices / edges of the mesh passed in. ----
std::vector<u32> verticesOfFaces(const EditMesh& m, const std::vector<u32>& faces);
std::vector<u32> verticesOfEdges(const std::vector<Edge>& edges);
std::vector<u32> facesOfVertices(const EditMesh& m, const std::vector<u32>& verts);   // faces using any of them
std::vector<Edge> edgesOfFaces(const EditMesh& m, const std::vector<u32>& faces);

void moveVertices(EditMesh& m, const std::vector<u32>& verts, const V3& delta);
void rotateVertices(EditMesh& m, const std::vector<u32>& verts, const V3& pivot, const V3& axis, double radians);
void scaleVertices(EditMesh& m, const std::vector<u32>& verts, const V3& pivot, const V3& scale);

// Extrudes the region of selected faces along its area-weighted average normal. The selected faces keep their indices (they become the cap); side faces are
// appended. result.verts = the moved boundary copies. Region interior vertices move in place.
OpResult extrudeFaces(EditMesh& m, const std::vector<u32>& faces, double distance, double uvTile = 100.0);
// Pulls the region's boundary toward the inside by `amount` in the face plane (a ring of quads around the faces, which keep their indices).
OpResult insetFaces(EditMesh& m, const std::vector<u32>& faces, double amount);
// Extrudes open-boundary edges by `delta` (each gets a quad). Refuses an edge shared by two faces.
OpResult extrudeEdges(EditMesh& m, const std::vector<Edge>& edges, const V3& delta, double uvTile = 100.0);
// Bevels edges shared by two faces: a strip `segments` wide (1 = chamfer, more = rounded) at `width` from the edge in each face. At a vertex either one or
// all of its edges must be selected; other junctions are refused. Needs a closed manifold neighbourhood.
OpResult bevelEdges(EditMesh& m, const std::vector<Edge>& edges, double width, int segments);
// Removes faces (and, for vertices / edges, every face using them). Vertices stay until compact().
OpResult deleteFaces(EditMesh& m, const std::vector<u32>& faces);
OpResult deleteVertices(EditMesh& m, const std::vector<u32>& verts);
OpResult deleteEdges(EditMesh& m, const std::vector<Edge>& edges);
// Merges the listed vertices into one at their centre. Faces that collapse are removed.
OpResult mergeVertices(EditMesh& m, const std::vector<u32>& verts);
// Reverses winding (and corner order of UVs) of the listed faces; empty = all.
void flipFaces(EditMesh& m, const std::vector<u32>& faces);
// Splits each face into one quad per corner (edge midpoints + centre); neighbours get the midpoints so the mesh stays watertight. Empty = all faces.
OpResult subdivideFlat(EditMesh& m, const std::vector<u32>& faces);
// Reflects across the plane axis = at (0 x, 1 y, 2 z) and appends the reflected copy, welding what lies on the plane.
OpResult mirror(EditMesh& m, int axis, double at, double weldTol);

// ---- booleans (step 4) ----
// a OP b through csg::combine. Both must be closed, outward-wound solids (a clear message otherwise). Materials of b are matched to a's by name. The result is
// welded, degenerate polygons dropped and T-junctions repaired; smoothing groups are not kept. out is left untouched on refusal.
bool boolean(const EditMesh& a, const EditMesh& b, csg::Op op, EditMesh& out, std::string* why = nullptr);

// ---- UV tools (step 5) ----
enum class UvProjection { Box, Planar, Cylindrical, Spherical };
struct UvProjectionParams {
    V3 origin;                   // cylinder axis point / sphere centre / planar origin
    V3 axis{0, 0, 1};            // cylinder axis, planar normal (ignored by Box; Spherical poles)
    double tile = 100.0;         // world units per 1.0 of UV (Box, Planar, and the axial direction of Cylindrical)
    V2 offset;
    double rotationDegrees = 0;
    V2 scale{1, 1};
};
// Projects UV0 for the listed faces (empty = all).
void projectUv(EditMesh& m, const std::vector<u32>& faces, UvProjection kind, const UvProjectionParams& p);
// Per-face transform of the existing UVs about each face's UV centre.
void transformUv(EditMesh& m, const std::vector<u32>& faces, const V2& offset, double rotationDegrees, const V2& scale);
// Texels per world unit if UV0 is mapped to a square texture of `textureSize` texels: sqrt(uvArea * size^2 / worldArea) over the listed faces (empty = all).
double texelDensity(const EditMesh& m, const std::vector<u32>& faces, double textureSize);
// Gives `d` a second UV set (UV1) with LightmapUnwrap: the vertex buffer is rebuilt (seam vertices duplicated), the indices remapped.
bool addLightmapUv1(OcMeshData& d, u32 atlasEdge, std::string* err = nullptr);

// ---- save / reopen (step 7, model half) ----
struct SaveOptions {
    u32 lightmapAtlasEdge = 0;                           // > 0: UV1 through LightmapUnwrap
    std::function<bool(OcMeshData&, std::string*)> cook; // optional last step (the editor passes the Trifactor cook); false aborts the save
};
// Writes `path` (.ocmesh) and `path` + ".edit" (the EditMesh as text, tied to the mesh file by a hash of what was written).
bool saveAuthoredMesh(const std::string& path, const EditMesh& m, const SaveOptions& o = {}, std::string* why = nullptr);
// Loads `path`. If the sidecar exists and still matches the file, the quads come back (*fromSidecar = true); otherwise the mesh opens as triangles.
bool loadAuthoredMesh(const std::string& path, EditMesh& out, bool* fromSidecar = nullptr, std::string* why = nullptr);
// The sidecar text, exposed for tests.
std::string serializeEditMesh(const EditMesh& m, u64 meshHash);
bool parseEditMesh(const std::string& text, EditMesh& out, u64* meshHash = nullptr);
u64 hashOcMesh(const OcMeshData& d);

}  // namespace aver::fmt::em
