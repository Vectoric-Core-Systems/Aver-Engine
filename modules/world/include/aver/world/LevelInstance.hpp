// Turning parsed .ocworld placements into live scene entities -- ONCE, for both hosts.
//
// WHY THIS EXISTS. Until this module, the same loop lived twice: Runtime/src/GameLevel.cpp
// and sandbox/src/SandboxApp.cpp. That was deliberate at the time -- Runtime/CMakeLists.txt
// says so in its own header: "The lift is a COPY: sandbox/src/SandboxApp.cpp is not edited by it, so
// the editor cannot regress. De-duplication is a later slice, proven by the gates staying identical."
// This is that slice, and that is the bar it is held to.
//
// The two copies had ALREADY diverged before they were merged, which is the argument for merging
// them: the editor applied SUN/SKY at load and drove the cloud layer from a PCGVOLUME named "Sky";
// the game applied only FOG and deferred the sky. Each narrowed f64 -> f32 in its own near-identical
// loop. Every future change to how a placement becomes an entity -- chunk ownership above all --
// would have had to be made twice, correctly, forever.
//
// WHAT IS SHARED AND WHAT IS NOT. Shared: the transform narrowing, the rotation contract, entity
// creation, the mesh renderer, material interning, and the static physics box. NOT shared, because
// they are genuinely host policy: the editor's label table and entity->body map, the game's resolved
// PCG field specs, sky application, camera framing, selection. Those stay with their hosts and read
// this function's output.
#pragma once

#include "aver/core/Types.hpp"
#include "aver/core/Math.hpp"
#include "aver/world/LevelTransform.hpp"

#include <unordered_map>
#include <vector>

namespace aver::world {

// A static collision box fitted to a mesh's LOCAL bounds under a world TRS: world-space centre,
// half-extents along the box's own axes, and its orientation.
struct StaticBoxFit {
    Vec3 centre;
    Vec3 halfExtents;
    Quat rotation;
    // `centre`'s offset from the placement's own position, in the placement's UNROTATED, scaled axes
    // (so centre == position + rotation.rotate(pivotToCentre)). Zero for a mesh centred on its pivot.
    Vec3 pivotToCentre;
};

// The built-in unit-cube placeholder's local bounds -- the fallback for a mesh whose bounds are
// unknown.
inline constexpr f32 kPlaceholderHalfExtentCm = 1.0f;

// Fits a static collision box to a mesh's LOCAL bounds (`localMin`/`localMax`, centimetres) under a
// placement's world transform `worldXf`. See LevelInstance.cpp for the exact rule; the short version
// is that for the unit cube ([-1,1] cm on every axis) and an identity rotation this reproduces
// EXACTLY the single old aver_phys_add_static_box(position, |scale|) call it replaces -- the case
// every level authored before this existed already depends on.
//
// Header-only dependency is Math.hpp alone (no Scene, no Physics), so a host that never links
// Aver.Physics can still ask what box a placement's transform implies -- the editor's collision
// overlay, for one.
StaticBoxFit fitStaticBox(const Transform& worldXf, const Vec3& localMin, const Vec3& localMax);

// Scales `localPositions` (3 f32 per vertex, `vertexCount` of them) component-wise by `scale` into
// `outPositions`, and copies `localIndices` (3 per triangle, `indexCount` of them) into `outIndices`
// -- reversing the second and third index of every triangle when `scale`'s determinant is negative,
// so the mesh's faces keep facing outward under a mirror. A component-wise scale is diagonal, so its
// determinant is simply scale.x * scale.y * scale.z: negative for an ODD number of negative axes (one,
// or all three); an EVEN number (two, or none) is a rotation by 180 degrees about the remaining axis,
// which preserves handedness and needs no correction.
//
// Always copies -- never edits `localPositions`/`localIndices` in place -- because the caller's
// source geometry (GameContent::collisionMeshFor's cached CollisionMesh, in every real caller) is
// shared across every placement that names that mesh, and one placement's scale must not corrupt it
// for the next placement that reads it unscaled.
//
// PURE, deliberately: no physics dependency, so LevelInstanceTest can check the scale-and-winding
// math with no physics world. addStaticMeshBody (below) is its one caller.
void scaleMeshForBody(const f32* localPositions, u32 vertexCount, const u32* localIndices,
                      u32 indexCount, const Vec3& scale,
                      std::vector<f32>& outPositions, std::vector<u32>& outIndices);

#if AVER_MODULE_PHYSICS
// Creates fitStaticBox(worldXf, localMin, localMax) as a static body: aver_phys_add_static_box, then
// aver_phys_body_set_rotation unless the fit's rotation is EXACTLY identity (x==0 && y==0 && z==0 &&
// w==1) -- so an unrotated placement still makes the one call it always made, and every rotated one
// makes the second call the old code never did.
//
// Returns the ABI's handle, or 0 if it refused. THE CALLER HAS ALREADY CHECKED aver_phys_ready() --
// this does not call it, and does not call aver_phys_set_entity either: stamping the entity is the
// caller's job, once, after it has decided the body exists.
i32 addStaticBoxBody(const Transform& worldXf, const Vec3& localMin, const Vec3& localMax);

// Creates a static TRIANGLE MESH body for concave geometry a box cannot represent: scaleMeshForBody
// into `worldXf.scale`, then aver_phys_add_mesh with that scaled geometry and worldXf.position as the
// body's centre (the shape is LOCAL geometry around that centre, matching addStaticBoxBody's own
// convention -- see physics_abi.h's own comment on aver_phys_add_mesh), then aver_phys_body_set_rotation
// unless worldXf.rotation is EXACTLY identity, same rule and same reason as addStaticBoxBody.
//
// Returns the ABI's handle, or 0 if it refused (fewer than 3 vertices/indices, or Jolt itself refused
// the shape -- e.g. every triangle degenerate). Same contract as addStaticBoxBody otherwise: the
// caller has already checked aver_phys_ready() and owns aver_phys_set_entity.
i32 addStaticMeshBody(const Transform& worldXf, const f32* localPositions, u32 vertexCount,
                      const u32* indices, u32 indexCount);

// The fitted box of addStaticBoxBody, but as a convex hull of its eight corners around the
// PLACEMENT'S POSITION rather than around the box's own centre: the body's origin is the entity's
// pivot, the frame addStaticMeshBody's bodies already have. That is what lets an animated placement's
// body be driven to its entity's world transform (driveKinematicBodies) whatever the mesh's bounds
// look like. Returns the ABI's handle, or 0 if Jolt refused the hull; same caller contract as
// addStaticBoxBody (physics ready, entity stamping left to the caller).
i32 addPivotBoxBody(const Transform& worldXf, const Vec3& localMin, const Vec3& localMax);
#endif

} // namespace aver::world

#if AVER_MODULE_SCENE
#  include "aver/formats/OcWorld.hpp"
#  include "aver/scene/World.hpp"

#  include <functional>
#  include <string>
#  include <vector>

namespace aver::world {

struct InstantiateOptions {
    // Called once per placement that interned a NON-ZERO surface token, with that token and the
    // authored surface name. Both hosts resolve the surface's .ocmat and cache the handle, but by
    // different means -- the game through GameContent, the editor into its own map -- so the
    // resolution is theirs and only the timing is shared.
    //
    // Called from inside the placement loop, in placement order, so a host that interns further
    // materials from within it gets the same token sequence it would have got before.
    std::function<void(i32 token, const std::string& surface)> bindMaterial;

    // False to skip static-body creation entirely. Bodies are also skipped when the module is built
    // without physics, or when aver_phys_ready() is false -- which is not an error: it is the state
    // a host is in before it has called aver_phys_init.
    bool createBodies = true;

    // Answers "what is the ground height at (x, y)" for placements that asked to be snapped to it.
    // False from the callback means there is no ground there and the placement keeps its authored Z.
    //
    // A HOST CALLBACK, for the reason every other seam in this struct is one: the ground might be a
    // heightfield section, a procedural field, or nothing at all, and Aver.World must not learn to
    // tell those apart -- it does not depend on Aver.Landscape and is not going to start.
    //
    // WHY THIS EXISTS AT ALL: adding terrain under a level whose placements were authored against a
    // flat plane at z=0 buries every one of them. The demo project's hand-placed pines sat at z=0
    // against terrain running -343..+843cm and sank up to eight metres. Re-authoring the Z of every
    // placement would fix that level and no other, and would break again the moment the terrain is
    // resculpted; asking for the ground at load does not.
    std::function<bool(f64 worldXCm, f64 worldYCm, f64& outGroundZCm)> groundHeightAt;

    // A mesh's LOCAL bounds (centimetres), by the placement's objectId -- the id CMeshRenderer::mesh
    // carries, and the same id GameContent/the editor's mesh cache already key their own bounds by.
    // False, or no callback at all, means the bounds are unknown, and the placement collides as the
    // unit-cube placeholder: the behaviour every level authored before fitStaticBox existed already
    // depends on, now named rather than silent.
    //
    // A HOST CALLBACK, for the reason every other seam in this struct is one: the bounds live on
    // GameContent (the game) or the editor's own mesh cache, and Aver.World must not learn to find
    // them -- it does not depend on GameContent, which Runtime links on top of it, and it does not
    // depend on a GPU device, which is what actually loaded the mesh that has them.
    std::function<bool(u64 meshId, Vec3& outLocalMin, Vec3& outLocalMax)> localBoundsFor;

    // A mesh's LOCAL triangles, by the placement's objectId -- the SAME id localBoundsFor above is
    // keyed by. True with at least one triangle (`outVertexCount` and `outIndexCount` both non-zero)
    // means the placement collides with THIS geometry, via addStaticMeshBody; false, no callback at
    // all, or fewer than one triangle's worth of indices, falls back to the fitted box exactly as a
    // level authored before this existed already collides.
    //
    // TRIED FIRST: instantiate() asks this before it asks localBoundsFor, so a mesh that answers here
    // never touches the box path at all -- see instantiate()'s own comment for why concave
    // architecture (an archway, a courtyard wall) needs this rather than the box either fitStaticBox
    // or a mesh's own local bounds could ever describe.
    //
    // A HOST CALLBACK, for the same reason localBoundsFor is one: the triangles live on
    // GameContent's lazily-built collision-mesh cache (the game) or the editor's own copy of the same
    // idea, and Aver.World must not learn to build or load one -- it does not depend on GameContent
    // and does not read .ocmesh files itself.
    //
    // POINTERS INTO GEOMETRY THE HOST ALREADY OWNS AND KEEPS ALIVE, not vectors this struct would
    // have to copy: GameContent::collisionMeshFor's cache is not cleared during instantiate(), so a
    // callback answering from it hands back a view, not a multi-million-triangle copy, per placement
    // that names the mesh.
    std::function<bool(u64 meshId, const f32*& outPositions, u32& outVertexCount,
                       const u32*& outIndices, u32& outIndexCount)> localTrianglesFor;
    // Physics mesh shapes by mesh id, kept by the caller across calls (a streamer's batches) and released
    // by it; null gives each call its own cache, released at its end.
    std::unordered_map<u64, i32>* meshShapes = nullptr;

    // How far through the placement loop instantiate() is: called with (done, w.placements.size())
    // at least every 256th placement, and once more at the very end with done == total -- even for a
    // level under 256 placements, so a caller never has to guess whether "no call yet" means "not
    // started" or "nothing to report" for a small level. `done` counts every placement WALKED, class
    // placements included, because collision-body creation (the slow part this exists to report on)
    // is what the count is standing in for, and a host cannot know in advance which placements that
    // will be.
    //
    // A HOST CALLBACK for the reason every other one in this struct is: whether progress becomes a
    // loading bar, a log line, or nothing at all is UI policy this module has no business deciding.
    // No callback (the default) costs nothing beyond the `done`/`total` increment already needed for
    // the modulo test.
    std::function<void(usize done, usize total)> progress;
};

// A moving placement's body: `body` is the physics ABI handle of `entity`'s KINEMATIC body, whose
// origin is the entity's own position (see addPivotBoxBody), for driveKinematicBodies. "Moving" is a
// placement with an `anim` clip or any descendant of one.
struct AnimatedBody {
    scene::Entity entity = scene::kInvalidEntity;
    i32 body = -1;
};

// The result, in placement order. `entities` holds only the placements that produced an entity, so
// it is NOT indexed by placement -- use `placementIndex` to get back to the record.
struct LevelInstance {
    std::vector<scene::Entity> entities;
    // Parallel to `entities`: the index into OcWorldData::placements each one came from.
    std::vector<u32> placementIndex;
    // Parallel to `entities`: the body that placement created (static, or kinematic for an animated
    // placement), or -1 if it made none.
    std::vector<i32> entityBody;
    // The bodies that were actually created, in creation order, for a host that only needs to
    // remove them again. Equal to `entityBody` with the -1s dropped.
    std::vector<i32> bodies;
    // The placements with an `anim` clip -- and their colliding descendants, which move with them
    // through the hierarchy -- whose body was made kinematic, for the host to hand to
    // driveKinematicBodies once a frame. One that collides but could not be made kinematic keeps its
    // static body and is absent here.
    std::vector<AnimatedBody> animatedBodies;

    // Physics body creation, broken down by which fitter actually produced each body -- for a host's
    // own summary log line (Runtime/src/GameLevel.cpp), which is the only reason this is collected
    // here rather than left for a caller to re-derive: only this loop knows which path
    // (addStaticMeshBody or addStaticBoxBody) a given placement took and how many triangles a mesh
    // body got, and re-deriving either from `entityBody`/`bodies` alone is not possible.
    u32 meshBodyCount = 0;
    u32 boxBodyCount = 0;
    u64 meshTriangleCount = 0;       // summed across every triangle-mesh body, indexCount/3 each
    f64 bodyCreationSeconds = 0.0;   // wall time inside the collide-and-fit branch, steady_clock

    // How many DISTINCT physics mesh shapes this instantiation actually built (aver_phys_create_mesh_
    // shape calls that succeeded) -- see instantiate()'s own comment on the shared-shape path. A
    // placement whose mesh another placement already named reuses that shape rather than building a
    // second one, so `meshBodyCount - uniqueMeshShapeCount` is how many placements got a FREE mesh
    // body: no triangle scaling, no second BVH. Excludes the rare zero-scale fallback to
    // addStaticMeshBody's own baked path (below), which never touches a shape handle at all.
    u32 uniqueMeshShapeCount = 0;
};

// Creates one entity per placement in the process-global World.
//
// ORDER IS OBSERVABLE AND IS PRESERVED. Entity indices, material token values and physics body ids
// are all assigned sequentially by the order of the calls made here, and the render gates compare
// bit-exact images that depend on all three. This walks placements in file order and does, per
// placement, exactly what the two hosts did: create, add the mesh renderer, intern the material,
// bind it, then the body.
//
// A placement with an `anim` clip also gets a CAnimator (clip = fnv1a64 of the path) and, when it
// collides, a kinematic body recorded in LevelInstance::animatedBodies; so does each colliding
// descendant of one. `animspeed 0` means HELD at animtime (kAnimatorPaused): a CAnimator reads a speed
// of 0 as 1, so the authored zero is turned into the pause flag rather than played at full speed.
LevelInstance instantiate(const fmt::OcWorldData& w, const InstantiateOptions& opt = {});

// Moves each animated placement's kinematic body to its entity's CURRENT world transform, so what
// stands on it is carried by the next physics step. Call once a frame after the animation tick, with
// the frame time as `dt`. A no-op for stale entities and dead bodies, and without the physics module.
//
// A JUMP IS TELEPORTED, NOT DRIVEN: when reaching the pose since the last call would take more than
// 60 m/s (at the pivot or the far edge of a turning body), or `dt` is not positive, the body is set to
// the pose directly and left at rest. Driving it there would give it a velocity that flings a character
// standing on it -- a clip wrapping with a gap, a level reset, a script moving the entity.
//
// Returns how many bodies physics refused to drive because they are gone or are no longer kinematic. A
// host that keeps `bodies` between frames rebuilds the list when this is not 0; one that rebuilds it
// every frame can ignore it. An entry whose entity is gone is skipped and not counted: rebuilding the
// list from the host's own tables would find it again.
u32 driveKinematicBodies(scene::World& world, const std::vector<AnimatedBody>& bodies, f32 dt);

} // namespace aver::world

#endif // AVER_MODULE_SCENE
