// The eight built-in component structs and their fixed dense type ids.
#pragma once
#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"
#include "aver/scene/Entity.hpp"

namespace aver::scene {

// Fixed dense ids, registered before anything else runs. These match AVER_SCENE_COMP_* in scene_abi.h.
inline constexpr u32 kComponentLocal        = 1;
inline constexpr u32 kComponentWorld        = 2;
inline constexpr u32 kComponentHierarchy    = 3;
inline constexpr u32 kComponentName         = 4;
inline constexpr u32 kComponentTags         = 5;
inline constexpr u32 kComponentMeshRenderer = 6;
inline constexpr u32 kComponentLight        = 7;
inline constexpr u32 kComponentCamera       = 8;
inline constexpr u32 kComponentBuiltinMax   = 8;

// Authored local transform, plus the revision the world-matrix pass compares against.
struct CLocal {
    Transform xf;          // centimetres, +Z up, LEFT-handed, row-vector
    u32       rev = 1;
};

// Derived world matrix, plus the two revisions it was composed from. One pass writes all of it.
struct CWorld {
    Mat4 m;
    u32  composedLocalRev  = 0;
    u32  composedParentRev = 0;   // 0 == no parent
    u32  rev               = 1;
};

// Intrusive parent/child/sibling links and cached depth.
struct CHierarchy {
    Entity parent      = kInvalidEntity;
    Entity firstChild  = kInvalidEntity;
    Entity nextSibling = kInvalidEntity;
    Entity prevSibling = kInvalidEntity;
    u32    depth       = 0;
};

// Persisted identity: a hashed object id plus an {offset, len} slice into the world's name blob.
struct CName {
    u64 objectId = 0;
    u32 offset   = 0;
    u32 len      = 0;
};

// Uninterpreted tag bits. This module never reads a meaning into one.
struct CTags {
    u32 bits = 0;
};

// What to draw: an opaque mesh id, local bounds, and an opaque material token.
struct CMeshRenderer {
    u64 mesh        = 0;
    f32 aabbMin[3]  = {0, 0, 0};
    f32 aabbMax[3]  = {0, 0, 0};
    i32 material    = 0;
    u32 flags       = 1;   // bit 0: visible
    u32 dirty       = 1;   // upload bookkeeping, set by a write and cleared by the GPU path
};

inline constexpr u32 kMeshRendererVisible = 0x1;

// A light's kind, colour, brightness and cone shaping.
struct CLight {
    i32 kind          = 0;
    f32 colour[3]     = {1, 1, 1};
    f32 intensityLux  = 100000.0f;
    f32 rangeCm       = 0.0f;
    f32 innerCos      = 1.0f;
    f32 outerCos      = 0.7f;
};

// Perspective camera parameters, plus the priority that picks between cameras.
struct CCamera {
    f32 fovYRad = 1.0472f;
    f32 nearCm  = 5.0f;
    f32 farCm   = 500000.0f;
    i32 priority = 0;
};

class World;

namespace detail {
// Registers the eight built-ins. Called once by World's constructor.
void registerBuiltinComponents(World& world);
} // namespace detail

} // namespace aver::scene
