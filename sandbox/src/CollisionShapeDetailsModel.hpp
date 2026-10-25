#pragma once
// The pure half of the Collision Shape Details section and Add Component entry (SHAPES_MATERIALS): when the component may be
// attached, what "Create .ocshape from the Rigid Body" writes, the asset path it writes to, the canonical COMPONENT record the section
// edits, the text of the Play-time read-outs and of the surface probe. Header-only and ImGui-free so tests/editor can drive every rule;
// CollisionShapeDetailsSection.cpp is the widget half. docs/PHYSICS_SHAPES.md "Editor".
#include "aver/core/Math.hpp"
#include "aver/core/SurfaceId.hpp"
#include "aver/core/Types.hpp"
#include "aver/formats/OcShape.hpp"
#include "aver/formats/OcWorld.hpp"
#if AVER_MODULE_SCENE
#  include "aver/scene/Components.hpp"
#endif

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace aver::editor::csmodel {

// The component needs a Rigid Body beside it: the body supplies the motion type, mass, friction and layer, the component only the shape.
// Adding the component attaches a default static one (the editor has no other way to add it, and no level file carries one), so this
// only speaks when the body was removed afterwards. nullptr when all is well.
inline const char* attachRefusal(bool hasRigidBody) {
    return hasRigidBody ? nullptr : "No Rigid Body on this entity: the shape builds no body without one (remove and add this component again to attach a default one).";
}

// "Chair 01" -> "Chair_01": the characters a file name and a surface or node id share. Empty falls back to "Shape".
inline std::string sanitiseName(const std::string& name) {
    std::string out;
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        out += ok ? c : '_';
    }
    while (!out.empty() && out.front() == '_') out.erase(out.begin());
    return out.empty() ? std::string("Shape") : out;
}

// The content-relative path "Create .ocshape" writes: Physics/<EntityName>.ocshape.
inline std::string assetPathForEntity(const std::string& entityName) { return "Physics/" + sanitiseName(entityName) + ".ocshape"; }

#if AVER_MODULE_SCENE
// The Rigid Body's own shape as a one-node tree. CRigidBody keeps a capsule's HALF height of the cylinder and a cylinder's HALF height;
// the file keeps the total and the full (physics_shape_builder_abi.h), so the conversion is the one PhysicsSceneSync already makes.
// False with `why` for a body that has no size yet (a freshly attached default one) or an unknown shape kind.
inline bool shapeFromRigidBody(const scene::CRigidBody& rb, fmt::OcShapeData& out, std::string& why) {
    out = fmt::OcShapeData{};
    fmt::ShapeNode n;
    n.id = "body";
    switch (rb.shapeKind) {
        case scene::kRigidBodyShapeBox: n.kind = fmt::ShapeNodeKind::Box; n.nums = {rb.dims[0], rb.dims[1], rb.dims[2]}; break;
        case scene::kRigidBodyShapeSphere: n.kind = fmt::ShapeNodeKind::Sphere; n.nums = {rb.dims[0]}; break;
        case scene::kRigidBodyShapeCapsule: n.kind = fmt::ShapeNodeKind::Capsule; n.nums = {rb.dims[0], 2.0f * (rb.dims[1] + rb.dims[0])}; break;
        case scene::kRigidBodyShapeCylinder: n.kind = fmt::ShapeNodeKind::Cylinder; n.nums = {rb.dims[0], 2.0f * rb.dims[1]}; break;
        default: why = "The Rigid Body's shape kind is not one the file can describe."; return false;
    }
    out.nodes.push_back(n);
    out.root = "body";
    out.massKg = rb.massKg > 0.0f ? rb.massKg : 0.0f;
    const std::vector<std::string> problems = fmt::validateOcShape(out);
    if (!problems.empty()) {
        out = fmt::OcShapeData{};
        why = "The Rigid Body has no usable size yet (" + problems.front() + ").";
        return false;
    }
    return true;
}
#endif

// What the section says under the asset row; "" when there is nothing to say.
inline std::string assetProblem(u64 assetId, const std::string& assetRel, bool parsed, const std::string& parseError) {
    if (assetId == 0) return {};
    if (assetRel.empty()) return "The shape asset is not under Content.";
    if (!parsed) return "The .ocshape does not load: " + parseError;
    return {};
}

// The component's record as the section edits it: keys at their default are omitted, so it compares equal to what capture reads back.
// `surfaceKey` is the name or `#hex` spelling of the surface ("" = none); `motion` is "kinematic" or "dynamic" for a body that moves
// ("" = static, omitted).
inline fmt::OcPlacedComponent makeRecord(const std::string& assetRel, const std::string& surfaceKey, bool noScale, const std::string& motion = {}) {
    fmt::OcPlacedComponent r;
    r.type = "CollisionShape";
    if (!assetRel.empty()) r.kv.emplace_back("asset", assetRel);
    if (!surfaceKey.empty()) r.kv.emplace_back("surface", surfaceKey);
    if (!motion.empty()) r.kv.emplace_back("motion", motion);
    if (noScale) r.kv.emplace_back("noscale", "1");
    return r;
}

// ---- Play-time read-outs ---------------------------------------------------------------------------------------------------

inline const char* typeLabel(int shapeType) {
    static const char* const kLabels[] = {"Unknown", "Box", "Sphere", "Capsule", "Cylinder", "Tapered capsule", "Tapered cylinder", "Convex hull",
                                          "Triangle", "Plane", "Empty", "Mesh", "Heightfield", "Static compound", "Mutable compound",
                                          "Rotated / translated", "Scaled", "Offset centre of mass", "Soft body"};
    return shapeType >= 0 && shapeType < static_cast<int>(sizeof kLabels / sizeof kLabels[0]) ? kLabels[shapeType] : "Unknown";
}

struct ShapeReadout {
    bool valid = false;
    int type = 0;
    f32 volumeCm3 = 0.0f, massKg = 0.0f;
    f32 com[3] = {0, 0, 0};
    f32 boundsMin[3] = {0, 0, 0}, boundsMax[3] = {0, 0, 0};
    int children = 0, leaves = 0, triangles = 0;
};

inline std::string fmtNum(f32 v, const char* format = "%.1f") {
    char b[48];
    std::snprintf(b, sizeof b, format, static_cast<double>(v));
    return b;
}

inline std::vector<std::string> readoutLines(const ShapeReadout& r) {
    std::vector<std::string> out;
    if (!r.valid) return out;
    out.push_back(std::string("Shape: ") + typeLabel(r.type));
    if (r.volumeCm3 > 0.0f) out.push_back("Volume: " + fmtNum(r.volumeCm3, "%.0f") + " cm3 (" + fmtNum(r.volumeCm3 * 1.0e-6f, "%.4f") + " m3)");
    out.push_back(r.massKg > 0.0f ? "Mass: " + fmtNum(r.massKg) + " kg" : std::string("Mass: none (the body's own mass stands)"));
    out.push_back("Centre of mass: (" + fmtNum(r.com[0]) + ", " + fmtNum(r.com[1]) + ", " + fmtNum(r.com[2]) + ") cm");
    out.push_back("Bounds: " + fmtNum(r.boundsMax[0] - r.boundsMin[0]) + " x " + fmtNum(r.boundsMax[1] - r.boundsMin[1]) + " x " +
                  fmtNum(r.boundsMax[2] - r.boundsMin[2]) + " cm");
    if (r.children > 0) out.push_back(std::to_string(r.children) + " child(ren), " + std::to_string(r.leaves) + " leaf(s)");
    if (r.triangles > 0) out.push_back(std::to_string(r.triangles) + " triangle(s)");
    return out;
}

// ---- the surface probe -----------------------------------------------------------------------------------------------------

struct Probe {
    bool hit = false;
    i32 surface = 0;
    std::string name;            // the table's name for the surface, "" when it has none
    f32 friction = -1.0f, restitution = -1.0f;
    i32 tag = 0;
    i32 child = -1, triangle = -1, cellX = -1, cellY = -1;
    f32 distanceCm = 0.0f;
};

inline std::string surfaceLabel(i32 surface, const std::string& name) {
    if (surface == 0) return "no surface";
    if (!name.empty()) return name;
    char b[24];
    std::snprintf(b, sizeof b, "#%08x", static_cast<unsigned>(surface));
    return b;
}

inline std::string formatProbe(const Probe& p) {
    if (!p.hit) return "Nothing hit.";
    std::string s = surfaceLabel(p.surface, p.name);
    s += "  friction " + (p.friction >= 0.0f ? fmtNum(p.friction, "%.2f") : std::string("unset"));
    s += "  restitution " + (p.restitution >= 0.0f ? fmtNum(p.restitution, "%.2f") : std::string("unset"));
    s += "  tag " + std::to_string(p.tag);
    std::string where;
    const auto add = [&](const std::string& w) { where += (where.empty() ? "" : ", ") + w; };
    if (p.child >= 0) add("child " + std::to_string(p.child));
    if (p.triangle >= 0) add("triangle " + std::to_string(p.triangle));
    if (p.cellX >= 0) add("cell " + std::to_string(p.cellX) + "," + std::to_string(p.cellY));
    if (!where.empty()) s += "  (" + where + ")";
    s += "  at " + fmtNum(p.distanceCm, "%.0f") + " cm";
    return s;
}

} // namespace aver::editor::csmodel

namespace aver::editor {

// ---- the section's environment ---------------------------------------------------------------------------------------------

// What SandboxApp hands the section (the Vehicle section's pattern): the undo helpers, the asset row, the content index.
struct CollisionShapeSectionEnv {
    bool playing = false;
    // Run `mutate` as ONE undo step when `changed` is true. Call them straight after the widget (gestureEdit reads the last item).
    std::function<void(bool changed, const std::function<void()>& mutate)> instantEdit;
    std::function<void(bool changed, const std::function<void()>& mutate)> gestureEdit;
    // Draws the .ocshape path row (current path, drop target, Change...) and applies a new path through the applier.
    std::function<void(const char* key, const char* label, u64 currentAssetId)> assetRow;
    // Content-relative path of a content id ("" when it is not under Content), and whether a content-relative file exists.
    std::function<std::string(u64)> pathOfId;
    std::function<bool(const std::string&)> fileExists;
    // Writes `text` to the content-relative `rel`, adds it to the Content Browser and assigns it to the component (one undo step).
    std::function<bool(const std::string& rel, const std::string& text)> createAsset;
    // Opens the .ocshape in its editor tab.
    std::function<void(const std::string& rel)> openEditor;
    std::function<void(const std::string&)> setStatus;
    // The project's content directory (for the surface table) and the entity's name (for the Create path).
    std::string contentDir;
    std::string entityName;
    // The ray the probe fires: out the camera's centre ray. Unset or false = the probe casts straight down from the entity.
    std::function<bool(Vec3& origin, Vec3& direction)> pickRay;
};

#if AVER_WITH_IMGUI
// The Collision Shape section's body for entity `entity` (nothing when it has no CCollisionShape). `worldPtr` is the live scene::World*.
void drawCollisionShapeSection(const CollisionShapeSectionEnv& env, void* worldPtr, u32 entity);
#endif

} // namespace aver::editor
