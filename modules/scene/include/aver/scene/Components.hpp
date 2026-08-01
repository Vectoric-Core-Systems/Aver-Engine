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
inline constexpr u32 kComponentSkeletalMesh = 9;
inline constexpr u32 kComponentAnimator     = 10;
inline constexpr u32 kComponentBuiltinMax   = 10;

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

// Binds a skeleton to whatever this entity already draws, rather than being a second kind of mesh
// renderer: a skinned actor carries CMeshRenderer for the geometry and this for the rig.
//
// The ids are OPAQUE, exactly as CMeshRenderer::mesh is. Aver.Scene links Core and Assets and may
// not gain a Formats edge -- resolving one to a loaded .ocskel is a job for the tier above.
struct CSkeletalMesh {
    u64 skeleton  = 0;   // .ocskel ObjectId
    u32 boneCount = 0;   // filled in when the asset resolves; 0 until then
    u32 dirty     = 1;
};

// THE FLAGS ARE NEGATIVE, and that is not a style choice. World::addComponent hands back ZERO-FILLED
// storage, so a struct's default member initialisers never run for a component attached directly --
// only the framework's spawn path memcpys defaults over. A positive kAnimatorPlaying therefore meant
// an animator you attached and then watched do nothing. CMeshRenderer's visible bit has already cost
// this codebase that exact bug once. Zero now means playing, looping, at full weight.
inline constexpr u32 kAnimatorPaused = 0x1;
inline constexpr u32 kAnimatorOnce   = 0x2;   // clear = loop

// A clip and the clock running it. The clock is DATA rather than a hidden player object, so a script
// can scrub it, a save can restore it, and the editor can drive it without owning a second timeline.
struct CAnimator {
    u64 clip        = 0;      // .ocanim ObjectId
    f32 time        = 0.0f;   // seconds into the clip
    f32 speed       = 1.0f;   // 0 is read as 1; see AnimSystem::tick
    f32 blendWeight = 1.0f;   // 0 is read as 1, for the same reason
    u32 flags       = 0;
};

// Field order is chosen so neither struct gets padding: World::verifyComponent is byte-exact and
// turns a mismatch into an abort inside World's constructor, so a padded component kills the editor
// at startup rather than failing a test.
static_assert(sizeof(CSkeletalMesh) == 16, "CSkeletalMesh must be padding-free");
static_assert(sizeof(CAnimator) == 24, "CAnimator must be padding-free");

class World;

namespace detail {
// Registers the ten built-ins. Called once by World's constructor.
void registerBuiltinComponents(World& world);
} // namespace detail

} // namespace aver::scene
