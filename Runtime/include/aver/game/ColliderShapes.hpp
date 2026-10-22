// ColliderShapes: an .ocshape tree -> a physics shape handle, and the CollisionShape component that gives a rigid body one.
// SHAPES_MATERIALS phase E. docs/PHYSICS_SHAPES.md.
//
// buildColliderShape walks a parsed tree (modules/formats OcShape.hpp) children first and issues the builder calls of
// physics_shape_builder_abi.h: a leaf becomes aver_phys_shape_box / _capsule / ... / _mesh, a decorator _rotated_translated / _offset_com /
// _scaled, a compound _compound. A leaf that names no surface takes the file's default; a mesh leaf's slot surfaces win over it and the
// default fills the slots that name none. Every handle made on the way is released except the root's, which the caller owns (a shape
// stays alive while a body uses it, so releasing it right after aver_phys_body_from_shape is correct).
//
// `hull mesh` and `mesh` leaves read geometry through the ColliderMeshSource seam: GameContent::collisionMeshFor in a host
// (makeColliderShapeHost), a hand-built cube in a test. They are built at load, not cooked.
//
// CCollisionShape is the runtime-registered companion of CRigidBody (the CMotionMatcher / CCloth pattern: no Components.hpp growth, no
// scene-ABI change). 32 bytes, zero-filled storage is valid. The motion type, mass, friction, restitution, gravity, damping and layer
// still come from the CRigidBody; the shape comes from the .ocshape. THE EDITOR'S PLAY BUILDS IT (sandbox PhysicsSceneSync) and only the
// editor, as CRigidBody is; the packaged game gets shapes through C# (Aver.Physics.CollisionShape).
//
// Without the physics module every builder returns false; without the scene module the component half is absent.
#pragma once
#include "aver/core/Types.hpp"
#include "aver/formats/OcShape.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace aver::fmt { struct OcPlacedComponent; }

namespace aver::game {

class GameContent;

// ---- the mesh seam -----------------------------------------------------------------------------------------------------------

struct ColliderMesh {
    std::vector<f32> vertices;     // xyz, engine centimetres, mesh-local
    std::vector<i32> indices;      // three per triangle
    std::vector<u8>  triPalette;   // one palette index per triangle; empty = all 0
    std::vector<i32> palette;      // surface ids (0 = none), at most 32; empty = the mesh names no surface
};

struct ColliderMeshSource {
    // The collision mesh a content path names ("Meshes/rock.ocmesh"), with its slot surfaces. False when the path names no mesh.
    std::function<bool(const std::string& contentPath, ColliderMesh& out)> mesh;
};

// ---- the walker --------------------------------------------------------------------------------------------------------------

// Builds the shape `spec` describes. On success `outShape` is the root's handle (release it with aver_phys_shape_release). On failure
// `why` says which node and what the physics module said, and nothing is left allocated. `spec` must pass fmt::validateOcShape.
bool buildColliderShape(const fmt::OcShapeData& spec, const ColliderMeshSource& meshes, i32& outShape, std::string& why);

// ---- the host ------------------------------------------------------------------------------------------------------------------

// What a host gives the component builder: the text of an .ocshape by asset id, and the mesh seam.
struct ColliderShapeHost {
    std::function<bool(u64 assetId, std::string& text, std::string& label)> readShape;   // label names the file in a log line
    ColliderMeshSource meshes;
};
// One process-wide host, set by the host before Play builds bodies (the editor) and cleared with an empty one.
void setColliderShapeHost(ColliderShapeHost host);
const ColliderShapeHost& colliderShapeHost();

// A host over GameContent: `pathOfId` maps an asset id to its content-relative path ("" when unknown), `contentDir` roots it. Mesh leaves
// resolve their content path to the id fnv1a64 gives it and ask content.collisionMeshFor.
ColliderShapeHost makeColliderShapeHost(GameContent& content, std::string contentDir, std::function<std::string(u64)> pathOfId);

// ---- the component -------------------------------------------------------------------------------------------------------------

inline constexpr u32 kCollisionShapeIgnoreScale = 0x1;   // negative sense: zero-filled applies the entity's world scale

struct CCollisionShape {                // "CollisionShape"
    u64 asset = 0;                      //  0 .ocshape ObjectId; 0 = none (CRigidBody's own shape stands)
    i32 surface = 0;                    //  8 body label: aver::surfaceIdFromName of the name; 0 = none
    u32 flags = 0;                      // 12
    i32 outShape = 0;                   // 16 OUT: the shape handle the body was built from (released when the body is)
    f32 outMassKg = 0;                  // 20 OUT
    f32 outVolumeCm3 = 0;               // 24 OUT
    u32 reserved = 0;                   // 28
};
static_assert(sizeof(CCollisionShape) == 32, "CCollisionShape must be padding-free");

} // namespace aver::game

#if AVER_MODULE_SCENE
#  include "aver/scene/World.hpp"

namespace aver::game {

// Registers CCollisionShape (idempotent) and returns its type id. GameSystemsWiring calls it with the other runtime components.
u32 registerCollisionShapeComponent(scene::World& world);
// The type id, 0 when it has not been registered.
u32 collisionShapeType(scene::World& world);

// COMPONENT CollisionShape asset=Physics/Chair.ocshape surface=Wood motion=dynamic noscale=1
//   asset    content-relative path (hashed with fnv1a64), or `asset#` holding the 16 hex digits of an id the editor could not turn back
//            into a path; absent = none
//   surface  a surface NAME, or `#<hex id>` for an id the table does not know; absent = none
//   motion   `static` (the default), `kinematic` or `dynamic`: the motion type of the CRigidBody the placement gets. No level file carries a
//            CRigidBody, so without this key a level could only make static shapes. Mass (the .ocshape MASS), friction and the rest stay on
//            the CRigidBody. Read, it sets the body's motion type (also on a body the entity already had) and an ABSENT key means static,
//            like every canonical default; on capture it is written when the entity's body is not static.
//   noscale  1 = ignore the entity's world scale
// Adds the component, and a CRigidBody when the entity has none. False with `why` set on a malformed key. Runtime OUT fields are untouched.
bool applyCollisionShape(scene::World& w, scene::Entity e, const fmt::OcPlacedComponent& r, std::string& why);
// The authored state as a record, keys at their default omitted; nullopt when the entity carries no CollisionShape. The OUT fields are never read.
std::optional<fmt::OcPlacedComponent> captureCollisionShape(scene::World& w, scene::Entity e, const std::function<std::string(u64)>& pathOfId);
// Removes the component and, with it, the Rigid Body applyCollisionShape attached when that body is still the unusable zero-size box it
// attached (no live body, no size): Add Component > Collision Shape and its undo leave nothing behind. False when the entity has no CollisionShape.
bool removeCollisionShape(scene::World& w, scene::Entity e);

} // namespace aver::game
#endif // AVER_MODULE_SCENE
