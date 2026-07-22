#pragma once
#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"
#include "aver/scene/Entity.hpp"

// The built-in component structs and their fixed dense type ids. They live in a header of their own
// rather than in World.hpp because Aver.Framework and the editor address these structs directly
// through the pools, and a consumer that only needs the layout should not have to include the world.
//
// Every one of them is registered through the same public API a script-declared component uses, so
// nothing about the built-ins is privileged beyond being registered first.
namespace aver::scene {

// Fixed and registered before anything else runs, so the generated C# constant file has something
// stable to assert against. These match the AVER_SCENE_COMP_* defines in scene_abi.h.
inline constexpr u32 kComponentLocal        = 1;
inline constexpr u32 kComponentWorld        = 2;
inline constexpr u32 kComponentHierarchy    = 3;
inline constexpr u32 kComponentName         = 4;
inline constexpr u32 kComponentTags         = 5;
inline constexpr u32 kComponentMeshRenderer = 6;
inline constexpr u32 kComponentLight        = 7;
inline constexpr u32 kComponentCamera       = 8;
inline constexpr u32 kComponentBuiltinMax   = 8;

// The split between these two is the point: CLocal is authored data the editor, the gizmo and
// scripts write; CWorld is derived data exactly one pass writes.
//
// `rev` is a revision counter rather than a dirty bit because a child's staleness is a question
// about its PARENT's last change, and a bit that has already been consumed by one reader cannot
// answer it. A revision never legally reads 0 once an entity exists; 0 is the "no parent" sentinel
// CWorld::composedParentRev carries for a root.
struct CLocal {
    Transform xf;          // Core's Transform: centimetres, +Z up, LEFT-handed, row-vector
    u32       rev = 1;
};

struct CWorld {
    Mat4 m;
    u32  composedLocalRev  = 0;
    u32  composedParentRev = 0;
    u32  rev               = 1;
};

// Intrusive links, so reparenting is four pointer writes and allocates nothing.
struct CHierarchy {
    Entity parent      = kInvalidEntity;
    Entity firstChild  = kInvalidEntity;
    Entity nextSibling = kInvalidEntity;
    Entity prevSibling = kInvalidEntity;
    u32    depth       = 0;
};

// Runtime identity is Entity and is NEVER serialised — a generational handle must not reach a file.
// Persisted identity is `objectId`, Assets' fnv1a64 of the name, the same kind of thing an asset id
// is. `offset`/`len` slice into the world's name blob.
struct CName {
    u64 objectId = 0;
    u32 offset   = 0;
    u32 len      = 0;
};

// Uninterpreted bits. This module never reads a meaning into one; the ownership table belongs to the
// gameplay layer, one module up.
struct CTags {
    u32 bits = 0;
};

// `mesh` is an opaque ObjectId and `material` an opaque i32 this module never dereferences. Naming
// pbr::MaterialHandle here would need aver/pbr/Material.hpp behind a Core+Assets link line — an
// undeclared edge — and the ABI already crosses the same value as int32_t.
//
// Member order is chosen so that the field table's trailing gap is smaller than the struct's
// alignment ONLY when every member is in the table: dropping `dirty` leaves eight unexplained bytes
// in a struct aligned to eight, which is exactly what ComponentBuilder::verify refuses.
struct CMeshRenderer {
    u64 mesh        = 0;
    f32 aabbMin[3]  = {0, 0, 0};
    f32 aabbMax[3]  = {0, 0, 0};
    i32 material    = 0;
    u32 flags       = 1;   // bit 0: visible
    u32 dirty       = 1;
};

inline constexpr u32 kMeshRendererVisible = 0x1;

struct CLight {
    i32 kind          = 0;
    f32 colour[3]     = {1, 1, 1};
    f32 intensityLux  = 100000.0f;
    f32 rangeCm       = 0.0f;
    f32 innerCos      = 1.0f;
    f32 outerCos      = 0.7f;
};

struct CCamera {
    f32 fovYRad = 1.0472f;
    f32 nearCm  = 5.0f;
    f32 farCm   = 500000.0f;
    i32 priority = 0;
};

class World;

namespace detail {
// Called once by World's constructor, before any caller can reach the registry, which is what makes
// the ids above constants rather than hopes.
void registerBuiltinComponents(World& world);
} // namespace detail

} // namespace aver::scene
