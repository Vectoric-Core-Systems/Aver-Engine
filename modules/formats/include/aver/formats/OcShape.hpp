#pragma once
// .ocshape: collision SHAPE TREE that an entity's CollisionShape component builds its rigid body from (text; spec in docs/PHYSICS_SHAPES.md).
// Units: cm, +X forward, +Y right, +Z up; quaternions are (x, y, z, w). No Jolt here: physics_shape_builder_abi.h builds the tree.
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

// File keyword for k. HullMesh is written as "hull" followed by "mesh".
const char* shapeKindWord(ShapeNodeKind k);
// Numbers a leaf kind takes before its options; -1 for kinds that take a path, a child or a variable count.
int shapeKindNumberCount(ShapeNodeKind k);

struct ShapeNode {
    std::string id;
    ShapeNodeKind kind = ShapeNodeKind::Box;
    std::vector<f32> nums;      // leaf numbers in grammar order; hullpoints: 3 per point
    std::string path;           // HullMesh, Mesh: the content path
    std::string child;          // decorators: the node id
    f32 pos[3] = {0, 0, 0};     // RotatedTranslated: at; OffsetCom: the offset
    f32 rot[4] = {0, 0, 0, 1};  // RotatedTranslated only
    f32 scale[3] = {1, 1, 1};   // Scaled only
    bool mutableCompound = false;           // belongs to one body: referenced once

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

// Every problem with the tree (empty = valid). parseOcShape refuses on the same list.
std::vector<std::string> validateOcShape(const OcShapeData& d);

// Reachable node indices from ROOT, children before parents, each once. Empty when ROOT is undefined; stops at a cycle.
// Builders walk this order.
std::vector<u32> ocShapeBuildOrder(const OcShapeData& d);

// Accepts CRLF, '#' comments and blank lines. Also requires validateOcShape to pass.
// On failure returns false, sets err and leaves out empty (never a partial state).
bool parseOcShape(std::string_view text, OcShapeData& out, std::string* err = nullptr);

// Canonical text: only non-default options are written. Numbers are the shortest text that reads back as the same f32.
std::string writeOcShape(const OcShapeData& d);

bool loadOcShape(const std::string& path, OcShapeData& out, std::string* err = nullptr);
// Creates the folder; a crash mid-save keeps the old file.
bool saveOcShape(const std::string& path, const OcShapeData& in, std::string* err = nullptr);

} // namespace aver::fmt
