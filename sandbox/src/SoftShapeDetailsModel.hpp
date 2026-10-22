#pragma once
// The pure half of the Jelly and Rope Details sections (docs/SOFTBODY.md "Jelly, ropes and cables"): what the COMPONENT record keys mean and
// when a key is at its default (so the canonical record omits it), the numbers the sliders show for an asset, the advice lines under them,
// the rope end handles' arithmetic, and the colours of the Show > Soft Constraints overlay. Header-only and ImGui-free so tests can drive
// every rule; SoftShapeGizmos.cpp is the widget, overlay and gizmo half (in its own translation unit because SandboxComponentDetails.cpp and
// SandboxRender.cpp are at the Debug COFF section cap).
//
// The component state itself is read and written through game::applyPlacedComponent / capturePlacedComponent (LevelComponents.cpp), so
// what the section edits is exactly what the level saves and reloads; this header only knows the record spelling.
#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/softbody/SoftAsset.hpp"

#include "SoftBodyDetailsModel.hpp"
#include "SoftFieldDetailsModel.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string>
#include <string_view>

namespace aver::editor::shapemodel {

namespace sm = softmodel;

// ---- Add Component rows ------------------------------------------------------------------------------------------

struct ShapeKind {
    const char* type;        // the COMPONENT record type
    const char* label;       // the Add Component menu row
    bool needsMesh;          // a jelly fills a mesh; a rope makes its own
    const char* tip;
};
inline constexpr std::array<ShapeKind, 2> kShapeKinds = {{
    {"Jelly", "Jelly", true, "Fills this closed mesh with a tetrahedral lattice that wobbles and keeps its volume."},
    {"Rope", "Rope", false, "A rope or cable between two points: a chain of rods drawn as a tube. Drag its end handles in the viewport."},
}};

inline const ShapeKind* shapeKind(std::string_view type) {
    for (const ShapeKind& k : kShapeKinds)
        if (type == k.type) return &k;
    return nullptr;
}

// ---- COMPONENT record keys ---------------------------------------------------------------------------------------

// Jelly: asset, stiffness, cell, gravity, damping, world (0 = no collision), gravityon (0 = no gravity).
// Rope:  asset, start=x,y,z end=x,y,z (world points), length, radius, world, startpin, endpin (0 = that end is free).
inline constexpr sm::KeyDefault kJellyKeys[] = {{"stiffness", 1}, {"cell", 0}, {"gravity", 1}, {"damping", 1}, {"world", 1}, {"gravityon", 1}};
inline constexpr sm::KeyDefault kRopeKeys[] = {{"length", 0}, {"radius", 0}, {"world", 1}, {"startpin", 1}, {"endpin", 1}};

// Drops every key that is at its default. Idempotent.
inline void canonicalize(fmt::OcPlacedComponent& r) {
    const auto strip = [&](const sm::KeyDefault* keys, usize n) {
        for (usize i = 0; i < n; ++i) {
            const std::string* v = r.find(keys[i].key);
            if (v && sm::numberOf(r, keys[i].key, keys[i].def + 1.0) == keys[i].def) sm::setKey(r, keys[i].key, std::string());
        }
    };
    if (r.type == "Jelly") strip(kJellyKeys, std::size(kJellyKeys));
    else if (r.type == "Rope") strip(kRopeKeys, std::size(kRopeKeys));
}

// "x,y,z" -> three numbers. False for anything else.
inline bool parseVec3(const std::string& text, Vec3& out) {
    float v[3] = {0, 0, 0};
    int n = 0;
    usize pos = 0;
    while (n < 3 && pos <= text.size()) {
        const usize comma = text.find(',', pos);
        const std::string part = text.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        char* end = nullptr;
        const double d = std::strtod(part.c_str(), &end);
        if (part.empty() || !end || *end != '\0') return false;
        v[n++] = static_cast<float>(d);
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    if (n != 3) return false;
    out = Vec3{v[0], v[1], v[2]};
    return true;
}

inline std::string vec3Text(const Vec3& v) {
    char b[96];
    std::snprintf(b, sizeof b, "%.6g,%.6g,%.6g", static_cast<double>(v.x), static_cast<double>(v.y), static_cast<double>(v.z));
    return b;
}

// A Rope record from its two world points (what Add Component writes); the ends are pinned.
inline fmt::OcPlacedComponent newRopeRecord(const Vec3& start, const Vec3& end, f32 lengthCm = 0.0f, f32 radiusCm = 0.0f) {
    fmt::OcPlacedComponent r;
    r.type = "Rope";
    r.kv.emplace_back("start", vec3Text(start));
    r.kv.emplace_back("end", vec3Text(end));
    char b[40];
    if (lengthCm > 0.0f) { std::snprintf(b, sizeof b, "%.6g", static_cast<double>(lengthCm)); r.kv.emplace_back("length", b); }
    if (radiusCm > 0.0f) { std::snprintf(b, sizeof b, "%.6g", static_cast<double>(radiusCm)); r.kv.emplace_back("radius", b); }
    return r;
}

// ---- starter assets ----------------------------------------------------------------------------------------------

inline softbody::SoftAsset starterJelly() { return softbody::makeSoftAsset(softbody::SoftKind::Jelly, softbody::SoftPreset::Jelly); }
inline softbody::SoftAsset starterRope() { return softbody::makeSoftAsset(softbody::SoftKind::Rope, softbody::SoftPreset::Rope); }

// "Meshes/Blob.ocmesh" -> "Meshes/Blob.ocsoft" (a jelly's asset sits beside its mesh); a rope has no mesh: "Soft/<name>.ocsoft".
inline std::string jellyAssetPath(const std::string& meshRel) { return sm::assetPathForMesh(meshRel); }
inline std::string ropeAssetPath(const std::string& entityName) {
    std::string stem;
    for (const char c : entityName)
        stem += (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') ? c : '_';
    return "Soft/" + (stem.empty() ? std::string("Rope") : stem) + ".ocsoft";
}

// ---- the numbers the sliders show ------------------------------------------------------------------------------

struct JellyView {
    f32 cellCm = 0.0f, snapCm = 0.0f, volumeCompliance = 0.0f, edgeCompliance = 0.0f, vertexRadiusCm = 0.0f, densityKgM3 = 0.0f;
    u32 maxNodes = 0, iterations = 0;
    f32 damping = 0.0f;
};

inline JellyView viewOf(const softbody::SoftAsset& a, f32 cellOverrideCm = 0.0f) {
    JellyView v;
    v.cellCm = cellOverrideCm > 0.0f ? cellOverrideCm : a.jelly.cellCm;
    v.snapCm = a.jelly.snapCm;
    v.volumeCompliance = a.jelly.volumeCompliance;
    v.edgeCompliance = a.jelly.edgeCompliance;
    v.vertexRadiusCm = softbody::jellyVertexRadiusCm(a.jelly, v.cellCm);
    v.densityKgM3 = a.jelly.densityKgM3;
    v.maxNodes = a.jelly.maxNodes;
    v.iterations = a.sim.iterations;
    v.damping = a.sim.damping;
    return v;
}

struct RopeView {
    f32 radiusCm = 0.0f, segmentCm = 0.0f, massPerMeterKg = 0.0f, slack = 0.0f, uvCmPerTile = 0.0f, stretchCompliance = 0.0f, bendTwistCompliance = 0.0f;
    u32 sides = 0;
    std::string material;
};

inline RopeView ropeViewOf(const softbody::SoftAsset& a, f32 radiusOverrideCm = 0.0f) {
    RopeView v;
    v.radiusCm = radiusOverrideCm > 0.0f ? radiusOverrideCm : a.rope.radiusCm;
    v.segmentCm = a.rope.segmentCm;
    v.massPerMeterKg = a.rope.massPerMeterKg;
    v.slack = a.rope.slack;
    v.uvCmPerTile = a.rope.uvCmPerTile;
    v.stretchCompliance = a.rope.stretchCompliance;
    v.bendTwistCompliance = a.rope.bendTwistCompliance;
    v.sides = a.rope.sides;
    v.material = a.rope.material;
    return v;
}

// The rope's rest length for two ends: the component's length when it names one, else the distance times the asset's slack.
inline f32 ropeRestLength(const Vec3& start, const Vec3& end, f32 lengthCm, f32 slack) {
    const f32 chord = dist(start, end);
    return lengthCm > 0.0f ? std::max(lengthCm, chord) : chord * std::max(slack, 1.0f);
}

// Nodes a rope of this length needs (what the system will build): at least 3, and the cap tells whether it is over.
inline u32 ropeNodeCount(f32 lengthCm, f32 segmentCm) {
    if (!(segmentCm > 0.0f) || !(lengthCm > 0.0f)) return 3;
    return std::max(3u, static_cast<u32>(std::ceil(lengthCm / segmentCm - 1e-4f)) + 1u);
}

// ---- advice and state lines ------------------------------------------------------------------------------------

inline std::string jellyAdvice(bool hasMesh, bool failed, bool overCap, bool assetStale, u32 nodeCap) {
    if (!hasMesh) return "A jelly needs a mesh to fill.";
    if (overCap) return "The lattice needs more than " + std::to_string(nodeCap) + " nodes: raise the cell size or the node cap. The entity keeps its authored shape.";
    if (failed) return "The mesh is not closed (every edge must be shared by two triangles), so it is not simulated and keeps its authored shape.";
    if (assetStale) return "The asset was not found or is not a jelly asset; the jelly preset is used (see the log).";
    return {};
}

inline std::string ropeAdvice(bool failed, bool overCap, bool assetStale, bool startPinned, bool endPinned, u32 nodes, u32 nodeCap) {
    if (overCap) return "The rope needs more than " + std::to_string(nodeCap) + " nodes: lengthen the segments or shorten the rope.";
    if (failed) return "The rope could not be made (see the log).";
    if (assetStale) return "The asset was not found or is not a rope asset; the rope preset is used (see the log).";
    if (!startPinned && !endPinned) return "Neither end is pinned: the rope falls.";
    if (nodes > 0 && nodes > nodeCap / 2) return "A long rope: " + std::to_string(nodes) + " nodes cost a tube of " + std::to_string(nodes * 9) + " vertices each tick it is awake.";
    return {};
}

inline std::string jellyStateText(bool awake, bool frozen, bool failed, u32 nodes, f32 volumeRatio, f32 maxStretch) {
    if (failed) return "not simulated";
    char b[120];
    std::snprintf(b, sizeof b, "%s, %u nodes, volume %.0f%%, stretch %.0f%%", frozen ? "frozen" : (awake ? "awake" : "asleep"), nodes,
                  static_cast<double>(volumeRatio) * 100.0, static_cast<double>(maxStretch) * 100.0);
    return b;
}

inline std::string ropeStateText(bool awake, bool frozen, bool failed, bool cut, u32 segments, f32 lengthCm, f32 strain) {
    if (failed) return "not simulated";
    char b[140];
    std::snprintf(b, sizeof b, "%s%s, %u segments, %.0f cm, strain %+.1f%%", frozen ? "frozen" : (awake ? "awake" : "asleep"), cut ? ", cut" : "", segments,
                  static_cast<double>(lengthCm), static_cast<double>(strain) * 100.0);
    return b;
}

// ---- rope end handles ------------------------------------------------------------------------------------------

inline Vec3 ropeXform(const Mat4& m, const Vec3& p) {
    return Vec3{p.x * m.m[0][0] + p.y * m.m[1][0] + p.z * m.m[2][0] + m.m[3][0], p.x * m.m[0][1] + p.y * m.m[1][1] + p.z * m.m[2][1] + m.m[3][1],
                p.x * m.m[0][2] + p.y * m.m[1][2] + p.z * m.m[2][2] + m.m[3][2]};
}

// Where an end is in the world: the offset in its anchor entity's space (a null matrix: the offset is a world point).
inline Vec3 ropeEndWorld(const Mat4* anchorWorld, const Vec3& offset) { return anchorWorld ? ropeXform(*anchorWorld, offset) : offset; }
// The offset that puts the end at a world point (the inverse of the above).
inline Vec3 ropeEndOffsetFor(const Mat4* anchorWorld, const Vec3& worldPoint) { return anchorWorld ? ropeXform(anchorWorld->inverse(), worldPoint) : worldPoint; }

// The handle (0 .. count - 1) nearest to a pick ray among those within radiusCm of it, in front of the origin; -1 for none.
inline int pickHandle(const Vec3* handles, int count, const Vec3& rayOrigin, const Vec3& rayDir, f32 radiusCm) {
    const Vec3 d = rayDir.getSafeNormal();
    int best = -1;
    f32 bestT = 1e30f;
    for (int i = 0; i < count; ++i) {
        const Vec3 to = handles[i] - rayOrigin;
        const f32 t = dot(to, d);
        if (t <= 0.0f) continue;
        const f32 off = (to - d * t).size();
        if (off <= radiusCm && t < bestT) { bestT = t; best = i; }
    }
    return best;
}

// ---- overlay colours -------------------------------------------------------------------------------------------

// A lattice edge by strain: blue compressed, green at rest, red at 25% stretch.
inline void stretchColour(f32 strain, f32& r, f32& g, f32& b) {
    const f32 t = std::clamp(std::fabs(strain) / 0.25f, 0.0f, 1.0f);
    if (strain < 0.0f) { r = 0.2f; g = 0.8f * (1.0f - t) + 0.3f * t; b = 0.3f + 0.7f * t; }
    else { r = 0.2f + 0.8f * t; g = 0.8f * (1.0f - t) + 0.1f * t; b = 0.3f * (1.0f - t); }
}

}  // namespace aver::editor::shapemodel

namespace aver::editor {

#if AVER_MODULE_SOFTBODY_SCENE
// The rope end gizmos (SoftShapeGizmos.cpp). `worldPtr` is the live scene::World. The first two fill the two end positions in the world (false
// when the entity is not a rope) and move an end to a world point by rewriting the component's offset, in its anchor entity's space when it
// has one (the undo step belongs to the caller: it captures the record before and after).
bool ropeGizmoHandles(void* worldPtr, u32 entity, Vec3 out[2]);
bool ropeGizmoDrag(void* worldPtr, u32 entity, int end, const Vec3& worldPoint);
// The mouse half, for the viewport: ropeHandleBegin grabs the nearer end handle of the selected rope when the click ray passes within a 12 pixel
// radius (`pixelWorldCm` is the world size of one pixel at the rope's distance) and returns true so the click is not also a selection change;
// ropeHandleUpdate moves the grabbed end on the camera-facing plane through it, keeping the offset of the grab, and returns true when it moved;
// ropeHandleEnd lets go. The drag is not an undo step (the decal box handles are not either); the Details rows are.
bool ropeHandleBegin(void* worldPtr, u32 entity, const Vec3& rayOrigin, const Vec3& rayDir, f32 pixelWorldCm);
bool ropeHandleActive();
bool ropeHandleUpdate(void* worldPtr, const Vec3& rayOrigin, const Vec3& rayDir);
void ropeHandleEnd();
#endif

// What the Jelly and Rope sections need from the host beyond SoftSectionEnv, as callbacks (each a thin wrapper over what the Cloth section
// already uses in SandboxComponentDetails.cpp): content id -> content-relative path, the "Asset (.ocsoft)" row, adopting a newly written asset
// into the Content Browser, and writing the component's asset field (an undo step).
struct SoftShapeEnv : SoftSectionEnv {
    std::function<std::string(u64 assetId)> pathOfId;
    std::function<void(u64 assetId)> drawAssetRow;
    std::function<void(const std::string& absPath)> adoptNewAsset;
    std::function<void(const std::string& relPath)> assignAsset;
};

#if AVER_WITH_IMGUI
// The Jelly or Rope section for entity `e` (nothing when it has no such component). `world` is the live scene.
void drawSoftShapeSection(const SoftShapeEnv& env, void* worldPtr, u32 entity, bool rope);
// The Solver, Inflate and Group subsections of a Cloth with a loaded .ocsoft (the asset edited in place and saved on a change). `editable` is
// false in Play or with no asset. closedKnown / closed: whether the system has looked at the mesh and found it closed. Returns true when the asset was saved.
bool drawClothShapeSections(const SoftSectionEnv& env, softbody::SoftAsset& asset, const std::string& absAssetPath, bool editable, bool closedKnown, bool closed);
// Live inflation: the pressure slider that calls SoftBodySystem::setPressure during Play.
void drawClothPressureLive(const SoftSectionEnv& env, void* worldPtr, u32 entity);
#endif

}  // namespace aver::editor
