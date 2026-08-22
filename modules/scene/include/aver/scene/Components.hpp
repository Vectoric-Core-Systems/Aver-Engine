// The twelve built-in component structs and their fixed dense type ids.
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
inline constexpr u32 kComponentParticleEmitter = 11;
inline constexpr u32 kComponentAttachment   = 12;
inline constexpr u32 kComponentSoftBody     = 13;
inline constexpr u32 kComponentBuiltinMax   = 13;

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

// THE FLAGS ARE NEGATIVE AGAIN, for the identical reason CAnimator's are: World::addComponent hands
// back zero-filled storage when a CParticleEmitter is attached directly (not through the framework's
// spawn path), so a POSITIVE "playing" bit would attach a component that silently does nothing --
// exactly CMeshRenderer's visible-bit mistake, repeated. Zero now means "emitting".
inline constexpr u32 kParticleEmitterStopped = 0x1;

// One emitter INSTANCE: an opaque effect id plus this entity's own playback clock. The effect's
// actual parameters -- emission shape and rate, lifetime, velocity, gravity, damping, size and
// colour over life, blend mode -- are NOT here; they are shared, authored data an effect id
// resolves to (DECIDED 3: a .ocparticle asset, one per EFFECT, read by a tier above modules/scene --
// see CSkeletalMesh's own comment on why an opaque id and not a loaded asset lives on the
// component). Two emitters can point at the same effect and each keeps its own clock, exactly as
// two CAnimator entities can share one clip.
//
// Everything belonging to a LIVE particle -- its position, velocity, age -- is deliberately NOT
// here either: a component has a fixed stride, and an emitter's particle count varies and can be in
// the thousands, so that state lives in aver::particles::ParticleSystem's own side table, keyed by
// Entity, the same way AnimSystem keeps posed skeletons OUT of CAnimator (see AnimSystem.hpp's
// `posed_` and its comment on why).
struct CParticleEmitter {
    u64 effect    = 0;      // .ocparticle ObjectId (opaque; see CAnimator::clip for the idiom)
    f32 age       = 0.0f;   // seconds since this emitter last transitioned stopped -> playing
    f32 emitAccum = 0.0f;   // fractional particles owed by emissionRate*dt, carried frame to frame
    u32 seed      = 0;      // this emitter's RNG stream; 0 = "not yet assigned" (a system picks one)
    u32 flags     = 0;      // bit 0: kParticleEmitterStopped
};

// RIDING A SOCKET on this entity's PARENT.
//
// It carries the socket and nothing else -- no target entity -- because the target is already
// answered: the parent link in CHierarchy is what "attached to" means, and SetParent is the call
// every script and every graph already has. A second, private notion of "who am I on" would be a
// second thing to keep in step with the hierarchy, and they would drift.
//
// WHAT THE UPDATE ACTUALLY WRITES is this entity's own CLocal, from the socket's transform in the
// parent's MODEL space. The hierarchy pass then composes CWorld = CLocal * parentWorld, which is
// exactly parentWorld * socketModel -- the right answer, produced by machinery that already exists
// rather than by a second world-space path that could disagree with the first.
//
// A HASH, NOT A NAME, and that is a constraint rather than a preference: a component has a fixed
// stride and the scene ABI marshals numbers, so the socket is named by fnv1a64 exactly as a mesh
// names its asset by ObjectId. C# hashes with Assets.ObjectIdOf, which is the same function.
struct CAttachment {
    u64 socket = 0;    // fnv1a64 of the socket name; 0 = attached to nothing in particular
};

// Marks an entity's mesh as SIMULATED RATHER THAN POSED: its vertices come from a Jolt soft body
// instead of from bone skinning, so it sags, drapes, squashes and collides with the world.
//
// THE MESH IS THE SIMULATION. There is no separate collision proxy to author and no cage asset: the
// entity's own CMeshRenderer mesh supplies both the particles (its vertices) and the constraints
// (its triangles). That is a real limit as well as a convenience -- a 50k-vertex mesh is a
// 50k-particle soft body and will not run -- and it is stated here because nothing downstream
// enforces a budget.
//
// EVERY FIELD IS ZERO-DEFAULTED ON PURPOSE. World::addComponent hands back ZERO-FILLED storage and a
// struct's in-class initialisers never run for a component attached directly (see the flags note
// above, which this codebase has already paid for twice), so a value that must not be zero is read
// through a helper that substitutes its default rather than being written here and silently lost.
struct CSoftBody {
    // How far a vertex may leave where ordinary skinning would have put it, in centimetres. It is
    // THE dial: 0 is indistinguishable from skinning, large is a free-floating cloth that happens to
    // hang near a skeleton, and jiggle lives in between. Zero here means "unset" and reads as
    // kSoftBodyDefaultMaxDistanceCm, not as "pinned" -- see softBodyMaxDistanceCm().
    f32 maxDistanceCm = 0.0f;
    // Inverse stiffness of the edge constraints. 0 is inextensible, which is also the sane default,
    // so this one field genuinely means what a zero-filled component says it means.
    f32 compliance = 0.0f;
    // The physics handle, filled in by whoever creates the body. PROCESS-LOCAL and therefore never
    // serialised -- see the read-only marker in Builtins.cpp, which is what keeps it out of a chunk.
    i32 body = 0;
    u32 flags = 0;
};

// Zero means "the author did not say", not "zero centimetres". A component that arrives zero-filled
// cannot distinguish those two, so the substitution happens HERE, in one place both the renderer and
// any future editor read through, rather than being re-derived at each call site.
inline constexpr f32 kSoftBodyDefaultMaxDistanceCm = 8.0f;
inline f32 softBodyMaxDistanceCm(const CSoftBody& sb) {
    return sb.maxDistanceCm > 0.0f ? sb.maxDistanceCm : kSoftBodyDefaultMaxDistanceCm;
}

// NEGATIVE-SENSE, for the reason spelled out above kAnimatorPaused: zero-filled storage must mean
// the useful state. Clear = simulated and drawn from the simulation.
inline constexpr u32 kSoftBodyDisabled = 0x1;

// Field order is chosen so neither struct gets padding: World::verifyComponent is byte-exact and
// turns a mismatch into an abort inside World's constructor, so a padded component kills the editor
// at startup rather than failing a test.
static_assert(sizeof(CAttachment) == 8, "CAttachment must be padding-free");
static_assert(sizeof(CSoftBody) == 16, "CSoftBody must be padding-free");
static_assert(sizeof(CSkeletalMesh) == 16, "CSkeletalMesh must be padding-free");
static_assert(sizeof(CAnimator) == 24, "CAnimator must be padding-free");
static_assert(sizeof(CParticleEmitter) == 24, "CParticleEmitter must be padding-free");

class World;

namespace detail {
// Registers the twelve built-ins. Called once by World's constructor.
void registerBuiltinComponents(World& world);
} // namespace detail

} // namespace aver::scene
