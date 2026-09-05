#pragma once
// Two-bone inverse kinematics: bend a three-joint chain so its tip reaches a goal.
//
// THE FIRST IK IN THIS ENGINE. Before this there was none of any kind -- no FABRIK, no two-bone, no
// aim or look-at constraint anywhere in the tree, and no API that set a bone transform at all. Poses
// were produced by sampling a clip and by nothing else.
//
// ANALYTIC, NOT ITERATIVE. A three-joint chain with fixed segment lengths and a goal is a triangle,
// and a triangle is solved by the law of cosines in closed form. FABRIK and CCD exist for chains
// longer than this; using one here would be iterating toward an answer that arithmetic already has.
//
// MODEL SPACE IN, LOCAL SPACE OUT. The goal and the pole are where you want things in the posed
// skeleton's own space, which is how a caller thinks about them ("put the hand on the grip"). What
// comes back is written into Pose::local, because that is the only pose the engine has -- see
// Pose.hpp, which has no model-space pose type at all.
#include "aver/anim/Pose.hpp"
#include "aver/core/Math.hpp"

namespace aver::formats { struct OcSkeleton; }

namespace aver::anim {

// Bends `root`->`mid`->`tip` so the tip lands on `goalModel`, writing new local rotations for `root`
// and `mid` into `pose`. `tip` itself is not rotated: it is carried by the two bones above it.
//
// `poleModel` decides WHICH WAY THE CHAIN BENDS, and it has to, because a triangle with a given base
// and two given sides has two solutions mirrored about that base. The elbow is placed on the pole's
// side of the line from root to goal -- point the pole behind an arm and the elbow goes backwards.
//
// AN UNREACHABLE GOAL STRAIGHTENS THE CHAIN AT IT rather than failing. The alternative is tearing
// the limb apart to reach, which is the one outcome a rig must never produce; a fully extended arm
// pointing at something out of range is what a person does and what an animator expects to see.
// Returns true whenever it solved, including that clamped case.
//
// FALSE means it did not touch the pose: an index out of range, a chain that is not actually
// parent-linked (`mid`'s parent must be `root`, `tip`'s must be `mid`), a pose whose bone count does
// not match the skeleton, or a degenerate segment of zero length.
bool twoBoneIk(const fmt::OcSkeleton& skel, Pose& pose,
               u32 root, u32 mid, u32 tip,
               const Vec3& goalModel, const Vec3& poleModel);

// Turns one bone so its own `localAxis` ends up pointing at `targetModel`.
//
// A head that watches something, a turret, an eye. Not IK -- there is no chain and nothing to solve;
// it is one rotation, and it is here because it is the other half of what a rig does and shares every
// helper with the solver above.
//
// `localAxis` is in the BONE's space, which is what makes it authorable: "the axis this head calls
// forward" is a property of the rig, not of where the head happens to be pointing this frame.
// Returns false on a bad index, a mismatched pose, a zero axis, or a target on top of the bone.
bool aimAt(const fmt::OcSkeleton& skel, Pose& pose, u32 bone,
           const Vec3& targetModel, const Vec3& localAxis);

// The model-space position of `bone` in `pose` -- the translation row of its model matrix.
//
// Exposed because a caller solving IK almost always needs it: a goal is usually expressed relative
// to where something currently is, and recomputing the whole model array to read one position is
// what this exists to avoid writing at every call site.
bool bonePositionModel(const fmt::OcSkeleton& skel, const Pose& pose, u32 bone, Vec3& out);

} // namespace aver::anim
