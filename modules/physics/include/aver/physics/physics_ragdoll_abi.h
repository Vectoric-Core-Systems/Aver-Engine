#pragma once
// Aver.Physics -- RAGDOLLS: a skeleton of capsule parts joined by swing-twist limits, built from a pose and
// then either animated (kinematic), left to fall (limp) or held toward a pose by motors (powered). Jolt's
// Ragdoll behind the same plain-C ABI and handle convention as physics_abi.h; its own header for the reason
// physics_joints_abi.h and physics_vehicle_abi.h are.
//
// UNITS AND AXES ARE THE ENGINE'S, exactly as in physics_abi.h: centimetres, kilograms, DEGREES for the
// joint limits, +X forward, +Y right, +Z up, left-handed, quaternions (x, y, z, w). No Jolt type crosses
// this boundary. 0 is always an invalid handle; a setter returns 1/0.
//
// EVERY POSE IS PER BONE AND IN WORLD SPACE, bones ordered parents before children (parents[i] < i, or -1).
// That is the shape a skinned character's evaluated pose already has, so a caller hands the animation
// system's output straight in and reads the physics result straight back.
//
// A BONE IS NOT A PART. Only a bone with a capsule radius > 0 gets a rigid body; the others (a twist
// helper, a finger tip, a socket) ride rigidly on their nearest ancestor that has one and have no body of
// their own. Joints, limits and masses are therefore defined over the REDUCED skeleton of parts, and the
// joint of a part connects it to the nearest ancestor part. A bone with no part and no ancestor part has
// nothing to ride and keeps its target pose.
//
// EVERY PART IS AN ORDINARY BODY HANDLE (drawn from the same counter as aver_phys_add_*): raycasts hit it,
// contacts report it, aver_phys_set_entity stamps it. Parts of one ragdoll never collide with each other;
// they collide with the world per the user layer given at creation (physics_layers_abi.h).
//
// THREE MODES.
//   KINEMATIC  the parts are moved to the target pose every fixed step (a velocity that reaches it, so
//              they push dynamic bodies instead of tunnelling through them). Joints are switched off.
//              This is what a new ragdoll starts in, with the creation pose as the target.
//   LIMP       the parts are simulated and nothing drives them: they fall and fold at their limits.
//              A switch from KINEMATIC carries the motion the parts had, so an animated body keeps its
//              momentum into the fall.
//   POWERED    simulated, and each joint's motor pulls the part toward its orientation in the target
//              pose RELATIVE TO ITS PARENT PART. The root part is not driven: the pose is held, the body
//              is free to move, so a caller that wants it anchored joins the root to something
//              (physics_joints_abi.h) or sets the target each frame from where the root has gone.
//
// WHICH SWING IS WHICH. A joint's twist axis is the part's capsule segment (from a to b, or the bone's
// local X for a sphere). swing1 is the half-angle of the swing toward the bone's local +Z (toward +Y when
// the segment itself lies along Z); swing2 is the half-angle toward the other perpendicular. All three
// limits are symmetric half-angles in degrees, clamped to [0, 180].
//
// TEARDOWN. aver_phys_ragdoll_destroy removes the ragdoll and every part. Removing ANY part with
// aver_phys_remove_body removes the whole ragdoll first, so the other parts' joints never outlive them;
// that remove_body call then finds its own handle already gone and returns 0. aver_phys_shutdown destroys
// every ragdoll before it destroys the world. A joint YOU made on a part (physics_joints_abi.h) must be
// removed before the ragdoll, as for any body.
#include <stdint.h>

#if defined(_WIN32)
#  if defined(AVER_PHYS_BUILD)
#    define AVER_PHYS_API __declspec(dllexport)
#  else
#    define AVER_PHYS_API __declspec(dllimport)
#  endif
#else
#  define AVER_PHYS_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---- Modes -------------------------------------------------------------------------------------------

#define AVER_PHYS_RAGDOLL_KINEMATIC 0
#define AVER_PHYS_RAGDOLL_LIMP      1
#define AVER_PHYS_RAGDOLL_POWERED   2

// ---- Lifetime ----------------------------------------------------------------------------------------

// Builds a ragdoll in the given pose and returns its handle, or 0 (aver_phys_last_error says why: no
// world, a non-finite or zero-length value, a parent that does not precede its child, no bone with a
// part, the body limit).
//
//   parents     boneCount ints: the parent bone's index, or -1 for a root.
//   worldPos    3 floats per bone (cm); worldRot 4 per bone (xyzw): the pose to build in.
//   capsules    7 floats per bone: ax ay az bx by bz radius -- a segment in the BONE's local frame (cm).
//               radius <= 0 means the bone has no part. a == b is a sphere.
//   limitsDeg   3 floats per bone: swing1 swing2 twist, the half-angles of the joint to the nearest
//               ancestor part. Ignored for a bone without a part and for a root part.
//   totalMassKg shared between the parts in proportion to the volume of their capsules.
//   userLayer   the collision layer of every part, 0 .. 15.
AVER_PHYS_API int32_t aver_phys_ragdoll_create(int32_t boneCount, const int32_t* parents,
                                               const float* worldPos, const float* worldRot,
                                               const float* capsules, const float* limitsDeg,
                                               float totalMassKg, uint32_t userLayer);

// Removes the ragdoll and all of its parts. Returns 0 for a dead handle.
AVER_PHYS_API int32_t aver_phys_ragdoll_destroy(int32_t ragdoll);

// How many ragdolls are live.
AVER_PHYS_API int32_t aver_phys_ragdoll_count(void);

// The bone count it was created with (not the part count), or 0 for a dead handle.
AVER_PHYS_API int32_t aver_phys_ragdoll_bone_count(int32_t ragdoll);

// ---- Driving ------------------------------------------------------------------------------------------

// One of the AVER_PHYS_RAGDOLL_* modes. Returns 0 for a dead handle or an unknown mode.
AVER_PHYS_API int32_t aver_phys_ragdoll_set_mode(int32_t ragdoll, int32_t mode);

// The current mode, or -1 for a dead handle.
AVER_PHYS_API int32_t aver_phys_ragdoll_mode(int32_t ragdoll);

// How hard POWERED mode pulls toward the target, 0 (no pull) to 1 (stiff); default 0.5. It scales the
// motors' spring frequency and their torque ceiling, which is itself relative to what each joint must
// carry, so the same value means the same thing for a child and a giant. Out of range is clamped.
AVER_PHYS_API int32_t aver_phys_ragdoll_set_strength(int32_t ragdoll, float strength);

// The pose KINEMATIC and POWERED modes aim for, 3n + 4n floats as at creation, read from the next step.
// Also fixes where a bone WITHOUT a part sits relative to the part it rides on.
AVER_PHYS_API int32_t aver_phys_ragdoll_set_target(int32_t ragdoll, const float* worldPos, const float* worldRot);

// Teleports every part to the pose and zeroes its velocity. The pose also becomes the target.
AVER_PHYS_API int32_t aver_phys_ragdoll_set_pose(int32_t ragdoll, const float* worldPos, const float* worldRot);

// Writes the current pose, 3n + 4n floats. A bone without a part is its ancestor part's transform times
// the target pose's offset between the two.
AVER_PHYS_API int32_t aver_phys_ragdoll_get_pose(int32_t ragdoll, float* outWorldPos, float* outWorldRot);

// An impulse (kg*cm/s) at a world point (cm) on the bone's part, or on its nearest ancestor part. Has no
// effect, and returns 0, in KINEMATIC mode.
AVER_PHYS_API int32_t aver_phys_ragdoll_add_impulse(int32_t ragdoll, int32_t bone, float ix, float iy, float iz,
                                                    float px, float py, float pz);

// ---- Per part drive and partial-body simulation (additive) -----------------------------------------------
//
// The calls above set one value for the whole ragdoll; these override it for the part a bone is or rides
// on (a bone with no part and no ancestor part answers 0). The ragdoll-wide set_strength stays the default
// every part inherits until it is given its own.

// POWERED-mode strength of this part's joint, 0..1; a negative value goes back to the ragdoll-wide strength.
AVER_PHYS_API int32_t aver_phys_ragdoll_set_part_strength(int32_t ragdoll, int32_t bone, float strength);

// Motor damping ratio of this part's joint (default 1, clamped to 0..20); negative goes back to the default.
AVER_PHYS_API int32_t aver_phys_ragdoll_set_part_damping(int32_t ragdoll, int32_t bone, float damping);

// Multiplies the torque ceiling of this part's motor (default 1, clamped to 0..100).
AVER_PHYS_API int32_t aver_phys_ragdoll_set_part_torque_scale(int32_t ragdoll, int32_t bone, float scale);

// PARTIAL-BODY SIMULATION. A pinned part stays kinematic in LIMP and POWERED mode: it is carried to its target
// every step like in KINEMATIC mode, while the unpinned parts are simulated and their joints still attach
// to it. Pin everything outside a region and only the region is simulated. A joint between two pinned parts
// is switched off. Pins have no effect in KINEMATIC mode (everything follows the target there) and survive a
// mode change. Default: nothing pinned.
AVER_PHYS_API int32_t aver_phys_ragdoll_set_part_pinned(int32_t ragdoll, int32_t bone, int32_t pinned);

// 1 if the bone's part is pinned, 0 if not, -1 for a dead handle, a bad bone or a bone with no part.
AVER_PHYS_API int32_t aver_phys_ragdoll_part_pinned(int32_t ragdoll, int32_t bone);

// ---- Parts ---------------------------------------------------------------------------------------------

// The body handle of the bone's part, or of the ancestor part it rides on; 0 if it has neither.
AVER_PHYS_API int32_t aver_phys_ragdoll_body_of_bone(int32_t ragdoll, int32_t bone);

// The ragdoll that owns a body handle (0 if it belongs to none), and the bone of that part in outBone
// (which may be null).
AVER_PHYS_API int32_t aver_phys_ragdoll_of_body(int32_t body, int32_t* outBone);

#ifdef __cplusplus
}
#endif
