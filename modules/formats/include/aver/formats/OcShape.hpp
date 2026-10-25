#pragma once
// .ocshape -- a collision SHAPE TREE: the simple collision an entity's CollisionShape component builds its rigid body from
// (docs/PHYSICS_SHAPES.md). Leaves are the shapes the physics module can build (box, sphere, capsule, cylinder, tapered capsule
// and cylinder, triangle, plane, empty, convex hull from points or from a mesh, triangle mesh), decorators move, re-centre or scale
// one child, and a compound holds many children with a pose each. The file names the tree; physics_shape_builder_abi.h builds it
// (Runtime ColliderShapes.hpp is the walker), so this layer knows no Jolt and no physics.
//
// TEXT, like .ocsurf and .oclanes: a thing a person edits and diffs. Units and axes are the engine's: centimetres, +X forward,
// +Y right, +Z up, quaternions (x, y, z, w). A capsule's height is TOTAL (both caps); a cylinder's is FULL end to end; both stand
// along +Z with the top radius at +Z.
//
// ---------------------------------------------------------------------------------------------
// OCSHAPE 1
// NAME Chair
// MASS 12                          # kg; 0 or absent = density times volume
// SURFACE Wood                     # default surface for leaves that name none
// NODE seat  box 40 40 4
// NODE back  box 40 3 25
// NODE leg   cylinder 3 45 radius 0.5
// NODE top   compound static
// CHILD top seat at 0 0 45
// CHILD top back at -37 0 75 rot 0 0 0 1
// CHILD top leg  at 32 32 22
// ROOT top
// ---------------------------------------------------------------------------------------------
//
//   NAME <text>                          rest of the line
//   MASS <kg>                            >= 0
//   SURFACE <name>                       a physical surface (Surfaces.ocsurf); the default for leaves that name none
//   NODE <id> <kind> <numbers> [options] one per node; an id is [A-Za-z0-9_]{1,63}, unique
//   CHILD <compound> <node> [at x y z] [rot qx qy qz qw] [user <n>]      in file order
//   ROOT <id>                            required: the node the body is built from
//
// LEAF KINDS (numbers in order):
//   box hx hy hz            half extents               sphere r                 capsule r h            cylinder r h
//   tcapsule top bottom h   tcylinder top bottom h     triangle ax ay az bx by bz cx cy cz
//   plane nx ny nz offset half   (the points with n.x = offset; the side opposite the normal is solid; half bounds its box)
//   empty x y z             no volume, centre of mass at that point
//   hullpoints x y z ...    at least four points, three numbers each
//   hull mesh <path>        the convex hull of a mesh's vertices (a content path; no spaces)
//   mesh <path>             the collision mesh of a content mesh, with its slot surfaces
// LEAF OPTIONS (trailing key value pairs, any order): radius <cm> (convex radius), density <kg/m3>, tolerance <cm> (hull),
//   edge <deg> (mesh active-edge angle; negative = every edge), quality runtime|build, surface <name>.
// DECORATORS (one child node each):
//   NODE <id> rt <child> [at x y z] [rot qx qy qz qw]     rotated and translated
//   NODE <id> com <child> at x y z                        the centre of mass moves by that offset, the geometry does not
//   NODE <id> scale <child> sx sy sz
// COMPOUND:
//   NODE <id> compound static|mutable     its children come from CHILD records. A mutable compound belongs to ONE body, so a
//                                         node of that kind may be referenced once.
//
// The graph is a tree: a leaf or decorator may be referenced by several parents (it is built once and shared), a cycle is an
// error. FORWARD COMPATIBILITY: an unknown record or an unknown option key is skipped with ONE warning per file; a newer VERSION
// is refused. Written canonically: parse(write(d)) is d and a second write is byte-identical to the first.
#include "aver/core/Types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::fmt {

inline constexpr u32 kOcShapeMaxNodes = 4096;
inline constexpr u32 kOcShapeMaxChildren = 65536;

enum class ShapeNodeKind : u8 {
    Box, Sphere, Capsule, Cylinder, TaperedCapsule, TaperedCylinder, Triangle, Plane, Empty, HullPoints, HullMesh, Mesh,
    RotatedTranslated, OffsetCom, Scaled, Compound,
};

inline bool shapeKindIsLeaf(ShapeNodeKind k) { return k <= ShapeNodeKind::Mesh; }
inline bool shapeKindIsDecorator(ShapeNodeKind k) {
    return k == ShapeNodeKind::RotatedTranslated || k == ShapeNodeKind::OffsetCom || k == ShapeNodeKind::Scaled;
}

// The word the file uses: box, sphere, capsule, cylinder, tcapsule, tcylinder, triangle, plane, empty, hullpoints, hull (for
// HullMesh, followed by "mesh"), mesh, rt, com, scale, compound.
const char* shapeKindWord(ShapeNodeKind k);
// How many numbers a leaf kind takes before its options; -1 for the kinds that take a path, a child or a variable count.
int shapeKindNumberCount(ShapeNodeKind k);

struct ShapeNode {
    std::string id;
    ShapeNodeKind kind = ShapeNodeKind::Box;
    std::vector<f32> nums;      // leaf numbers in the grammar's order (hullpoints: 3 per point)
    std::string path;           // HullMesh, Mesh: the content path
    std::string child;          // decorators: the node id
    f32 pos[3] = {0, 0, 0};     // RotatedTranslated: at; OffsetCom: the offset
    f32 rot[4] = {0, 0, 0, 1};  // RotatedTranslated: rot (x y z w)
    f32 scale[3] = {1, 1, 1};   // Scaled
    bool mutableCompound = false;

    // Leaf options. A negative radius / density / tolerance means the shape's own default.
    f32 radius = -1.0f;
    f32 density = -1.0f;
    f32 tolerance = -1.0f;
    bool hasEdge = false;
    f32 edge = 5.0f;
    i32 quality = -1;           // -1 unset, 0 runtime performance, 1 build speed
    std::string surface;        // a surface name, empty = the file's default
};

struct ShapeChildRec {
    std::string compound;
    std::string node;
    f32 pos[3] = {0, 0, 0};
    f32 rot[4] = {0, 0, 0, 1};
    i32 user = 0;
};

struct OcShapeData {
    std::string name;
    f32 massKg = 0.0f;
    std::string surface;
    std::vector<ShapeNode> nodes;          // file order
    std::vector<ShapeChildRec> children;   // file order
    std::string root;

    const ShapeNode* find(std::string_view id) const;
    ShapeNode* find(std::string_view id);
};

// [A-Za-z0-9_]{1,63}.
bool ocShapeIdValid(std::string_view id);

// Every problem with the tree as it stands (empty = valid). The same list the editor shows and parseOcShape refuses on:
// a missing or undefined ROOT, a duplicate or invalid id, a leaf with the wrong count of numbers, a non-finite number, a value out
// of the ABI's range, an undefined reference, a compound with no children, a cycle, a mutable compound referenced twice, a bad
// surface name.
std::vector<std::string> validateOcShape(const OcShapeData& d);

// The node indices reachable from ROOT, children before parents, each once (a shared leaf appears once). Empty when ROOT is
// undefined; stops at a cycle rather than looping. A builder walks this order.
std::vector<u32> ocShapeBuildOrder(const OcShapeData& d);

// Parses `text`. TOLERANT OF CRLF, '#' comments and blank lines; strict about everything else, and validateOcShape must pass.
// False on a problem with `err` set to what and (for a syntax error) which line, and `out` left EMPTY -- never the partial state of
// a file that failed halfway.
bool parseOcShape(std::string_view text, OcShapeData& out, std::string* err = nullptr);

// The canonical form: the header, NAME / MASS / SURFACE when set, every NODE in order with only the options that are set, every
// CHILD in order with `at` / `rot` / `user` only when not at their defaults, then ROOT. Numbers are the shortest text that reads
// back as the same f32.
std::string writeOcShape(const OcShapeData& d);

bool loadOcShape(const std::string& path, OcShapeData& out, std::string* err = nullptr);
// Creates the folder; a crash mid-save keeps the old file.
bool saveOcShape(const std::string& path, const OcShapeData& in, std::string* err = nullptr);

} // namespace aver::fmt
