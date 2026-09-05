// The fifteen built-in component structs and their fixed dense type ids.
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
inline constexpr u32 kComponentRigidBody    = 14;
inline constexpr u32 kComponentJoint        = 15;
inline constexpr u32 kComponentBuiltinMax   = 15;

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
    u32 flags       = 1;   // bit 0: visible, bit 1: hidden from owner (see kMeshRendererHiddenFromOwner)
    u32 dirty       = 1;   // upload bookkeeping, set by a write and cleared by the GPU path
};

inline constexpr u32 kMeshRendererVisible = 0x1;

// DRAWN FOR EVERY CAMERA EXCEPT THE ONE BELONGING TO WHATEVER ENTITY OWNS THIS MESH -- what a
// first-person character needs for its own skinned body. A camera sitting at the character's eye
// position is a camera sitting INSIDE that character's mesh, so with nothing to opt out, every
// triangle the near plane is inside of fills the screen with its inward-facing surface: the
// FirstPerson template's whole-screen dark-red-brown fill this bit exists to fix. Third person
// pulls the camera outside the body by construction (a boom offset, not a flag), so the identical
// character keeps showing its body from any OTHER view -- a chase camera, a spectator, another
// player's screen -- with no second code path for "third person" to fall through, because nothing
// there ever matches "this mesh's own owner is the one looking".
//
// A ZERO-SAFE DEFAULT, unlike kMeshRendererVisible just above it. World::addComponent hands back
// zero-filled storage (see kAnimatorPaused's own comment on why that trap already cost this codebase
// a bug), and zero here means "not hidden from anyone" -- the only sane thing an unauthored mesh
// renderer can mean, and the same answer it already gave before this bit existed. Nothing needed to
// change about EnsureMeshRenderer/SetVisible's explicit `flags = 1` seed for that to stay true; this
// bit simply never needs seeding.
//
// OBEYED BY THE SCENE WALK, NOT BY WHETHER THE MESH DRAWS AT ALL -- see SandboxApp.cpp's own
// scene-entity pass (the owner-hide check beside its frustum/occlusion culls) and its submitShadowOnly
// helper, which is also what a culled-but-still-shadow-casting entity already goes through. A mesh
// hidden from its owner is submitted to Voxi's shadow cascades, GI voxelisation and RT geometry table
// exactly as if it had drawn -- ONLY the rasterised colour draw is skipped -- so a first-person
// character still throws its own shadow. Losing that would trade this bug for the "shadows are
// screen-space" one the culling fix already paid to close.
//
// SET FROM `COMP ... hidden=owner` in an .ocgraph's component tree (OcGraph.hpp's COMP record --
// see GraphComponentTree.cs's ApplyKind, which is the one place that turns the authored string into
// this bit) rather than a new component or a scene-ABI field of its own: the flag is exactly as
// per-mesh as kMeshRendererVisible already is, so it belongs on the SAME struct that already answers
// "does this mesh draw", not a second one an author has to remember to attach alongside it.
inline constexpr u32 kMeshRendererHiddenFromOwner = 0x2;

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

// What CRigidBody::shapeKind names. Box is 0 so a zero-filled, unauthored body still describes a
// real (if degenerate, all-zero half-extents) shape rather than an out-of-range one.
//
// CAPSULE AND CYLINDER ARE AUTHORING-ONLY TODAY: physics_abi.h has no aver_phys_add_dynamic_*
// entry point for either -- only box, sphere, convex hull, mesh and heightfield exist for a rigid
// body (a capsule elsewhere in that header is aver_phys_character_create's swept, non-rigid
// character). Whoever wires this component to body creation has to add those two entry points
// first; the schema is authored ahead of the ABI on purpose so a level can already say "this is a
// capsule" before the code that spawns one exists.
inline constexpr i32 kRigidBodyShapeBox      = 0;
inline constexpr i32 kRigidBodyShapeSphere   = 1;
inline constexpr i32 kRigidBodyShapeCapsule  = 2;
inline constexpr i32 kRigidBodyShapeCylinder = 3;

// What makes an entity a simulated physics body, authored in a level rather than built by a script.
//
// EVERY FIELD IS ZERO-DEFAULTED ON PURPOSE, for the same ComponentPool reason as CSoftBody just
// above: addComponent hands back zero-filled storage and this struct's in-class initialisers never
// run for it, so a field whose sane default is not zero is read through a helper (see
// rigidBodyFriction/rigidBodyGravityFactor below) rather than trusted to arrive already defaulted.
struct CRigidBody {
    // One of the kRigidBodyShape* constants above. Zero (box) is the shape an unauthored body gets.
    i32 shapeKind = kRigidBodyShapeBox;
    // Shape dimensions in CENTIMETRES, meaning fixed by shapeKind so one component covers all four
    // shapes without a field each kind mostly leaves unused:
    //   Box:      half-extents (x, y, z) -- matches aver_phys_add_dynamic_box's hx/hy/hz.
    //   Sphere:   dims[0] = radius; dims[1], dims[2] unused (leave 0).
    //   Capsule:  dims[0] = radius, dims[1] = half-height of the CYLINDRICAL portion only, NOT
    //             including the two hemispherical caps -- JPH::CapsuleShape's own constructor
    //             argument, and deliberately unlike aver_phys_character_create's `height`, which is
    //             the total capsule height including both caps. dims[2] unused.
    //   Cylinder: dims[0] = radius, dims[1] = half-height. dims[2] unused.
    f32 dims[3] = {0, 0, 0};
    // AVER_PHYS_MOTION_STATIC/KINEMATIC/DYNAMIC from physics_abi.h, in that same order because it in
    // turn matches JPH::EMotionType's declared order -- mirrored here as raw i32 rather than by
    // including the physics header, exactly as CLight/CCamera never reach into the RHI for their own
    // enums. Zero (static) is the zero-safe default: an unauthored body sits still instead of a
    // dynamic body silently missing the mass/shape setup a real spawn path would have given it.
    i32 motionType = 0;
    // Kilograms. <= 0 means "derive mass from the shape's volume", matching
    // aver_phys_add_dynamic_box's own massKg parameter -- so zero is already the right unauthored
    // default and needs no substitution helper, unlike friction and gravityFactor below.
    f32 massKg = 0.0f;
    // 0 is frictionless ice, 1 is roughly rubber; Jolt's own default is 0.2, NOT 0. Zero here means
    // "the author did not say" and reads as kRigidBodyDefaultFriction through rigidBodyFriction(),
    // not as an explicit choice of frictionless -- the same limitation CSoftBody::maxDistanceCm
    // already accepts for the identical reason (a zero-filled component cannot tell "unset" from
    // "the real value happens to be zero" apart).
    f32 friction = 0.0f;
    // 0 keeps none of the approach speed, 1 (in theory) keeps all of it. Jolt's own default is 0, so
    // this field genuinely means what a zero-filled component says -- no helper needed.
    f32 restitution = 0.0f;
    // Scales world gravity for this body: 1 is normal, 0 floats, negative falls up. Jolt's (and this
    // ABI's) own default is 1, NOT 0, so zero here means "unset" and reads as
    // kRigidBodyDefaultGravityFactor through rigidBodyGravityFactor() -- the same accepted ambiguity
    // as friction above: an author who wants a genuinely weightless body cannot spell exactly 0.
    f32 gravityFactor = 0.0f;
    // Velocity bleed per second, linear and angular independently. 0 is a vacuum, which is both the
    // sane default and Jolt's own -- no helper needed, unlike friction/gravityFactor above.
    f32 linearDamping  = 0.0f;
    f32 angularDamping = 0.0f;
    // Which of physics_layers_abi.h's AVER_PHYS_LAYER_COUNT (16) layers this body collides as. 0 is
    // the default layer, which collides with everything -- the same "nothing changed" answer a body
    // never touching this field already got before collision layers existed.
    i32 collisionLayer = 0;
    // Spelled i32, not bool: FieldKind::Bool reads and writes a 4-byte int (see SceneAbi.cpp's
    // aver_scene_get_i32/set_i32, which serve Bool exactly as I32), and a real 1-byte bool field
    // would misalign every offset after it without verify() necessarily catching a struct that still
    // happened to come out padding-free. Zero (not a sensor) is the sane default: a sensor is a
    // deliberate choice a level author opts into, matching aver_phys_add_sensor_box/_sphere being
    // separate calls from the ordinary dynamic/static ones rather than the default.
    i32 isSensor = 0;
    // The physics handle, filled in at run time by whatever creates the body from this component.
    // PROCESS-LOCAL and therefore never serialised -- see the read-only marker in Builtins.cpp,
    // which is what keeps it out of a chunk, for the identical reason as CSoftBody::body.
    i32 body = 0;
};

// Zero means "the author did not say", not "explicitly frictionless" -- see CRigidBody::friction.
inline constexpr f32 kRigidBodyDefaultFriction = 0.2f;   // Jolt's own default.
inline f32 rigidBodyFriction(const CRigidBody& rb) {
    return rb.friction > 0.0f ? rb.friction : kRigidBodyDefaultFriction;
}

// Zero means "the author did not say", not "explicitly weightless" -- see CRigidBody::gravityFactor.
// Compared against 1 rather than substituted whenever it is exactly 0, because 0 is itself a
// meaningful (if presently inexpressible-as-authored) answer and 1 is the only OTHER value that
// would silently arrive from a zero-filled component with nobody having asked for it.
inline constexpr f32 kRigidBodyDefaultGravityFactor = 1.0f;
inline f32 rigidBodyGravityFactor(const CRigidBody& rb) {
    return rb.gravityFactor != 0.0f ? rb.gravityFactor : kRigidBodyDefaultGravityFactor;
}

// What CJoint::jointType names -- the six constraint shapes a level author reaches for. Jolt (and
// physics_joints_abi.h) has twelve; gear, rack-and-pinion, pulley, path, six-DOF and swing-twist
// stay code-only, built by a script against the existing aver_phys_joint_* ABI rather than authored
// on a component, because each needs either a second body's own joint to already exist (gear,
// rack-and-pinion) or an array argument (six-DOF's twelve limits, path's polyline) a fixed-stride
// component cannot carry.
inline constexpr i32 kJointTypeFixed    = 0;
inline constexpr i32 kJointTypePoint    = 1;
inline constexpr i32 kJointTypeDistance = 2;
inline constexpr i32 kJointTypeHinge    = 3;
inline constexpr i32 kJointTypeSlider   = 4;
inline constexpr i32 kJointTypeCone     = 5;

// Mirrors AVER_PHYS_MOTOR_OFF/VELOCITY/POSITION from physics_joints_abi.h, as raw i32 rather than by
// including the physics header -- the same choice CRigidBody::motionType makes for
// AVER_PHYS_MOTION_*, for the same reason: Aver.Scene has no dependency on Aver.Physics.
inline constexpr i32 kJointMotorOff      = 0;
inline constexpr i32 kJointMotorVelocity = 1;
inline constexpr i32 kJointMotorPosition = 2;

// A constraint anchored ON THIS ENTITY, holding it against another entity or against the world.
//
// EVERY FIELD IS ZERO-DEFAULTED ON PURPOSE, for the identical ComponentPool reason as CSoftBody and
// CRigidBody above.
struct CJoint {
    // One of the kJointType* constants above.
    i32 jointType = kJointTypeFixed;
    // The entity at the OTHER end of the constraint. ZERO MEANS THE WORLD -- matching
    // physics_joints_abi.h's AVER_PHYS_WORLD_BODY, which is also 0, and matching kInvalidEntity
    // (also 0): a joint an author never set a target on constrains its entity to an immovable frame
    // rather than silently resolving to some other entity 0 might otherwise mean. That is how a
    // door hangs on a wall that is not itself simulated, and it is the answer a zero-filled,
    // unauthored CJoint already gives for free.
    Entity otherEntity = kInvalidEntity;
    // The anchor point, as three f32 in THIS ENTITY'S LOCAL SPACE -- not world space. Every
    // aver_phys_joint_* creator wants a world-space point at the moment of creation, but a joint
    // authored in a level has to survive the level being moved, rotated, or the entity being
    // instanced somewhere else entirely; only a local-space anchor does that. Whatever spawns the
    // physics joint from this component multiplies this by the entity's CWorld first.
    f32 anchorLocal[3] = {0, 0, 0};
    // The constraint's primary axis, entity-local, meaning fixed by jointType: axisX for fixed,
    // hingeAxis for hinge, sliderAxis for slider, twistAxis for cone. Unused (and left zero) for
    // point and distance, which have no axis to orient.
    f32 primaryAxis[3] = {0, 0, 0};
    // The constraint's secondary/reference axis, entity-local: axisY for fixed, normalAxis for hinge
    // and slider, perpendicular to primaryAxis in both cases. Unused for point, distance and cone.
    f32 normalAxis[3] = {0, 0, 0};
    // Meaning fixed by jointType, matching each constraint's own creator in physics_joints_abi.h:
    //   Distance: minDistanceCm / maxDistanceCm, centimetres. A negative max there means "whatever
    //             they are apart right now"; that convention is the joint CREATOR's, not this
    //             field's, so it is preserved here unchanged rather than reinterpreted.
    //   Hinge:    minAngleRad / maxAngleRad, radians. -PI/+PI (or wider) means unlimited.
    //   Slider:   minCm / maxCm, centimetres.
    //   Cone:     only limitMax is read, as halfConeAngleRad; limitMin is unused.
    //   Fixed, Point: both unused -- neither constraint has a limit to set.
    f32 limitMin = 0.0f;
    f32 limitMax = 0.0f;
    // One of the kJointMotor* constants. HINGE AND SLIDER ONLY, matching
    // aver_phys_joint_set_motor's own restriction -- ignored on fixed/point/distance/cone, which
    // have no motor in Jolt. Zero (off) is the zero-safe default: an unauthored joint just
    // constrains, exactly as it did before motors existed.
    i32 motorState = kJointMotorOff;
    // Radians (or radians/second) for a hinge, centimetres (or cm/s) for a slider -- a target ANGLE
    // or OFFSET under Position, a target SPEED under Velocity, matching aver_phys_joint_set_motor.
    f32 motorTarget = 0.0f;
    // The physics handle, filled in at run time by whatever creates the joint from this component.
    // PROCESS-LOCAL and therefore never serialised, for the identical reason as CRigidBody::body and
    // CSoftBody::body -- see the read-only marker in Builtins.cpp.
    i32 joint = 0;
};

// Field order is chosen so neither struct gets padding: World::verifyComponent is byte-exact and
// turns a mismatch into an abort inside World's constructor, so a padded component kills the editor
// at startup rather than failing a test.
static_assert(sizeof(CAttachment) == 8, "CAttachment must be padding-free");
static_assert(sizeof(CSoftBody) == 16, "CSoftBody must be padding-free");
static_assert(sizeof(CSkeletalMesh) == 16, "CSkeletalMesh must be padding-free");
static_assert(sizeof(CAnimator) == 24, "CAnimator must be padding-free");
static_assert(sizeof(CParticleEmitter) == 24, "CParticleEmitter must be padding-free");
static_assert(sizeof(CRigidBody) == 56, "CRigidBody must be padding-free");
static_assert(sizeof(CJoint) == 64, "CJoint must be padding-free");

class World;

namespace detail {
// Registers the fifteen built-ins. Called once by World's constructor.
void registerBuiltinComponents(World& world);
} // namespace detail

} // namespace aver::scene
