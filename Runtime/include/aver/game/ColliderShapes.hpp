// ColliderShapes: an .ocshape tree -> a physics shape handle, and the CollisionShape component that gives a rigid body one.
// Format: docs/PHYSICS_SHAPES.md. Children build first; mesh leaves are built at load, not cooked.
// Only the editor's Play builds shapes (sandbox PhysicsSceneSync), as for CRigidBody; the packaged game gets them through C# (Aver.Physics.CollisionShape).
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

struct ColliderMesh {
    std::vector<f32> vertices;     // xyz, engine centimetres, mesh-local
    std::vector<i32> indices;
    std::vector<u8>  triPalette;   // one palette index per triangle; empty = all 0
    std::vector<i32> palette;      // surface ids (0 = none), at most 32; empty = the mesh names no surface
};

struct ColliderMeshSource {
    // Collision mesh for a content path, with its slot surfaces; false when the path names no mesh.
    std::function<bool(const std::string& contentPath, ColliderMesh& out)> mesh;
};

// On success `outShape` is the root handle, owned by the caller (release with aver_phys_shape_release). On failure `why` names the
// node and nothing stays allocated. `spec` must pass fmt::validateOcShape.
bool buildColliderShape(const fmt::OcShapeData& spec, const ColliderMeshSource& meshes, i32& outShape, std::string& why);

struct ColliderShapeHost {
    std::function<bool(u64 assetId, std::string& text, std::string& label)> readShape;   // label names the file in a log line
    ColliderMeshSource meshes;
};
// Process-wide. The host sets it before Play builds bodies (the editor) and clears it with an empty one.
void setColliderShapeHost(ColliderShapeHost host);
const ColliderShapeHost& colliderShapeHost();

// `pathOfId` maps an asset id to its content-relative path ("" when unknown). Mesh leaves resolve their path to the id fnv1a64 gives it.
ColliderShapeHost makeColliderShapeHost(GameContent& content, std::string contentDir, std::function<std::string(u64)> pathOfId);

inline constexpr u32 kCollisionShapeIgnoreScale = 0x1;   // negative sense: zero-filled applies the entity's world scale

struct CCollisionShape {                // "CollisionShape"
    u64 asset = 0;                      // .ocshape ObjectId; 0 = none (CRigidBody's own shape stands)
    i32 surface = 0;                    // body label: aver::surfaceIdFromName of the name; 0 = none
    u32 flags = 0;
    // The fields from outShape down are OUT: written at runtime, never authored.
    i32 outShape = 0;                   // the shape handle the body was built from (released when the body is)
    f32 outMassKg = 0;
    f32 outVolumeCm3 = 0;
    u32 reserved = 0;
};
static_assert(sizeof(CCollisionShape) == 32, "CCollisionShape must be padding-free");

} // namespace aver::game

#if AVER_MODULE_SCENE
#  include "aver/scene/World.hpp"

namespace aver::game {

// Idempotent. GameSystemsWiring calls it with the other runtime components.
u32 registerCollisionShapeComponent(scene::World& world);
u32 collisionShapeType(scene::World& world);

// COMPONENT CollisionShape asset=Physics/Chair.ocshape surface=Wood motion=dynamic noscale=1
//   asset: content path, or `asset#` + 16 hex digits when the id could not be turned back into a path; absent = none
//   surface: a surface name, or `#<hex id>`; absent = none
//   motion: static | kinematic | dynamic; absent = static. Level files carry no CRigidBody, so only this key makes a non-static shape.
//   noscale=1 ignores the entity's world scale. Mass, friction and the rest stay on the CRigidBody.
// Adds the component, and a CRigidBody when the entity has none. False with `why` set on a malformed key. OUT fields are untouched.
bool applyCollisionShape(scene::World& w, scene::Entity e, const fmt::OcPlacedComponent& r, std::string& why);
// Keys at their default are omitted; nullopt when the entity has no CollisionShape. OUT fields are never read.
std::optional<fmt::OcPlacedComponent> captureCollisionShape(scene::World& w, scene::Entity e, const std::function<std::string(u64)>& pathOfId);
// Also removes the CRigidBody that applyCollisionShape attached, if it is still the unusable zero-size box with no live body, so
// Add Component > Collision Shape and its undo leave nothing behind. False when the entity has no CollisionShape.
bool removeCollisionShape(scene::World& w, scene::Entity e);

} // namespace aver::game
#endif // AVER_MODULE_SCENE
