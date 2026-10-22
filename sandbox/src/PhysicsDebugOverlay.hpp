#pragma once
// Physics viewport overlay (Show > Physics): Jolt's own debug drawing -- body shapes, bounds, constraints and limits, the last fixed
// step's contacts, soft body vertices and constraints, character capsules, the static mesh and heightfield triangles around the
// camera, labels -- as a line feed from aver_phys_debug_* (physics_debug_abi.h). Not a second overlay system: it sits beside Show
// Colliders (the cheap per-body AABB boxes, unchanged) and follows the Vehicle and Soft Body overlays' pattern.
//
// Pure feed -> rhi::LineVertex, drawn like the motion overlay (see MotionDebugOverlay.hpp): once per frame while an option is on,
// physicsDebugCollect(...) fills a frame, physicsDebugDraw(...) uploads it as a line mesh and draws it with an identity matrix, then
// physicsDebugDrawLabels inside the viewport window. The feed is the module's lines byte for byte (the layout is two LineVertex),
// so D3D12 and Vulkan agree by construction. The packaged game has no line overlay and none is added.
//
// Also the editor's profiler section for the module (Window > GPU Profiler > Physics (Jolt)), PhysicsProfilePanel.cpp.
#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"
#include "aver/rhi/RHI.hpp"

#include <string>
#include <vector>

namespace aver::editor {

struct PhysicsDebugOptions {
    bool bodies = false;           // wireframe shapes (meshes and heightfields are Static triangles)
    bool bounds = false;
    bool centreOfMass = false;
    bool velocity = false;
    bool sleep = false;
    bool mass = false;
    bool constraints = false;      // joint frames, ragdoll joints, vehicle wheels and suspension
    bool limits = false;
    bool contacts = false;         // the last fixed step's contact points and normals
    bool supportFaces = false;
    bool softVertices = false;
    bool softEdges = false;
    bool characters = false;
    bool staticTriangles = false;  // mesh / heightfield triangles inside the radius
    bool linearCast = false;       // where continuous collision ran
    bool labels = false;           // handle, mass and speed per moving body
    int colourScheme = 0;          // AVER_PHYS_DBG_SCHEME_*: 0 motion, 1 sleep, 2 layer, 3 shape, 4 island, 5 instance
    f32 radiusCm = 4000.0f;
    int maxLines = 60000;
    bool any() const {
        return bodies || bounds || centreOfMass || velocity || sleep || mass || constraints || limits || contacts || supportFaces ||
               softVertices || softEdges || characters || staticTriangles || linearCast || labels;
    }
};

struct PhysicsDebugLabel {
    Vec3 world;
    std::string text;
    u32 rgba = 0xFFFFFFFFu;
};

struct PhysicsDebugFrame {
    std::vector<rhi::LineVertex> lines;   // two vertices per line, colours 0..1, world space
    std::vector<PhysicsDebugLabel> labels;
    bool truncated = false;               // the line budget ran out
    void clear() { lines.clear(); labels.clear(); truncated = false; }
};

// The module calls physicsDebugCollect makes, so a test can stand a fake feed in for the physics module.
struct PhysicsDebugSource {
    i32 (*setScheme)(i32 scheme);
    i32 (*arm)(u32 flags);
    i32 (*collect)(u32 flags, const f32* cameraXyz, f32 radiusCm, i32 maxLines);
    i32 (*truncated)();
    i32 (*lines)(f32* outLines, i32 maxLines);
    i32 (*labelCount)();
    i32 (*labelGet)(i32 index, f32* outXyz, f32* outRgb, u8* outTextUtf8, i32 textCap);
};

// The options as AVER_PHYS_DBG_* flags (0 when nothing is on). The island scheme is a scheme, not a flag.
u32 physicsDebugFlags(const PhysicsDebugOptions& opts);

// Clears `out`; with any option on, sets the colour scheme, arms the per-step captures (contacts, supporting faces, continuous
// collision: they are drawn inside the step, so they are asked for every frame the overlay is on), collects around `eye` and copies
// the lines and labels. With nothing on it only disarms. A source that answers 0 lines leaves `out` empty.
void physicsDebugCollect(const PhysicsDebugOptions& opts, const Vec3& eye, PhysicsDebugFrame& out, const PhysicsDebugSource& src);

#if AVER_MODULE_PHYSICS
// The real feed: the aver_phys_debug_* functions.
const PhysicsDebugSource& physicsDebugModuleSource();

// One frame of the overlay: destroys last frame's line mesh, collects, uploads and draws it (identity matrix). `mesh` is the caller's
// persistent handle. Nothing is drawn while `chromeHidden` (screenshots without editor chrome), but the labels stay collected.
void physicsDebugDraw(rhi::IDevice* device, const PhysicsDebugOptions& opts, const Vec3& eye, PhysicsDebugFrame& frame,
                      rhi::LineHandle& mesh, bool chromeHidden);
#endif

#if AVER_WITH_IMGUI
// The whole "Show Physics" submenu (BeginMenu to EndMenu), so the menu bar's shared edit is one call.
void drawPhysicsDebugMenu(PhysicsDebugOptions& opts, const PhysicsDebugFrame& frame);
// The labels, projected into the viewport (`viewProj` is the camera's), skipping those beyond `maxDistCm` of `eye`.
void physicsDebugDrawLabels(const PhysicsDebugFrame& frame, const Mat4& viewProj, f32 vpX, f32 vpY, f32 vpW, f32 vpH, const Vec3& eye, f32 maxDistCm);
// The "Physics (Jolt)" collapsing header of the profiler panel: the stats line, enable / reset, and the scope tree.
void drawPhysicsProfileSection(float dpi);
#endif

} // namespace aver::editor
