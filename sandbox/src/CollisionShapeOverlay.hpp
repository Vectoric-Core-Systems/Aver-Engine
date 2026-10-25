#pragma once
// Show > Collision Shapes (SHAPES_MATERIALS): the SHAPE-ACCURATE wire of the selected entity's body and of every moving body,
// where Show > Colliders draws only boxes. Each body's collision triangles (aver_phys_body_triangles) are drawn as their edges, tinted by
// the surface each triangle carries (a stable hue per surface id, grey for none), with a cross at the body's centre of mass (yellow) and at
// its origin (cyan): for a compound or an offset shape the two differ, and a pose write-back must use the origin.
//
// Also the PREVIEW the .ocshape editor tab asks for: the triangles of the shape being edited, built into a throw-away static body and read
// back, drawn at the selected entity's pose (or the world origin). The editor writes collisionShapePreview(); the host draws it.
//
// Pure feed -> rhi::LineVertex like the vehicle overlay: collisionShapeOverlayRender collects once per frame while the option is on or a
// preview is active, uploads the lines as a line mesh and draws it with an identity matrix. Static bodies keep the existing AABB overlay
// (SandboxViewport.cpp), whose 20,000-body optimisations are not disturbed. Both backends draw the same lines.
//
// THE HOOK INTO THE HOST IS TWO LINES, in the files at the Debug COFF cap:
//   ImGui::MenuItem("Show Collision Shapes", nullptr, &editor::collisionShapeOverlayOptions().enabled);        // SandboxShell.cpp
//   editor::collisionShapeOverlayRender(e, static_cast<u32>(selEntity_), noEditorChrome_);                      // SandboxRender.cpp
#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"
#include "aver/rhi/RHI.hpp"

#include <string>
#include <vector>

namespace aver { class Engine; }

namespace aver::editor {

struct CollisionShapeOverlayOptions {
    bool enabled = false;          // Show > Collision Shapes
    bool selected = true;          // the selected entity's body
    bool movers = true;            // every kinematic and dynamic body
    bool crosses = true;           // centre of mass and origin
    int maxBodies = 64;            // movers drawn per frame
    int maxTrianglesPerBody = 3000;
};
CollisionShapeOverlayOptions& collisionShapeOverlayOptions();

struct CollisionShapeFrame {
    std::vector<rhi::LineVertex> lines;   // two vertices per line, colours 0..1, world space
    void clear() { lines.clear(); }
};

// A stable colour for a surface id: grey for none (0), else a hue taken from the id so one surface is always the same colour.
void surfaceColour(i32 surface, f32& r, f32& g, f32& b);

// A pose the triangles are carried through (the shape's local space -> world).
struct OverlayPose {
    Vec3 position{0.0f, 0.0f, 0.0f};
    Quat rotation = Quat::identity();
};

// Appends the three edges of each triangle (`tris` holds 9 floats per triangle, engine centimetres), tinted by `surfaces[i]` when given,
// carried through `pose` when given. Pure.
void collisionShapeFeedTriangles(const f32* tris, const i32* surfaces, usize triCount, const OverlayPose* pose, CollisionShapeFrame& out);
// A three-axis cross of half-size `size` at `at`.
void collisionShapeFeedCross(const Vec3& at, f32 size, f32 r, f32 g, f32 b, CollisionShapeFrame& out);

// The shape the .ocshape editor tab is showing (a copy, so closing the tab never leaves the overlay holding a dangling pointer): the
// triangles of its built shape in shape space, and one surface per triangle.
struct CollisionShapePreview {
    bool active = false;
    const void* owner = nullptr;   // the tab that wrote it, so only that tab clears it
    std::vector<f32> triangles;
    std::vector<i32> surfaces;
    std::string note;              // why there is no preview ("the physics world is not running", ...), shown by the tab
};
CollisionShapePreview& collisionShapePreview();

#if AVER_MODULE_PHYSICS
// Collects the selected body (`selectedBody`, a physics handle, 0 = none) and the moving bodies into `out` (cleared first).
void collisionShapeCollect(const CollisionShapeOverlayOptions& opts, int selectedBody, CollisionShapeFrame& out);
#endif

// Once per frame inside the scene pass: collects, uploads and draws the lines when the option is on or a preview is active, and frees the
// previous frame's mesh either way. `selectedEntity` is the scene entity id (anything invalid = none); its CRigidBody's body is the selected
// body and its world transform places the preview. Does nothing visible while `noEditorChrome` (a clean capture).
void collisionShapeOverlayRender(Engine& e, u32 selectedEntity, bool noEditorChrome);

} // namespace aver::editor
